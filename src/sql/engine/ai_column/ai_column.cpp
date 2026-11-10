/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column/ai_column.h"
#include "sql/parser/parse_node.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/dml/ob_insert_stmt.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_trigger_info.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_stmt_expr_visitor.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "query/vector/ob_vector_index_util.h"
#include "query/session/ob_session_inner_sql.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/time/ob_time_utility.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace ai_column {
using namespace common;
using namespace share::schema;
namespace {
thread_local bool internal = false;
const char *MARKER = "seekdb.ai-column.v1:";
const char *META = "oceanbase.__all_ai_column";
const char *TASK = "oceanbase.__all_ai_column_task";
std::string str(const ObString &s) { return std::string(s.ptr() ? s.ptr() : "", s.length()); }
std::string text(const ParseNode *n) { return n && n->str_value_ ? std::string(n->str_value_, n->str_len_) : ""; }
std::string lower(std::string s) { for (auto &c : s) c = std::tolower(static_cast<unsigned char>(c)); return s; }
std::string trim(std::string s) {
  auto b = s.find_first_not_of(" \r\n\t");
  return b == s.npos ? "" : s.substr(b, s.find_last_not_of(" \r\n\t") - b + 1);
}
std::string ident(const std::string &s) {
  std::string r = "`";
  for (char c : s) { r += c; if (c == '`') r += '`'; }
  return r + "`";
}
// Hex string literals are independent of NO_BACKSLASH_ESCAPES and connection encoding.
std::string literal(const std::string &s) {
  static const char hex[] = "0123456789abcdef";
  std::string r = "convert(X'";
  for (unsigned char c : s) { r += hex[c >> 4]; r += hex[c & 15]; }
  return r + "' using utf8mb4)";
}
std::string hidden(uint64_t id, char kind) { return "__ai_" + std::to_string(id) + "_" + kind; }
int unsupported(const char *what) { LOG_USER_ERROR(OB_NOT_SUPPORTED, what); return OB_NOT_SUPPORTED; }
struct Definition {
  uint64_t id = 0;
  int retries = 2;
  std::string name, expression, type, target_spec;
  std::vector<std::string> dependencies;
};
uint64_t version(const Definition &d) {
  // Stable across process restarts and unrelated table DDL.
  const std::string spec=d.expression+":"+d.type+":"+std::to_string(d.retries)+":"+d.target_spec;
  uint64_t hash=14695981039346656037ULL;
  for(unsigned char c:spec){hash^=c;hash*=1099511628211ULL;}
  return (hash & INT64_MAX) | 1;
}
bool decode(const ObColumnSchemaV2 &c, Definition &d) {
  if(c.is_unused()||c.is_hidden())return false;
  auto s = str(c.get_comment_str());
  if (s.compare(0, strlen(MARKER), MARKER)) return false;
  size_t p = strlen(MARKER), end = s.find(':', p);
  if (end == s.npos) return false;
  d.retries = atoi(s.substr(p, end-p).c_str());
  p = end+1; end = s.find(':', p);
  if (end == s.npos) return false;
  d.type = s.substr(p, end-p);
  p = end+1; end = s.find('\n', p);
  if (end == s.npos) return false;
  std::string deps = s.substr(p,end-p);
  size_t off=0;
  while(off < deps.size()) {
    size_t e=deps.find(',',off);
    if(e==deps.npos)e=deps.size();
    d.dependencies.push_back(deps.substr(off,e-off)); off=e+1;
  }
  d.expression=s.substr(end+1); d.id=c.get_column_id(); d.name=str(c.get_column_name_str());
  d.target_spec=std::to_string(c.get_data_type())+":"+std::to_string(c.get_data_length())+":"+
    std::to_string(c.get_data_precision())+":"+std::to_string(c.get_data_scale())+":"+
    std::to_string(c.get_length_semantics())+":"+std::to_string(c.get_collation_type());
  return true;
}
int encode(ObColumnSchemaV2 &c, const Definition &d) {
  std::string deps;
  for(auto &s:d.dependencies) { if(!deps.empty())deps+=','; deps+=s; }
  std::string s=std::string(MARKER)+std::to_string(d.retries)+":"+d.type+":"+deps+"\n"+d.expression;
  return c.set_comment(ObString(s.size(),s.data()));
}
void collect_deps(const ParseNode *n, std::set<std::string> &deps) {
  if (!n) return;
  if (n->type_ == T_COLUMN_REF) {
    if(n->num_child_ >= 3 && n->children_[2]) deps.insert(text(n->children_[2]));
  } else {
    for(int i=0;i<n->num_child_;i++) collect_deps(n->children_[i],deps);
  }
}
bool valid_expression(const ParseNode *node,bool root=true) {
  if(!node)return true;
  if(node->type_==T_SELECT||node->type_==T_REF_QUERY||node->type_==T_SYSTEM_VARIABLE||
     node->type_==T_USER_VARIABLE_IDENTIFIER||node->type_==T_QUESTIONMARK)return false;
  if(!root&&(node->type_==T_FUN_SYS_AI_COMPLETE||node->type_==T_FUN_SYS_AI_EMBED||
             node->type_==T_FUN_SYS_AI_RERANK))return false;
  if(node->type_==T_FUN_SYS&&node->num_child_>0) {
    auto fn=lower(text(node->children_[0]));
    if((!root&&(fn=="ai_complete"||fn=="ai_embed"||fn=="ai_rerank"))||
       fn=="sleep"||fn=="benchmark"||fn=="load_file"||fn=="get_lock"||fn=="release_lock")return false;
  }
  for(int i=0;i<node->num_child_;++i)if(!valid_expression(node->children_[i],false))return false;
  return true;
}
// Split SQL arguments without changing literal, quoted identifier, or nested expression text.
bool arguments(const std::string &s, std::string &fn, std::vector<std::string> &args) {
  size_t p=s.find('('); if(p==s.npos)return false;
  fn=lower(trim(s.substr(0,p)));
  int depth=0; char quote=0; size_t start=p+1;
  for(size_t i=p+1;i<s.size();i++) {
    char c=s[i];
    if(quote) {
      if(c=='\\' && quote!='`')i++;
      else if(c==quote) { if(i+1<s.size()&&s[i+1]==quote)i++; else quote=0; }
    } else if(c=='\''||c=='"'||c=='`')quote=c;
    else if(c=='(')depth++;
    else if(c==')') {
      if(depth--==0) { args.push_back(trim(s.substr(start,i-start))); return trim(s.substr(i+1)).empty(); }
    } else if(c==','&&depth==0) { args.push_back(trim(s.substr(start,i-start))); start=i+1; }
  }
  return false;
}
int parse_expr(const std::string &s, ObResolverParams &params, ParseNode *&expr) {
  ObParser parser(*params.allocator_,params.session_info_->get_sql_mode());
  ParseResult result;
  std::string q="SELECT "+s;
  int ret=parser.parse(ObString(q.size(),q.data()),result);
  if(OB_SUCC(ret)) {
    auto *root=result.result_tree_;
    if(!root||root->num_child_!=1||!root->children_[0]) ret=OB_ERR_UNEXPECTED;
    else {
      // SELECT's projection list contains one project-string node.
      auto *list=root->children_[0]->children_[PARSE_SELECT_SELECT];
      if(!list||list->num_child_!=1||!list->children_[0])ret=OB_ERR_UNEXPECTED;
      else expr=list->children_[0]->children_[0];
    }
  }
  return ret;
}
int write(query::ObSessionInnerSql &sql,const std::string &q) {
  ObSqlString s; int64_t affected=0; int ret=s.assign(q.c_str());
  if(OB_SUCC(ret))ret=sql.execute_write(s,affected);
  if(OB_FAIL(ret))LOG_WARN("AI column internal DDL/DML failed",K(ret),"sql",q.c_str());
  return ret;
}
int write(ObISQLClient &sql,const std::string &q,int64_t *count=nullptr) {
  int64_t affected=0; int ret=sql.write(q.c_str(),affected);
  if(count)*count=affected;
  return ret;
}
int read(ObISQLClient &sql,const std::string &q,ObISQLClient::ReadResult &res) {
  return sql.read(res,q.c_str());
}
std::string field(sqlclient::ObMySQLResult &r,const char *name) {
  ObString s;
  if(OB_SUCCESS!=r.get_varchar(name,s))return "";
  return str(s);
}
int64_t number(sqlclient::ObMySQLResult &r,const char *name) {
  int64_t n=0;r.get_int(name,n);return n;
}
std::string key_expression(const ObTableSchema &table,const char *prefix) {
  std::string q="concat(";
  for(int i=0;i<table.get_rowkey_column_num();i++) {
    auto *col=table.get_column_schema(table.get_rowkey_info().get_column(i)->column_id_);
    if(i)q+=","+literal(" AND ")+",";
    q+=literal(ident(str(col->get_column_name_str()))+" = ")+",";
    std::string ref=std::string(prefix)+"."+ident(str(col->get_column_name_str()));
    if(ob_is_integer_type(col->get_data_type()))q+="cast("+ref+" as char)";
    else q+="concat('X',char(39),hex("+ref+"),char(39))";
  }
  return q+")";
}
std::string changed(const Definition &d,const ObTableSchema &table) {
  std::set<std::string> names(d.dependencies.begin(),d.dependencies.end());
  for(int i=0;i<table.get_rowkey_column_num();i++)
    names.insert(str(table.get_column_schema(table.get_rowkey_info().get_column(i)->column_id_)->get_column_name_str()));
  std::string q="FALSE";
  for(auto &n:names)q+=" OR NOT (NEW."+ident(n)+" <=> OLD."+ident(n)+")";
  return "("+q+")";
}
std::string enqueue(uint64_t tid,const Definition &d,const std::string &key) {
  std::string rid="NEW."+ident(hidden(d.id,'r'));
  return "REPLACE INTO "+std::string(TASK)+"(table_id,rowkey_hash,column_id,request_id,definition_version,rowkey_predicate,state,retry_count,eligible_ts) VALUES("+
    std::to_string(tid)+",sha2("+key+",256),"+std::to_string(d.id)+","+rid+",NEW."+ident(hidden(d.id,'v'))+","+key+",0,0,now(6)); ";
}
} // namespace
bool internal_access(){return internal;}
InternalGuard::InternalGuard():saved_(internal){internal=true;}
InternalGuard::~InternalGuard(){internal=saved_;}
bool is_private(const ObString &name){return name.prefix_match("__ai_");}
bool is_ai(const ObColumnSchemaV2 &c){return !c.is_unused()&&!c.is_hidden()&&c.get_comment_str().prefix_match(MARKER);}
int print_attribute(const ObColumnSchemaV2 &c,char *buf,int64_t len,int64_t &pos) {
  Definition d;
  if(!decode(c,d))return OB_INVALID_ARGUMENT;
  return databuff_printf(buf,len,pos," AI COLUMN (%s) WITH (sync_mode='async', max_retries=%d)",
                         d.expression.c_str(),d.retries);
}

