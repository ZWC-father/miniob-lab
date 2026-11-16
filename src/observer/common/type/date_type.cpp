/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/date_type.h"

#include <cstdio>

#include "common/log/log.h"
#include "common/value.h"
#include "storage/common/column.h"

int DateType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::DATES && right.attr_type() == AttrType::DATES, "invalid date type");
  const int left_date  = left.get_int();
  const int right_date = right.get_int();
  return (left_date > right_date) - (left_date < right_date);
}

int DateType::compare(const Column &left, const Column &right, int left_idx, int right_idx) const
{
  return compare(left.get_value(left_idx), right.get_value(right_idx));
}

RC DateType::set_value_from_str(Value &val, const string &data) const
{
  // 仅接受 YYYY-M-D；月份和日期允许补零，不接受空白、符号或尾随字符。
  if (data.size() < 8 || data.size() > 10) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  int    parts[3] = {};
  size_t pos      = 0;
  for (int i = 0; i < 3; i++) {
    const size_t start = pos;
    const size_t limit = i == 0 ? 4 : 2;
    while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
      if (pos - start >= limit) {
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      parts[i] = parts[i] * 10 + data[pos++] - '0';
    }
    if (pos == start || (i == 0 && pos - start != 4)) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    if (i < 2) {
      if (pos == data.size() || data[pos++] != '-') {
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
    } else if (pos != data.size()) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }

  const int year  = parts[0];
  const int month = parts[1];
  const int day   = parts[2];
  if (year == 0 || month < 1 || month > 12) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int       max_day          = days_per_month[month - 1];
  if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
    max_day++;
  }
  if (day < 1 || day > max_day) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  val.set_date(year * 10000 + month * 100 + day);
  return RC::SUCCESS;
}

RC DateType::to_string(const Value &val, string &result) const
{
  const int date = val.get_int();
  char      buffer[32];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date / 10000, date / 100 % 100, date % 100);
  result = buffer;
  return RC::SUCCESS;
}
