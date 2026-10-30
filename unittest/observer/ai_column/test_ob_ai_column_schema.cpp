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
#include "share/schema/ob_ai_column_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

using namespace common;

// The metadata of an AI column is the only thing a restarted server has: it is
// persisted inside the table schema and rebuilt from there. These cases pin the two
// properties that hurt when they break silently, the round trip through the persisted
// text form and the result type the write back path validates against.
class TestObAiColumnSchema : public ::testing::Test
{
public:
  TestObAiColumnSchema() : allocator_(), schema_(&allocator_) {}
  virtual ~TestObAiColumnSchema() {}

  virtual void SetUp() override
  {
    allocator_.reuse();
    ASSERT_EQ(OB_SUCCESS, schema_.set_column_name(ObString::make_string("summary")));
    schema_.set_table_id(500001);
    schema_.set_column_id(16);
    schema_.set_func_type(share::EndpointType::COMPLETION);
    schema_.set_result_type(ObLongTextType);
    ASSERT_EQ(OB_SUCCESS, schema_.set_model_name(ObString::make_string("text_model")));
    ASSERT_EQ(OB_SUCCESS, schema_.set_expr_str(ObString::make_string("ai_complete('text_model', body)")));
    ASSERT_EQ(OB_SUCCESS, schema_.set_prompt_hint(ObString::make_string("Reply with plain text only.")));
    ASSERT_EQ(OB_SUCCESS, schema_.add_depend_column_id(12));
    schema_.set_vector_dimension(0);
    schema_.set_max_retry(3);
    schema_.set_timeout_sec(30);
    schema_.set_definition_version(7);
    schema_.set_sync_mode_async(true);
    schema_.set_status_column_id(17);
    schema_.set_lease_column_id(18);
    schema_.set_retry_column_id(19);
  }

  virtual void TearDown() override { allocator_.reuse(); }

protected:
  ObArenaAllocator allocator_;
  ObAiColumnSchema schema_;
};

TEST_F(TestObAiColumnSchema, validity)
{
  EXPECT_TRUE(schema_.is_valid());
  EXPECT_TRUE(schema_.is_completion());
  EXPECT_FALSE(schema_.is_embedding());
  EXPECT_EQ(OB_SUCCESS, schema_.check_valid());

  // A definition without a name is not usable, that is what a half parsed clause looks
  // like and it must not reach the table schema.
  ObAiColumnSchema empty(&allocator_);
  EXPECT_FALSE(empty.is_valid());
}

TEST_F(TestObAiColumnSchema, result_type_must_match_function_type)
{
  // A completion writes a scalar into the result column.
  EXPECT_EQ(OB_SUCCESS, schema_.check_result_type(ObLongTextType, 0));
  EXPECT_EQ(OB_SUCCESS, schema_.check_result_type(ObJsonType, 0));
  EXPECT_EQ(OB_SUCCESS, schema_.check_result_type(ObInt32Type, 0));
  EXPECT_EQ(OB_SUCCESS, schema_.check_result_type(ObDateTimeType, 0));
  // A vector column cannot hold the text of a completion.
  EXPECT_EQ(OB_INVALID_ARGUMENT, schema_.check_result_type(ObCollectionSQLType, 4));

  // An embedding writes a vector, and its dimension has to be in the definition.
  ObAiColumnSchema embedding(&allocator_);
  embedding.set_func_type(share::EndpointType::DENSE_EMBEDDING);
  EXPECT_EQ(OB_INVALID_ARGUMENT, embedding.check_result_type(ObCollectionSQLType, 0));
  EXPECT_EQ(OB_INVALID_ARGUMENT, embedding.check_result_type(ObLongTextType, 4));
  EXPECT_EQ(OB_SUCCESS, embedding.check_result_type(ObCollectionSQLType, 4));

  // Only the two asynchronous function types are implemented.
  ObAiColumnSchema rerank(&allocator_);
  rerank.set_func_type(share::EndpointType::RERANK);
  EXPECT_EQ(OB_NOT_SUPPORTED, rerank.check_result_type(ObLongTextType, 0));
}

