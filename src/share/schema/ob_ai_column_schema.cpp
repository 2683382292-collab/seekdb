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

#define USING_LOG_PREFIX SHARE_SCHEMA

#include "share/schema/ob_ai_column_schema.h"
#include "lib/utility/ob_print_utils.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace share
{
namespace schema
{

const char *const OB_AI_COLUMN_STATUS_PREFIX = "__ai_status_";
const char *const OB_AI_COLUMN_LEASE_PREFIX = "__ai_lease_";
const char *const OB_AI_COLUMN_RETRY_PREFIX = "__ai_retry_";
const char *const OB_AI_COLUMN_INFO_TAG = "AI_COLUMN_V1";

namespace
{

// The definition is persisted as a sequence of length prefixed entries
//   key=<byte length>,<raw bytes>
// so that an arbitrary model name, expression or prompt hint never needs escaping.
// 20 characters are always enough for the decimal length of a column value.
const int64_t MAX_LEN_DIGITS = 20;
// Upper bound of the "col_id,col_id,..." rendering of the dependency list.
const int64_t OB_AI_COLUMN_DEPEND_COLUMN_BUF_LEN = 4096;

int64_t entry_size(const char *key, const int64_t value_len)
{
  return static_cast<int64_t>(STRLEN(key)) + 1 + MAX_LEN_DIGITS + 1 + value_len;
}

int write_entry(char *buf, const int64_t buf_len, int64_t &pos,
                const char *key, const char *value, const int64_t value_len)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(databuff_printf(buf, buf_len, pos, "%s=%ld,", key, value_len))) {
    LOG_WARN("fail to write ai column info entry head", KR(ret), K(key), K(value_len));
  } else if (pos + value_len > buf_len) {
    ret = OB_BUF_NOT_ENOUGH;
    LOG_WARN("ai column info buffer is not enough", KR(ret), K(pos), K(value_len), K(buf_len));
  } else {
    if (value_len > 0) {
      MEMCPY(buf + pos, value, value_len);
      pos += value_len;
    }
  }
  return ret;
}

// Parse one "<key>=<len>,<bytes>" entry starting at pos.
int read_entry(const char *buf, const int64_t buf_len, int64_t &pos,
               common::ObString &key, common::ObString &value)
{
  int ret = OB_SUCCESS;
  key.reset();
  value.reset();
  int64_t key_start = pos;
  int64_t equal_pos = -1;
  int64_t comma_pos = -1;
  while (OB_SUCC(ret) && pos < buf_len && -1 == comma_pos) {
    if ('=' == buf[pos] && -1 == equal_pos) {
      equal_pos = pos;
      ++pos;
      int64_t value_len = 0;
      int64_t digits = 0;
      while (pos < buf_len && buf[pos] >= '0' && buf[pos] <= '9') {
        value_len = value_len * 10 + (buf[pos] - '0');
        ++digits;
        ++pos;
      }
      if (0 == digits) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("ai column info entry has no value length", KR(ret), K(key_start));
      } else if (pos >= buf_len || ',' != buf[pos]) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("ai column info entry has no value separator", KR(ret), K(pos));
      } else {
        ++pos;
        if (pos + value_len > buf_len) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("ai column info value overflows the buffer", KR(ret), K(pos), K(value_len));
        } else {
          key.assign_ptr(buf + key_start, static_cast<int32_t>(equal_pos - key_start));
          value.assign_ptr(buf + pos, static_cast<int32_t>(value_len));
          pos += value_len;
          comma_pos = pos;
        }
      }
    } else {
      ++pos;
    }
  }
  if (OB_SUCC(ret) && -1 == comma_pos) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai column info entry is truncated", KR(ret), K(key_start), K(buf_len));
  }
  return ret;
}

int parse_int64(const common::ObString &str, int64_t &value)
{
  int ret = OB_SUCCESS;
  int64_t result = 0;
  bool negative = false;
  int64_t i = 0;
  if (str.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty integer in ai column info", KR(ret));
  } else if ('-' == str[0]) {
    negative = true;
    i = 1;
  }
  for (; OB_SUCC(ret) && i < str.length(); ++i) {
    const char c = str[i];
    if (c < '0' || c > '9') {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("unexpected character in ai column info", KR(ret), K(str));
    } else {
      result = result * 10 + (c - '0');
    }
  }
  if (OB_SUCC(ret)) {
    value = negative ? -result : result;
  }
  return ret;
}