int resolve_attribute(ObColumnSchemaV2 &column,const ParseNode &node) {
  if(node.value_!=600495||node.num_child_!=2||!node.children_[0])return OB_ERR_UNEXPECTED;
  if(!valid_expression(node.children_[0]))return unsupported("AI COLUMN subqueries, session parameters or nested model calls");
  if(!column.get_comment_str().empty())return unsupported("AI COLUMN with a separate column comment");
  Definition d;
  d.expression=text(node.children_[0]);
  if(d.expression.empty()||d.expression.size()>12000)return OB_INVALID_ARGUMENT;
  std::set<std::string> deps;collect_deps(node.children_[0],deps);
  for(auto &s:deps) {
    if(s.find(',')!=s.npos||s.find('\n')!=s.npos)return unsupported("AI COLUMN dependency identifier");
    d.dependencies.push_back(s);
  }
  auto *options=node.children_[1]; std::set<std::string> seen;
  for(int i=0;options&&i<options->num_child_;i++) {
    auto *o=options->children_[i];
    if(!o||o->num_child_!=2||!o->children_[0]||!o->children_[1])return OB_INVALID_ARGUMENT;
    auto key=lower(text(o->children_[0]));auto *v=o->children_[1];
    if(!seen.insert(key).second)return OB_INVALID_ARGUMENT;
    if(key=="sync_mode") { if(v->type_!=T_VARCHAR||lower(text(v))!="async")return unsupported("AI COLUMN synchronous execution"); }
    else if(key=="max_retries") { if(v->type_!=T_INT||v->value_<0||v->value_>16)return OB_INVALID_ARGUMENT; d.retries=v->value_; }
    else return unsupported("AI COLUMN option");
  }
  column.set_nullable(true);
  return encode(column,d);
}

