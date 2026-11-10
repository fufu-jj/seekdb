/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include <set>

#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_utils.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/ob_sql_context.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase {
namespace sql {
using namespace common;
namespace {
std::string node_string(const ParseNode *n) {
  return n && n->str_value_ ? std::string(n->str_value_, n->str_len_) : std::string();
}
std::string lowercase(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
bool file_literal(const ParseNode *n, std::string &s, const ParamStore *params = nullptr) {
  if (!n) return false;
  if (n->type_ == T_QUESTIONMARK && params && n->value_ >= 0 && n->value_ < params->count()) {
    const auto &obj = params->at(n->value_);
    if (obj.is_string_type()) {
      auto v = obj.get_string();
      s.assign(v.ptr(), v.length());
      return true;
    }
    if (obj.is_integer_type()) {
      s = std::to_string(obj.get_int());
      return true;
    }
    return false;
  }
  if (n->type_ == T_VARCHAR || n->type_ == T_CHAR) {
    s = node_string(n);
    return true;
  }
  if (n->type_ == T_INT) {
    s = std::to_string(n->value_);
    return true;
  }
  if (n->type_ == T_BOOL) {
    s = n->value_ ? "true" : "false";
    return true;
  }
  if (n->type_ == T_OP_NEG && n->num_child_ == 1 && n->children_[0]->type_ == T_INT) {
    s = "-" + std::to_string(n->children_[0]->value_);
    return true;
  }
  return false;
}
}  // namespace
int ObDMLResolver::resolve_file_table_item(const ParseNode &tree, TableItem *&item) {
  int ret = OB_SUCCESS;
  ObObj enabled;
  ObString secure;
  file_sql::Descriptor desc;
  std::string error;
  const ParseNode *call = tree.children_[0];
  const ParseNode *args = call->children_[1];
  std::string name = lowercase(node_string(call->children_[0]));

  if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (enabled.get_int() != 1) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled; SET SESSION enable_file_sql=ON");
  } else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
                 *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, *allocator_, secure))) {
  } else if (name != "read_csv" && name != "read_json" && name != "read_ndjson" &&
             name != "read_parquet" && name != "glob") {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "unknown file table function");
  } else if (!args || args->num_child_ < 1 ||
             !file_literal(args->children_[0], desc.options.path, params_.param_list_)) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    desc.options.format = name == "read_csv"       ? "csv"
                          : name == "read_parquet" ? "parquet"
                          : name == "glob"         ? "glob"
                                                   : "json";
    file_literal(args->children_[0], desc.options.path, params_.param_list_);
    std::set<std::string> seen;
    for (int i = 1; OB_SUCC(ret) && i < args->num_child_; i++) {
      const ParseNode *arg = args->children_[i];
      std::string key, value;
      if (arg->type_ != T_OP_EQ || arg->num_child_ != 2 ||
          arg->children_[0]->type_ != T_COLUMN_REF || arg->children_[0]->num_child_ != 3 ||
          arg->children_[0]->children_[0] || arg->children_[0]->children_[1] ||
          !file_literal(arg->children_[1], value, params_.param_list_)) {
        ret = OB_ERR_PARSE_SQL;
        break;
      }
      key = lowercase(node_string(arg->children_[0]->children_[2]));
      if (!seen.insert(key).second) {
        ret = OB_ERR_PARSE_SQL;
        break;
      }
      if (key == "sample_size") {
        if (value != "-1") {
          ret = OB_NOT_SUPPORTED;
          error = "file SQL only supports sample_size=-1";
        }
      } else if (key == "header" && desc.options.format == "csv") {
        if (value == "true" || value == "1")
          desc.options.header = true;
        else if (value == "false" || value == "0")
          desc.options.header = false;
        else
          ret = OB_ERR_PARSE_SQL;
      } else if ((key == "delim" || key == "delimiter" || key == "sep") &&
                 desc.options.format == "csv") {
        if (value.size() != 1)
          ret = OB_ERR_PARSE_SQL;
        else
          desc.options.delimiter = value[0];
      } else if (key == "quote" && desc.options.format == "csv") {
        if (value.size() != 1)
          ret = OB_ERR_PARSE_SQL;
        else
          desc.options.quote = value[0];
      } else if (key == "escape" && desc.options.format == "csv") {
        if (value.size() != 1)
          ret = OB_ERR_PARSE_SQL;
        else
          desc.options.escape = value[0];
      } else if ((key == "nullstr" || key == "null") && desc.options.format == "csv")
        desc.options.nullstr = value;
      else if (key == "format" && desc.options.format == "json") {
        if (lowercase(value) != "newline_delimited" && lowercase(value) != "ndjson")
          ret = OB_ERR_PARSE_SQL;
      } else
        ret = OB_ERR_PARSE_SQL;
    }
    if (seen.count("quote") && !seen.count("escape")) desc.options.escape = desc.options.quote;
    auto &o = desc.options;
    if (OB_SUCC(ret) &&
        (o.delimiter == '\0' || o.delimiter == '\n' || o.delimiter == '\r' || o.quote == '\0' ||
         o.quote == '\n' || o.quote == '\r' || o.escape == '\0' || o.escape == '\n' ||
         o.escape == '\r' || o.delimiter == o.quote))
      ret = OB_ERR_PARSE_SQL;
    if (OB_SUCC(ret)) {
      params_.query_ctx_->has_file_sql_ = true;
      params_.query_ctx_->disable_udf_parallel_ = true;
      ret = file_sql::infer_schema(desc, std::string(secure.ptr(), secure.length()), error);
    }
  }
  if (OB_FAIL(ret)) {
    if (!error.empty()) {
      LOG_WARN("file SQL schema error", K(ret), "detail", error.c_str());
      LOG_USER_ERROR(OB_NOT_SUPPORTED, error.c_str());
    }
  } else {
    ObConstRawExpr *expr = nullptr;
    std::string encoded = desc.encode();
    ObString value;
    if (OB_FAIL(ob_write_string(*allocator_, ObString(encoded.size(), encoded.data()), value))) {
    } else if (OB_FAIL(ObRawExprUtils::build_const_string_expr(
                   *params_.expr_factory_, ObVarcharType, value, CS_TYPE_BINARY, expr))) {
    } else if (nullptr == (item = get_stmt()->create_table_item(*allocator_)))
      ret = OB_ALLOCATE_MEMORY_FAILED;
    else {
      ObString alias;
      const ParseNode *a = tree.children_[1];
      if (a)
        alias.assign_ptr(a->str_value_, a->str_len_);
      else
        alias.assign_ptr(call->children_[0]->str_value_, call->children_[0]->str_len_);
      item->table_name_ = alias;
      item->alias_name_ = alias;
      item->table_id_ = generate_table_id();
      item->type_ = TableItem::FUNCTION_TABLE;
      item->function_table_expr_ = expr;
      if (OB_FAIL(get_stmt()->add_table_item(session_info_, item))) {
      } else {
        ObSEArray<ColumnItem, 16> cols;
        ret = resolve_file_table_columns(*item, cols);
      }
    }
  }
  return ret;
}
int ObDMLResolver::resolve_file_table_columns(const TableItem &table, ObIArray<ColumnItem> &cols) {
  int ret = OB_SUCCESS;
  file_sql::Descriptor d;
  auto *e = static_cast<ObConstRawExpr *>(table.function_table_expr_);
  ObString s = e->get_value().get_string();
  if (!d.decode(std::string(s.ptr(), s.length()))) return OB_NOT_SUPPORTED;
  for (size_t i = 0; OB_SUCC(ret) && i < d.columns.size(); i++) {
    auto &c = d.columns[i];
    ObString name;
    ColumnItem *col = nullptr;
    if (OB_FAIL(ob_write_string(*allocator_, ObString(c.name.size(), c.name.data()), name))) {
    } else if (nullptr != (col = get_stmt()->get_column_item(table.table_id_, name))) {
    } else {
      ObObjMeta meta;
      ObAccuracy accuracy;
      switch (c.type) {
        case file_sql::Type::BIGINT:
        case file_sql::Type::BOOLEAN:
          meta.set_int();
          break;
        case file_sql::Type::DOUBLE:
          meta.set_double();
          break;
        case file_sql::Type::DATE:
          meta.set_date();
          break;
        case file_sql::Type::DATETIME:
          meta.set_datetime();
          accuracy.set_scale(c.scale);
          break;
        case file_sql::Type::NIL:
          meta.set_null();
          break;
        default:
          meta.set_varchar();
          meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
          meta.set_collation_level(CS_LEVEL_IMPLICIT);
          accuracy.set_length(file_sql::MAX_RECORD);
          break;
      }
      ret = resolve_function_table_column_item(table, meta, accuracy, name,
                                               OB_APP_MIN_COLUMN_ID + i, col);
    }
    if (OB_SUCC(ret)) ret = cols.push_back(*col);
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase

namespace oceanbase {
namespace sql {
namespace {
std::string sql_text(const std::string &s) {
  static const char hex[] = "0123456789abcdef";
  std::string out = "CONVERT(X'";
  for (unsigned char c : s) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out + "' USING utf8mb4)";
}
}  // namespace
int ObResolver::resolve_file_special(const ParseNode &tree, ObStmt *&stmt) {
  int ret = OB_SUCCESS;
  const ParseNode *query = tree.children_[4];
  ObSelectStmt *select = nullptr;
  if (!query) return OB_ERR_UNEXPECTED;
  // DESCRIBE SELECT historically means EXPLAIN for ordinary SQL. Only file
  // table syntax opts into dynamic schema output.
  bool has_file = false;
  std::vector<const ParseNode *> pending{query};
  while (!pending.empty() && !has_file) {
    const auto *node = pending.back();
    pending.pop_back();
    has_file = node->type_ == T_TABLE_COLLECTION_EXPRESSION && node->value_ == 600492;
    for (int64_t i = 0; !has_file && i < node->num_child_; ++i) {
      if (node->children_[i] != nullptr) pending.push_back(node->children_[i]);
    }
  }
  if (!has_file) {
    ParseNode ordinary = tree;
    ParseNode *children[5];
    for (int i = 0; i < 5; ++i) children[i] = tree.children_[i];
    children[0] = nullptr;
    ordinary.children_ = children;
    ObResolver nested(params_);
    return nested.resolve(IS_NOT_PREPARED_STMT, ordinary, stmt);
  }
  ObSelectResolver resolver(params_);
  if (OB_FAIL(resolver.resolve(*query))) return ret;
  select = resolver.get_select_stmt();
  if (!select) return OB_ERR_UNEXPECTED;
  {
    // DESCRIBE emits metadata rows only; none of the source records are retained.
    std::string sql;
    for (int64_t i = 0; i < select->get_select_item_size(); i++) {
      const auto &item = select->get_select_item(i);
      auto *e = item.expr_;
      const auto &name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      std::string type = "VARCHAR";
      bool nullable = true;
      if (e->is_column_ref_expr()) {
        auto *col = static_cast<ObColumnRefRawExpr *>(e);
        auto *table = select->get_table_item_by_id(col->get_table_id());
        if (table && table->is_function_table() &&
            table->function_table_expr_->is_const_raw_expr()) {
          auto s =
              static_cast<ObConstRawExpr *>(table->function_table_expr_)->get_value().get_string();
          file_sql::Descriptor d;
          int64_t idx = col->get_column_id() - OB_APP_MIN_COLUMN_ID;
          if (d.decode(std::string(s.ptr(), s.length())) && idx >= 0 &&
              idx < int64_t(d.columns.size())) {
            type = file_sql::type_name(d.columns[idx].type);
            nullable = d.columns[idx].nullable;
          }
        }
      } else
        type = common::ob_obj_type_str(e->get_data_type());
      if (i) sql += " UNION ALL ";
      sql += "SELECT " + sql_text(std::string(name.ptr(), name.length())) + " AS column_name, " +
             sql_text(type) + " AS column_type, " + sql_text(nullable ? "YES" : "NO") +
             " AS `null`, NULL AS `key`, NULL AS `default`, NULL AS extra";
    }
    if (sql.empty()) return OB_ERR_UNEXPECTED;
    ParseResult result;
    ObParser parser(*params_.allocator_, params_.session_info_->get_sql_mode());
    if (OB_FAIL(parser.parse(ObString(sql.size(), sql.data()), result))) return ret;
    if (!result.result_tree_ || result.result_tree_->num_child_ != 1) return OB_ERR_UNEXPECTED;
    ObResolver nested(params_);
    ret = nested.resolve(IS_NOT_PREPARED_STMT, *result.result_tree_->children_[0], stmt);
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase

namespace oceanbase {
namespace sql {
int ObSelectResolver::resolve_file_copy_into(const ParseNode &tree) {
  int ret = OB_SUCCESS;
  auto *select = get_select_stmt();
  if (is_sub_stmt_ || params_.is_from_create_view_) {
    return OB_INAPPROPRIATE_INTO;
  }
  ObObj enabled;
  if (OB_FAIL(params_.session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled)))
    return ret;
  if (enabled.get_int() != 1) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL COPY is disabled");
    return OB_NOT_SUPPORTED;
  }
  file_sql::Descriptor d;
  d.options.format = "copy";
  d.options.header = false;
  file_literal(tree.children_[0], d.options.path, params_.param_list_);
  std::set<std::string> seen;
  const ParseNode *options = tree.children_[1];
  for (int i = 0; options && i < options->num_child_; i++) {
    auto *o = options->children_[i];
    std::string key = lowercase(node_string(o->children_[0])), value;
    if (!seen.insert(key).second) return OB_ERR_PARSE_SQL;
    if (!o->children_[1])
      value = "true";
    else if (!file_literal(o->children_[1], value, params_.param_list_))
      value = node_string(o->children_[1]);
    if (key == "format") {
      if (lowercase(value) != "csv") return OB_NOT_SUPPORTED;
    } else if (key == "header") {
      if (value != "true" && value != "false" && value != "0" && value != "1")
        return OB_ERR_PARSE_SQL;
      d.options.header = value == "true" || value == "1";
    } else if (key == "delimiter" || key == "delim" || key == "sep") {
      if (value.size() != 1) return OB_ERR_PARSE_SQL;
      d.options.delimiter = value[0];
    } else if (key == "quote") {
      if (value.size() != 1) return OB_ERR_PARSE_SQL;
      d.options.quote = value[0];
    } else if (key == "escape") {
      if (value.size() != 1) return OB_ERR_PARSE_SQL;
      d.options.escape = value[0];
    } else if (key == "nullstr" || key == "null")
      d.options.nullstr = value;
    else
      return OB_ERR_PARSE_SQL;
  }
  if (seen.count("quote") && !seen.count("escape")) d.options.escape = d.options.quote;
  for (int64_t i = 0; i < select->get_select_item_size(); i++) {
    auto &item = select->get_select_item(i);
    auto &name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
    d.columns.push_back({std::string(name.ptr(), name.length()), file_sql::Type::VARCHAR, true});
  }
  void *into_mem = params_.allocator_->alloc(sizeof(ObSelectIntoItem));
  if (!into_mem) return OB_ALLOCATE_MEMORY_FAILED;
  auto *into = new (into_mem) ObSelectIntoItem();
  into->into_type_ = T_INTO_OUTFILE;
  std::string encoded = d.encode();
  if (OB_FAIL(ob_write_string(*params_.allocator_, ObString(encoded.size(), encoded.data()),
                              into->external_properties_)))
    return ret;
  ObString stored_path;
  if (OB_FAIL(ob_write_string(*params_.allocator_,
                              ObString(d.options.path.size(), d.options.path.data()), stored_path)))
    return ret;
  into->outfile_name_.set_varchar(stored_path);
  select->set_select_into(into);
  params_.query_ctx_->has_file_sql_ = true;
  params_.query_ctx_->disable_udf_parallel_ = true;
  return ret;
}
}  // namespace sql
}  // namespace oceanbase
