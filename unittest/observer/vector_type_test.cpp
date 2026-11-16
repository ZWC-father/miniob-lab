/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/vector_type.h"
#include "common/value.h"
#include "sql/expr/vector_function_expr.h"
#include "sql/parser/parse.h"
#include "gtest/gtest.h"

static Value vector_value(const char *text)
{
  Value value;
  EXPECT_EQ(VectorType().set_value_from_str(value, text), RC::SUCCESS);
  return value;
}

TEST(VectorBasic, DecimalScientificAndWhitespace)
{
  Value value = vector_value(" \t[ +1.05, -17.8, 3.2e1, .5, 1. ]\n");
  ASSERT_EQ(value.vector_dimension(), 5);
  EXPECT_FLOAT_EQ(value.vector_at(0), 1.05F);
  EXPECT_FLOAT_EQ(value.vector_at(2), 32);
  EXPECT_EQ(value.to_string(), "[1.05000e+00,-1.78000e+01,3.20000e+01,5.00000e-01,1.00000e+00]");
}

TEST(VectorBasic, RejectMalformedVectors)
{
  for (const char *text : {"", "[]", "[ ]", "1,2", "[1,]", "[,1]", "[1,,2]", "[1 2]", "[NaN]",
           "[inf]", "[1e]", "[1e+]", "[0x1p2]", "[1e999]", "[1]trailing", "[[1]]", "[+]"}) {
    Value value(123);
    EXPECT_NE(VectorType().set_value_from_str(value, text), RC::SUCCESS) << text;
    EXPECT_EQ(value.get_int(), 123);
  }
}

TEST(VectorBasic, MaximumDimension)
{
  string text = "[0";
  for (int i = 1; i < VectorType::MAX_DIMENSION; ++i) text += ",0";
  text += "]";
  Value value;
  ASSERT_EQ(VectorType().set_value_from_str(value, text), RC::SUCCESS);
  EXPECT_EQ(value.length(), VectorType::MAX_DIMENSION * sizeof(float));
  text.insert(text.size() - 1, ",0");
  EXPECT_NE(VectorType().set_value_from_str(value, text), RC::SUCCESS);
}

TEST(VectorBasic, CopyMoveAndReuseOwnThePayload)
{
  Value original = vector_value("[1,2,3]");
  Value copy(original);
  Value assigned(7.5F);
  assigned = original;
  EXPECT_NE(copy.data(), original.data());
  EXPECT_NE(assigned.data(), original.data());
  original = Value("replaced");
  EXPECT_FLOAT_EQ(copy.vector_at(1), 2);
  EXPECT_FLOAT_EQ(assigned.vector_at(2), 3);
  Value moved(std::move(copy));
  Value reused("old");
  reused = std::move(assigned);
  reused.set_value(reused);
  EXPECT_EQ(moved.compare(reused), 0);
  Value small = vector_value("[1,2]");
  EXPECT_NE(moved.compare(small), 0);
  Value minus_zero = vector_value("[-0]");
  EXPECT_EQ(minus_zero.compare(vector_value("[0]")), 0);
}

static RC function_value(const char *name, vector<Value> values, Value &result)
{
  vector<unique_ptr<Expression>> arguments;
  for (auto &value : values) arguments.emplace_back(make_unique<ValueExpr>(value));
  VectorFunctionExpr function(name, std::move(arguments));
  RC rc = function.validate();
  return OB_FAIL(rc) ? rc : function.try_get_value(result);
}

TEST(VectorBasic, DistancesAndAliases)
{
  Value a = vector_value("[1,2,3]"), b = vector_value("[4,6,3]"), result;
  ASSERT_EQ(function_value("distance", {a, b, Value("EUCLIDEAN")}, result), RC::SUCCESS);
  EXPECT_FLOAT_EQ(result.get_float(), 5);
  ASSERT_EQ(function_value("vector_distance", {a, b, Value("DOT")}, result), RC::SUCCESS);
  EXPECT_FLOAT_EQ(result.get_float(), 25);
  ASSERT_EQ(function_value("cosine_distance", {a, a}, result), RC::SUCCESS);
  EXPECT_NEAR(result.get_float(), 0, 1e-6);
  ASSERT_EQ(function_value("TO_VECTOR", {Value("[1,2,3]")}, result), RC::SUCCESS);
  EXPECT_EQ(result.compare(a), 0);
  ASSERT_EQ(function_value("FROM_VECTOR", {a}, result), RC::SUCCESS);
  EXPECT_EQ(result.get_string(), "[1.00000e+00,2.00000e+00,3.00000e+00]");
}

TEST(VectorBasic, InvalidFunctionArguments)
{
  Value a = vector_value("[1,2]"), b = vector_value("[1,2,3]"), zero = vector_value("[0,0]"), result;
  EXPECT_NE(function_value("distance", {a, b, Value("DOT")}, result), RC::SUCCESS);
  EXPECT_NE(function_value("distance", {a, a, Value("L2_DISTANCE")}, result), RC::SUCCESS);
  EXPECT_NE(function_value("distance", {a, a, Value(1)}, result), RC::SUCCESS);
  EXPECT_NE(function_value("distance", {a, a}, result), RC::SUCCESS);
  EXPECT_NE(function_value("cosine_distance", {a, zero}, result), RC::SUCCESS);
  EXPECT_NE(function_value("string_to_vector", {a}, result), RC::SUCCESS);
  EXPECT_NE(function_value("vector_to_string", {Value("[1,2]")}, result), RC::SUCCESS);
}

TEST(VectorBasic, ParseDimensionsAndNestedFunctions)
{
  for (const char *sql : {"create table t(v vector)", "create table t(v vector(16383))",
           "insert into t values(to_vector('[1,2,3]'))",
           "select distance(to_vector('[1,2]'),to_vector('[3,4]'),'DOT')",
           "select from_vector(v) from t where v=to_vector('[1,2]')"}) {
    ParsedSqlResult result;
    parse(sql, &result);
    ASSERT_EQ(result.sql_nodes().size(), 1);
    EXPECT_NE(result.sql_nodes()[0]->flag, SCF_ERROR) << sql;
  }
  for (const char *sql : {"create table t(v vector())", "create table t(v vector(0))",
           "create table t(v vector(16384))", "create table t(v vector(-1))",
           "create table t(v vector(4294967297))", "insert into t values(to_vector('[1,]'))"}) {
    ParsedSqlResult result;
    parse(sql, &result);
    ASSERT_FALSE(result.sql_nodes().empty());
    EXPECT_EQ(result.sql_nodes()[0]->flag, SCF_ERROR) << sql;
  }
  ParsedSqlResult result;
  parse("create table t(v vector)", &result);
  EXPECT_EQ(result.sql_nodes()[0]->create_table.attr_infos[0].length, 2048 * sizeof(float));
}
