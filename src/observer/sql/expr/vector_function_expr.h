/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/expr/expression.h"

// Only the conversion and distance functions in vector-basic.
class VectorFunctionExpr : public Expression
{
public:
  VectorFunctionExpr(const char *name, vector<unique_ptr<Expression>> arguments);
  ExprType type() const override { return ExprType::VECTOR_FUNCTION; }
  AttrType value_type() const override;
  int value_length() const override;
  unique_ptr<Expression> copy() const override;
  bool equal(const Expression &other) const override;
  RC get_value(const Tuple &tuple, Value &value) const override;
  RC try_get_value(Value &value) const override;
  RC validate() const;
  vector<unique_ptr<Expression>> &arguments() { return arguments_; }

private:
  RC evaluate(const vector<Value> &arguments, Value &value) const;
  string function_name_;
  vector<unique_ptr<Expression>> arguments_;
};