int prepare_schema(ObTableSchema &schema,ObResolverParams &params,const ObTableSchema *original) {
  int ret=OB_SUCCESS;
  std::vector<Definition> definitions;
  int count=schema.get_column_count();
  uint64_t next = original ? original->get_max_used_column_id() : schema.get_max_used_column_id();
  if (original) {
    for (int i=0; i<count; ++i) {
      auto *c=static_cast<AlterColumnSchema *>(schema.get_column_schema_by_idx(i));
      if(c->alter_type_==OB_DDL_ADD_COLUMN)c->set_column_id(++next);
    }
  }
  auto find_column = [&](const std::string &name) -> const ObColumnSchemaV2 * {
    for(int i=0;i<schema.get_column_count();++i) {
      auto *c=schema.get_column_schema_by_idx(i);
      if(c && c->get_column_name_str().case_compare(name.c_str())==0)return c;
    }
    return original ? original->get_column_schema(ObString(name.size(),name.data())) : nullptr;
  };
  if(original) {
    // Reject a dependency disappearing while its AI definition remains. Native
    // generated-column checks cannot see expressions stored in AI metadata.
    for(int i=0;i<original->get_column_count();++i) {
      Definition d;auto *old=original->get_column_schema_by_idx(i);
      if(!old||!decode(*old,d))continue;
      bool replaced=false;
      for(int j=0;j<count;++j) {
        auto *change=static_cast<AlterColumnSchema *>(schema.get_column_schema_by_idx(j));
        if(change&&change->get_origin_column_name().case_compare(d.name.c_str())==0)replaced=true;
      }
      if(replaced)continue;
      for(auto &dep:d.dependencies)for(int j=0;j<count;++j) {
        auto *change=static_cast<AlterColumnSchema *>(schema.get_column_schema_by_idx(j));
        if(change&&change->get_origin_column_name().case_compare(dep.c_str())==0&&
           (change->alter_type_==OB_DDL_DROP_COLUMN||
            change->get_column_name_str().case_compare(dep.c_str())!=0))
          return unsupported("dropping or renaming an AI COLUMN dependency");
      }
    }
  }
  for(int i=0;OB_SUCC(ret)&&i<count;i++) {
    auto *c=schema.get_column_schema_by_idx(i);Definition d;
    if(!c||!decode(*c,d))continue;
    std::string fn;std::vector<std::string> args;
    if(!arguments(d.expression,fn,args)||(fn!="ai_complete"&&fn!="ai_embed")||args.size()<2||args.size()>3)
      return unsupported("AI COLUMN must contain one AI_COMPLETE or AI_EMBED call");
    for(auto &dep:d.dependencies) {
      auto *dc=find_column(dep);
      if(!dc)return OB_ERR_BAD_FIELD_ERROR;
      if(is_ai(*dc)||dc->is_hidden())return OB_NOT_SUPPORTED;
    }
    ParseNode *model_node=nullptr;
    if(OB_FAIL(parse_expr(args[0],params,model_node)))return ret;
    if(!model_node||model_node->type_!=T_VARCHAR)return unsupported("AI COLUMN dynamic model names");
    const ObAiModelSchema *model=nullptr;
    ObString model_name(model_node->str_len_,model_node->str_value_);
    if(OB_FAIL(params.schema_checker_->get_schema_guard()->get_ai_model_schema(model_name,model)))return ret;
    if(!model){LOG_USER_ERROR(OB_AI_FUNC_MODEL_NOT_FOUND,model_name.length(),model_name.ptr());return OB_AI_FUNC_MODEL_NOT_FOUND;}
    if(model->get_type()!=(fn=="ai_embed"?share::EndpointType::DENSE_EMBEDDING:share::EndpointType::COMPLETION))
      return unsupported("AI COLUMN model type does not match its function");
    if(fn=="ai_embed") {
      if(c->get_data_type()!=ObCollectionSQLType)return unsupported("AI_EMBED result requires VECTOR");
      int64_t dim=0;
      if(OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(c->get_extended_type_info(),dim)))return ret;
      if(args.size()==3) {
        ParseNode *n=nullptr;if(OB_FAIL(parse_expr(args[2],params,n)))return ret;
        if(!n||n->type_!=T_INT)return OB_INVALID_ARGUMENT;
        if(n->value_!=dim){LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM,unsigned(dim),unsigned(n->value_));return OB_ERR_INVALID_VECTOR_DIM;}
      }
      d.type="VECTOR";
      d.expression="AI_EMBED("+args[0]+","+args[1]+","+std::to_string(dim)+")";
    } else {
      std::string hint;
      if(c->get_data_type()==ObJsonType){d.type="JSON";hint="Return exactly one valid JSON value. Do not include Markdown or commentary.";}
      else if(ob_is_integer_type(c->get_data_type())){d.type="SIGNED";hint="Return exactly one base-10 integer within the target SQL integer range. Do not include any other text.";}
      else if(ob_is_string_type(c->get_data_type()))d.type="CHAR";
      else return unsupported("AI_COMPLETE result type");
      std::string config=args.size()==3?args[2]:"JSON_OBJECT()";
      if(!hint.empty()) {
        config="JSON_MERGE_PATCH("+config+",JSON_OBJECT('messages',JSON_ARRAY(JSON_OBJECT('role','system','content',"+("'"+hint+"'")+"),JSON_OBJECT('role','user','content',"+args[1]+"))))";
      }
      d.expression="AI_COMPLETE("+args[0]+","+args[1]+","+config+")";
    }
    if(OB_FAIL(encode(*c,d)))return ret;
    definitions.push_back(d);
  }
  if(definitions.empty())return ret;
  if(!original&&schema.get_rowkey_column_num()==0)return unsupported("AI COLUMN table without a primary key");

  for(auto &d:definitions)for(char kind:{'s','r','v'}) {
    std::string name=hidden(d.id,kind);
    if(find_column(name))continue;
    AlterColumnSchema c;
    c.set_column_id(++next);c.set_table_id(schema.get_table_id());
    c.set_data_type(kind=='r'?ObVarcharType:ObIntType);c.set_data_length(kind=='r'?64:8);
    if(kind=='r')c.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    c.set_nullable(true);c.set_is_hidden(true);
    c.alter_type_=OB_DDL_ADD_COLUMN;
    if(OB_FAIL(c.set_column_name(ObString(name.size(),name.data()))))return ret;
    ret=original?static_cast<AlterTableSchema &>(schema).add_alter_column(c,true):schema.add_column(c);
    if(OB_FAIL(ret))return ret;
  }
  return ret;
}

