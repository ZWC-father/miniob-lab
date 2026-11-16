/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Longda on 2021/4/13.
//

#include <string.h>

#include "resolve_stage.h"

#include "common/conf/ini.h"
#include "common/io/io.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/calc_stmt.h"
#include "sql/expr/expression_iterator.h"

using namespace common;

RC ResolveStage::handle_request(SQLStageEvent *sql_event)
{
  RC            rc            = RC::SUCCESS;
  SessionEvent *session_event = sql_event->session_event();
  SqlResult    *sql_result    = session_event->sql_result();

  Db *db = session_event->session()->get_current_db();
  if (nullptr == db) {
    LOG_ERROR("cannot find current db");
    rc = RC::SCHEMA_DB_NOT_EXIST;
    sql_result->set_return_code(rc);
    sql_result->set_state_string("no db selected");
    return rc;
  }

  ParsedSqlNode *sql_node = sql_event->sql_node().get();
  Stmt          *stmt     = nullptr;

  rc = Stmt::create_stmt(db, *sql_node, stmt);
  if (rc != RC::SUCCESS && rc != RC::UNIMPLEMENTED) {
    LOG_WARN("failed to create stmt. rc=%d:%s", rc, strrc(rc));
    sql_result->set_return_code(rc);
    return rc;
  }

  bool vector_result = false;
  function<RC(unique_ptr<Expression> &)> inspect = [&](unique_ptr<Expression> &expression) {
    if (!expression) return RC::SUCCESS;
    if (expression->type() == ExprType::VECTOR_FUNCTION || expression->value_type() == AttrType::VECTORS) {
      vector_result = true;
      return RC::SUCCESS;
    }
    if (expression->type() == ExprType::UNBOUND_AGGREGATION) return RC::SUCCESS;
    return ExpressionIterator::iterate_child_expr(*expression, inspect);
  };
  if (stmt && stmt->type() == StmtType::CALC) {
    for (auto &expression : static_cast<CalcStmt *>(stmt)->expressions()) inspect(expression);
  } else if (stmt && stmt->type() == StmtType::SELECT) {
    auto *select = static_cast<SelectStmt *>(stmt);
    for (auto &expression : select->query_expressions()) inspect(expression);
    for (Table *table : select->tables()) {
      for (const FieldMeta &field : *table->table_meta().field_metas()) {
        vector_result |= field.type() == AttrType::VECTORS;
      }
    }
    // Functions may also occur only in WHERE on a non-vector source table.
    for (const auto &condition : sql_node->selection.conditions) {
      if (condition.left_expression || condition.right_expression) vector_result = true;
    }
  }
  sql_result->set_vector_result(vector_result);
  sql_event->set_stmt(stmt);

  return rc;
}
