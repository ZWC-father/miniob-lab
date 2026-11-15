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
// Created by WangYunlai on 2022/11/18.
//

#include "sql/executor/sql_result.h"
#include "common/log/log.h"
#include "common/sys/rc.h"
#include "session/session.h"
#include "storage/trx/trx.h"

SqlResult::SqlResult(Session *session) : session_(session) {}

void SqlResult::set_tuple_schema(const TupleSchema &schema) { tuple_schema_ = schema; }

RC SqlResult::open()
{
  if (nullptr == operator_) {
    return RC::INVALID_ARGUMENT;
  }

  Trx *trx = session_->current_trx();
  trx->start_if_need();
  RC rc = operator_->open(trx);
  if (OB_FAIL(rc) || !vector_result_) return rc;
  vector_rows_.clear();
  vector_row_position_ = 0;
  while ((rc = operator_->next()) == RC::SUCCESS) {
    ValueListTuple row;
    rc = ValueListTuple::make(*operator_->current_tuple(), row);
    if (OB_FAIL(rc)) return rc;
    vector_rows_.emplace_back(std::move(row));
  }
  return rc == RC::RECORD_EOF ? RC::SUCCESS : rc;
}

RC SqlResult::close()
{
  if (nullptr == operator_) {
    return RC::INVALID_ARGUMENT;
  }
  RC rc = operator_->close();
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to close operator. rc=%s", strrc(rc));
  }

  operator_.reset();

  if (session_ && !session_->is_trx_multi_operation_mode()) {
    if (rc == RC::SUCCESS) {
      rc = session_->current_trx()->commit();
    } else {
      RC rc2 = session_->current_trx()->rollback();
      if (rc2 != RC::SUCCESS) {
        LOG_PANIC("rollback failed. rc=%s", strrc(rc2));
      }
    }
    session_->destroy_trx();
  }
  return rc;
}

RC SqlResult::next_tuple(Tuple *&tuple)
{
  if (vector_result_) {
    if (vector_row_position_ == vector_rows_.size()) return RC::RECORD_EOF;
    tuple = &vector_rows_[vector_row_position_++];
    return RC::SUCCESS;
  }
  RC rc = operator_->next();
  if (rc != RC::SUCCESS) {
    return rc;
  }

  tuple = operator_->current_tuple();
  return rc;
}

RC SqlResult::next_chunk(Chunk &chunk)
{
  RC rc = operator_->next(chunk);
  return rc;
}

void SqlResult::set_operator(unique_ptr<PhysicalOperator> oper)
{
  ASSERT(operator_ == nullptr, "current operator is not null. Result is not closed?");
  operator_ = std::move(oper);
  operator_->tuple_schema(tuple_schema_);
}
