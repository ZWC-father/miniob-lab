/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <memory>
#include <string>
#include <vector>

#include "sql/expr/expression.h"
#include "sql/parser/parse.h"
#include "gtest/gtest.h"

using namespace std;

TEST(ParserTest, vector_select_alias)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("SELECT ID, VECTOR_TO_STRING(C1) AS VEC_STR FROM TEST;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_SELECT);
  auto &expressions = result.sql_nodes()[0]->selection.expressions;
  ASSERT_EQ(expressions.size(), 2);
  EXPECT_STREQ(expressions[0]->name(), "ID");
  EXPECT_FALSE(expressions[0]->has_alias());
  EXPECT_EQ(expressions[1]->type(), ExprType::VECTOR_FUNCTION);
  EXPECT_STREQ(expressions[1]->name(), "VEC_STR");
  EXPECT_TRUE(expressions[1]->has_alias());
}

TEST(ParserTest, vector_constant_alias)
{
  ParsedSqlResult result;
  ASSERT_EQ(parse("SELECT VECTOR_TO_STRING(TO_VECTOR('[1,2]')) as Vec_Str, 1 AS ID;", &result), RC::SUCCESS);
  ASSERT_EQ(result.sql_nodes().size(), 1);
  ASSERT_EQ(result.sql_nodes()[0]->flag, SCF_CALC);
  auto &expressions = result.sql_nodes()[0]->calc.expressions;
  ASSERT_EQ(expressions.size(), 2);
  EXPECT_STREQ(expressions[0]->name(), "Vec_Str");
  EXPECT_STREQ(expressions[1]->name(), "ID");
}

TEST(ParserTest, aliases_only_in_select_list)
{
  for (const char *sql : {"SELECT VECTOR_TO_STRING(C1 AS V) FROM TEST;",
           "SELECT ID FROM TEST WHERE ID AS X=1;", "SELECT ID AS FROM TEST;"}) {
    ParsedSqlResult result;
    parse(sql, &result);
    ASSERT_EQ(result.sql_nodes().size(), 1) << sql;
    EXPECT_EQ(result.sql_nodes()[0]->flag, SCF_ERROR) << sql;
  }
}

TEST(ParserTest, DISABLED_aggregation_test)
{
  ParsedSqlResult result;
  {
    const char *sql = "select sum(a), sum(b) from tab";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select sum(a+b*c+d/e-f*(g-h)) from tab";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
}

TEST(ParserTest, DISABLED_group_by_test)
{
  ParsedSqlResult result;
  {
    const char *sql = "select a, sum(b) from tab group by a";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select sum(a+b*c+d/e-f*(g+1)) from tab group by c+d-e";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select a,b,sum(a+b*c+d/e-f*(g+1)),e,f,d from tab group by c+d-e";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select a,b,sum(a+b*c+d/e-f*(g+1)),e,f,d from tab where a>1 and b< 10 and c='abc' group by c+d-e";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select t.a,s.c,sum(u.rr) from t,s,u where t.a<10 and t.b!='vfdv' and u.c='abc' group by s.c+u.q";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select a+a,sum(b+b)+sum(b) from tab group by a";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
  {
    const char *sql = "select a+a,sum(b+b)+sum(b) from tab group by a+a";
    ASSERT_EQ(parse(sql, &result), RC::SUCCESS);
  }
}

int main(int argc, char **argv)
{

  // 分析gtest程序的命令行参数
  testing::InitGoogleTest(&argc, argv);

  // 调用RUN_ALL_TESTS()运行所有测试用例
  // main函数返回RUN_ALL_TESTS()的运行结果
  return RUN_ALL_TESTS();
}
