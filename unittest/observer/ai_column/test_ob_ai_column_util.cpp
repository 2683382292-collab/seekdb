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

#include <gtest/gtest.h>

#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "observer/ai_column/ob_ai_column_util.h"
#include "share/schema/ob_ai_column_schema.h"

namespace oceanbase
{
namespace observer
{

using namespace common;
using namespace share::schema;

// The answer of a model is text, the column it lands in is typed. build_result_literal()
// is the only place that turns one into the other, so it is where a wrong answer has to
// be stopped: storing it would look like a successful generation forever.
class TestObAiColumnUtil : public ::testing::Test
{
public:
  TestObAiColumnUtil() : allocator_(), ai_column_(&allocator_) {}
  virtual ~TestObAiColumnUtil() {}

  virtual void SetUp() override
  {
    allocator_.reuse();
    ASSERT_EQ(OB_SUCCESS, ai_column_.set_column_name(ObString::make_string("answer")));
    ai_column_.set_table_id(500001);
    ai_column_.set_column_id(16);
    ai_column_.set_func_type(share::EndpointType::COMPLETION);
    ai_column_.set_result_type(ObLongTextType);
    ASSERT_EQ(OB_SUCCESS, ai_column_.set_model_name(ObString::make_string("text_model")));
    ASSERT_EQ(OB_SUCCESS, ai_column_.set_expr_str(
                              ObString::make_string("ai_complete('text_model', body)")));
    ASSERT_EQ(OB_SUCCESS, ai_column_.add_depend_column_id(12));
    ai_column_.set_sync_mode_async(true);
  }

  virtual void TearDown() override { allocator_.reuse(); }

  int build(const common::ObString &answer, common::ObString &literal, bool &is_null)
  {
    return ObAiColumnUtil::build_result_literal(allocator_, ai_column_, answer, literal, is_null);
  }

protected:
  ObArenaAllocator allocator_;
  ObAiColumnSchema ai_column_;
};

TEST_F(TestObAiColumnUtil, string_result_unquotes_the_json_answer)
{
  bool is_null = false;
  ObString literal;
  // The engine hands back the content of the model response as JSON, so a text answer
  // arrives quoted. The column has to hold the content, not the JSON.
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("\"hello world\""), literal, is_null));
  EXPECT_FALSE(is_null);
  EXPECT_TRUE(literal == ObString::make_string("'hello world'"));
}

TEST_F(TestObAiColumnUtil, plain_text_answer_is_kept)
{
  bool is_null = false;
  ObString literal;
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("the answer is 4"), literal, is_null));
  EXPECT_FALSE(is_null);
  EXPECT_TRUE(literal == ObString::make_string("'the answer is 4'"));
}

TEST_F(TestObAiColumnUtil, quotes_and_backslashes_are_escaped)
{
  bool is_null = false;
  ObString literal;
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("it's C:\\\\tmp"), literal, is_null));
  EXPECT_TRUE(literal == ObString::make_string("'it\\'s C:\\\\tmp'"));
}

TEST_F(TestObAiColumnUtil, json_null_keeps_the_column_null)
{
  bool is_null = false;
  ObString literal;
  // A model that answers nothing has not produced a value; the column stays NULL and the
  // row is done instead of being retried.
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("null"), literal, is_null));
  EXPECT_TRUE(is_null);
  EXPECT_TRUE(literal == ObString::make_string("NULL"));
}

TEST_F(TestObAiColumnUtil, integer_result_rejects_a_lossy_answer)
{
  ai_column_.set_result_type(ObInt32Type);
  bool is_null = false;
  ObString literal;
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("\"42\""), literal, is_null));
  EXPECT_TRUE(literal == ObString::make_string("42"));

  // 1.5 must not become 1: truncation would silently store a wrong value.
  EXPECT_EQ(OB_INVALID_DATA, build(ObString::make_string("\"1.5\""), literal, is_null));
  EXPECT_EQ(OB_INVALID_DATA, build(ObString::make_string("\"forty two\""), literal, is_null));
  EXPECT_EQ(OB_INVALID_DATA, build(ObString::make_string("\"\""), literal, is_null));
}

TEST_F(TestObAiColumnUtil, numeric_result_accepts_a_decimal)
{
  ai_column_.set_result_type(ObNumberType);
  bool is_null = false;
  ObString literal;
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("\"1.5\""), literal, is_null));
  EXPECT_TRUE(literal == ObString::make_string("1.5"));
  EXPECT_EQ(OB_INVALID_DATA, build(ObString::make_string("\"1.5kg\""), literal, is_null));
}

TEST_F(TestObAiColumnUtil, json_result_requires_a_document)
{
  ai_column_.set_result_type(ObJsonType);
  bool is_null = false;
  ObString literal;
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("{\"a\": 1}"), literal, is_null));
  EXPECT_TRUE(literal == ObString::make_string("'{\"a\": 1}'"));
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("[1, 2]"), literal, is_null));
  EXPECT_TRUE(literal == ObString::make_string("'[1, 2]'"));
  // Neither prose nor a bare scalar is a document.
  EXPECT_EQ(OB_INVALID_DATA, build(ObString::make_string("here you go"), literal, is_null));
  EXPECT_EQ(OB_INVALID_DATA, build(ObString::make_string("42"), literal, is_null));
}

TEST_F(TestObAiColumnUtil, datetime_result_is_written_as_a_literal)
{
  ai_column_.set_result_type(ObDateTimeType);
  bool is_null = false;
  ObString literal;
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("\"2024-01-02 03:04:05\""), literal, is_null));
  EXPECT_TRUE(literal == ObString::make_string("'2024-01-02 03:04:05'"));
}

TEST_F(TestObAiColumnUtil, vector_result_is_checked_against_the_metadata_dimension)
{
  ai_column_.set_func_type(share::EndpointType::DENSE_EMBEDDING);
  ai_column_.set_result_type(ObCollectionSQLType);
  ai_column_.set_vector_dimension(3);
  bool is_null = false;
  ObString literal;
  ASSERT_EQ(OB_SUCCESS, build(ObString::make_string("[0.1, 0.2, 0.3]"), literal, is_null));
  EXPECT_TRUE(literal == ObString::make_string("'[0.1, 0.2, 0.3]'"));

  // A model is free to answer a different size; storing it would corrupt the column and
  // the index built on it.
  EXPECT_EQ(OB_INVALID_DATA, build(ObString::make_string("[0.1, 0.2]"), literal, is_null));
  EXPECT_EQ(OB_INVALID_DATA,
            build(ObString::make_string("[0.1, 0.2, 0.3, 0.4]"), literal, is_null));
  EXPECT_EQ(OB_INVALID_DATA,
            build(ObString::make_string("[0.1, \"x\", 0.3]"), literal, is_null));
  EXPECT_EQ(OB_INVALID_DATA, build(ObString::make_string("not a vector"), literal, is_null));
}

TEST_F(TestObAiColumnUtil, string_literal_escaping_is_stable)
{
  ObString literal;
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::build_string_literal(
                            allocator_, ObString::make_string(""), literal));
  EXPECT_TRUE(literal == ObString::make_string("''"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::build_string_literal(
                            allocator_, ObString::make_string("a'b"), literal));
  EXPECT_TRUE(literal == ObString::make_string("'a\\'b'"));
}

} // namespace observer
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