int install(ObSQLSessionInfo &session,const ObString &database,const ObString &name) {
  InternalGuard guard;
  int ret=OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table=nullptr;
  if(!GCTX.schema_service_)return OB_NOT_INIT;
  if(OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard)))return ret;
  if(OB_FAIL(schema_guard.get_table_schema(database,name,false,table)))return ret;
  if(!table)return OB_SUCCESS;
  std::vector<Definition> defs;
  for(int i=0;i<table->get_column_count();i++) {
    Definition d;auto *c=table->get_column_schema_by_idx(i);
    if(c&&decode(*c,d))defs.push_back(d);
  }
  bool has_triggers=false;
  for(int64_t i=0;i<table->get_trigger_list().count();++i) {
    const ObTriggerInfo *info=nullptr;
    if(OB_SUCCESS==schema_guard.get_trigger_info(table->get_trigger_list().at(i),info)&&info&&
       info->get_trigger_name().prefix_match(("__ai_"+std::to_string(table->get_table_id())+"_").c_str()))has_triggers=true;
  }
  if(defs.empty()&&!has_triggers)return ret;
  query::ObSessionInnerSql sql(&session);
  const std::string db=str(database),tn=str(name),full=ident(db)+"."+ident(tn);
  const uint64_t tid=table->get_table_id();
  std::string old_db=str(session.get_database_name());
  if(OB_FAIL(write(sql,"USE "+ident(db))))return ret;
  // Remove metadata for dropped AI columns before publishing the current set.
  std::string ids;
  for(auto &d:defs){if(!ids.empty())ids+=",";ids+=std::to_string(d.id);}
  if(OB_FAIL(write(sql,"DELETE FROM "+std::string(META)+" WHERE table_id="+std::to_string(tid)+
      (ids.empty()?"":" AND column_id NOT IN ("+ids+")"))))return ret;
  if(OB_FAIL(write(sql,"DELETE FROM "+std::string(TASK)+" WHERE table_id="+std::to_string(tid)+
      (ids.empty()?"":" AND column_id NOT IN ("+ids+")"))))return ret;
  for(auto &d:defs) {
    std::string ids;
    for(char k:{'s','r','v'}) {
      auto n=hidden(d.id,k);auto *c=table->get_column_schema(ObString(n.size(),n.data()));
      if(!c)return OB_ERR_UNEXPECTED;
      ids+=","+std::to_string(c->get_column_id());
    }
    std::string deps;
    for(auto &n:d.dependencies){if(!deps.empty())deps+=',';deps+=n;}
    std::string q="REPLACE INTO "+std::string(META)+"(table_id,column_id,column_name,normalized_expr,dependency_columns,max_retries,definition_version,state_column_id,request_column_id,result_spec_column_id,database_name,table_name,result_type,owner_id) VALUES("+
      std::to_string(tid)+","+std::to_string(d.id)+","+literal(d.name)+","+literal(d.expression)+","+literal(deps)+","+std::to_string(d.retries)+","+std::to_string(version(d))+ids+","+literal(db)+","+literal(tn)+","+literal(d.type)+","+std::to_string(session.get_priv_user_id())+")";
    if(OB_FAIL(write(sql,q)))return ret;
  }
  const std::string key=key_expression(*table,"NEW"),old_key=key_expression(*table,"OLD");
  std::string bi="BEGIN DECLARE ai_request VARCHAR(64); SET ai_request=uuid(); ";
  std::string bu=bi;
  std::string ai="BEGIN ",au="BEGIN ",ad="BEGIN ";
  au+="IF NOT ("+key+" <=> "+old_key+") THEN DELETE FROM "+TASK+" WHERE table_id="+std::to_string(tid)+" AND rowkey_hash=sha2("+old_key+",256); END IF; ";
  for(auto &d:defs) {
    auto s=ident(hidden(d.id,'s')),r=ident(hidden(d.id,'r')),v=ident(hidden(d.id,'v'));
    std::string invalidate="SET NEW."+ident(d.name)+"=NULL; SET NEW."+s+"=0; SET NEW."+r+"=ai_request; SET NEW."+v+"="+std::to_string(version(d))+"; ";
    bi+=invalidate;
    bu+="IF "+changed(d,*table)+" OR NOT (NEW."+r+" <=> OLD."+r+") THEN "+invalidate+"END IF; ";
    ai+=enqueue(tid,d,key);
    au+="IF NOT (NEW."+r+" <=> OLD."+r+") THEN "+enqueue(tid,d,key)+"END IF; ";
  }
  ad+="DELETE FROM "+std::string(TASK)+" WHERE table_id="+std::to_string(tid)+" AND rowkey_hash=sha2("+old_key+",256); ";
  struct Trigger { const char *suffix,*event;std::string body; };
  std::vector<Trigger> triggers={{"bi","BEFORE INSERT",bi},{"bu","BEFORE UPDATE",bu},{"ai","AFTER INSERT",ai},{"au","AFTER UPDATE",au},{"ad","AFTER DELETE",ad}};
  for(auto &t:triggers) {
    std::string tr=ident(db)+"."+ident("__ai_"+std::to_string(tid)+"_"+t.suffix);
    bool exists=false;
    const auto &trigger_ids=table->get_trigger_list();
    for(int64_t i=0;i<trigger_ids.count();i++) {
      const ObTriggerInfo *info=nullptr;
      if(OB_SUCCESS==schema_guard.get_trigger_info(trigger_ids.at(i),info) && info &&
          info->get_trigger_name().case_compare(("__ai_"+std::to_string(tid)+"_"+t.suffix).c_str())==0)exists=true;
    }
    if(exists && OB_FAIL(write(sql,"DROP TRIGGER "+tr)))break;
    if(!defs.empty() && OB_FAIL(write(sql,"CREATE TRIGGER "+tr+" "+t.event+" ON "+full+" FOR EACH ROW "+t.body+"END")))break;
  }
  // ADD/MODIFY must also enqueue pre-existing rows. One UPDATE gives all newly
  // invalidated columns the same mutation request, without touching ready ones.
  std::string assignments,needs_backfill;
  for(auto &d:defs) {
    auto r=ident(hidden(d.id,'r')),v=ident(hidden(d.id,'v'));
    if(!assignments.empty()){assignments+=",";needs_backfill+=" OR ";}
    auto stale="NOT ("+v+" <=> "+std::to_string(version(d))+")";
    assignments+=r+"=IF("+stale+",uuid(),"+r+")";needs_backfill+=stale;
  }
  if(OB_SUCC(ret)&&!assignments.empty())ret=write(sql,"UPDATE "+full+" SET "+assignments+" WHERE "+needs_backfill);
  if(!old_db.empty()){int restore=write(sql,"USE "+ident(old_db));if(OB_SUCC(ret))ret=restore;}
  return ret;
}

