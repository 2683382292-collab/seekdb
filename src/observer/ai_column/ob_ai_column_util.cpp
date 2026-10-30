/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SERVER

#include "observer/ai_column/ob_ai_column_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_common.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string.h"
#include "share/schema/ob_ai_column_schema.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

namespace
{

const char *skip_space(const char *ptr, const char *end)
{
  while (ptr < end && (' ' == *ptr || '\t' == *ptr || '\n' == *ptr || '\r' == *ptr)) {
    ++ptr;
  }
  return ptr;
}

// A model answers a number either as JSON number or as a JSON string holding the number.
// Accepting the second form is why the answer is validated as text and not cast blindly:
// "1.5" must be rejected for an integer column instead of silently becoming 1.
bool parse_int64(const ObString &text, int64_t &value)
{
  bool bret = false;
  if (!text.empty()) {
    const char *ptr = skip_space(text.ptr(), text.ptr() + text.length());
    const char *end = text.ptr() + text.length();
    while (end > ptr && (' ' == *(end - 1) || '\t' == *(end - 1)
                         || '\n' == *(end - 1) || '\r' == *(end - 1))) {
      --end;
    }
    bool negative = false;
    if (ptr < end && ('-' == *ptr || '+' == *ptr)) {
      negative = ('-' == *ptr);
      ++ptr;
    }
    bool overflow = (ptr >= end);
    int64_t result = 0;
    for (; !overflow && ptr < end; ++ptr) {
      if (*ptr < '0' || *ptr > '9') {
        overflow = true;
      } else {
        const int64_t digit = *ptr - '0';
        if (result > (INT64_MAX - digit) / 10) {
          overflow = true;
        } else {
          result = result * 10 + digit;
        }
      }
    }
    if (!overflow) {
      value = negative ? -result : result;
      bret = true;
    }
  }
  return bret;
}

bool parse_double(const ObString &text, double &value)
{
  bool bret = false;
  // A number longer than this is not a number this column can hold losslessly anyway.
  const int64_t MAX_NUMBER_LEN = 64;
  if (!text.empty() && text.length() < MAX_NUMBER_LEN) {
    char buf[MAX_NUMBER_LEN];
    memcpy(buf, text.ptr(), text.length());
    buf[text.length()] = '\0';
    errno = 0;
    char *end_ptr = NULL;
    const double result = strtod(buf, &end_ptr);
    if (ERANGE != errno && NULL != end_ptr && '\0' == *end_ptr && end_ptr != buf) {
      value = result;
      bret = true;
    }
  }
  return bret;
}

bool is_numeric_json_type(const ObJsonNodeType type)
{
  return ObJsonNodeType::J_INT == type || ObJsonNodeType::J_UINT == type
         || ObJsonNodeType::J_DOUBLE == type || ObJsonNodeType::J_DECIMAL == type
         || ObJsonNodeType::J_OFLOAT == type || ObJsonNodeType::J_ODOUBLE == type
         || ObJsonNodeType::J_ODECIMAL == type || ObJsonNodeType::J_OINT == type
         || ObJsonNodeType::J_OLONG == type;
}

int print_json(ObIAllocator &allocator, ObIJsonBase *json, const bool is_quoted, ObString &text)
{
  int ret = OB_SUCCESS;
  ObJsonBuffer buffer(&allocator);
  if (OB_ISNULL(json)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("json node is null", KR(ret));
  } else if (OB_FAIL(json->print(buffer, is_quoted))) {
    LOG_WARN("fail to print json node", KR(ret));
  } else if (OB_FAIL(ob_write_string(allocator, buffer.string(), text))) {
    LOG_WARN("fail to copy json text", KR(ret));
  }
  return ret;
}

} // namespace

