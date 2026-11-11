#!/usr/bin/env python3
"""Exercise independent word maps during concurrent TOKENIZE and indexed DML.

Run on a dedicated test instance; creates and drops fts_word_map_concurrency.
Requires PyMySQL. Supply credentials through SEEKDB_TEST_PASSWORD.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import threading

import pymysql


def run(config):
    config = dict(config, charset="utf8mb4", autocommit=True,
                  read_timeout=120, write_timeout=120)
    database = "fts_word_map_concurrency"
    workers = 4
    iterations = 50
    rows_per_worker = 16
    ready = threading.Barrier(workers, timeout=30)

    def texts(worker):
        return (
            ("beng", f"threadword{worker} repeated repeated alphabet"),
            ("ik", f"数据库 全文索引 一百二十三平方米 threadword{worker}"),
        )

    with pymysql.connect(**config) as setup:
        with setup.cursor() as cur:
            cur.execute(f"DROP DATABASE IF EXISTS {database}")
            cur.execute(f"CREATE DATABASE {database}")
            cur.execute(f"USE {database}")
            cur.execute("SET ob_query_timeout=120000000")
            cur.execute("CREATE TABLE docs(id INT PRIMARY KEY, cn VARCHAR(1024), "
                        "en VARCHAR(1024), FULLTEXT INDEX f_cn(cn) WITH PARSER ik, "
                        "FULLTEXT INDEX f_en(en) WITH PARSER beng)")
            expected = {}
            for worker in range(workers):
                for parser, text in texts(worker):
                    cur.execute("SELECT tokenize(%s,%s,'[{\"output\":\"all\"}]') AS tokens",
                                (text, parser))
                    expected[worker, parser] = cur.fetchone()[0]

        def exercise(worker):
            with pymysql.connect(**config, database=database) as connection:
                with connection.cursor() as cur:
                    cur.execute("SET ob_query_timeout=120000000")
                    ready.wait()
                    for _ in range(iterations):
                        for parser, text in texts(worker):
                            cur.execute("SELECT tokenize(%s,%s,'[{\"output\":\"all\"}]') AS tokens",
                                        (text, parser))
                            assert cur.fetchone()[0] == expected[worker, parser], (worker, parser)
                    connection.begin()
                    en, cn = texts(worker)
                    for row in range(rows_per_worker):
                        cur.execute("INSERT INTO docs VALUES(%s,%s,%s)",
                                    (worker * 1000 + row, cn[1], en[1]))
                    if worker % 2:
                        connection.rollback()
                    else:
                        connection.commit()

        try:
            with ThreadPoolExecutor(max_workers=workers) as pool:
                list(pool.map(exercise, range(workers)))
            with setup.cursor() as cur:
                for query in (
                    "SELECT COUNT(*) FROM docs",
                    "SELECT COUNT(*) FROM docs WHERE MATCH(en) AGAINST('repeated alphabet')",
                    "SELECT COUNT(*) FROM docs WHERE MATCH(cn) AGAINST('全文索引 数据库')",
                ):
                    cur.execute(query)
                    assert cur.fetchone()[0] == rows_per_worker * 2, query
                cur.execute("UPDATE docs SET en='replacement repeated' WHERE id=0")
                cur.execute("SELECT id FROM docs WHERE MATCH(en) AGAINST('replacement')")
                assert cur.fetchall() == ((0,),)
                cur.execute("SELECT COUNT(*) FROM docs WHERE MATCH(en) AGAINST('threadword0')")
                assert cur.fetchone()[0] == rows_per_worker - 1
                cur.execute("DELETE FROM docs WHERE id >= 2000")
                cur.execute("SELECT COUNT(*) FROM docs WHERE MATCH(cn) AGAINST('全文索引')")
                assert cur.fetchone()[0] == rows_per_worker
        finally:
            with setup.cursor() as cur:
                cur.execute(f"DROP DATABASE {database}")
    print(f"PASS: {workers * iterations * 2} concurrent TOKENIZE comparisons; "
          "indexed writes, commit/rollback, update and delete")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    args = parser.parse_args()
    run(dict(host=args.host, port=args.port, user=args.user,
             password=os.environ.get("SEEKDB_TEST_PASSWORD", "")))
