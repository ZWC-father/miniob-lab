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
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>
#include <vector>

int VectorType::compare(const Value &left, const Value &right) const
{
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return INT32_MAX;
  }
  if (left.length() != right.length()) {
    return 1;
  }
  for (int i = 0; i < left.vector_dimension(); ++i) {
    if (left.vector_at(i) != right.vector_at(i)) {
      return 1;
    }
  }
  return 0;
}

RC VectorType::set_value_from_str(Value &val, const string &data) const
{
  size_t pos = 0;
  auto skip_space = [&]() {
    while (pos < data.size() && std::isspace(static_cast<unsigned char>(data[pos]))) {
      ++pos;
    }
  };
  auto digit = [&]() { return pos < data.size() && data[pos] >= '0' && data[pos] <= '9'; };
  skip_space();
  if (pos == data.size() || data[pos++] != '[') {
    return RC::INVALID_ARGUMENT;
  }
  std::vector<float> entries;
  while (true) {
    skip_space();
    size_t start = pos;
    if (pos < data.size() && (data[pos] == '+' || data[pos] == '-')) {
      ++pos;
    }
    bool has_digits = false;
    while (digit()) {
      has_digits = true;
      ++pos;
    }
    if (pos < data.size() && data[pos] == '.') {
      ++pos;
      while (digit()) {
        has_digits = true;
        ++pos;
      }
    }
    if (!has_digits) {
      return RC::INVALID_ARGUMENT;
    }
    if (pos < data.size() && (data[pos] == 'e' || data[pos] == 'E')) {
      ++pos;
      if (pos < data.size() && (data[pos] == '+' || data[pos] == '-')) {
        ++pos;
      }
      if (!digit()) {
        return RC::INVALID_ARGUMENT;
      }
      while (digit()) {
        ++pos;
      }
    }
    string number = data.substr(start, pos - start);
    char *end = nullptr;
    float entry = std::strtof(number.c_str(), &end);
    if (*end != '\0' || !std::isfinite(entry) || entries.size() >= MAX_DIMENSION) {
      return RC::INVALID_ARGUMENT;
    }
    entries.push_back(entry);
    skip_space();
    if (pos == data.size()) {
      return RC::INVALID_ARGUMENT;
    }
    char separator = data[pos++];
    if (separator == ']') {
      break;
    }
    if (separator != ',') {
      return RC::INVALID_ARGUMENT;
    }
  }
  skip_space();
  if (pos != data.size()) {
    return RC::INVALID_ARGUMENT;
  }
  val.set_vector(entries.data(), entries.size());
  return RC::SUCCESS;
}

RC VectorType::to_string(const Value &val, string &result) const
{
  if (val.attr_type() != AttrType::VECTORS || val.vector_dimension() == 0) {
    return RC::INVALID_ARGUMENT;
  }
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << '[' << std::scientific << std::setprecision(5);
  for (int i = 0; i < val.vector_dimension(); ++i) {
    if (i) {
      out << ',';
    }
    out << val.vector_at(i);
  }
  out << ']';
  result = out.str();
  return RC::SUCCESS;
}