TEST_F(TestObAiColumnSchema, hidden_column_names)
{
  ObString hidden_name;
  ASSERT_EQ(OB_SUCCESS, ObAiColumnSchema::build_status_column_name(
                            allocator_, schema_.get_column_name(), hidden_name));
  EXPECT_TRUE(hidden_name == ObString::make_string("__ai_status_summary"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnSchema::build_lease_column_name(
                            allocator_, schema_.get_column_name(), hidden_name));
  EXPECT_TRUE(hidden_name == ObString::make_string("__ai_lease_summary"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnSchema::build_retry_column_name(
                            allocator_, schema_.get_column_name(), hidden_name));
  EXPECT_TRUE(hidden_name == ObString::make_string("__ai_retry_summary"));
}

TEST_F(TestObAiColumnSchema, depend_column_ids_are_a_set)
{
  EXPECT_EQ(1, schema_.get_depend_column_ids().count());
  EXPECT_TRUE(schema_.depend_on(12));
  EXPECT_FALSE(schema_.depend_on(13));
  // The same column twice is recorded once, a duplicate would make the DML hook compare
  // it twice for no reason.
  ASSERT_EQ(OB_SUCCESS, schema_.add_depend_column_id(12));
  ASSERT_EQ(OB_SUCCESS, schema_.add_depend_column_id(13));
  EXPECT_EQ(2, schema_.get_depend_column_ids().count());
  EXPECT_TRUE(schema_.depend_on(13));
}

TEST_F(TestObAiColumnSchema, serialize_deserialize_round_trip)
{
  ObString info;
  ASSERT_EQ(OB_SUCCESS, schema_.serialize(allocator_, info));
  EXPECT_FALSE(info.empty());
  EXPECT_TRUE(ObAiColumnSchema::is_ai_column_info(info));

  ObAiColumnSchema restored(&allocator_);
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(info));
  EXPECT_EQ(schema_.get_func_type(), restored.get_func_type());
  EXPECT_EQ(schema_.get_result_type(), restored.get_result_type());
  EXPECT_EQ(schema_.get_vector_dimension(), restored.get_vector_dimension());
  EXPECT_EQ(schema_.get_max_retry(), restored.get_max_retry());
  EXPECT_EQ(schema_.get_timeout_sec(), restored.get_timeout_sec());
  EXPECT_EQ(schema_.get_definition_version(), restored.get_definition_version());
  EXPECT_EQ(schema_.is_sync_mode_async(), restored.is_sync_mode_async());
  EXPECT_TRUE(schema_.get_model_name() == restored.get_model_name());
  EXPECT_TRUE(schema_.get_expr_str() == restored.get_expr_str());
  EXPECT_TRUE(schema_.get_prompt_hint() == restored.get_prompt_hint());
  ASSERT_EQ(schema_.get_depend_column_ids().count(), restored.get_depend_column_ids().count());
  for (int64_t i = 0; i < restored.get_depend_column_ids().count(); ++i) {
    EXPECT_EQ(schema_.get_depend_column_ids().at(i), restored.get_depend_column_ids().at(i));
  }
}

TEST_F(TestObAiColumnSchema, serialize_keeps_the_vector_dimension)
{
  // The dimension used to be dropped when the definition was persisted, which made the
  // write back path accept an embedding of any length.
  schema_.set_func_type(share::EndpointType::DENSE_EMBEDDING);
  schema_.set_result_type(ObCollectionSQLType);
  schema_.set_vector_dimension(4);
  ObString info;
  ASSERT_EQ(OB_SUCCESS, schema_.serialize(allocator_, info));
  ObAiColumnSchema restored(&allocator_);
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(info));
  EXPECT_EQ(4, restored.get_vector_dimension());
  EXPECT_TRUE(restored.is_embedding());
}

TEST_F(TestObAiColumnSchema, foreign_info_is_rejected)
{
  // Another column of the table must never be read as an AI column definition.
  EXPECT_FALSE(ObAiColumnSchema::is_ai_column_info(
                   ObString::make_string("not an ai column")));
  EXPECT_FALSE(ObAiColumnSchema::is_ai_column_info(ObString()));
}

TEST_F(TestObAiColumnSchema, assign_and_deep_copy)
{
  ObAiColumnSchema assigned(&allocator_);
  ASSERT_EQ(OB_SUCCESS, assigned.assign(schema_));
  EXPECT_TRUE(assigned.get_column_name() == schema_.get_column_name());
  EXPECT_EQ(schema_.get_column_id(), assigned.get_column_id());
  EXPECT_EQ(schema_.get_status_column_id(), assigned.get_status_column_id());

  // A copy owns its text, so it stays readable after the source allocator is reset.
  ObArenaAllocator other_allocator;
  ObAiColumnSchema copied(&other_allocator);
  ASSERT_EQ(OB_SUCCESS, copied.deep_copy(other_allocator, schema_));
  allocator_.reuse();
  EXPECT_TRUE(copied.get_model_name() == ObString::make_string("text_model"));
  EXPECT_TRUE(copied.get_expr_str() == ObString::make_string("ai_complete('text_model', body)"));
  EXPECT_EQ(1, copied.get_depend_column_ids().count());
  EXPECT_LT(0, copied.get_convert_size());
}

} // namespace schema
} // namespace share
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