int parse_bool(const common::ObString &str, bool &value)
{
  int ret = OB_SUCCESS;
  int64_t int_value = 0;
  if (OB_FAIL(parse_int64(str, int_value))) {
    LOG_WARN("fail to parse bool", KR(ret), K(str));
  } else {
    value = (0 != int_value);
  }
  return ret;
}

int parse_depend_columns(const common::ObString &value, common::ObIArray<uint64_t> &column_ids)
{
  int ret = OB_SUCCESS;
  column_ids.reuse();
  int64_t start = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i <= value.length(); ++i) {
    if (i == value.length() || ',' == value[i]) {
      int64_t column_id = 0;
      const common::ObString token(value.length() - start, value.ptr() + start);
      if (OB_FAIL(parse_int64(token, column_id))) {
        LOG_WARN("fail to parse depend column id", KR(ret), K(token));
      } else if (OB_FAIL(column_ids.push_back(static_cast<uint64_t>(column_id)))) {
        LOG_WARN("fail to push back depend column id", KR(ret), K(column_id));
      }
      start = i + 1;
    }
  }
  return ret;
}

int build_depend_columns(const common::ObIArray<uint64_t> &column_ids,
                        char *buf, const int64_t buf_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
    if (OB_FAIL(databuff_printf(buf, buf_len, pos, "%s%lu",
                                (0 == i) ? "" : ",", column_ids.at(i)))) {
      LOG_WARN("fail to format depend column id", KR(ret), K(i));
    }
  }
  return ret;
}

} // anonymous namespace

const char *get_ai_column_status_str(const int64_t status)
{
  const char *str = "PENDING";
  switch (status) {
    case AI_COLUMN_STATUS_PENDING:
      str = "PENDING";
      break;
    case AI_COLUMN_STATUS_PROCESSING:
      str = "PROCESSING";
      break;
    case AI_COLUMN_STATUS_READY:
      str = "READY";
      break;
    case AI_COLUMN_STATUS_FAILED:
      str = "FAILED";
      break;
    default:
      str = "UNKNOWN";
      break;
  }
  return str;
}

ObAiColumnSchema::ObAiColumnSchema()
  : allocator_(nullptr),
    table_id_(OB_INVALID_ID),
    column_id_(OB_INVALID_ID),
    column_name_(),
    func_type_(share::EndpointType::MAX_TYPE),
    model_name_(),
    expr_str_(),
    prompt_hint_(),
    depend_column_ids_(),
    result_type_(ObMaxType),
    vector_dimension_(0),
    max_retry_(3),
    timeout_sec_(30),
    definition_version_(1),
    sync_mode_async_(true),
    status_column_id_(OB_INVALID_ID),
    lease_column_id_(OB_INVALID_ID),
    retry_column_id_(OB_INVALID_ID)
{
}

ObAiColumnSchema::ObAiColumnSchema(common::ObIAllocator *allocator)
  : ObAiColumnSchema()
{
  allocator_ = allocator;
}

ObAiColumnSchema &ObAiColumnSchema::operator=(const ObAiColumnSchema &other)
{
  if (this != &other) {
    int ret = OB_SUCCESS;
    if (OB_SUCCESS != (ret = assign(other))) {
      LOG_WARN("fail to assign ai column schema", KR(ret), K(other));
      reset();
    }
  }
  return *this;
}

void ObAiColumnSchema::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  column_name_.reset();
  func_type_ = share::EndpointType::MAX_TYPE;
  model_name_.reset();
  expr_str_.reset();
  prompt_hint_.reset();
  depend_column_ids_.reset();
  result_type_ = ObMaxType;
  vector_dimension_ = 0;
  max_retry_ = 3;
  timeout_sec_ = 30;
  definition_version_ = 1;
  sync_mode_async_ = true;
  status_column_id_ = OB_INVALID_ID;
  lease_column_id_ = OB_INVALID_ID;
  retry_column_id_ = OB_INVALID_ID;
}

