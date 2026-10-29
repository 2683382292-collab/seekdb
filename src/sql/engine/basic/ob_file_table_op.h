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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_OP_H_

#include "lib/allocator/page_arena.h"
#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan.h"

namespace oceanbase
{
namespace sql
{

class ObFileTableSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
      : ObOpSpec(alloc, type),
        kind_(0),
        header_(true),
        file_size_(-1),
        mtime_sec_(-1),
        path_(),
        col_names_(alloc),
        col_kinds_(alloc),
        col_nullable_(alloc),
        column_exprs_(alloc),
        column_ids_(alloc)
  {}

  int64_t kind_;
  bool header_;
  int64_t file_size_;
  int64_t mtime_sec_;
  common::ObString path_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> col_names_;
  common::ObFixedArray<int64_t, common::ObIAllocator> col_kinds_;
  common::ObFixedArray<int64_t, common::ObIAllocator> col_nullable_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_ids_;
};

class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
      : ObOperator(exec_ctx, spec, input),
        def_(),
        cursor_(),
        row_arena_(common::ObModIds::OB_SQL_EXPR)
  {}
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  int open_cursor();
  ObFileTableDef def_;
  ObFileRowCursor cursor_;
  common::ObArenaAllocator row_arena_;
};

} // namespace sql
} // namespace oceanbase

#endif
