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

#ifndef OCEANBASE_SQL_ENGINE_CMD_OB_AI_COLUMN_SERVICE_H_
#define OCEANBASE_SQL_ENGINE_CMD_OB_AI_COLUMN_SERVICE_H_

#include "lib/ob_define.h"
#include "sql/parser/parse_node.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
class ObString;
}
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;
}
}
namespace sql
{
class ObSQLSessionInfo;
class ObAlterTableStmt;
class ObTableModifyOp;

bool ob_table_has_ai_column(const uint64_t table_id);

class ObAIColumnService
{
public:
  static void clear_staged();
  static int stage_column(const share::schema::ObColumnSchemaV2 &column, ParseNode *attr);
  // MODIFY COLUMN stages an AI definition just like ADD COLUMN, but re-declaring an
  // AI column in place is not supported, so the staged definition must be discarded
  // instead of leaking into the next DDL that consumes the staged buffer.
  static int reject_staged();
  static int append_hidden_columns(share::schema::ObTableSchema &schema,
                                   const common::ObString &database_name,
                                   const common::ObString &table_name,
                                   uint64_t &next_column_id);
  static int append_hidden_alter(ObAlterTableStmt &stmt,
                                 const common::ObString &database_name,
                                 const common::ObString &table_name,
                                 uint64_t table_id);
  static int persist_table(ObSQLSessionInfo *session, uint64_t table_id);
  static int on_after_dml(ObTableModifyOp *op, ObDMLModifyRowsList *rows);
  static int prepare_retry(common::ObIAllocator &allocator,
                           uint64_t table_id,
                           ParseNode *column_node,
                           ParseNode *where_node,
                           common::ObString &column_name,
                           common::ObString &where_sql);
  static int retry(ObSQLSessionInfo *session,
                   uint64_t table_id,
                   const common::ObString &column_name,
                   const common::ObString &where_sql);
  static int strict_enabled(ObSQLSessionInfo *session, bool &enabled);
  static int check_strict_column(ObSQLSessionInfo *session,
                                 uint64_t table_id,
                                 uint64_t column_id);

  template <typename Array>
  static int check_strict_read(ObSQLSessionInfo *session,
                               uint64_t table_id,
                               const Array &column_ids)
  {
    int ret = common::OB_SUCCESS;
    bool enabled = false;
    if (OB_FAIL(strict_enabled(session, enabled))) {
    } else if (enabled) {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
        if (OB_FAIL(check_strict_column(session, table_id, column_ids.at(i)))) {
        }
      }
    }
    return ret;
  }
};

} // namespace sql
} // namespace oceanbase

#endif