int ObAiColumnSchema::assign(const ObAiColumnSchema &other)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("allocator is null", KR(ret));
  } else if (OB_FAIL(ob_write_string(*allocator_, other.column_name_, column_name_))) {
    LOG_WARN("fail to copy column name", KR(ret));
  } else if (OB_FAIL(ob_write_string(*allocator_, other.model_name_, model_name_))) {
    LOG_WARN("fail to copy model name", KR(ret));
  } else if (OB_FAIL(ob_write_string(*allocator_, other.expr_str_, expr_str_))) {
    LOG_WARN("fail to copy expr string", KR(ret));
  } else if (OB_FAIL(ob_write_string(*allocator_, other.prompt_hint_, prompt_hint_))) {
    LOG_WARN("fail to copy prompt hint", KR(ret));
  } else {
    table_id_ = other.table_id_;
    column_id_ = other.column_id_;
    func_type_ = other.func_type_;
    result_type_ = other.result_type_;
    vector_dimension_ = other.vector_dimension_;
    max_retry_ = other.max_retry_;
    timeout_sec_ = other.timeout_sec_;
    definition_version_ = other.definition_version_;
    sync_mode_async_ = other.sync_mode_async_;
    status_column_id_ = other.status_column_id_;
    lease_column_id_ = other.lease_column_id_;
    retry_column_id_ = other.retry_column_id_;
    if (OB_FAIL(set_depend_column_ids(other.depend_column_ids_))) {
      LOG_WARN("fail to copy depend column ids", KR(ret));
    }
  }
  return ret;
}

int ObAiColumnSchema::deep_copy(common::ObIAllocator &allocator, const ObAiColumnSchema &other)
{
  int ret = OB_SUCCESS;
  allocator_ = &allocator;
  if (OB_FAIL(assign(other))) {
    LOG_WARN("fail to assign ai column schema", KR(ret));
  }
  return ret;
}

int64_t ObAiColumnSchema::get_convert_size() const
{
  int64_t size = static_cast<int64_t>(sizeof(ObAiColumnSchema));
  size += column_name_.length();
  size += model_name_.length();
  size += expr_str_.length();
  size += prompt_hint_.length();
  size += depend_column_ids_.count() * static_cast<int64_t>(sizeof(uint64_t));
  return size;
}

int ObAiColumnSchema::check_valid() const
{
  int ret = OB_SUCCESS;
  if (!is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid ai column schema", KR(ret), K(*this));
  } else if (share::EndpointType::MAX_TYPE == func_type_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai column function type is not set", KR(ret), K(*this));
  } else if (!sync_mode_async_) {
    // This version only supports asynchronous generation, a synchronous AI column
    // would block the DML statement on a remote model call.
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("ai column only supports sync_mode='async'", KR(ret), K(*this));
  } else if (model_name_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai column model name is empty", KR(ret), K(*this));
  } else if (expr_str_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai column expression is empty", KR(ret), K(*this));
  } else if (depend_column_ids_.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai column has no depend column", KR(ret), K(*this));
  } else if (OB_FAIL(check_result_type(result_type_, vector_dimension_))) {
    LOG_WARN("ai column result type is invalid", KR(ret), K(*this));
  }
  return ret;
}

int ObAiColumnSchema::check_result_type(const ObObjType result_type,
                                        const int64_t vector_dimension) const
{
  int ret = OB_SUCCESS;
  if (share::EndpointType::DENSE_EMBEDDING == func_type_) {
    if (vector_dimension <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("embedding ai column requires a positive vector dimension", KR(ret),
               K(vector_dimension), K(result_type));
    } else if (!ob_is_collection_sql_type(result_type)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("embedding ai column requires a vector result column", KR(ret), K(result_type));
    }
  } else if (share::EndpointType::COMPLETION == func_type_) {
    if (vector_dimension > 0 || ob_is_collection_sql_type(result_type)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("completion ai column can not use a vector result column", KR(ret),
               K(vector_dimension), K(result_type));
    } else if (!ob_is_string_type(result_type) && !ob_is_json(result_type)
               && !ob_is_integer_type(result_type) && !ob_is_number_or_decimal_int_tc(result_type)
               && !ob_is_datetime_tc(result_type) && !ob_is_date_tc(result_type)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("unsupported ai column result type", KR(ret), K(result_type));
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("only completion and embedding ai columns are supported", KR(ret), K_(func_type));
  }
  return ret;
}

int ObAiColumnSchema::add_depend_column_id(const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  bool found = false;
  for (int64_t i = 0; !found && i < depend_column_ids_.count(); ++i) {
    if (depend_column_ids_.at(i) == column_id) {
      found = true;
    }
  }
  if (!found) {
    if (OB_FAIL(depend_column_ids_.push_back(column_id))) {
      LOG_WARN("fail to push back depend column id", KR(ret), K(column_id));
    }
  }
  return ret;
}

bool ObAiColumnSchema::depend_on(const uint64_t column_id) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < depend_column_ids_.count(); ++i) {
    if (depend_column_ids_.at(i) == column_id) {
      found = true;
    }
  }
  return found;
}