int ObAiColumnUtil::build_string_literal(ObIAllocator &allocator,
                                         const ObString &value,
                                         ObString &literal)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  if (OB_FAIL(buf.append("'"))) {
    LOG_WARN("fail to append quote", KR(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ('\'' == c || '\\' == c) {
      // Both characters are special inside a MySQL string literal.
      const char escape = '\\';
      if (OB_FAIL(buf.append(&escape, 1))) {
        LOG_WARN("fail to append escape character", KR(ret));
      } else if (OB_FAIL(buf.append(&c, 1))) {
        LOG_WARN("fail to append character", KR(ret));
      }
    } else if (OB_FAIL(buf.append(&c, 1))) {
      LOG_WARN("fail to append character", KR(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(buf.append("'"))) {
    LOG_WARN("fail to append quote", KR(ret));
  } else if (OB_FAIL(ob_write_string(allocator, buf.string(), literal))) {
    LOG_WARN("fail to copy string literal", KR(ret));
  }
  return ret;
}

int ObAiColumnUtil::decode_answer(ObIAllocator &allocator,
                                  const ObString &raw_answer,
                                  ObString &answer,
                                  bool &is_json_null)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *json = NULL;
  is_json_null = false;
  if (raw_answer.empty()) {
    // The model returned nothing at all: an empty content is a valid empty answer for a
    // text column, so it is kept as the empty string instead of being rejected.
    is_json_null = true;
  } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator,
                                                      raw_answer,
                                                      ObJsonInType::JSON_TREE,
                                                      ObJsonInType::JSON_TREE,
                                                      json))) {
    // A model is free to answer plain text instead of JSON. That answer is used as is for
    // the string like result types and rejected by the type check of the numeric ones.
    is_json_null = false;
    if (OB_FAIL(ob_write_string(allocator, raw_answer, answer))) {
      LOG_WARN("fail to copy raw answer", KR(ret));
    }
  } else if (OB_ISNULL(json)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("json node is null", KR(ret));
  } else if (ObJsonNodeType::J_NULL == json->json_type()) {
    is_json_null = true;
  } else if (ObJsonNodeType::J_STRING == json->json_type()) {
    // A JSON string is unquoted: the escaped content is what the column has to hold.
    if (OB_FAIL(print_json(allocator, json, false, answer))) {
      LOG_WARN("fail to decode json string answer", KR(ret));
    }
  } else {
    if (OB_FAIL(print_json(allocator, json, false, answer))) {
      LOG_WARN("fail to print json answer", KR(ret));
    }
  }
  return ret;
}

int ObAiColumnUtil::check_scalar_result(ObIAllocator &allocator,
                                        const ObAiColumnSchema &ai_column,
                                        const ObString &answer,
                                        ObString &literal)
{
  int ret = OB_SUCCESS;
  const ObObjType result_type = ai_column.get_result_type();
  if (ob_is_string_type(result_type)) {
    if (OB_FAIL(build_string_literal(allocator, answer, literal))) {
      LOG_WARN("fail to build string literal", KR(ret));
    }
  } else if (ob_is_json(result_type)) {
    // A JSON column accepts only a complete JSON document, and a scalar is not one: the
    // definition of a JSON AI column promises an object or an array.
    ObIJsonBase *json = NULL;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator,
                                                 answer,
                                                 ObJsonInType::JSON_TREE,
                                                 ObJsonInType::JSON_TREE,
                                                 json))) {
      LOG_WARN("the answer of a json ai column is not json", KR(ret), K(answer));
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(json)
               || (ObJsonNodeType::J_OBJECT != json->json_type()
                   && ObJsonNodeType::J_ARRAY != json->json_type())) {
      ret = OB_INVALID_DATA;
      LOG_WARN("the answer of a json ai column is not an object", KR(ret),
               K(answer), K(ai_column));
    } else if (OB_FAIL(build_string_literal(allocator, answer, literal))) {
      LOG_WARN("fail to build json literal", KR(ret));
    }
  } else if (ob_is_integer_type(result_type)) {
    int64_t value = 0;
    if (!parse_int64(answer, value)) {
      // 1.5 for an INT column is rejected instead of being truncated to 1.
      ret = OB_INVALID_DATA;
      LOG_WARN("the answer of an integer ai column is not a lossless integer",
               KR(ret), K(answer), K(ai_column));
    } else if (OB_FAIL(ob_write_string(allocator, answer, literal))) {
      LOG_WARN("fail to copy integer literal", KR(ret));
    }
  } else if (ob_is_numeric_type(result_type)) {
    double value = 0;
    if (!parse_double(answer, value)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("the answer of a numeric ai column is not a lossless number",
               KR(ret), K(answer), K(ai_column));
    } else if (OB_FAIL(ob_write_string(allocator, answer, literal))) {
      LOG_WARN("fail to copy numeric literal", KR(ret));
    }
  } else if (ob_is_datetime_tc(result_type)) {
    // The date and time types are written as a string literal and converted by the engine,
    // which reports an out of range value instead of storing it.
    if (OB_FAIL(build_string_literal(allocator, answer, literal))) {
      LOG_WARN("fail to build datetime literal", KR(ret));
    }
  } else {
    ret = OB_INVALID_DATA;
    LOG_WARN("unsupported ai column result type", KR(ret), K(ai_column));
  }
  return ret;
}

