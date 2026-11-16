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
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/operator/update_physical_operator.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

RC UpdatePhysicalOperator::open(Trx *trx)
{
  result_ = RC::SUCCESS;
  if (trx == nullptr || children_.size() > 1) {
    return result_ = RC::INVALID_ARGUMENT;
  }
  // 恒假条件会被优化器消除，空子计划表示没有待更新记录。
  if (children_.empty()) {
    return RC::SUCCESS;
  }
  auto &child = children_.front();
  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    return result_ = rc;
  }

  // 先保存匹配记录并释放扫描器，避免更新索引键后重复扫描、漏行或锁死。
  // Record 可能引用页面内存，必须显式深拷贝后才能继续扫描。
  vector<Record> records;
  while ((rc = child->next()) == RC::SUCCESS) {
    Tuple *tuple = child->current_tuple();
    if (tuple == nullptr) {
      rc = RC::INTERNAL;
      break;
    }
    Record &source = static_cast<RowTuple *>(tuple)->record();
    Record record;
    rc = record.copy_data(source.data(), table_->table_meta().record_size());
    if (rc != RC::SUCCESS) {
      break;
    }
    record.set_rid(source.rid());
    record.set_key(source.key());
    records.emplace_back(std::move(record));
  }
  RC close_rc = child->close();
  if (rc != RC::RECORD_EOF) {
    return result_ = rc;
  }
  if (close_rc != RC::SUCCESS) {
    return result_ = close_rc;
  }

  for (Record &old_record : records) {
    Record new_record = old_record;
    char *dest = new_record.data() + field_.offset();
    // 短字符串覆盖长字符串时清空尾部；长度处理与原有 INSERT 保持一致。
    memset(dest, 0, field_.len());
    size_t copy_len = field_.len();
    if (field_.type() == AttrType::CHARS && copy_len > static_cast<size_t>(value_.length())) {
      copy_len = value_.length();
    }
    memcpy(dest, value_.data(), copy_len);
    rc = trx->update_record(table_, old_record, new_record);
    if (rc != RC::SUCCESS) {
      return result_ = rc;
    }
  }
  return RC::SUCCESS;
}
