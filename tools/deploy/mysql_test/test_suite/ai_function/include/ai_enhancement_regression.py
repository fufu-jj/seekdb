#!/usr/bin/env python3
"""Exercise AI SQL against an in-process HTTP mock; requires pymysql.

Run against an isolated, freshly built seekdb:
  SEEKDB_TEST_PASSWORD=... python3 ai_enhancement_regression.py --port 2883
The mock binds to loopback on an ephemeral port. No real model key is used.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import threading
import traceback
import uuid

import pymysql


def embedding(text, dimensions=4):
    digest = hashlib.sha256(text.encode()).digest()
    return [round(digest[i % len(digest)] / 256, 6) for i in range(dimensions)]


def relevance(text):
    match = re.search(r'\d+', text)
    return (int(match.group()) % 7) / 8 if match else 0.5


class MockHandler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append((self.path, body))
        fault = self.path.split('/')[2] if self.path.startswith('/bad/') else ''
        status = 200
        if self.path.endswith('/generate'):
            assert body['stream'] is False, 'Ollama must use non-streaming generation'
            response = {'response': body['prompt'], 'done': True}
        elif self.path.endswith('/completions'):
            response = {'choices': [{'message': {'content': body['messages'][-1]['content']}}]}
            if fault == 'empty':
                response['choices'][0]['message']['content'] = ''
            elif fault == 'null':
                response['choices'][0]['message']['content'] = None
            elif fault == 'object':
                response['choices'][0]['message']['content'] = {}
            elif fault == 'missing':
                response = {'choices': []}
        elif self.path.endswith('/embeddings') or self.path.endswith('/embed'):
            inputs = body['input']
            assert isinstance(inputs, list) and all(isinstance(x, str) for x in inputs)
            vectors = [embedding(x, body.get('dimensions', 4)) for x in inputs]
            if fault == 'dimension':
                vectors[0].append(0)
            elif fault == 'string':
                vectors[0][0] = 'invalid'
            elif fault == 'null':
                vectors[0][0] = None
            elif fault == 'empty':
                vectors[0] = []
            if self.path.endswith('/embed'):
                response = {'embeddings': vectors}
            else:
                # Deliberately return the opposite of request order.
                data = [{'index': i, 'embedding': v} for i, v in enumerate(vectors)][::-1]
                if fault == 'duplicate' and len(data) > 1:
                    data[0]['index'] = data[1]['index']
                elif fault == 'range':
                    data[0]['index'] = len(data)
                elif fault == 'missing':
                    data.pop()
                response = {'data': data}
        elif self.path.endswith('/rerank'):
            documents = body.get('documents', body.get('input', {}).get('documents'))
            assert isinstance(documents, list) and all(isinstance(x, str) for x in documents)
            results = [{'index': i, 'relevance_score': relevance(t)} for i, t in enumerate(documents)][::-1]
            if fault == 'duplicate':
                results[0]['index'] = results[1]['index']
            elif fault == 'range':
                results[0]['index'] = len(documents)
            elif fault == 'missing':
                results.pop()
            elif fault == 'score':
                results[0]['relevance_score'] = 'bad'
            response = {'results': results}
        else:
            status, response = 404, {'error': 'unexpected test route'}
        payload = json.dumps(response, ensure_ascii=False).encode()
        if fault == 'array':
            payload = b'[]'
        elif fault == 'malformed':
            payload = b'{broken'
        elif fault == 'http':
            status = 400
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def run(args):
    server = ThreadingHTTPServer(('127.0.0.1', 0), MockHandler)
    server.requests = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    conn = pymysql.connect(host=args.host, port=args.port, user=args.user,
                           password=os.getenv('SEEKDB_TEST_PASSWORD', ''),
                           autocommit=True, charset='utf8mb4', read_timeout=90)
    prefix = 'ai_reg_' + uuid.uuid4().hex[:8]
    models, endpoints, checks = [], [], []
    database = prefix
    cursor = conn.cursor()

    def execute(sql, params=None):
        cursor.execute(sql, params)
        return cursor.fetchall() if cursor.description else ()

    def scalar(sql, params=None):
        return execute(sql, params)[0][0]

    def decode(value):
        return json.loads(value)

    def check(name, operation):
        operation()
        checks.append(name)
        print('PASS', name, flush=True)

    def equal(actual, expected):
        assert actual == expected, (str(actual)[:400], str(expected)[:400])

    def fails(sql, params=None):
        try:
            execute(sql, params)
        except pymysql.MySQLError:
            return
        raise AssertionError('Expected SQL error: ' + sql)

    def endpoint(kind, route, provider='openai', key='mock-key'):
        name = prefix + '_' + str(len(models))
        ep = name + '_ep'
        execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                (name, json.dumps({'type': kind, 'model_name': name + '_request'})))
        models.append(name)
        execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                (ep, json.dumps({'ai_model_name': name, 'scope': 'all',
                                'url': f'http://127.0.0.1:{server.server_port}' + route,
                                'access_key': key, 'provider': provider})))
        endpoints.append(ep)
        return name

    try:
        execute('CREATE DATABASE ' + database)
        execute('USE ' + database)
        complete = endpoint('completion', '/v1/chat/completions')
        embed = endpoint('dense_embedding', '/v1/embeddings')
        rerank = endpoint('rerank', '/v1/rerank', 'siliconflow')
        check('JSON prompt preserves arrays and objects', lambda: equal(
            decode(scalar("SELECT AI_PROMPT('{0}/{0}/{1}',JSON_ARRAY(1,true),JSON_OBJECT('k',2))"))['args'],
            [[1, True], {'k': 2}]))
        check('Repeated long placeholder', lambda: equal(
            scalar("SELECT AI_COMPLETE(%s, AI_PROMPT('{0}{0}{0}', %s))", (complete, '长文' * 12000)),
            '长文' * 36000))
        check('Nested prompt and Unicode escapes', lambda: equal(
            scalar("SELECT AI_COMPLETE(%s, AI_PROMPT('A{0}B',AI_PROMPT('{0}/{0}',%s)))",
                   (complete, '你好\n"\\\x00')),
            'A' + '你好\n"\\\x00/你好\n"\\\x00' + 'B'))
        execute('CREATE TABLE lob_inputs(id INT PRIMARY KEY, template LONGTEXT, content LONGTEXT)')
        execute('INSERT INTO lob_inputs VALUES(1,%s,%s)', ('模板:{0}', '很长的正文' * 12000))
        check('LOB template and argument', lambda: equal(
            scalar('SELECT AI_COMPLETE(%s,AI_PROMPT(template,content)) FROM lob_inputs', (complete,)),
            '模板:' + '很长的正文' * 12000))
        check('Illegal and overflowing placeholders', lambda: [fails(
            'SELECT AI_COMPLETE(%s, AI_PROMPT(%s,\'x\'))', (complete, template))
            for template in ['{1}', '{99999999999999999999999999999999999999}']])
        check('SQL NULL completion config', lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,'hello',NULL)", (complete,)), 'hello'))
        check('JSON completion config', lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,'hello',JSON_OBJECT('temperature',0.2,'max_tokens',16))", (complete,)), 'hello'))
        _, body = server.requests[-1]
        equal(body['max_tokens'], 16)
        equal(body['temperature'], 0.2)
        check('Config must be object', lambda: fails("SELECT AI_COMPLETE(%s,'hello','[]')", (complete,)))
        for fault in ['empty', 'null', 'object', 'missing', 'array', 'malformed', 'http']:
            bad = endpoint('completion', f'/bad/{fault}/chat/completions')
            check('Completion rejects ' + fault, lambda m=bad: fails("SELECT AI_COMPLETE(%s,'hello')", (m,)))
        check('Default embedding values', lambda: equal(
            decode(scalar("SELECT AI_EMBED(%s,'single')", (embed,))), embedding('single')))
        check('Explicit embedding dimensions', lambda: equal(
            decode(scalar("SELECT AI_EMBED(%s,'single',768)", (embed,))), embedding('single', 768)))
        check('Embedding NULL propagation', lambda: equal(
            scalar('SELECT AI_EMBED(%s,NULL)', (embed,)), None))
        for dimension in ['0', '-1', '1.5', "'4'", 'NULL']:
            check('Reject dimension ' + dimension, lambda d=dimension: fails(
                "SELECT AI_EMBED(%s,'single'," + d + ')', (embed,)))
        for fault in ['dimension', 'string', 'null', 'empty', 'range', 'missing', 'array']:
            bad = endpoint('dense_embedding', f'/bad/{fault}/embeddings')
            check('Embedding rejects ' + fault, lambda m=bad: fails("SELECT AI_EMBED(%s,'single',4)", (m,)))
        ollama_complete = endpoint('completion', '/api/generate', 'oLlAmA', '')
        execute('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
                (endpoints[-1], '{"provider":"Ollama"}'))
        check('Ollama non-streaming completion', lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,'ollama',JSON_OBJECT('stream',true))", (ollama_complete,)), 'ollama'))
        ollama_embed = endpoint('dense_embedding', '/api/embed', 'OLLAMA', '')
        check('Ollama native embedding', lambda: equal(
            decode(scalar("SELECT AI_EMBED(%s,'ollama')", (ollama_embed,))), embedding('ollama')))
        documents = ['doc ' + str(i) for i in range(47)]
        order = sorted(range(len(documents)), key=lambda i: (-relevance(documents[i]), i))
        check('Rerank global indices and stable ties across batches', lambda: equal(
            [x['index'] for x in decode(scalar('SELECT AI_RERANK(%s,\'query\',%s,NULL)',
                                             (rerank, json.dumps(documents))))], order))
        objects = [{'id': 'id-' + str(i), 'body': text, 'extra': i} for i, text in enumerate(documents)]
        check('Rerank object IDs across batches', lambda: equal(
            [x['id'] for x in decode(scalar("SELECT AI_RERANK(%s,'query',%s,'body')",
                                          (rerank, json.dumps(objects))))], [objects[i]['id'] for i in order]))
        for fault in ['duplicate', 'range', 'missing', 'score', 'array']:
            bad = endpoint('rerank', f'/bad/{fault}/rerank', 'siliconflow')
            check('Rerank rejects ' + fault, lambda m=bad: fails(
                "SELECT AI_RERANK(%s,'query','[\"a\",\"b\"]')", (m,)))
        execute('CREATE TABLE batch_inputs(id INT PRIMARY KEY, content LONGTEXT, dimensions INT)')
        rows = [(i, None if i % 7 == 0 else 'row ' + str(i), 3 + i % 2) for i in range(145)]
        cursor.executemany('INSERT INTO batch_inputs VALUES(%s,%s,%s)', rows)
        before = len(server.requests)
        results = execute('SELECT id,AI_EMBED(%s,content) FROM batch_inputs ORDER BY id', (embed,))
        check('Batch NULL and row mapping', lambda: equal(
            [(i, decode(v) if v is not None else None) for i, v in results],
            [(i, embedding(text) if text is not None else None) for i, text, _ in rows]))
        check('At least one request contains multiple rows', lambda: equal(
            any(len(body.get('input', [])) > 1 for _, body in server.requests[before:]), True))
        check('Row-dependent dimensions', lambda: equal(
            [(i, decode(v) if v is not None else None) for i, v in execute(
                'SELECT id,AI_EMBED(%s,content,dimensions) FROM batch_inputs ORDER BY id', (embed,))],
            [(i, embedding(text, dim) if text is not None else None) for i, text, dim in rows]))
        check('Filtered rows and LIMIT', lambda: equal(
            [(i, decode(v) if v is not None else None) for i, v in execute(
                'SELECT id,AI_EMBED(%s,content) FROM batch_inputs WHERE id%%3=1 ORDER BY id LIMIT 11', (embed,))],
            [(i, embedding(text) if text is not None else None) for i, text, _ in rows if i % 3 == 1][:11]))
        before = len(server.requests)
        check('All-NULL batch makes no HTTP request', lambda: equal(
            execute('SELECT AI_EMBED(%s,content) FROM batch_inputs WHERE id%%7=0', (embed,)),
            tuple((None,) for _ in rows[::7])))
        equal(len(server.requests), before)
        other = endpoint('dense_embedding', '/v1/embeddings')
        check('Row-dependent models', lambda: equal(
            [(i, decode(v) if v is not None else None) for i, v in execute(
                'SELECT id,AI_EMBED(CASE WHEN id%%2=0 THEN %s ELSE %s END,content) '
                'FROM batch_inputs ORDER BY id', (embed, other))],
            [(i, embedding(text) if text is not None else None) for i, text, _ in rows]))
        bad = endpoint('dense_embedding', '/bad/duplicate/embeddings')
        check('Duplicate embedding indices fail the batch', lambda: fails(
            'SELECT AI_EMBED(%s,content) FROM batch_inputs WHERE content IS NOT NULL', (bad,)))
        check('Healthy query after batch failure', lambda: equal(
            decode(scalar("SELECT AI_EMBED(%s,'after-error')", (embed,))), embedding('after-error')))
        print(json.dumps({'passed': len(checks), 'requests': len(server.requests), 'checks': checks}, ensure_ascii=False))
    finally:
        for ep in reversed(endpoints):
            try:
                execute('CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (ep,))
            except pymysql.MySQLError:
                traceback.print_exc()
        for model in reversed(models):
            try:
                execute('CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (model,))
            except pymysql.MySQLError:
                traceback.print_exc()
        execute('DROP DATABASE IF EXISTS ' + database)
        cursor.close()
        conn.close()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2883)
    parser.add_argument('--user', default='root')
    run(parser.parse_args())