namespace {
int check_column(ObDMLStmt &stmt,ObResolverParams &params,const ObColumnRefRawExpr *expr,bool writing,bool strict) {
  if(!expr)return OB_SUCCESS;
  auto *item=stmt.get_table_item_by_id(expr->get_table_id());
  if(!item||!item->is_basic_table())return OB_SUCCESS;
  const ObTableSchema *table=nullptr;
  int ret=params.schema_checker_->get_table_schema(item->ref_id_,table);
  if(OB_FAIL(ret)||!table)return ret;
  auto *col=table->get_column_schema(expr->get_column_id());
  if(!col)return OB_SUCCESS;
  if(writing&&(is_ai(*col)||is_private(col->get_column_name_str())))return unsupported("direct writes to AI COLUMN result or state");
  if(!is_ai(*col))return OB_SUCCESS;
  // AI statements must be resolved on every execution, including prepared statements.
  params.query_ctx_->has_ai_column_=true;
  if(!strict)return OB_SUCCESS;
  std::string sql="SELECT 1 FROM "+ident(str(item->database_name_))+"."+ident(str(table->get_table_name_str()))+
    " WHERE coalesce("+ident(hidden(col->get_column_id(),'s'))+",0)<>1 LIMIT 1";
  query::ObSessionInnerSql conn(params.session_info_);InternalGuard guard;
  ObISQLClient::ReadResult result;ObSqlString q;q.assign(sql.c_str());
  if(OB_FAIL(conn.execute_read(q,result)))return ret;
  if(!result.get_result())return OB_ERR_UNEXPECTED;
  ret=result.get_result()->next();
  if(ret==OB_ITER_END)return OB_SUCCESS;
  if(OB_SUCC(ret)) {
    ret=OB_AI_COLUMN_NOT_READY;
    LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY,table->get_table_id(),col->get_column_id());
  }
  return ret;
}
}
int check_statement(ObDMLStmt &stmt,ObResolverParams &params) {
  if(internal_access())return OB_SUCCESS;
  int ret=OB_SUCCESS;int64_t value=0;
  if(OB_FAIL(params.session_info_->get_sys_variable_by_name(ObString::make_string("ai_column_strict_read"),value)))return ret;
  auto &columns=stmt.get_column_items();
  for(int64_t i=0;i<columns.count();i++) {
    if(OB_FAIL(check_column(stmt,params,columns.at(i).expr_,false,false)))return ret;
  }
  // Trigger row records contain all physical columns. Only expressions read by
  // the user's statement count towards strict reads, not those implicit fields.
  ObSEArray<ObRawExpr*,16> roots;
  ObStmtExprGetter visitor;
  visitor.set_relation_scope();
  visitor.remove_scope(SCOPE_DML_COLUMN);
  visitor.remove_scope(SCOPE_DML_VALUE);
  visitor.remove_scope(SCOPE_DMLINFOS);
  visitor.remove_scope(SCOPE_DML_CONSTRAINT);
  if(OB_FAIL(stmt.get_relation_exprs(roots,visitor)))return ret;
  if(stmt.is_update_stmt()) {
    auto &infos=static_cast<ObUpdateStmt&>(stmt).get_update_table_info();
    for(int64_t i=0;i<infos.count();i++) {
      auto &assignments=infos.at(i)->assignments_;
      for(int64_t j=0;j<assignments.count();j++) {
        if(assignments.at(j).is_implicit_)continue;
        if(OB_FAIL(check_column(stmt,params,assignments.at(j).column_expr_,true,false)))return ret;
        if(OB_FAIL(roots.push_back(assignments.at(j).expr_)))return ret;
      }
    }
  } else if(stmt.is_insert_stmt()) {
    auto &insert=static_cast<ObInsertStmt&>(stmt);
    auto &values=insert.get_values_desc();
    for(int64_t i=0;i<values.count();i++)if(OB_FAIL(check_column(stmt,params,values.at(i),true,false)))return ret;
    auto &assignments=insert.get_table_assignments();
    for(int64_t i=0;i<assignments.count();i++)if(!assignments.at(i).is_implicit_ && OB_FAIL(check_column(stmt,params,assignments.at(i).column_expr_,true,false)))return ret;
  }
  if(value!=0) {
    ObSEArray<ObRawExpr*,16> reads;
    if(OB_FAIL(ObRawExprUtils::extract_column_exprs(roots,reads)))return ret;
    for(auto *expr:reads)if(OB_FAIL(check_column(stmt,params,static_cast<ObColumnRefRawExpr*>(expr),false,true)))return ret;
  }
  ObSEArray<ObSelectStmt*,8> children;
  if(OB_FAIL(stmt.get_child_stmts(children)))return ret;
  for(auto *s:children)if(s&&OB_FAIL(check_statement(*s,params)))return ret;
  return ret;
}

