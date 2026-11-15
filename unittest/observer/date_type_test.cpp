/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "gtest/gtest.h"

#include <cstring>

#include "common/type/date_type.h"
#include "common/value.h"
#include "storage/common/column.h"

TEST(DateTypeTest, valid_dates)
{
  DateType type;
  const char *inputs[] = {"1970-1-1", "2000-02-29", "2016-2-29", "2020-1-01", "2020-01-1", "2038-2-28"};
  const char *outputs[] = {"1970-01-01", "2000-02-29", "2016-02-29", "2020-01-01", "2020-01-01", "2038-02-28"};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
    SCOPED_TRACE(inputs[i]);
    Value value;
    ASSERT_EQ(RC::SUCCESS, type.set_value_from_str(value, inputs[i]));
    EXPECT_EQ(AttrType::DATES, value.attr_type());
    EXPECT_EQ(4, value.length());
    EXPECT_EQ(outputs[i], value.to_string());
  }
}

TEST(DateTypeTest, invalid_dates)
{
  DateType type;
  const char *inputs[] = {"", "2017-2-29", "1900-2-29", "2100-2-29", "2017-21-29", "2017-12-32",
      "2017-11-31", "2020-0-1", "2020-1-0", "0000-1-1", "2020-13-1", "2020-01-01x",
      "2020/01/01", "2020-+1-1", "2020- 1-1", "2020-001-1", "2020-1-001", " 2020-1-1", "2020-1-1 "};
  for (const char *input : inputs) {
    SCOPED_TRACE(input);
    Value value(42);
    EXPECT_NE(RC::SUCCESS, type.set_value_from_str(value, input));
    EXPECT_EQ(AttrType::INTS, value.attr_type());
    EXPECT_EQ(42, value.get_int());
  }
}

TEST(DateTypeTest, string_cast)
{
  Value date;
  ASSERT_EQ(RC::SUCCESS, Value::cast_to(Value("2020-1-2"), AttrType::DATES, date));
  EXPECT_EQ("2020-01-02", date.to_string());
  EXPECT_NE(RC::SUCCESS, Value::cast_to(Value("2021-2-29"), AttrType::DATES, date));
  EXPECT_EQ("2020-1-2", Value("2020-1-2").to_string());
  EXPECT_EQ("not a date", Value("not a date").to_string());
  EXPECT_EQ(1, DataType::type_instance(AttrType::CHARS)->cast_cost(AttrType::DATES));
}

TEST(DateTypeTest, storage_and_comparison)
{
  DateType type;
  Value earlier, later, same;
  ASSERT_EQ(RC::SUCCESS, type.set_value_from_str(earlier, "2019-12-31"));
  ASSERT_EQ(RC::SUCCESS, type.set_value_from_str(later, "2020-1-1"));
  ASSERT_EQ(RC::SUCCESS, type.set_value_from_str(same, "2020-01-01"));
  EXPECT_LT(earlier.compare(later), 0);
  EXPECT_GT(later.compare(earlier), 0);
  EXPECT_EQ(0, later.compare(same));

  char storage[5];
  memcpy(storage + 1, later.data(), later.length());
  Value restored(AttrType::DATES, storage + 1, 4);
  Value copied;
  copied.set_value(restored);
  EXPECT_EQ("2020-01-01", copied.to_string());
  EXPECT_EQ(0, later.compare(copied));

  Column dates(AttrType::DATES, 4, 2);
  ASSERT_EQ(RC::SUCCESS, dates.append_value(earlier));
  ASSERT_EQ(RC::SUCCESS, dates.append_value(later));
  EXPECT_LT(type.compare(dates, dates, 0, 1), 0);
  EXPECT_EQ(0, type.compare(dates, dates, 1, 1));
  EXPECT_EQ(AttrType::DATES, attr_type_from_string("dates"));
  EXPECT_STREQ("dates", attr_type_to_string(AttrType::DATES));
}
