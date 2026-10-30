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

#include <new>
#include "share/schema/ob_ai_column_mgr.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace share
{
namespace schema
{

namespace
{

int find_column_id(const ObTableSchema &table_schema, const char *prefix,
                   const ObString &column_name, uint64_t &column_id)
{
  int ret = OB_SUCCESS;
  char buf[OB_MAX_COLUMN_NAME_LENGTH + 32];
  int64_t pos = 0;
  const int64_t prefix_len = static_cast<int64_t>(STRLEN(prefix));
  column_id = OB_INVALID_ID;
  if (OB_UNLIKELY(prefix_len + column_name.length() >= static_cast<int64_t>(sizeof(buf)))) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("hidden column name is too long", KR(ret), K(column_name));
  } else if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%.*s%.*s",
                                     static_cast<int>(prefix_len), prefix,
                                     column_name.length(), column_name.ptr()))) {
    LOG_WARN("fail to build hidden column name", KR(ret));
  } else {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema(ObString(pos, buf));
    if (NULL != column) {
      column_id = column->get_column_id();
    }
  }
  return ret;
}

} // anonymous namespace

int ObAiColumnMgr::has_ai_column(const ObTableSchema &table_schema, bool &has_ai_column)
{
  int ret = OB_SUCCESS;
  has_ai_column = false;
  for (int64_t i = 0; OB_SUCC(ret) && !has_ai_column && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column schema is null", KR(ret), K(i), K(table_schema.get_table_id()));
    } else if (column->is_ai_column()) {
      has_ai_column = true;
    }
  }
  return ret;
}

int ObAiColumnMgr::resolve_hidden_columns(const ObTableSchema &table_schema,
                                          ObAiColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  uint64_t column_id = OB_INVALID_ID;
  if (OB_FAIL(find_column_id(table_schema, OB_AI_COLUMN_STATUS_PREFIX,
                             ai_column.get_column_name(), column_id))) {
    LOG_WARN("fail to resolve status column", KR(ret), K(ai_column));
  } else if (OB_INVALID_ID == column_id) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("status column of ai column is missing", KR(ret), K(ai_column));
  } else {
    ai_column.set_status_column_id(column_id);
    if (OB_FAIL(find_column_id(table_schema, OB_AI_COLUMN_LEASE_PREFIX,
                               ai_column.get_column_name(), column_id))) {
      LOG_WARN("fail to resolve lease column", KR(ret), K(ai_column));
    } else if (OB_INVALID_ID == column_id) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("lease column of ai column is missing", KR(ret), K(ai_column));
    } else {
      ai_column.set_lease_column_id(column_id);
      if (OB_FAIL(find_column_id(table_schema, OB_AI_COLUMN_RETRY_PREFIX,
                                 ai_column.get_column_name(), column_id))) {
        LOG_WARN("fail to resolve retry column", KR(ret), K(ai_column));
      } else if (OB_INVALID_ID == column_id) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("retry column of ai column is missing", KR(ret), K(ai_column));
      } else {
        ai_column.set_retry_column_id(column_id);
      }
    }
  }
  return ret;
}

int ObAiColumnMgr::parse_ai_column(common::ObIAllocator &allocator,
                                   const ObTableSchema &table_schema,
                                   const ObColumnSchemaV2 &column_schema,
                                   ObAiColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  ai_column.set_allocator(&allocator);
  ai_column.set_table_id(table_schema.get_table_id());
  ai_column.set_column_id(column_schema.get_column_id());
  ai_column.set_result_type(column_schema.get_data_type());
  if (OB_FAIL(ai_column.set_column_name(column_schema.get_column_name_str()))) {
    LOG_WARN("fail to set ai column name", KR(ret), K(column_schema));
  } else if (OB_FAIL(ai_column.deserialize(column_schema.get_ai_column_info()))) {
    LOG_WARN("fail to deserialize ai column definition", KR(ret), K(column_schema));
  } else if (OB_FAIL(resolve_hidden_columns(table_schema, ai_column))) {
    LOG_WARN("fail to resolve ai hidden columns", KR(ret), K(ai_column));
  } else {
    // A dependency column may have been dropped or renamed, the definition stays but
    // the table is not usable for generation any more. Report it instead of silently
    // generating against a wrong column.
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_column.get_depend_column_ids().count(); ++i) {
      const uint64_t depend_column_id = ai_column.get_depend_column_ids().at(i);
      if (NULL == table_schema.get_column_schema(depend_column_id)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("depend column of ai column is missing", KR(ret),
                 K(depend_column_id), K(ai_column));
      }
    }
  }
  return ret;
}

