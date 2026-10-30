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
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_obj_type.h"
#include "share/ai_service/ob_ai_model_info.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// Values of the hidden status column __ai_status_<col>. They are persisted in the
// user table itself, so they must stay stable across versions.
enum ObAiColumnStatus : int64_t
{
  AI_COLUMN_STATUS_PENDING = 0,     // waiting for generation
  AI_COLUMN_STATUS_PROCESSING = 1,  // claimed by a worker
  AI_COLUMN_STATUS_READY = 2,       // result is ready
  AI_COLUMN_STATUS_FAILED = 3,      // terminally failed after retries
  AI_COLUMN_STATUS_MAX
};

extern const char *const OB_AI_COLUMN_STATUS_PREFIX;
extern const char *const OB_AI_COLUMN_LEASE_PREFIX;
extern const char *const OB_AI_COLUMN_RETRY_PREFIX;
// Tag of the persisted definition inside __all_column.ai_column_info.
extern const char *const OB_AI_COLUMN_INFO_TAG;

const char *get_ai_column_status_str(const int64_t status);

// A single AI column definition. It is persisted in __all_column.ai_column_info of
// the AI result column and rebuilt on demand by ObAiColumnMgr, so the table schema
// stays the single source of truth.
class ObAiColumnSchema
{
public:
  ObAiColumnSchema();
  explicit ObAiColumnSchema(common::ObIAllocator *allocator);
  ~ObAiColumnSchema() = default;
  ObAiColumnSchema &operator=(const ObAiColumnSchema &other);

  void reset();
  int assign(const ObAiColumnSchema &other);
  int deep_copy(common::ObIAllocator &allocator, const ObAiColumnSchema &other);
  int64_t get_convert_size() const;

  // Validation of a resolved definition, called by the DDL resolver.
  int check_valid() const;
  // Whether the declared result type is compatible with the function type.
  int check_result_type(const common::ObObjType result_type, const int64_t vector_dimension) const;

  int add_depend_column_id(const uint64_t column_id);
  bool depend_on(const uint64_t column_id) const;
  bool is_depended_by(const uint64_t column_id) const { return depend_on(column_id); }

  // Hidden column names are derived from the AI column name, keeping the metadata
  // self-describing so that a cold start can rebuild everything from the schema.
  static int build_status_column_name(common::ObIAllocator &allocator,
                                      const common::ObString &column_name,
                                      common::ObString &hidden_name);
  static int build_lease_column_name(common::ObIAllocator &allocator,
                                     const common::ObString &column_name,
                                     common::ObString &hidden_name);
  static int build_retry_column_name(common::ObIAllocator &allocator,
                                     const common::ObString &column_name,
                                     common::ObString &hidden_name);
  static int build_hidden_column_name(common::ObIAllocator &allocator,
                                      const char *prefix,
                                      const common::ObString &column_name,
                                      common::ObString &hidden_name);

  bool is_valid() const { return OB_INVALID_ID != table_id_ && OB_INVALID_ID != column_id_ && !column_name_.empty(); }
  bool is_embedding() const { return share::EndpointType::DENSE_EMBEDDING == func_type_; }
  bool is_completion() const { return share::EndpointType::COMPLETION == func_type_; }

  // The definition is persisted in __all_column.ai_column_info of the AI result
  // column as a sequence of length prefixed "key=<len>,<bytes>" entries. Storing it
  // next to the column instead of in a new inner table means a cold start rebuilds
  // everything from the table schema and no extra DDL bookkeeping is needed when a
  // table or a column is dropped.
  int serialize(common::ObIAllocator &allocator, common::ObString &info) const;
  int deserialize(const common::ObString &info);
  static bool is_ai_column_info(const common::ObString &info);

  uint64_t get_table_id() const { return table_id_; }
  uint64_t get_column_id() const { return column_id_; }
  const common::ObString &get_column_name() const { return column_name_; }
  share::EndpointType::TYPE get_func_type() const { return func_type_; }
  const common::ObString &get_model_name() const { return model_name_; }
  const common::ObString &get_expr_str() const { return expr_str_; }
  const common::ObString &get_prompt_hint() const { return prompt_hint_; }
  const common::ObIArray<uint64_t> &get_depend_column_ids() const { return depend_column_ids_; }
  common::ObIArray<uint64_t> &get_depend_column_ids() { return depend_column_ids_; }
  common::ObObjType get_result_type() const { return result_type_; }
  int64_t get_vector_dimension() const { return vector_dimension_; }
  int64_t get_max_retry() const { return max_retry_; }
  int64_t get_timeout_sec() const { return timeout_sec_; }
  int64_t get_definition_version() const { return definition_version_; }
  bool is_sync_mode_async() const { return sync_mode_async_; }
  uint64_t get_status_column_id() const { return status_column_id_; }
  uint64_t get_lease_column_id() const { return lease_column_id_; }
  uint64_t get_retry_column_id() const { return retry_column_id_; }

  void set_table_id(const uint64_t table_id) { table_id_ = table_id; }
  void set_column_id(const uint64_t column_id) { column_id_ = column_id; }
  int set_column_name(const common::ObString &column_name) { return common::ob_write_string(*allocator_, column_name, column_name_); }
  void set_func_type(const share::EndpointType::TYPE type) { func_type_ = type; }
  int set_model_name(const common::ObString &model_name) { return common::ob_write_string(*allocator_, model_name, model_name_); }
  int set_expr_str(const common::ObString &expr_str) { return common::ob_write_string(*allocator_, expr_str, expr_str_); }
  int set_prompt_hint(const common::ObString &hint) { return common::ob_write_string(*allocator_, hint, prompt_hint_); }
  void set_result_type(const common::ObObjType type) { result_type_ = type; }
  void set_vector_dimension(const int64_t dimension) { vector_dimension_ = dimension; }
  void set_max_retry(const int64_t max_retry) { max_retry_ = max_retry; }
  void set_timeout_sec(const int64_t timeout_sec) { timeout_sec_ = timeout_sec; }
  void set_definition_version(const int64_t version) { definition_version_ = version; }
  void set_sync_mode_async(const bool async) { sync_mode_async_ = async; }
  void set_status_column_id(const uint64_t column_id) { status_column_id_ = column_id; }
  void set_lease_column_id(const uint64_t column_id) { lease_column_id_ = column_id; }
  void set_retry_column_id(const uint64_t column_id) { retry_column_id_ = column_id; }
  void set_allocator(common::ObIAllocator *allocator) { allocator_ = allocator; }
  int set_depend_column_ids(const common::ObIArray<uint64_t> &column_ids);

  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(func_type), K_(model_name),
               K_(expr_str), K_(depend_column_ids), K_(result_type), K_(vector_dimension),
               K_(max_retry), K_(timeout_sec), K_(definition_version), K_(sync_mode_async),
               K_(status_column_id), K_(lease_column_id), K_(retry_column_id));

private:
  common::ObIAllocator *allocator_;
  uint64_t table_id_;
  uint64_t column_id_;
  common::ObString column_name_;
  share::EndpointType::TYPE func_type_;
  common::ObString model_name_;
  common::ObString expr_str_;
  common::ObString prompt_hint_;
  common::ObSEArray<uint64_t, 4> depend_column_ids_;
  common::ObObjType result_type_;
  int64_t vector_dimension_;
  int64_t max_retry_;
  int64_t timeout_sec_;
  int64_t definition_version_;
  bool sync_mode_async_;
  uint64_t status_column_id_;
  uint64_t lease_column_id_;
  uint64_t retry_column_id_;
};

} // namespace schema
} // namespace share
} // namespace oceanbase
