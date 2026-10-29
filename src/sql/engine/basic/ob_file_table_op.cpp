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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_table_op.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFileTableSpec, ObOpSpec),
                    kind_, header_, file_size_, mtime_sec_, path_,
                    col_names_, col_kinds_, col_nullable_,
                    column_exprs_, column_ids_);

int ObFileTableOp::open_cursor()
{
  int ret = OB_SUCCESS;
  const ObFileTableSpec &spec = static_cast<const ObFileTableSpec &>(spec_);
  def_.kind_ = static_cast<int>(spec.kind_);
  def_.header_ = spec.header_;
  def_.sample_size_ = -1;
  def_.file_size_ = spec.file_size_;
  def_.mtime_sec_ = spec.mtime_sec_;
  def_.path_ = spec.path_;
  def_.col_names_.reset();
  def_.col_kinds_.reset();
  def_.col_nullable_.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < spec.col_names_.count(); ++i) {
    if (OB_FAIL(def_.col_names_.push_back(spec.col_names_.at(i)))) {
    } else if (OB_FAIL(def_.col_kinds_.push_back(spec.col_kinds_.at(i)))) {
    } else if (OB_FAIL(def_.col_nullable_.push_back(static_cast<int8_t>(spec.col_nullable_.at(i))))) {
    }
  }
  if (OB_SUCC(ret)) {
    cursor_.close();
    cursor_.project_all();
    bool any = false;
    for (int64_t i = 0; i < spec.column_ids_.count(); ++i) {
      int64_t idx = spec.column_ids_.at(i) - OB_APP_MIN_COLUMN_ID;
      cursor_.project_column(idx);
      any = true;
    }
    if (!any && OB_FILE_SQL_PARQUET == def_.kind_) {
      cursor_.set_count_only(true);
    }
    bool skip_header = def_.header_ && OB_FILE_SQL_CSV == def_.kind_;
    if (OB_FAIL(cursor_.open(def_, skip_header))) {
      LOG_WARN("open file table cursor failed", K(ret), K(def_.path_));
    }
  }
  return ret;
}

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObFileScan::check_enabled(ctx_.get_my_session()))) {
    LOG_WARN("file sql is disabled", K(ret));
  } else if (OB_FAIL(open_cursor())) {
  }
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (OB_FAIL(ObFileScan::check_enabled(ctx_.get_my_session()))) {
  } else if (OB_FAIL(open_cursor())) {
  }
  return ret;
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  const ObFileTableSpec &spec = static_cast<const ObFileTableSpec &>(spec_);
  ObSEArray<ObObj, 16> row;
  clear_evaluated_flag();
  row_arena_.reuse();
  if (OB_FAIL(cursor_.next_objs(row, row_arena_))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("file table next row failed", K(ret));
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_exprs_.count(); ++i) {
      ObExpr *expr = spec.column_exprs_.at(i);
      int64_t idx = spec.column_ids_.at(i) - OB_APP_MIN_COLUMN_ID;
      if (OB_ISNULL(expr) || idx < 0 || idx >= row.count()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const ObObjDatumMapType &datum_map = expr->obj_datum_map_;
        ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
        if (OB_FAIL(datum.from_obj(row.at(idx), datum_map))) {
          LOG_WARN("file table datum conversion failed", K(ret), K(idx));
        } else {
          expr->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  cursor_.close();
  row_arena_.reset();
  return OB_SUCCESS;
}

void ObFileTableOp::destroy()
{
  cursor_.close();
  row_arena_.reset();
  ObOperator::destroy();
}

} // namespace sql
} // namespace oceanbase