int ObAiColumnMgr::get_ai_columns(common::ObIAllocator &allocator,
                                  const ObTableSchema &table_schema,
                                  common::ObIArray<const ObAiColumnSchema *> &ai_columns)
{
  int ret = OB_SUCCESS;
  ai_columns.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column schema is null", KR(ret), K(i), K(table_schema.get_table_id()));
    } else if (!column->is_ai_column()) {
      // skip ordinary columns
    } else {
      void *buf = allocator.alloc(sizeof(ObAiColumnSchema));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to allocate ai column schema", KR(ret));
      } else {
        ObAiColumnSchema *ai_column = new (buf) ObAiColumnSchema(&allocator);
        if (OB_FAIL(parse_ai_column(allocator, table_schema, *column, *ai_column))) {
          LOG_WARN("fail to parse ai column", KR(ret), K(*column));
          ai_column->~ObAiColumnSchema();
        } else if (OB_FAIL(ai_columns.push_back(ai_column))) {
          LOG_WARN("fail to push back ai column", KR(ret));
          ai_column->~ObAiColumnSchema();
        }
      }
    }
  }
  return ret;
}

int ObAiColumnMgr::get_ai_column(common::ObIAllocator &allocator,
                                 const ObTableSchema &table_schema,
                                 const uint64_t column_id,
                                 const ObAiColumnSchema *&ai_column)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table_schema.get_column_schema(column_id);
  ai_column = NULL;
  if (OB_ISNULL(column)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("column schema is null", KR(ret), K(column_id), K(table_schema.get_table_id()));
  } else if (!column->is_ai_column()) {
    // not an ai column, return NULL without error
  } else {
    void *buf = allocator.alloc(sizeof(ObAiColumnSchema));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate ai column schema", KR(ret));
    } else {
      ObAiColumnSchema *tmp = new (buf) ObAiColumnSchema(&allocator);
      if (OB_FAIL(parse_ai_column(allocator, table_schema, *column, *tmp))) {
        LOG_WARN("fail to parse ai column", KR(ret), K(*column));
        tmp->~ObAiColumnSchema();
      } else {
        ai_column = tmp;
      }
    }
  }
  return ret;
}

int ObAiColumnMgr::find_ai_column_by_hidden_column(common::ObIAllocator &allocator,
                                                   const ObTableSchema &table_schema,
                                                   const uint64_t hidden_column_id,
                                                   const ObAiColumnSchema *&ai_column)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObAiColumnSchema *, 4> ai_columns;
  ai_column = NULL;
  if (OB_FAIL(get_ai_columns(allocator, table_schema, ai_columns))) {
    LOG_WARN("fail to get ai columns", KR(ret), K(table_schema.get_table_id()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && NULL == ai_column && i < ai_columns.count(); ++i) {
    const ObAiColumnSchema *cur = ai_columns.at(i);
    if (OB_ISNULL(cur)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column is null", KR(ret), K(i));
    } else if (cur->get_status_column_id() == hidden_column_id
               || cur->get_lease_column_id() == hidden_column_id
               || cur->get_retry_column_id() == hidden_column_id) {
      ai_column = cur;
    }
  }
  return ret;
}

int ObAiColumnMgr::find_ai_column_by_name(common::ObIAllocator &allocator,
                                          const ObTableSchema &table_schema,
                                          const common::ObString &column_name,
                                          const ObAiColumnSchema *&ai_column)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table_schema.get_column_schema(column_name);
  ai_column = NULL;
  if (OB_ISNULL(column)) {
    ret = OB_ERR_KEY_COLUMN_DOES_NOT_EXITS;
    LOG_WARN("ai column does not exist", KR(ret), K(column_name), K(table_schema.get_table_id()));
  } else if (OB_FAIL(get_ai_column(allocator, table_schema, column->get_column_id(), ai_column))) {
    LOG_WARN("fail to get ai column", KR(ret), K(column_name));
  } else if (NULL == ai_column) {
    ret = OB_ERR_KEY_COLUMN_DOES_NOT_EXITS;
    LOG_WARN("column is not an ai column", KR(ret), K(column_name));
  }
  return ret;
}

int ObAiColumnMgr::build_status_column_name(common::ObIAllocator &allocator,
                                            const common::ObString &column_name,
                                            common::ObString &hidden_name)
{
  return ObAiColumnSchema::build_status_column_name(allocator, column_name, hidden_name);
}

} // namespace schema
} // namespace share
} // namespace oceanbase
