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

#include "sql/stmt/update_stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

UpdateStmt::~UpdateStmt() { delete filter_stmt_; }

RC UpdateStmt::create(Db *db, const UpdateSqlNode &update, Stmt *&stmt)
{
  stmt = nullptr;
  if (db == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  Table *table = db->find_table(update.relation_name.c_str());
  if (table == nullptr) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // 仅允许更新用户字段，不能修改事务等系统字段。
  const TableMeta &meta = table->table_meta();
  const FieldMeta *field = nullptr;
  for (int i = meta.sys_field_num(); i < meta.field_num(); i++) {
    if (update.attribute_name == meta.field(i)->name()) {
      field = meta.field(i);
      break;
    }
  }
  if (field == nullptr) {
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  // 在扫描前校验赋值类型，空表或 WHERE 无匹配时也必须拒绝非法赋值。
  Value value = update.value;
  if (value.attr_type() != field->type()) {
    RC rc = Value::cast_to(update.value, field->type(), value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }

  unordered_map<string, Table *> table_map;
  table_map.emplace(update.relation_name, table);
  FilterStmt *filter_stmt = nullptr;
  RC rc = FilterStmt::create(db, table, &table_map, update.conditions.data(),
      static_cast<int>(update.conditions.size()), filter_stmt);
  if (rc != RC::SUCCESS) {
    delete filter_stmt;
    return rc;
  }
  stmt = new UpdateStmt(table, *field, value, filter_stmt);
  return RC::SUCCESS;
}
