#!/usr/bin/env python3
"""Run actual seekdb SQL against generated files. Requires PyMySQL and PyArrow.
Pass a connection dictionary and a disposable directory to run().
"""
import datetime
import decimal
import json
from pathlib import Path
import pymysql
import pyarrow as pa
import pyarrow.parquet as pq


def run(login, root):
    root = Path(root)
    data = root / 'regression'
    data.mkdir()
    checks = []
    db = pymysql.connect(**login, autocommit=True, charset='utf8mb4')
    cursor = db.cursor()

    def execute(sql, params=None):
        cursor.execute(sql, params)
        return cursor.fetchall()

    def equal(name, sql, expected):
        actual = execute(sql)
        assert actual == expected, (name, actual, expected)
        checks.append(name)
        print('PASS:', name, flush=True)

    def failure(name, sql, code=None):
        try:
            execute(sql)
        except pymysql.MySQLError as exc:
            if code is not None:
                assert exc.args[0] == code, (name, exc)
        else:
            raise AssertionError(name + ': expected failure')
        checks.append(name)
        print('PASS:', name, flush=True)

    def save(name, content):
        p = data / name
        p.write_text(content, encoding='utf8', newline='')
        return str(p)

    def literal(s):
        return db.escape(str(s))

    def table(fmt, name, options=''):
        return 'read_' + fmt + '(' + literal(data / name) + options + ')'

    execute('USE file_sql_regression')
    equal('Default feature switch', 'SELECT @@enable_file_sql', ((0,),))
    csv = save('rows.csv', 'id,name,value,flag,day,stamp,nil\r\n1,"中文,一",1.5,TRUE,2024-02-29,2026-01-01 12:00:00,NULL\r\n2,"say ""hi""",2,false,2026-01-01,2026-01-02T12:00:00,\r\n3,"multi\nline",,true,,,NULL\r\n')
    failure('Disabled scan', 'SELECT * FROM ' + table('csv', 'rows.csv'), 1235)
    execute('SET enable_file_sql=ON')
    equal('CSV CRLF, Unicode, quotes, multiline and NULL', 'SELECT id,name,value,flag,nil FROM ' + table('csv', 'rows.csv') + ' ORDER BY id',
          ((1, '中文,一', 1.5, 1, None), (2, 'say "hi"', 2.0, 0, None), (3, 'multi\nline', None, 1, None)))
    equal('Typed dates', 'SELECT day,stamp FROM ' + table('csv', 'rows.csv') + ' WHERE id=1', ((datetime.date(2024, 2, 29), datetime.datetime(2026, 1, 1, 12)),))
    equal('Projection, filter, order, limit and expression', 'SELECT id*10 AS x FROM ' + table('csv', 'rows.csv') + ' f WHERE id>=2 ORDER BY x DESC LIMIT 1', ((30,),))
    equal('Group and HAVING', 'SELECT flag,COUNT(*),SUM(id),ROUND(AVG(value),2),MIN(id),MAX(id) FROM ' + table('csv', 'rows.csv') + ' GROUP BY flag HAVING COUNT(*)>1', ((1, 2, decimal.Decimal('4'), 1.5, 1, 3),))
    schema = execute('DESCRIBE SELECT * FROM ' + table('csv', 'rows.csv'))
    assert [x[1] for x in schema] == ['BIGINT', 'VARCHAR', 'DOUBLE', 'BOOLEAN', 'DATE', 'DATETIME', 'NULL'], schema
    assert schema[-1][2] == 'YES'
    checks.append('DESCRIBE seven inferred types')
    failure('Unsupported sampling', 'SELECT * FROM ' + table('csv', 'rows.csv', ',sample_size=10'), 1235)
    failure('Positional format rejected', 'SELECT * FROM ' + table('csv', 'rows.csv', ",'xml'"), 1064)
    failure('Unknown option rejected', 'SELECT * FROM ' + table('csv', 'rows.csv', ',bogus=1'), 1064)
    failure('Duplicate option rejected', 'SELECT * FROM ' + table('csv', 'rows.csv', ',header=true,header=false'), 1064)
    save('nohead.csv', '1;alpha\n2;beta\n')
    equal('CSV delimiter and header=false', 'SELECT column0,column1 FROM ' + table('csv', 'nohead.csv', ",header=false,delim=';'"), ((1, 'alpha'), (2, 'beta')))
    save('quotes.csv', "id;text\n1;'it''s fine'\n")
    equal('Custom quote and escape default', 'SELECT text FROM ' + table('csv', 'quotes.csv', ",delim=';',quote='\\\''"), (("it's fine",),))
    save('bounds.csv', 'min,max,overflow,invalid_date\n-9223372036854775808,9223372036854775807,9223372036854775808,2023-02-29\n')
    equal('BIGINT bounds and overflow text', 'SELECT * FROM ' + table('csv', 'bounds.csv'), ((-9223372036854775808, 9223372036854775807, '9223372036854775808', '2023-02-29'),))
    save('mixed.csv', 'x,y\n1,2024-01-01\n2.25,2024-01-02T10:00:00\n')
    equal('Numeric/date widening', 'SELECT * FROM ' + table('csv', 'mixed.csv'), ((1.0, datetime.datetime(2024, 1, 1)), (2.25, datetime.datetime(2024, 1, 2, 10))))
    save('duplicates.csv', 'x,x,X__2\n1,2,3\n')
    equal('Header collision names', 'SELECT x,x__2,X__2__2 FROM ' + table('csv', 'duplicates.csv'), ((1, 2, 3),))
    for name, content in [('bad_width.csv', 'a,b\n1\n'), ('bad_quote.csv', 'a\n"unfinished\n'), ('bad_tail.csv', 'a\n"x"junk\n')]:
        save(name, content)
        failure('Reject ' + name, 'SELECT * FROM ' + table('csv', name))
    (data/'utf8.csv').write_bytes(b'a\n\xff\n')
    failure('Invalid UTF-8', 'SELECT * FROM ' + table('csv', 'utf8.csv'))
    save('rows.jsonl', '\n{"id":1,"label":"a","score":1.5,"flag":true}\n{"id":3,"extra":"x","flag":false}\n{"id":4,"label":null}\n')
    equal('JSONL union and missing fields', 'SELECT id,label,score,flag,extra FROM ' + table('ndjson', 'rows.jsonl') + ' ORDER BY id', ((1,'a',1.5,1,None),(3,None,None,0,'x'),(4,None,None,None,None)))
    save('conflict.jsonl', '{"x":1,"y":"1"}\n{"x":"two","y":2}\n')
    equal('JSON type conflict retains text', 'SELECT x,y FROM ' + table('json', 'conflict.jsonl', ",format='newline_delimited'"), (('1','1'),('two','2')))
    for name, content in [('nested.jsonl','{"x":{}}\n'),('array.jsonl','{"x":[]}\n'),('malformed.jsonl','{"x":}\n'),('duplicate.jsonl','{"x":1,"X":2}\n')]:
        save(name,content)
        failure('Reject '+name,'SELECT * FROM '+table('json',name))
    equal('INNER JOIN across formats', 'SELECT a.id,b.label FROM ' + table('csv','rows.csv')+' a INNER JOIN '+table('json','rows.jsonl')+' b ON a.id=b.id ORDER BY a.id', ((1,'a'),(3,None)))
    equal('LEFT JOIN missing matches', 'SELECT a.id,b.extra FROM '+table('csv','rows.csv')+' a LEFT JOIN '+table('json','rows.jsonl')+' b ON a.id=b.id ORDER BY a.id', ((1,None),(2,None),(3,'x')))
    equal('Nested-loop rescan', 'SELECT /*+ USE_NL(a b) */ a.id,b.id FROM '+table('csv','rows.csv')+' a CROSS JOIN '+table('json','rows.jsonl')+' b WHERE a.id<3 ORDER BY a.id,b.id', ((1,1),(1,3),(1,4),(2,1),(2,3),(2,4)))
    t=pa.table({'id':[1,2,None,4,5],'name':['中文','Bob',None,'x',''],'score':[1.25,2.5,None,4.0,-0.1],'flag':[True,False,None,True,False]})
    for codec in ['NONE','snappy','gzip']:
        for version in ['1.0','2.0']:
            name=codec+'-'+version+'.parquet'
            pq.write_table(t,data/name,compression=codec,row_group_size=2,data_page_version=version)
            equal('Parquet '+codec+' '+version,'SELECT id,name,score,flag FROM '+table('parquet',name)+' ORDER BY id', ((None,None,None,None),(1,'中文',1.25,1),(2,'Bob',2.5,0),(4,'x',4.0,1),(5,'',-0.1,0)))
            equal('Parquet projected aggregate '+codec+' '+version,'SELECT COUNT(*),COUNT(id),SUM(id) FROM '+table('parquet',name),((5,4,decimal.Decimal(12)),))
    nulls = pa.table({'id':pa.array([None,None,3],type=pa.int64()),
                      'label':pa.array([None,None,'three'],type=pa.string())})
    for codec in ['NONE','snappy','gzip']:
        for version in ['1.0','2.0']:
            name = 'null-group-'+codec+'-'+version+'.parquet'
            pq.write_table(nulls,data/name,row_group_size=2,data_page_version=version,compression=codec)
            equal('Parquet all NULL row group '+codec+' '+version,'SELECT * FROM '+table('parquet',name),
                  ((None,None),(None,None),(3,'three')))
    temporal=pa.table({'day':pa.array([datetime.date(2024,2,29),None],type=pa.date32()),'stamp':pa.array([datetime.datetime(2024,2,29,1,2,3,456789),None],type=pa.timestamp('us'))})
    pq.write_table(temporal,data/'time.parquet',compression='snappy')
    equal('Parquet logical DATE and TIMESTAMP','SELECT * FROM '+table('parquet','time.parquet'),((datetime.date(2024,2,29),datetime.datetime(2024,2,29,1,2,3,456789)),(None,None)))
    equal('CSV-Parquet JOIN','SELECT a.id,p.name FROM '+table('csv','rows.csv')+' a JOIN '+table('parquet','snappy-2.0.parquet')+' p ON a.id=p.id ORDER BY a.id',((1,'中文'),(2,'Bob')))
    failure('Corrupt Parquet footer','SELECT * FROM '+table('parquet','rows.csv'))
    out=data/'export.csv'
    execute('COPY (SELECT id,name,value,nil FROM '+table('csv','rows.csv')+' ORDER BY id) TO '+literal(out)+" (FORMAT csv,HEADER,NULLSTR 'NULL')")
    equal('COPY roundtrip','SELECT id,name,value,nil FROM '+table('csv','export.csv')+' ORDER BY id',((1,'中文,一',1.5,None),(2,'say "hi"',2.0,None),(3,'multi\nline',None,None)))
    failure('COPY existing target','COPY (SELECT 1 AS id) TO '+literal(out)+' (HEADER)')
    bad=data/'failed.csv'
    failure('COPY failed query','COPY (SELECT CAST(name AS JSON) FROM '+table('csv','rows.csv')+') TO '+literal(bad)+' (HEADER)')
    assert not bad.exists() and not list(data.glob('.seekdb-copy-*'))
    checks.append('COPY error cleans temporary files')
    execute('CREATE TABLE native_copy(id INT,name VARCHAR(20))')
    execute("INSERT INTO native_copy VALUES(1,'normal SQL')")
    execute('COPY native_copy TO '+literal(data/'native.csv')+' (HEADER)')
    equal('COPY native table','SELECT * FROM '+table('csv','native.csv'),((1,'normal SQL'),))
    execute('DROP TABLE native_copy')
    equal('Glob discovery sorted file column','SELECT SUBSTRING_INDEX(file,\'/\',-1) FROM glob('+literal(str(data/'*.jsonl'))+') ORDER BY file',tuple((x.name,) for x in sorted(data.glob('*.jsonl'))))
    failure('Outside secure root','SELECT * FROM read_csv(\'/etc/passwd\')')
    (data/'escape.csv').symlink_to('/etc/passwd')
    failure('Symlink escape','SELECT * FROM '+table('csv','escape.csv'))
    failure('Glob outside secure root',"SELECT * FROM glob('/etc/*')")
    failure('COPY outside secure root',"COPY (SELECT 1) TO '/tmp/seekdb-file-sql-outside.csv'")
    execute('SET GLOBAL secure_file_priv=\'\'')
    failure('Empty secure_file_priv denies reads','SELECT * FROM '+table('csv','rows.csv'))
    execute('SET GLOBAL secure_file_priv='+literal(root))
    save('change.csv','id\n1\n')
    equal('Initial dynamic schema','SELECT * FROM '+table('csv','change.csv'),((1,),))
    save('change.csv','value,other\nnew,2\n')
    equal('Repeated query refreshes dynamic schema','SELECT * FROM '+table('csv','change.csv'),(('new',2),))
    # Streaming exercises multiple engine batches and aggregate execution.
    save('large.csv','id,group_id,payload\n'+''.join(f'{i},{i%7},value-{i:06}\n' for i in range(10000)))
    equal('Ten thousand streamed rows','SELECT COUNT(*),SUM(id),MIN(id),MAX(id) FROM '+table('csv','large.csv'),((10000,decimal.Decimal(49995000),0,9999),))
    # Force real multirow batches, including string lifetimes and the final
    # partial batch; a one-row compatibility adapter cannot satisfy this check.
    batch_hint = "/*+ OPT_PARAM('rowsets_max_rows' 64) */ "
    equal('Multi-batch projection and filter', 'SELECT '+batch_hint+'id,payload,CONCAT(\'row-\',id) FROM '+table('csv','large.csv')+' WHERE id%97=0 ORDER BY id',
          tuple((i, f'value-{i:06}', 'row-'+str(i)) for i in range(10000) if i%97==0))
    equal('Multi-batch groups', 'SELECT '+batch_hint+'group_id,COUNT(*),SUM(id) FROM '+table('csv','large.csv')+' GROUP BY group_id ORDER BY group_id',
          tuple((g, len(range(g,10000,7)), decimal.Decimal(sum(range(g,10000,7)))) for g in range(7)))
    plan = execute('EXPLAIN EXTENDED SELECT '+batch_hint+'id FROM '+table('csv','large.csv'))
    plan_text = '\n'.join(str(row) for row in plan)
    assert 'FUNCTION_TABLE' in plan_text and 'rowset=64' in plan_text, plan_text
    checks.append('Native FUNCTION_TABLE plan with rowset=64')
    execute('COPY (SELECT '+batch_hint+'id,payload FROM '+table('csv','large.csv')+') TO '+literal(data/'large-copy.csv')+' (HEADER)')
    equal('COPY multiple batches', 'SELECT COUNT(*),SUM(id),MAX(payload) FROM '+table('csv','large-copy.csv'), ((10000,decimal.Decimal(49995000),'value-009999'),))
    execute('COPY (SELECT id,name,nil FROM '+table('csv','rows.csv')+') TO '+literal(data/'custom-copy.csv')+
            " (HEADER,DELIMITER ';',QUOTE "+literal("'")+",ESCAPE "+literal('\\')+",NULLSTR 'NA')")
    equal('COPY custom delimiter quote escape and null text', 'SELECT * FROM '+table('csv','custom-copy.csv',
          ",delim=';',quote="+literal("'")+",escape="+literal('\\')+",nullstr='NA'")+' ORDER BY id',
          ((1,'中文,一',None),(2,'say "hi"',None),(3,'multi\nline',None)))
    described = execute('DESCRIBE SELECT 1')
    explained = execute('EXPLAIN SELECT 1')
    assert described == explained, (described, explained)
    checks.append('Ordinary DESCRIBE SELECT preserves EXPLAIN behavior')
    execute('SET enable_file_sql=OFF')
    failure('Switch off after successful scan','SELECT * FROM '+table('csv','rows.csv'),1235)
    equal('Ordinary SQL remains available','SELECT 1+2,UPPER(\'seekdb\')',((3,'SEEKDB'),))
    cursor.close();db.close()
    return {'passed':len(checks),'checks':checks}
