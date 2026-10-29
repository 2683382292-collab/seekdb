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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;

enum ObFileSqlKind
{
  OB_FILE_SQL_CSV = 1,
  OB_FILE_SQL_JSONL = 2,
  OB_FILE_SQL_PARQUET = 3,
  OB_FILE_SQL_GLOB = 4,
};

enum ObFileColKind
{
  OB_FILE_COL_VARCHAR = 0,
  OB_FILE_COL_BIGINT = 1,
  OB_FILE_COL_DOUBLE = 2,
  OB_FILE_COL_BOOL = 3,
  OB_FILE_COL_DATE = 4,
  OB_FILE_COL_DATETIME = 5,
};

struct ObFileTableDef
{
  ObFileTableDef()
      : kind_(OB_FILE_SQL_CSV),
        header_(true),
        sample_size_(-1),
        file_size_(-1),
        mtime_sec_(-1)
  {}
  int kind_;
  common::ObString path_;
  bool header_;
  int64_t sample_size_;
  int64_t file_size_;
  int64_t mtime_sec_;
  common::ObSEArray<common::ObString, 8> col_names_;
  common::ObSEArray<int64_t, 8> col_kinds_;
  common::ObSEArray<int8_t, 8> col_nullable_;
  int deep_copy(const ObFileTableDef &other, common::ObIAllocator &allocator);
  const char *kind_name() const;
};

class ObFileRowCursor
{
public:
  ObFileRowCursor();
  ~ObFileRowCursor();
  // skip_header is set by execution. Schema inference must see the header row.
  int open(const ObFileTableDef &def, bool skip_header = false);
  void project_all();
  void project_column(int64_t idx);
  void set_count_only(bool value)
  {
    count_only_ = value;
    if (value) {
      project_all_ = false;
    }
  }
  int next(common::ObIArray<common::ObString> &fields);
  int next_objs(common::ObIArray<common::ObObj> &row, common::ObIAllocator &alloc);
  void close();
  bool is_parquet() const { return parquet_; }
  bool field_quoted(int64_t idx) const;

private:
  int open_text();
  int fill();
  int next_csv(common::ObIArray<common::ObString> &fields);
  int next_json(common::ObIArray<common::ObString> &fields);
  int next_glob(common::ObIArray<common::ObString> &fields);
  int next_parquet(common::ObIArray<common::ObObj> &row, common::ObIAllocator &alloc);
  int parse_csv_row(common::ObIArray<common::ObString> &fields, bool &got);
  int ensure_row(int64_t need);
  int ensure_decode(int64_t need);
  int own_field(const common::ObString &src, common::ObString &dst);
  void free_owned();
  bool column_projected(int64_t idx) const;
  int push_quoted(bool quoted);
  int json_value(const char *s, const char *end, const char *&out, common::ObString &text, int &json_type);

  const ObFileTableDef *def_;
  int fd_;
  char *buf_;
  int64_t buf_cap_;
  int64_t buf_len_;
  int64_t buf_pos_;
  bool eof_;
  char *row_buf_;
  int64_t row_cap_;
  int64_t row_len_;
  char *decode_buf_;
  int64_t decode_cap_;
  int64_t decode_len_;
  int64_t line_no_;
  bool header_skipped_;
  bool skip_header_;
  bool parquet_;
  bool project_all_;
  bool count_only_;
  uint64_t project_words_[64];
  int8_t *quoted_;
  int64_t quoted_cap_;
  int64_t quoted_len_;
  char **owned_;
  int owned_n_;
  int owned_cap_;
  void *parquet_state_;
  char **glob_paths_;
  int glob_count_;
  int glob_idx_;
  char *glob_root_;
};

class ObFileScan
{
public:
  static int check_enabled(ObSQLSessionInfo *session);
  static int resolve_real_path(ObSQLSessionInfo *session,
                               const common::ObString &path,
                               common::ObIAllocator &allocator,
                               common::ObString &real_path,
                               int64_t &file_size,
                               int64_t &mtime_sec,
                               bool allow_missing_file);
  static int infer(ObSQLSessionInfo *session,
                   common::ObIAllocator &allocator,
                   int kind,
                   const common::ObString &path,
                   bool header,
                   int64_t sample_size,
                   ObFileTableDef &def);
  static common::ObObjType obj_type(int64_t kind);
  static const char *type_name(int64_t kind);
};

} // namespace sql
} // namespace oceanbase

#endif