namespace {
std::string status_predicate(const std::string &input,const std::string &state) {
  std::string out;char quote=0;
  for(size_t i=0;i<input.size();) {
    char c=input[i];
    if(quote) {
      out+=c;i++;
      if(c=='\\'&&quote!='`'&&i<input.size())out+=input[i++];
      else if(c==quote){if(i<input.size()&&input[i]==quote)out+=input[i++];else quote=0;}
    } else if(c=='\''||c=='"'||c=='`'){quote=c;out+=c;i++;}
    else if(c=='#'||(c=='-'&&i+2<input.size()&&input[i+1]=='-'&&isspace(input[i+2]))) {
      size_t e=input.find('\n',i);if(e==input.npos)e=input.size();out+=input.substr(i,e-i);i=e;
    } else if(c=='/'&&i+1<input.size()&&input[i+1]=='*') {
      size_t e=input.find("*/",i+2);e=e==input.npos?input.size():e+2;out+=input.substr(i,e-i);i=e;
    } else if(isalpha(static_cast<unsigned char>(c))||c=='_') {
      size_t e=i+1;while(e<input.size()&&(isalnum(static_cast<unsigned char>(input[e]))||input[e]=='_'||input[e]=='$'))e++;
      auto word=input.substr(i,e-i);out+=lower(word)=="status"?state:word;i=e;
    } else {out+=c;i++;}
  }
  return out;
}
}
int rewrite_special(const ParseNode &node,ObResolverParams &params,ObString &sql) {
  if(node.num_child_<1||!node.children_[0])return OB_INVALID_ARGUMENT;
  const auto *relation=node.children_[0];
  if(relation->num_child_<2||!relation->children_[1])return OB_INVALID_ARGUMENT;
  ObString db=relation->children_[0]?ObString(relation->children_[0]->str_len_,relation->children_[0]->str_value_):params.session_info_->get_database_name();
  ObString tn(relation->children_[1]->str_len_,relation->children_[1]->str_value_);
  const ObTableSchema *table=nullptr;
  int ret=params.schema_checker_->get_schema_guard()->get_table_schema(db,tn,false,table);
  if(OB_FAIL(ret))return ret;
  if(!table)return OB_TABLE_NOT_EXIST;
  std::string q;
  if(node.value_==600497) {
    q="SELECT column_name,normalized_expr,max_retries FROM "+std::string(META)+" WHERE table_id="+std::to_string(table->get_table_id())+" ORDER BY column_id";
  } else {
    if((node.num_child_!=2&&node.num_child_!=3)||!node.children_[1])return OB_INVALID_ARGUMENT;
    auto name=text(node.children_[1]);auto *c=table->get_column_schema(ObString(name.size(),name.data()));
    if(!c||!is_ai(*c))return unsupported("retrying a column that is not an AI column");
    auto state="CASE "+ident(hidden(c->get_column_id(),'s'))+" WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END";
    auto predicate=node.num_child_==3?status_predicate(text(node.children_[2]),"("+state+")"):"TRUE";
    q="UPDATE "+ident(str(db))+"."+ident(str(tn))+" SET "+ident(hidden(c->get_column_id(),'r'))+"=uuid() WHERE "+predicate;
  }
  return ob_write_string(*params.allocator_,ObString(q.size(),q.data()),sql);
}

