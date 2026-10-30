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

#pragma once

#include "lib/ob_define.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_obj_type.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObAiColumnSchema;
}
}
namespace observer
{

// Result checking and SQL text helpers of the AI column background service.
//
// The service never trusts the model. The raw answer is decoded, validated against the
// result type declared by the AI column and only then turned into a SQL literal. A
// rejected answer leaves the result column NULL and the task FAILED, so a value that
// does not fit the declared type can never be observed as a valid result: this is the
// difference between "the answer was stored" and "the answer was stored correctly".
class ObAiColumnUtil
{
public:
  // Decodes the raw answer returned by the model and validates it against the declared
  // result type of the AI column.
  //   literal  : SQL text that can be assigned to the result column. Quoted for the
  //              string like types, raw for the numeric ones.
  //   is_null  : the answer is JSON null, or the model produced no answer at all. The
  //              result column stays NULL and the task is still considered done.
  // Returns OB_INVALID_DATA when the answer cannot be losslessly represented by the
  // declared result type.
  static int build_result_literal(common::ObIAllocator &allocator,
                                  const share::schema::ObAiColumnSchema &ai_column,
                                  const common::ObString &raw_answer,
                                  common::ObString &literal,
                                  bool &is_null);

  // Quotes and escapes a value so that it can be embedded in a SQL text statement.
  static int build_string_literal(common::ObIAllocator &allocator,
                                  const common::ObString &value,
                                  common::ObString &literal);

private:
  // A model answers through JSON. A string answer arrives as a JSON string, an object or
  // array answer arrives as the printed JSON text. Both shapes are normalized to the
  // plain text the result column expects.
  static int decode_answer(common::ObIAllocator &allocator,
                           const common::ObString &raw_answer,
                           common::ObString &answer,
                           bool &is_json_null);
  // Validates a textual answer against the declared scalar result type and produces the
  // literal that can be assigned to the result column.
  static int check_scalar_result(common::ObIAllocator &allocator,
                                 const share::schema::ObAiColumnSchema &ai_column,
                                 const common::ObString &answer,
                                 common::ObString &literal);
  // Validates a dense embedding answer: it must be a JSON array of numbers whose length
  // matches the vector dimension persisted in the metadata.
  static int check_vector_result(common::ObIAllocator &allocator,
                                 const share::schema::ObAiColumnSchema &ai_column,
                                 const common::ObString &answer,
                                 common::ObString &literal);
};

} // namespace observer
} // namespace oceanbase