int ObAiColumnSchema::set_depend_column_ids(const common::ObIArray<uint64_t> &column_ids)
{
  int ret = OB_SUCCESS;
  depend_column_ids_.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
    if (OB_FAIL(add_depend_column_id(column_ids.at(i)))) {
      LOG_WARN("fail to add depend column id", KR(ret), K(i));
    }
  }
  return ret;
}

int ObAiColumnSchema::build_hidden_column_name(common::ObIAllocator &allocator,
                                               const char *prefix,
                                               const common::ObString &column_name,
                                               common::ObString &hidden_name)
{
  int ret = OB_SUCCESS;
  char buf[OB_MAX_COLUMN_NAME_LENGTH + 32];
  int64_t pos = 0;
  const int64_t prefix_len = static_cast<int64_t>(STRLEN(prefix));
  hidden_name.reset();
  if (OB_ISNULL(prefix) || column_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(column_name));
  } else if (prefix_len + column_name.length() >= static_cast<int64_t>(sizeof(buf))) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("hidden column name is too long", KR(ret), K(prefix), K(column_name));
  } else if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%.*s%.*s",
                                     static_cast<int>(prefix_len), prefix,
                                     column_name.length(), column_name.ptr()))) {
    LOG_WARN("fail to build hidden column name", KR(ret));
  } else if (OB_FAIL(ob_write_string(allocator, ObString(static_cast<int32_t>(pos), buf), hidden_name))) {
    LOG_WARN("fail to copy hidden column name", KR(ret));
  }
  return ret;
}

int ObAiColumnSchema::build_status_column_name(common::ObIAllocator &allocator,
                                               const common::ObString &column_name,
                                               common::ObString &hidden_name)
{
  return build_hidden_column_name(allocator, OB_AI_COLUMN_STATUS_PREFIX, column_name, hidden_name);
}

int ObAiColumnSchema::build_lease_column_name(common::ObIAllocator &allocator,
                                              const common::ObString &column_name,
                                              common::ObString &hidden_name)
{
  return build_hidden_column_name(allocator, OB_AI_COLUMN_LEASE_PREFIX, column_name, hidden_name);
}

int ObAiColumnSchema::build_retry_column_name(common::ObIAllocator &allocator,
                                              const common::ObString &column_name,
                                              common::ObString &hidden_name)
{
  return build_hidden_column_name(allocator, OB_AI_COLUMN_RETRY_PREFIX, column_name, hidden_name);
}

int ObAiColumnSchema::serialize(common::ObIAllocator &allocator,
                                common::ObString &info) const
{
  int ret = OB_SUCCESS;
  info.reset();
  char dep_buf[OB_AI_COLUMN_DEPEND_COLUMN_BUF_LEN];
  int64_t dep_len = 0;
  if (OB_FAIL(build_depend_columns(depend_column_ids_, dep_buf, sizeof(dep_buf), dep_len))) {
    LOG_WARN("fail to build depend column id list", KR(ret));
  } else {
    const int64_t total = entry_size("tag", static_cast<int64_t>(STRLEN(OB_AI_COLUMN_INFO_TAG)))
                        + entry_size("ft", MAX_LEN_DIGITS)
                        + entry_size("mn", model_name_.length())
                        + entry_size("es", expr_str_.length())
                        + entry_size("ph", prompt_hint_.length())
                        + entry_size("dc", dep_len)
                        + entry_size("rt", MAX_LEN_DIGITS)
                        + entry_size("vd", MAX_LEN_DIGITS)
                        + entry_size("mr", MAX_LEN_DIGITS)
                        + entry_size("ts", MAX_LEN_DIGITS)
                        + entry_size("dv", MAX_LEN_DIGITS)
                        + entry_size("sa", MAX_LEN_DIGITS);
    char *buf = static_cast<char *>(allocator.alloc(total));
    int64_t pos = 0;
    char num_buf[MAX_LEN_DIGITS + 1];
    int64_t num_pos = 0;
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate ai column info buffer", KR(ret), K(total));
    } else if (OB_FAIL(write_entry(buf, total, pos, "tag", OB_AI_COLUMN_INFO_TAG,
                                   static_cast<int64_t>(STRLEN(OB_AI_COLUMN_INFO_TAG))))) {
    } else {
      // text fields first, then the numeric ones
      if (OB_SUCC(ret)) {
        num_pos = 0;
        if (OB_FAIL(databuff_printf(num_buf, sizeof(num_buf), num_pos, "%ld",
                                    static_cast<int64_t>(func_type_)))) {
        } else {
          ret = write_entry(buf, total, pos, "ft", num_buf, num_pos);
        }
      }
      if (OB_SUCC(ret)) {
        ret = write_entry(buf, total, pos, "mn", model_name_.ptr(), model_name_.length());
      }
      if (OB_SUCC(ret)) {
        ret = write_entry(buf, total, pos, "es", expr_str_.ptr(), expr_str_.length());
      }
      if (OB_SUCC(ret)) {
        ret = write_entry(buf, total, pos, "ph", prompt_hint_.ptr(), prompt_hint_.length());
      }
      if (OB_SUCC(ret)) {
        ret = write_entry(buf, total, pos, "dc", dep_buf, dep_len);
      }
      const int64_t numbers[] = {static_cast<int64_t>(result_type_), vector_dimension_,
                                 max_retry_, timeout_sec_, definition_version_,
                                 sync_mode_async_ ? 1 : 0};
      const char *keys[] = {"rt", "vd", "mr", "ts", "dv", "sa"};
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(ARRAYSIZEOF(keys)); ++i) {
        num_pos = 0;
        if (OB_FAIL(databuff_printf(num_buf, sizeof(num_buf), num_pos, "%ld", numbers[i]))) {
          LOG_WARN("fail to format ai column info number", KR(ret), K(i));
        } else {
          ret = write_entry(buf, total, pos, keys[i], num_buf, num_pos);
        }
      }
      if (OB_SUCC(ret)) {
        info.assign_ptr(buf, static_cast<int32_t>(pos));
      }
    }
  }
  return ret;
}