namespace {
struct Task {
  uint64_t tid=0,cid=0,version=0;
  int retries=0,attempt=0,state=0;
  std::string request,key,hash,db,table,column,expression,type,result;
};
std::string filter(const Task &t) {
  return "table_id="+std::to_string(t.tid)+" AND rowkey_hash="+literal(t.hash)+" AND request_id="+literal(t.request);
}
std::string row_guard(const Task &t) {
  return "("+t.key+") AND "+ident(hidden(t.cid,'r'))+"="+literal(t.request)+" AND "+ident(hidden(t.cid,'v'))+"="+std::to_string(t.version);
}
std::string qualified(const Task &t){return ident(t.db)+"."+ident(t.table);}
std::string fromhex(const std::string &hex) {
  std::string s;
  if(hex.size()%2)return s;
  auto digit=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return -1;};
  for(size_t i=0;i<hex.size();i+=2) {int a=digit(hex[i]),b=digit(hex[i+1]);if(a<0||b<0)return "";s+=char(a*16+b);}
  return s;
}
int evaluate(ObISQLClient &sql,const Task &t,const ObColumnSchemaV2 &column,std::string &value) {
  ObISQLClient::ReadResult res;
  int ret=read(sql,"SELECT hex(cast("+t.expression+" as char)) AS ai_value FROM "+qualified(t)+" WHERE "+row_guard(t),res);
  if(OB_FAIL(ret))return ret;
  if(!res.get_result())return OB_ERR_UNEXPECTED;
  if(OB_FAIL(res.get_result()->next()))return ret;
  ObString hex;
  ret=res.get_result()->get_varchar("ai_value",hex);
  if(ret==OB_ERR_NULL_VALUE){value="NULL";return OB_SUCCESS;}
  if(OB_FAIL(ret))return ret;
  std::string output=fromhex(str(hex));
  if(t.type=="SIGNED") {
    std::string s=trim(output);
    if(s.empty())return OB_INVALID_ARGUMENT;
    size_t p=(s[0]=='-'||s[0]=='+')?1:0;
    if(p==s.size())return OB_INVALID_ARGUMENT;
    for(size_t i=p;i<s.size();i++)if(!isdigit(static_cast<unsigned char>(s[i])))return OB_INVALID_ARGUMENT;
    const bool unsign=ob_is_unsigned_type(column.get_data_type());
    const int type=column.get_data_type();
    const int bits=(type==ObTinyIntType||type==ObUTinyIntType)?8:
                   (type==ObSmallIntType||type==ObUSmallIntType)?16:
                   (type==ObMediumIntType||type==ObUMediumIntType)?24:
                   (type==ObInt32Type||type==ObUInt32Type)?32:64;
    errno=0;char *end=nullptr;
    if(unsign) {
      if(s[0]=='-')return OB_DATA_OUT_OF_RANGE;
      auto n=strtoull(s.c_str(),&end,10);
      if(errno==ERANGE||!end||*end||(bits<64&&n>((1ULL<<bits)-1)))return OB_DATA_OUT_OF_RANGE;
    } else {
      auto n=strtoll(s.c_str(),&end,10);
      if(errno==ERANGE||!end||*end||(bits<64&&(n<-(1LL<<(bits-1))||n>((1LL<<(bits-1))-1))))return OB_DATA_OUT_OF_RANGE;
    }
    value="CAST("+literal(s)+(unsign?" AS UNSIGNED)":" AS SIGNED)");
  } else if(t.type=="JSON")value="CAST("+literal(output)+" AS JSON)";
  else {
    if(t.type=="CHAR") {
      const auto size=column.get_length_semantics()==LS_CHAR?
        ObCharset::strlen_char(CS_TYPE_UTF8MB4_BIN,output.data(),output.size()):output.size();
      if(size>static_cast<uint64_t>(column.get_data_length()))return OB_ERR_DATA_TOO_LONG;
    }
    value=literal(output);
  }
  if(value.size()>16000)return OB_SIZE_OVERFLOW;
  // Validate JSON before marking this task successful. Collection dimensions and
  // target SQL ranges are also checked by the final ordinary storage update.
  if(t.type=="JSON") {
    ObISQLClient::ReadResult valid;
    if(OB_FAIL(read(sql,"SELECT "+value+" AS value",valid)))return ret;
    if(!valid.get_result())return OB_ERR_UNEXPECTED;
    ret=valid.get_result()->next();
  }
  return ret;
}
int cycle(ObMySQLProxy &proxy,const std::string &owner) {
  int ret=OB_SUCCESS;
  Task group;
  {
    ObISQLClient::ReadResult res;
    std::string q="SELECT table_id,rowkey_hash,request_id FROM "+std::string(TASK)+
      " WHERE eligible_ts<=now(6) AND (lease_until IS NULL OR lease_until<now(6)) ORDER BY eligible_ts LIMIT 1";
    if(OB_FAIL(read(proxy,q,res)))return ret;
    if(!res.get_result())return OB_ERR_UNEXPECTED;
    if(OB_FAIL(res.get_result()->next()))return ret;
    group.tid=number(*res.get_result(),"table_id");group.hash=field(*res.get_result(),"rowkey_hash");group.request=field(*res.get_result(),"request_id");
  }
  int64_t claimed=0;
  std::string claim="UPDATE "+std::string(TASK)+" SET lease_owner="+literal(owner)+",lease_until=date_add(now(6),interval 300 second) WHERE "+filter(group)+
    " AND (lease_until IS NULL OR lease_until<now(6))";
  if(OB_FAIL(write(proxy,claim,&claimed))||claimed==0)return ret;
  std::vector<Task> tasks;
  {
    ObISQLClient::ReadResult res;
    std::string q="SELECT t.table_id,t.column_id,t.definition_version,t.rowkey_predicate,t.retry_count,t.state,t.result_sql,a.database_name,a.table_name,a.column_name,a.normalized_expr,a.result_type,a.max_retries FROM "+std::string(TASK)+" t JOIN "+META+" a ON a.table_id=t.table_id AND a.column_id=t.column_id AND a.definition_version=t.definition_version WHERE t."+filter(group)+" AND t.lease_owner="+literal(owner)+" ORDER BY t.column_id";
    if(OB_FAIL(read(proxy,q,res)))return ret;
    if(!res.get_result())return OB_ERR_UNEXPECTED;
    while(OB_SUCC(ret=res.get_result()->next())) {
      Task t=group;auto &r=*res.get_result();t.cid=number(r,"column_id");t.version=number(r,"definition_version");t.key=field(r,"rowkey_predicate");t.attempt=number(r,"retry_count");t.state=number(r,"state");t.result=field(r,"result_sql");t.retries=number(r,"max_retries");t.db=field(r,"database_name");t.table=field(r,"table_name");t.column=field(r,"column_name");t.expression=field(r,"normalized_expr");t.type=field(r,"result_type");tasks.push_back(t);
    }
    if(ret==OB_ITER_END)ret=OB_SUCCESS;
  }
  if(OB_FAIL(ret))return ret;
  if(tasks.empty())return write(proxy,"DELETE FROM "+std::string(TASK)+" WHERE "+filter(group)+" AND lease_owner="+literal(owner));
  // Resolve the current object identity before making any network request. A
  // drop/recreate must never cause a task to run against the replacement table.
  ObSchemaGetterGuard guard;const ObTableSchema *schema=nullptr;
  if(OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard)))return ret;
  if(OB_FAIL(guard.get_table_schema(group.tid,schema)))return ret;
  if(!schema||schema->is_in_recyclebin())return write(proxy,"DELETE FROM "+std::string(TASK)+" WHERE "+filter(group));
  bool pending=false;
  for(auto &t:tasks) {
    auto *col=schema->get_column_schema(t.cid);
    Definition d;
    if(!col||!decode(*col,d)||d.expression!=t.expression||version(d)!=t.version) {
      return write(proxy,"DELETE FROM "+std::string(TASK)+" WHERE "+filter(group)+" AND lease_owner="+literal(owner));
    }
    if(t.state==2||t.state==4)continue;
    int result=evaluate(proxy,t,*col,t.result);
    if(result==OB_ITER_END) {
      // Row deleted, moved, or superseded while this request was running.
      return write(proxy,"DELETE FROM "+std::string(TASK)+" WHERE "+filter(group)+" AND lease_owner="+literal(owner));
    }
    if(result==OB_SUCCESS)t.state=2;
    else if(t.attempt<t.retries){t.state=3;++t.attempt;pending=true;}
    else {t.state=4;t.result.clear();}
    std::string q="UPDATE "+std::string(TASK)+" SET state="+std::to_string(t.state)+",retry_count="+std::to_string(t.attempt)+",result_sql="+(t.state==2?literal(t.result):"NULL")+",last_error="+std::to_string(result)+" WHERE "+filter(t)+" AND column_id="+std::to_string(t.cid)+" AND lease_owner="+literal(owner);
    if(OB_FAIL(write(proxy,q)))return ret;
  }
  if(pending)return write(proxy,"UPDATE "+std::string(TASK)+" SET lease_owner=NULL,lease_until=NULL,eligible_ts=date_add(now(6),interval 1 second) WHERE "+filter(group)+" AND lease_owner="+literal(owner));
  // Publish every terminal column of this row/request in one storage transaction.
  // Lock the business row before the task rows, matching trigger lock ordering.
  ObMySQLTransaction tx;
  if(OB_FAIL(tx.start(&proxy)))return ret;
  std::string assignments,where;
  for(auto &t:tasks) {
    if(!assignments.empty()){assignments+=",";where+=" AND ";}
    assignments+=ident(t.column)+"="+(t.state==2?t.result:"NULL")+","+ident(hidden(t.cid,'s'))+"="+(t.state==2?"1":"2");
    where+="("+row_guard(t)+")";
  }
  int64_t affected=0;
  // The update obtains the business row lock first. Its values remain private
  // until task ownership has also been locked and verified below.
  ret=write(tx,"UPDATE "+qualified(tasks.front())+" SET "+assignments+" WHERE "+where,&affected);
  if(OB_SUCC(ret)&&affected>0) {
    ObISQLClient::ReadResult lease;
    ret=read(tx,"SELECT column_id FROM "+std::string(TASK)+" WHERE "+filter(group)+" AND lease_owner="+literal(owner)+" FOR UPDATE",lease);
    size_t count=0;
    if(OB_SUCC(ret)&&lease.get_result()) {
      while(OB_SUCCESS==(ret=lease.get_result()->next()))++count;
      if(ret==OB_ITER_END)ret=OB_SUCCESS;
    }
    if(OB_SUCC(ret)&&count!=tasks.size())ret=OB_EAGAIN;
  }
  if(OB_SUCC(ret))ret=write(tx,"DELETE FROM "+std::string(TASK)+" WHERE "+filter(group)+" AND lease_owner="+literal(owner));
  int end=tx.end(OB_SUCC(ret));if(OB_SUCC(ret))ret=end;
  if(OB_FAIL(ret)) {
    LOG_WARN("AI column publication deferred",K(ret),"table_id",group.tid);
    const int error=ret;
    write(proxy,"UPDATE "+std::string(TASK)+" SET last_error="+std::to_string(error)+
      ",lease_owner=NULL,lease_until=NULL,eligible_ts=date_add(now(6),interval 1 second) WHERE "+filter(group)+" AND lease_owner="+literal(owner));
  }
  return ret;
}
}
Worker::Worker():share::ObThreadPool(4){}
Worker &Worker::instance(){static Worker worker;return worker;}
void Worker::run1() {
  InternalGuard guard;
  if(get_thread_idx()==0) {
    // A new server process owns this embedded database exclusively. Reclaim
    // durable work interrupted in the previous process before any new claims.
    while(!has_set_stop()) {
      if(GCTX.sql_proxy_ && OB_SUCCESS==write(*GCTX.sql_proxy_,"UPDATE "+std::string(TASK)+" SET lease_owner=NULL,lease_until=NULL")) {
        recovered_.store(true);break;
      }
      ob_usleep(100000);
    }
  }
  while(!has_set_stop()&&!recovered_.load())ob_usleep(10000);
  std::string owner=std::to_string(ObTimeUtility::current_time())+"-"+std::to_string(get_thread_idx());
  while(!has_set_stop()) {
    int ret=GCTX.sql_proxy_?cycle(*GCTX.sql_proxy_,owner):OB_NOT_INIT;
    if(ret!=OB_SUCCESS&&ret!=OB_ITER_END&&REACH_TIME_INTERVAL(10000000))LOG_WARN("AI column worker cycle deferred",K(ret));
    ob_usleep(ret==OB_SUCCESS?10000:100000);
  }
}
}}} // namespace oceanbase::sql::ai_column