int ObAiColumnUtil::check_vector_result(ObIAllocator &allocator,
                                        const ObAiColumnSchema &ai_column,
                                        const ObString &answer,
                                        ObString &literal)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *json = NULL;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator,
                                               answer,
                                               ObJsonInType::JSON_TREE,
                                               ObJsonInType::JSON_TREE,
                                               json))) {
    LOG_WARN("the answer of a vector ai column is not json", KR(ret), K(ai_column));
    ret = OB_INVALID_DATA;
  } else if (OB_ISNULL(json) || ObJsonNodeType::J_ARRAY != json->json_type()) {
    ret = OB_INVALID_DATA;
    LOG_WARN("the answer of a vector ai column is not an array", KR(ret), K(ai_column));
  } else if (json->element_count() != static_cast<uint64_t>(ai_column.get_vector_dimension())) {
    // The dimension is part of the metadata, so a model that answers a different size is
    // rejected here instead of being truncated or padded by the storage layer.
    ret = OB_INVALID_DATA;
    LOG_WARN("the answer dimension does not match the declared vector dimension", KR(ret),
             K(answer), "expected", ai_column.get_vector_dimension(),
             "actual", json->element_count(), K(ai_column));
  } else {
    for (uint64_t i = 0; OB_SUCC(ret) && i < json->element_count(); ++i) {
      ObIJsonBase *element = NULL;
      if (OB_FAIL(json->get_array_element(i, element))) {
        LOG_WARN("fail to get array element", KR(ret), K(i));
      } else if (OB_ISNULL(element) || !is_numeric_json_type(element->json_type())) {
        ret = OB_INVALID_DATA;
        LOG_WARN("the answer of a vector ai column holds a non numeric element",
                 KR(ret), K(i), K(ai_column));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(build_string_literal(allocator, answer, literal))) {
      LOG_WARN("fail to build vector literal", KR(ret));
    }
  }
  return ret;
}

int ObAiColumnUtil::build_result_literal(ObIAllocator &allocator,
                                         const ObAiColumnSchema &ai_column,
                                         const ObString &raw_answer,
                                         ObString &literal,
                                         bool &is_null)
{
  int ret = OB_SUCCESS;
  ObString answer;
  bool is_json_null = false;
  is_null = false;
  if (OB_FAIL(decode_answer(allocator, raw_answer, answer, is_json_null))) {
    LOG_WARN("fail to decode the model answer", KR(ret), K(ai_column));
  } else if (is_json_null) {
    is_null = true;
    if (OB_FAIL(ob_write_string(allocator, ObString::make_string("NULL"), literal))) {
      LOG_WARN("fail to build null literal", KR(ret));
    }
  } else if (ai_column.is_embedding()) {
    if (OB_FAIL(check_vector_result(allocator, ai_column, answer, literal))) {
      LOG_WARN("the embedding answer is rejected", KR(ret), K(ai_column));
    }
  } else if (OB_FAIL(check_scalar_result(allocator, ai_column, answer, literal))) {
    LOG_WARN("the completion answer is rejected", KR(ret), K(ai_column));
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
