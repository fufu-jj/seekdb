#!/usr/bin/env python3
"""Generate disposable data for test_file_scan_utils; requires PyArrow only.
Production scanning does not depend on Python or PyArrow.
"""
import argparse
import base64
from pathlib import Path
import shutil
import pyarrow as pa
import pyarrow.parquet as pq

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[4]
    data = repo / 'tools/deploy/mysql_test/test_suite/file_sql/data'
    for name in ['basic.csv', 'basic.jsonl']:
        shutil.copy2(data / name, args.directory / name)
    (args.directory / 'simple.parquet').write_bytes(
        base64.b64decode((data / 'simple.parquet.b64').read_bytes()))
    table = pa.table({'id':[1,2,None,4,5], 'name':['中文','Bob',None,'x',''],
                      'score':[1.25,2.5,None,4.0,-0.1], 'flag':[True,False,None,True,False]})
    for codec in ['NONE','snappy','gzip']:
        for version in ['1.0','2.0']:
            pq.write_table(table, args.directory / (codec+'-'+version+'.parquet'),
                           compression=codec, row_group_size=2, data_page_version=version)
