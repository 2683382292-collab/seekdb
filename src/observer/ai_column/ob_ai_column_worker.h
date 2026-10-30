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
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string.h"
#include "common/mysqlclient/ob_isql_client.h"

namespace oceanbase
{
namespace common
{
namespace sqlclient
{
class ObMySQLResult;
}
}
namespace share
{
namespace schema
{
class ObTableSchema;
class ObAiColumnSchema;
class ObDatabaseSchema;
class ObSchemaGetterGuard;
}
}
namespace observer
{
class ObInnerSQLConnection;

// One scheduler thread of the AI column background service.
//
// A round is: discover the rows whose AI column is pending, claim a few of them, compute
// the answers outside of any transaction and publish the results atomically. Everything
// goes through the inner SQL connection, so the worker runs exactly the statement a user
// would run and reuses the model plumbing of the ai_complete/ai_embed functions.
//
// The state machine lives in the hidden columns of the row itself:
//   status  0 PENDING / 1 PROCESSING / 2 READY / 3 FAILED
//   lease   PENDING or FAILED: earliest time the row may be claimed, in seconds since the
//           epoch. PROCESSING: the claim deadline, which doubles as the claim token.
//   retry   number of attempts already made
// A claim is a compare and set on (status, lease, retry), so exactly one worker wins a
// row. A publish is guarded by the same triple, so a result computed from values that
// have changed in the meantime is dropped instead of overwriting the newer state.
class ObAiColumnWorker
{
public:
  // How long a claimed row is protected from other workers, in seconds.
  static const int64_t LEASE_DURATION_SEC = 60;
  // How long a failed task waits before the next attempt, in seconds.
  static const int64_t RETRY_BACKOFF_SEC = 5;
  // Rows claimed per AI column per round.
  static const int64_t DISCOVER_LIMIT = 16;
  // Rows handled per round and per table.
  static const int64_t TASK_LIMIT = 64;
  // Sleep of the scheduler thread when a round found nothing to do, in microseconds.
  static const int64_t IDLE_INTERVAL_US = 1000L * 1000L;

  ObAiColumnWorker();
  ~ObAiColumnWorker();

  int init();
  void destroy();
  // One scheduling round. Returns OB_SUCCESS even when it skipped work because the
  // server is not ready yet, so that a startup race never kills the thread.
  int run_once();

private:
  // One pending AI column of one table.
  struct ObAiColumnTask
  {
    ObAiColumnTask()
      : table_schema_(NULL),
        ai_column_(NULL),
        database_name_(),
        table_name_(),
        result_column_name_(),
        status_column_name_(),
        lease_column_name_(),
        retry_column_name_()
    {}
    const share::schema::ObTableSchema *table_schema_;
    const share::schema::ObAiColumnSchema *ai_column_;
    common::ObString database_name_;
    common::ObString table_name_;
    // Names of the result column and of the three hidden state columns, derived once so
    // that the statement builders never rebuild them.
    common::ObString result_column_name_;
    common::ObString status_column_name_;
    common::ObString lease_column_name_;
    common::ObString retry_column_name_;
  };

  // One claimable row of one AI column.
  struct ObAiColumnRow
  {
    ObAiColumnRow() : row_filter_(), status_(0), lease_(0), retry_(0) {}
    // "`pk1` = 1 AND `pk2` = 'x'", the identity of the row within its table.
    common::ObString row_filter_;
    // The state observed by the discovery, used as the compare and set condition of the
    // claim and of the publish. Keeping the whole triple means a re-claim that happens
    // in the same wall clock second as the previous publish is still detected.
    int64_t status_;
    int64_t lease_;
    int64_t retry_;
  };

  // The whole round shares one schema guard and one allocator, which keeps the metadata
  // consistent with the schema version the SQL statements are planned against.
  int collect_tasks(share::schema::ObSchemaGetterGuard &guard,
                    common::ObIArray<ObAiColumnTask> &tasks);
  // Copies one definition into a task and derives the names of its columns.
  int build_task(const share::schema::ObTableSchema &table_schema,
                 const share::schema::ObDatabaseSchema &database_schema,
                 const share::schema::ObAiColumnSchema &ai_column,
                 ObAiColumnTask &task);
  int run_task(const ObAiColumnTask &task);
  // Builds "`db`.`table`".
  int build_table_name(const ObAiColumnTask &task, common::ObSqlString &sql) const;
  // Builds the expression the model is called with. The prompt hint chosen by the result
  // type at DDL time is prepended to the dependency values, and a NULL dependency turns
  // the whole expression into NULL instead of being sent to the model.
  int build_result_expr(const ObAiColumnTask &task, common::ObSqlString &expr);
  // Reads the claimable rows of one AI column together with their state.
  int discover_rows(const ObAiColumnTask &task,
                    const int64_t now_sec,
                    common::ObIArray<ObAiColumnRow> &rows);
  int claim_row(const ObAiColumnTask &task,
                const ObAiColumnRow &row,
                const int64_t claim_token,
                bool &claimed);
  int compute_answer(const ObAiColumnTask &task,
                     const common::ObString &row_filter,
                     common::ObString &answer,
                     bool &has_answer);
  int publish_result(const ObAiColumnTask &task,
                     const ObAiColumnRow &row,
                     const int64_t claim_token,
                     const common::ObString &literal,
                     const bool is_null);
  int publish_failure(const ObAiColumnTask &task,
                      const ObAiColumnRow &row,
                      const int64_t claim_token,
                      const int64_t now_sec);
  int get_connection(ObInnerSQLConnection *&conn);
  void reset_connection();
  int get_rowkey_column_ids(const ObAiColumnTask &task,
                            common::ObIArray<uint64_t> &column_ids) const;
  // Turns the rowkey values of the current row of res into the WHERE clause that
  // addresses that row again later on.
  int read_row_filter(const ObAiColumnTask &task,
                      const common::ObIArray<uint64_t> &rowkey_column_ids,
                      const common::sqlclient::ObMySQLResult &res,
                      common::ObString &row_filter);

private:
  bool is_inited_;
  common::ObArenaAllocator allocator_;
  common::sqlclient::ObISQLConnectionGuard conn_guard_;
  ObInnerSQLConnection *conn_;
};

} // namespace observer
} // namespace oceanbase
