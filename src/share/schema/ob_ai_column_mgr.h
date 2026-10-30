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

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_array.h"
#include "share/schema/ob_ai_column_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

class ObTableSchema;
class ObColumnSchemaV2;

// Registry of the AI column definitions of one table.
//
// The registry deliberately keeps no global "loaded" state. Every answer is derived
// from the table schema the caller already holds, which is the authoritative and
// always available source: the definition is persisted in __all_column.ai_column_info
// and the AI column is marked by AI_COLUMN_FLAG.
// An earlier design cached a global "table has ai column" map that started empty,
// so after a restart every table looked like it had no AI column and the DML hook
// never recorded a row. Answering from the table schema makes that bug impossible,
// at the cost of a cheap flag scan on the hot path.
class ObAiColumnMgr final
{
public:
  // Cheap flag scan, no allocation. Returns false only when the table really has no
  // AI column at the schema version the caller passed in.
  static int has_ai_column(const ObTableSchema &table_schema, bool &has_ai_column);
  // Parse every AI column definition of the table. The returned objects are owned by
  // the caller provided allocator and point into it, so they must not outlive it.
  static int get_ai_columns(common::ObIAllocator &allocator,
                            const ObTableSchema &table_schema,
                            common::ObIArray<const ObAiColumnSchema *> &ai_columns);
  static int get_ai_column(common::ObIAllocator &allocator,
                           const ObTableSchema &table_schema,
                           const uint64_t column_id,
                           const ObAiColumnSchema *&ai_column);
  // Reverse lookup used by the background service: a claimed row only names the
  // hidden status/lease/retry column, it has to be mapped back to its AI column.
  static int find_ai_column_by_hidden_column(common::ObIAllocator &allocator,
                                             const ObTableSchema &table_schema,
                                             const uint64_t hidden_column_id,
                                             const ObAiColumnSchema *&ai_column);
  static int find_ai_column_by_name(common::ObIAllocator &allocator,
                                    const ObTableSchema &table_schema,
                                    const common::ObString &column_name,
                                    const ObAiColumnSchema *&ai_column);

  // Resolve the three hidden column ids of one AI column from the table schema.
  static int resolve_hidden_columns(const ObTableSchema &table_schema,
                                    ObAiColumnSchema &ai_column);
  // Names of the hidden columns and of the result column, used to build the SQL that
  // the background service runs.
  static int build_status_column_name(common::ObIAllocator &allocator,
                                      const common::ObString &column_name,
                                      common::ObString &hidden_name);

private:
  static int parse_ai_column(common::ObIAllocator &allocator,
                             const ObTableSchema &table_schema,
                             const ObColumnSchemaV2 &column_schema,
                             ObAiColumnSchema &ai_column);
};

} // namespace schema
} // namespace share
} // namespace oceanbase