bool ObAiColumnSchema::is_ai_column_info(const common::ObString &info)
{
  bool is_ai = false;
  common::ObString key;
  common::ObString value;
  int64_t pos = 0;
  if (!info.empty() && OB_SUCCESS == read_entry(info.ptr(), info.length(), pos, key, value)) {
    is_ai = (key == common::ObString(static_cast<int32_t>(STRLEN("tag")), "tag"))
            && (value == common::ObString(static_cast<int32_t>(STRLEN(OB_AI_COLUMN_INFO_TAG)),
                                          OB_AI_COLUMN_INFO_TAG));
  }
  return is_ai;
}

int ObAiColumnSchema::deserialize(const common::ObString &info)
{
  int ret = OB_SUCCESS;
  int64_t int_value = 0;
  common::ObString key;
  common::ObString value;
  if (OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("allocator is not set", KR(ret));
  } else if (!is_ai_column_info(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("not an ai column definition", KR(ret), K(info));
  }
  depend_column_ids_.reuse();
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < info.length()) {
    if (OB_FAIL(read_entry(info.ptr(), info.length(), pos, key, value))) {
      LOG_WARN("fail to read ai column info entry", KR(ret), K(pos));
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("tag")), "tag")) {
      // version tag, nothing to restore
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("ft")), "ft")) {
      if (OB_FAIL(parse_int64(value, int_value))) {
        LOG_WARN("fail to parse func type", KR(ret));
      } else {
        func_type_ = share::EndpointType::convert_type_from_int(int_value);
      }
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("mn")), "mn")) {
      ret = ob_write_string(*allocator_, value, model_name_);
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("es")), "es")) {
      ret = ob_write_string(*allocator_, value, expr_str_);
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("ph")), "ph")) {
      ret = ob_write_string(*allocator_, value, prompt_hint_);
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("dc")), "dc")) {
      ret = parse_depend_columns(value, depend_column_ids_);
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("rt")), "rt")) {
      if (OB_SUCC(parse_int64(value, int_value))) {
        result_type_ = static_cast<ObObjType>(int_value);
      }
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("vd")), "vd")) {
      if (OB_SUCC(parse_int64(value, int_value))) {
        vector_dimension_ = int_value;
      }
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("mr")), "mr")) {
      if (OB_SUCC(parse_int64(value, int_value))) {
        max_retry_ = int_value;
      }
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("ts")), "ts")) {
      if (OB_SUCC(parse_int64(value, int_value))) {
        timeout_sec_ = int_value;
      }
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("dv")), "dv")) {
      if (OB_SUCC(parse_int64(value, int_value))) {
        definition_version_ = int_value;
      }
    } else if (key == common::ObString(static_cast<int32_t>(STRLEN("sa")), "sa")) {
      if (OB_SUCC(parse_bool(value, sync_mode_async_))) {
      }
    } else {
      // forward compatible: unknown keys are ignored
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
