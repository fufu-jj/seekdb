# 本地文件 SQL

File SQL 在 seekdb 进程所在主机上直接查询 CSV、JSONL 和 Parquet。
过滤、聚合、排序和 JOIN 由 seekdb 原生计划执行；不调用外部查询引擎。

## 启用和使用

功能默认关闭。先由管理员通过本地连接把 `secure_file_priv` 设置为允许的数据目录，
再在查询会话开启功能。输入路径必须落在该目录内；空值或 NULL 禁止访问。

```sql
SET GLOBAL secure_file_priv = '/absolute/data-directory';
SET SESSION enable_file_sql = ON;

DESCRIBE SELECT * FROM read_csv('/absolute/data-directory/input.csv');
SELECT kind, COUNT(*), SUM(amount)
FROM read_csv('/absolute/data-directory/input.csv', header=true, sample_size=-1) AS f
GROUP BY kind HAVING COUNT(*) > 1 ORDER BY kind;

SELECT a.id, b.label
FROM read_csv('/absolute/data-directory/input.csv') AS a
LEFT JOIN read_ndjson('/absolute/data-directory/labels.jsonl') AS b ON a.id=b.id;

SELECT id FROM read_parquet('/absolute/data-directory/values.parquet') WHERE id > 0;
SELECT file FROM glob('/absolute/data-directory/*.csv');

COPY (SELECT id FROM read_csv('/absolute/data-directory/input.csv'))
TO '/absolute/data-directory/export.csv' (FORMAT csv, HEADER, DELIMITER ',', NULLSTR 'NULL');
```

`COPY table_name TO path` 也支持普通表。目标必须不存在；临时文件在同一目录中独占创建，
查询成功后通过不覆盖目标的原子链接发布，失败或关闭时清理。导出内容中的非 NULL 值加引号。

## 格式约定

- CSV 默认有表头，分隔符为逗号，引号和转义均为双引号。支持 UTF-8/BOM、LF/CRLF、
  双引号转义和字段内换行。未加引号的空字段或 `NULL` 为 SQL NULL；加引号的空字符串
  和 `"NULL"` 保留为字符串值。可设置 `header`、`delim`/`delimiter`/`sep`、`quote`、
  `escape` 和 `nullstr`。没有表头时列名从 `column0` 起；重复表头附加 `__2` 等后缀。
- `read_json` 和 `read_ndjson` 均读取每行一个扁平对象，忽略空白行。字段按首次出现顺序取并集，
  缺失值补 NULL，拒绝嵌套对象、数组和大小写冲突的重复键。`read_json` 可显式设置
  `format='newline_delimited'` 或 `format='ndjson'`。
- CSV/JSONL 全量推断元数据且不保存源记录；`sample_size` 仅支持 `-1`。类型包括 BIGINT、
  DOUBLE、BOOLEAN、VARCHAR、DATE、DATETIME 和 NULL；整数溢出保留为字符串。
  BIGINT/DOUBLE 宽化为 DOUBLE，DATE/DATETIME 宽化为 DATETIME，其余冲突为 VARCHAR。
- Parquet 读取 Footer Schema 和多个 Row Group，按实际引用列解码。支持可空的平坦基础列，
  INT32/INT64、FLOAT/DOUBLE、BOOLEAN、UTF-8 字节字符串、DATE、TIMESTAMP 和 INT96。
  支持 PLAIN、字典/RLE 编码以及不压缩、Snappy、Gzip 的 v1/v2 数据页。
  无符号整数、嵌套结构、其他逻辑类型、编码和压缩格式明确报错。
- `glob` 只发现文件，返回排序后的一列 `file`，不合并文件数据。普通 `DESCRIBE SELECT`
  保留原有 EXPLAIN 行为；含文件扫描的查询返回动态列元数据。

## 执行和资源限制

使用原生 FUNCTION_TABLE 扫描算子，支持单行、批量取数和重扫。文件的设备、inode、大小、
修改时间和状态变更时间在推断及扫描期间检查，路径经过规范化、目录边界校验和逐段打开。
含动态文件元数据的计划不放入 SQL 计划缓存。单进程本地文件场景不启用分布式文件扫描。

记录/元数据最大 16 MiB，列数最大 1024，glob 结果最大 10000，Parquet Row Group
最大 10000，数据页最大 32 MiB，每页最大 262144 个值，已投影页和字典有 128 MiB 限制。
异常结构、越界路径、文件变化或超过限制会报错。全量推断与执行会分别扫描输入，
因此不适合频繁查询巨大、持续变化的文件。

## 验证

扫描器单元检查及可复现命令见 `unittest/sql/engine/basic/README.md`。
数据库回归位于 `tools/deploy/mysql_test/test_suite/file_sql/include/file_sql_regression.py`。
Python 和 PyArrow 仅供测试数据生成，不是生产执行依赖。
