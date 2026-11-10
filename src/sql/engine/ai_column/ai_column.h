/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/ob_thread_pool.h"
#include <atomic>
#include "lib/string/ob_string.h"

#include "sql/parser/parse_node.h"
namespace oceanbase {
namespace share { namespace schema {
class ObColumnSchemaV2;
class ObTableSchema;
class AlterTableSchema;
}}
namespace sql {
class ObSQLSessionInfo;
struct ObResolverParams;
class ObDMLStmt;
class ObExecContext;
namespace ai_column {
bool is_ai(const share::schema::ObColumnSchemaV2 &column);
bool is_private(const common::ObString &name);
int print_attribute(const share::schema::ObColumnSchemaV2 &column, char *buffer,
                    int64_t length, int64_t &position);
int resolve_attribute(share::schema::ObColumnSchemaV2 &column, const ParseNode &attribute);
int prepare_schema(share::schema::ObTableSchema &schema, ObResolverParams &params,
                   const share::schema::ObTableSchema *original = nullptr);
int install(ObSQLSessionInfo &session, const common::ObString &database,
            const common::ObString &table);
int check_statement(ObDMLStmt &stmt, ObResolverParams &params);
int rewrite_special(const ParseNode &node, ObResolverParams &params, common::ObString &sql);
bool internal_access();
class InternalGuard {
public:
  InternalGuard();
  ~InternalGuard();
private:
  bool saved_;
};
class Worker final : public share::ObThreadPool {
public:
  Worker();
  static Worker &instance();
  void run1() override;
private:
  std::atomic<bool> recovered_{false};
};
} // namespace ai_column
} // namespace sql
} // namespace oceanbase
