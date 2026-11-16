/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/vector_function_expr.h"
#include "common/type/vector_type.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

VectorFunctionExpr::VectorFunctionExpr(const char *name, vector<unique_ptr<Expression>> arguments)
    : function_name_(name), arguments_(std::move(arguments))
{
  std::transform(function_name_.begin(), function_name_.end(), function_name_.begin(),
      [](unsigned char c) { return std::tolower(c); });
  if (function_name_ == "to_vector") function_name_ = "string_to_vector";
  if (function_name_ == "from_vector") function_name_ = "vector_to_string";
  if (function_name_ == "vector_distance") function_name_ = "distance";
}

AttrType VectorFunctionExpr::value_type() const
{
  if (function_name_ == "string_to_vector") return AttrType::VECTORS;
  if (function_name_ == "vector_to_string") return AttrType::CHARS;
  return AttrType::FLOATS;
}

int VectorFunctionExpr::value_length() const
{
  if (value_type() == AttrType::VECTORS) return VectorType::MAX_DIMENSION * sizeof(float);
  if (value_type() == AttrType::CHARS) return 16 * VectorType::MAX_DIMENSION;
  return sizeof(float);
}

unique_ptr<Expression> VectorFunctionExpr::copy() const
{
  vector<unique_ptr<Expression>> arguments;
  for (auto &argument : arguments_) arguments.emplace_back(argument->copy());
  auto result = make_unique<VectorFunctionExpr>(function_name_.c_str(), std::move(arguments));
  result->set_name(name());
  return result;
}

bool VectorFunctionExpr::equal(const Expression &other) const
{
  if (other.type() != type()) return false;
  const auto &function = static_cast<const VectorFunctionExpr &>(other);
  if (function_name_ != function.function_name_ || arguments_.size() != function.arguments_.size()) return false;
  for (size_t i = 0; i < arguments_.size(); ++i) {
    if (!arguments_[i]->equal(*function.arguments_[i])) return false;
  }
  return true;
}

RC VectorFunctionExpr::validate() const
{
  const bool from_text = function_name_ == "string_to_vector";
  const bool to_text = function_name_ == "vector_to_string";
  const size_t count = from_text || to_text ? 1 : function_name_ == "distance" ? 3 : 2;
  if (arguments_.size() != count) return RC::INVALID_ARGUMENT;
  if (from_text) {
    if (arguments_[0]->value_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;
  } else {
    for (size_t i = 0; i < (to_text ? 1 : 2); ++i) {
      if (arguments_[i]->value_type() != AttrType::VECTORS) return RC::INVALID_ARGUMENT;
    }
    if (count == 3 && arguments_[2]->value_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;
  }
  if (count == 3) {
    Value metric;
    if (arguments_[2]->try_get_value(metric) == RC::SUCCESS) {
      string name = metric.get_string();
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
      if (name != "cosine" && name != "dot" && name != "euclidean") return RC::INVALID_ARGUMENT;
    }
  }
  // Validate constant arguments even for an empty source table.
  for (const auto &argument : arguments_) {
    if (argument->type() == ExprType::VECTOR_FUNCTION) {
      RC rc = static_cast<const VectorFunctionExpr &>(*argument).validate();
      if (OB_FAIL(rc)) return rc;
    }
  }
  Value result;
  RC rc = try_get_value(result);
  return rc == RC::UNIMPLEMENTED ? RC::SUCCESS : rc;
}

RC VectorFunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  vector<Value> arguments(arguments_.size());
  for (size_t i = 0; i < arguments_.size(); ++i) {
    RC rc = arguments_[i]->get_value(tuple, arguments[i]);
    if (OB_FAIL(rc)) return rc;
  }
  return evaluate(arguments, value);
}

RC VectorFunctionExpr::try_get_value(Value &value) const
{
  vector<Value> arguments(arguments_.size());
  for (size_t i = 0; i < arguments_.size(); ++i) {
    RC rc = arguments_[i]->try_get_value(arguments[i]);
    if (OB_FAIL(rc)) return rc;
  }
  return evaluate(arguments, value);
}

RC VectorFunctionExpr::evaluate(const vector<Value> &arguments, Value &value) const
{
  if (function_name_ == "string_to_vector") {
    if (arguments.size() != 1 || arguments[0].attr_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;
    return VectorType().set_value_from_str(value, arguments[0].get_string());
  }
  if (function_name_ == "vector_to_string") {
    if (arguments.size() != 1 || arguments[0].attr_type() != AttrType::VECTORS) return RC::INVALID_ARGUMENT;
    value.set_string(arguments[0].to_string().c_str());
    return RC::SUCCESS;
  }
  if (arguments.size() != (function_name_ == "distance" ? 3U : 2U)) return RC::INVALID_ARGUMENT;
  const Value &a = arguments[0], &b = arguments[1];
  if (a.attr_type() != AttrType::VECTORS || b.attr_type() != AttrType::VECTORS ||
      a.vector_dimension() == 0 || a.vector_dimension() != b.vector_dimension()) return RC::INVALID_ARGUMENT;
  string metric = function_name_;
  if (function_name_ == "distance") {
    if (arguments[2].attr_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;
    metric = arguments[2].get_string();
    std::transform(metric.begin(), metric.end(), metric.begin(), [](unsigned char c) { return std::tolower(c); });
    if (metric != "cosine" && metric != "dot" && metric != "euclidean") return RC::INVALID_ARGUMENT;
  }
  double dot = 0, square_a = 0, square_b = 0, square_diff = 0;
  for (int i = 0; i < a.vector_dimension(); ++i) {
    double x = a.vector_at(i), y = b.vector_at(i);
    dot += x * y;
    square_a += x * x;
    square_b += y * y;
    square_diff += (x - y) * (x - y);
  }
  double result;
  if (metric == "euclidean" || metric == "l2_distance") {
    result = std::sqrt(square_diff);
  } else if (metric == "dot" || metric == "inner_product") {
    result = dot;
  } else if (metric == "cosine" || metric == "cosine_distance") {
    if (square_a == 0 || square_b == 0) return RC::INVALID_ARGUMENT;
    double similarity = dot / (std::sqrt(square_a) * std::sqrt(square_b));
    result = 1 - std::clamp(similarity, -1.0, 1.0);
  } else {
    return RC::INVALID_ARGUMENT;
  }
  if (!std::isfinite(result) || std::abs(result) > std::numeric_limits<float>::max()) return RC::INVALID_ARGUMENT;
  value.set_float(static_cast<float>(result));
  return RC::SUCCESS;
}
