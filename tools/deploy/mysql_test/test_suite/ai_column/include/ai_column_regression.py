#!/usr/bin/env python3
"""Additional AI column boundary tests against an isolated native server.

Requires pymysql and SEEKDB_TEST_PASSWORD. The seed/resume phases must use the
same database directory with a server restart between them. Run resume with
--mock-port matching the port printed by seed. No external model is used.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import threading
import time

import pymysql


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        value = next(m['content'] for m in reversed(body['messages']) if m['role'] == 'user')
        content = 'unquoted-text' if value == 'invalid-types' else 'ok:' + value
        data = json.dumps({'choices': [{'message': {'content': content}}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def run(args):
    checks = 0
    def check(condition, label):
        nonlocal checks
        assert condition, label
        checks += 1
        print('PASS', label, flush=True)

    conn = pymysql.connect(host=args.host, port=args.port, user=args.user,
                           password=os.environ['SEEKDB_TEST_PASSWORD'],
                           charset='utf8mb4', autocommit=True, read_timeout=30)
    def rows(sql):
        with conn.cursor() as cur:
            cur.execute(sql)
            return cur.fetchall()
    def ready(sql, expected):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            result = rows(sql)
            if result == expected:
                return result
            time.sleep(.1)
        raise AssertionError((sql, result, expected))
    def error(sql, code):
        try:
            rows(sql)
        except pymysql.MySQLError as exc:
            check(exc.args[0] == code, 'expected error %d for %s' % (code, sql))
        else:
            raise AssertionError('Unexpected success: ' + sql)
    def drop_model(name, endpoint):
        for kind, value, missing in [('AI_MODEL_ENDPOINT', endpoint,11112), ('AI_MODEL',name,11118)]:
            try:
                rows("call DBMS_AI_SERVICE.DROP_%s('%s')" % (kind,value))
            except pymysql.MySQLError as exc:
                if exc.args[0] != missing:
                    raise

    server = None
    port = args.mock_port
    # Seed leaves the HTTP service offline and a durable, unexpired lease.
    if args.phase != 'seed':
        server = ThreadingHTTPServer(('127.0.0.1', args.mock_port), Handler)
        port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        if args.phase == 'resume':
            ready('select result from ai_column_restart.docs where id=1', (('ok:restart',),))
            check(True, 'unexpired lease reclaimed after server restart')
            check(rows('select count(*) from oceanbase.__all_ai_column_task t join oceanbase.__all_table a on a.table_id=t.table_id where a.table_name=\'docs\'') == ((0,),), 'recovered task removed after publication')
            rows('drop database ai_column_restart')
            rows("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('restart_ep')")
            rows("call DBMS_AI_SERVICE.DROP_AI_MODEL('restart_model')")
        elif args.phase == 'seed':
            print('RESTART_MOCK_PORT', port, flush=True)
            rows('create database ai_column_restart')
            rows("call DBMS_AI_SERVICE.CREATE_AI_MODEL('restart_model', '{\"type\":\"completion\",\"model_name\":\"test\"}')")
            endpoint = json.dumps({'ai_model_name': 'restart_model', 'url': 'http://127.0.0.1:%d/v1/chat/completions' % port, 'access_key': 'test-only', 'provider': 'openai', 'request_model_name': 'test'})
            with conn.cursor() as cur:
                cur.execute('call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)', ('restart_ep', endpoint))
            rows("create table ai_column_restart.docs(id bigint primary key, content varchar(100), result varchar(100) AI COLUMN(AI_COMPLETE('restart_model',content)) WITH(max_retries=16))")
            rows("insert into ai_column_restart.docs(id,content) values(1,'restart')")
            rows("update oceanbase.__all_ai_column_task set lease_owner='interrupted-process',lease_until=date_add(now(6),interval 300 second)")
            check(rows('select count(*) from oceanbase.__all_ai_column_task where lease_owner=\'interrupted-process\'') == ((1,),), 'durable pending task with unexpired lease prepared')
        else:
            rows('drop database if exists ai_column_extended')
            drop_model('extended_model', 'extended_ep')
            rows('create database ai_column_extended')
            rows("call DBMS_AI_SERVICE.CREATE_AI_MODEL('extended_model', '{\"type\":\"completion\",\"model_name\":\"test\"}')")
            endpoint = json.dumps({'ai_model_name': 'extended_model', 'url': 'http://127.0.0.1:%d/v1/chat/completions' % port, 'access_key': 'test-only', 'provider': 'openai', 'request_model_name': 'test'})
            with conn.cursor() as cur:
                cur.execute('call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)', ('extended_ep', endpoint))
            rows("create table ai_column_extended.docs(id bigint primary key, content varchar(100), result varchar(100) AI COLUMN(AI_COMPLETE('extended_model',content)))")
            rows("insert into ai_column_extended.docs(id,content) values(1,'one')")
            ready('select result from ai_column_extended.docs', (('ok:one',),))
            ddl = rows('show create table ai_column_extended.docs')[0][1]
            check('AI COLUMN' in ddl and 'seekdb.ai-column.' not in ddl and '__ai_' not in ddl, 'SHOW CREATE exposes native AI definition and hides storage fields')
            check(rows('show ai columns from ai_column_extended.docs')[0][0] == 'result', 'SHOW AI COLUMNS exposes definitions')
            error('alter table ai_column_extended.docs drop column content', 1235)
            error('alter table ai_column_extended.docs change content renamed_content varchar(100)', 1235)
            rows("alter table ai_column_extended.docs modify result varchar(100) AI COLUMN(AI_COMPLETE('extended_model',concat('new:',content)))")
            ready('select result from ai_column_extended.docs', (('ok:new:one',),))
            check(True, 'MODIFY AI definition invalidates and recomputes existing data')
            rows('alter table ai_column_extended.docs rename to ai_column_extended.renamed')
            rows("update ai_column_extended.renamed set content='two' where id=1")
            ready('select result from ai_column_extended.renamed', (('ok:new:two',),))
            check(True, 'table rename keeps task names and trigger definitions consistent')
            rows("prepare ai_select from 'select result from ai_column_extended.renamed'")
            check(rows('execute ai_select') == (('ok:new:two',),), 'prepared statement returns ready results')
            rows('start transaction')
            rows("update ai_column_extended.renamed set content='pending' where id=1")
            rows('set session ai_column_strict_read=on')
            error('execute ai_select', 11120)
            rows('rollback')
            check(rows('execute ai_select') == (('ok:new:two',),), 'prepared strict read recovers after rollback')
            rows('deallocate prepare ai_select')
            rows('set session ai_column_strict_read=off')
            rows("create table ai_column_extended.typed(id bigint primary key, content varchar(100), n tinyint AI COLUMN(AI_COMPLETE('extended_model',content)) WITH(max_retries=1), j json AI COLUMN(AI_COMPLETE('extended_model',content)) WITH(max_retries=1), short_text varchar(3) AI COLUMN(AI_COMPLETE('extended_model',content)) WITH(max_retries=1), good varchar(100) AI COLUMN(AI_COMPLETE('extended_model',content)) WITH(max_retries=1))")
            rows("insert into ai_column_extended.typed(id,content) values(1,'invalid-types')")
            ready('select good from ai_column_extended.typed', (('unquoted-text',),))
            check(rows('select n,j,short_text,good from ai_column_extended.typed') == ((None,None,None,'unquoted-text'),), 'type failures publish NULL while successful group member is READY')
            rows('set session ai_column_strict_read=on')
            check(rows('select good from ai_column_extended.typed') == (('unquoted-text',),), 'strict read accepts successful member of partially failed group')
            error('select n from ai_column_extended.typed', 11120)
            rows('set session ai_column_strict_read=off')
            check(rows("select count(*) from oceanbase.__all_ai_column_task t join oceanbase.__all_table a on a.table_id=t.table_id where a.table_name='typed'") == ((0,),), 'terminal type failures do not leave pending tasks')
            rows('insert into ai_column_extended.typed(id,content) values(2,NULL)')
            ready("select count(*) from oceanbase.__all_ai_column_task t join oceanbase.__all_table a on a.table_id=t.table_id where a.table_name='typed'", ((0,),))
            rows('set session ai_column_strict_read=on')
            # Native AI_COMPLETE rejects a NULL prompt. The durable task must
            # reach FAILED, and strict reads must distinguish it from READY.
            error('select good from ai_column_extended.typed where id=2', 11120)
            rows('set session ai_column_strict_read=off')
            rows("create table ai_column_extended.composite(code varchar(30), seq bigint, content varchar(100), result varchar(100) AI COLUMN(AI_COMPLETE('extended_model',content)), primary key(code,seq))")
            with conn.cursor() as cur:
                cur.execute('insert into ai_column_extended.composite(code,seq,content) values(%s,%s,%s)', ("测试'\\标识", 1, 'composite'))
            ready('select result from ai_column_extended.composite', (('ok:composite',),))
            check(True, 'composite primary key handles Unicode, quotes and backslashes')
            rows('drop database ai_column_extended')
            rows("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('extended_ep')")
            rows("call DBMS_AI_SERVICE.DROP_AI_MODEL('extended_model')")
    finally:
        conn.close()
        if args.phase != 'seed':
            server.shutdown()
            server.server_close()
    print('%d checks passed (%s)' % (checks,args.phase), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2883)
    parser.add_argument('--user', default='root')
    parser.add_argument('--mock-port', type=int, default=19126)
    parser.add_argument('--phase', choices=['extended','seed','resume'], default='extended')
    run(parser.parse_args())
