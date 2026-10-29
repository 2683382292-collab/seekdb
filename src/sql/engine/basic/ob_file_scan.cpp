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

#include "sql/engine/basic/ob_file_scan.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_sql_string.h"
#include "lib/timezone/ob_time_convert.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_basic_session_info.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{
const int64_t OB_FILE_SQL_MAX_COLUMNS = 4096;
const int64_t OB_FILE_SQL_MAX_FIELD = 8 * 1024 * 1024;
const int64_t OB_FILE_SQL_READ_CHUNK = 64 * 1024;

int copy_string(ObIAllocator &allocator, const ObString &src, ObString &dst)
{
  return ob_write_string(allocator, src, dst, true);
}

bool name_equal(const ObString &left, const ObString &right)
{
  return left.case_compare(right) == 0;
}

int append_unique_name(ObIAllocator &allocator, ObIArray<ObString> &names, const ObString &raw)
{
  int ret = OB_SUCCESS;
  ObString name = raw;
  bool clash = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    if (name_equal(names.at(i), name)) {
      clash = true;
    }
  }
  if (OB_SUCC(ret) && clash) {
    bool found = false;
    for (int n = 2; OB_SUCC(ret) && !found && n < 100000; ++n) {
      char buf[256];
      int len = snprintf(buf, sizeof(buf), "%.*s__%d", raw.length(), raw.ptr(), n);
      if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        ObString candidate(len, buf);
        bool used = false;
        for (int64_t i = 0; i < names.count(); ++i) {
          if (name_equal(names.at(i), candidate)) {
            used = true;
          }
        }
        if (!used && OB_FAIL(copy_string(allocator, candidate, name))) {
        } else if (!used) {
          found = true;
        }
      }
    }
    if (OB_SUCC(ret) && !found) {
      ret = OB_SIZE_OVERFLOW;
    }
  } else if (OB_SUCC(ret) && OB_FAIL(copy_string(allocator, raw, name))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(names.push_back(name))) {
  }
  return ret;
}

bool is_leap(int year)
{
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

bool valid_date(int year, int month, int day)
{
  static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 1 || year > 9999 || month < 1 || month > 12) {
    return false;
  }
  int max_day = days[month];
  if (month == 2 && is_leap(year)) {
    max_day = 29;
  }
  return day >= 1 && day <= max_day;
}

bool parse_date_parts(const ObString &text, int &year, int &month, int &day, int &time_pos)
{
  if (text.length() < 10) {
    return false;
  }
  const char *s = text.ptr();
  for (int i = 0; i < 10; ++i) {
    if ((i == 4 || i == 7) ? s[i] != '-' : !isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  month = (s[5] - '0') * 10 + (s[6] - '0');
  day = (s[8] - '0') * 10 + (s[9] - '0');
  if (!valid_date(year, month, day)) {
    return false;
  }
  time_pos = 10;
  return true;
}

bool parse_time_parts(const char *s, int len, int &hour, int &minute, int &second)
{
  if (len < 8) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    if ((i == 2 || i == 5) ? s[i] != ':' : !isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  hour = (s[0] - '0') * 10 + (s[1] - '0');
  minute = (s[3] - '0') * 10 + (s[4] - '0');
  second = (s[6] - '0') * 10 + (s[7] - '0');
  if (hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  for (int i = 8; i < len; ++i) {
    if (i == 8 && s[i] == '.') {
      continue;
    }
    if (!isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  return true;
}

int classify_text(const ObString &text)
{
  if (text.empty()) {
    return -1;
  }
  if (text.case_compare("true") == 0 || text.case_compare("false") == 0) {
    return OB_FILE_COL_BOOL;
  }
  bool digits = true;
  int start = 0;
  if (text.ptr()[0] == '+' || text.ptr()[0] == '-') {
    start = 1;
    if (text.length() == 1) {
      digits = false;
    }
  }
  bool dot = false;
  bool exp = false;
  for (int i = start; digits && i < text.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(text.ptr()[i]);
    if (!isdigit(c)) {
      digits = false;
    }
  }
  if (digits && start < text.length()) {
    errno = 0;
    char tmp[128];
    if (text.length() < static_cast<int64_t>(sizeof(tmp))) {
      memcpy(tmp, text.ptr(), text.length());
      tmp[text.length()] = '\0';
      char *end = NULL;
      strtoll(tmp, &end, 10);
      if (end == tmp + text.length() && errno != ERANGE) {
        return OB_FILE_COL_BIGINT;
      }
    }
    return OB_FILE_COL_DOUBLE;
  }
  bool numeric = start < text.length();
  for (int i = start; numeric && i < text.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(text.ptr()[i]);
    if (c == '.' && !dot && !exp) {
      dot = true;
    } else if ((c == 'e' || c == 'E') && !exp && i > start) {
      exp = true;
      if (i + 1 < text.length() && (text.ptr()[i + 1] == '+' || text.ptr()[i + 1] == '-')) {
        ++i;
      }
    } else if (!isdigit(c)) {
      numeric = false;
    }
  }
  if (numeric && (dot || exp)) {
    return OB_FILE_COL_DOUBLE;
  }
  int year = 0, month = 0, day = 0, time_pos = 0;
  if (parse_date_parts(text, year, month, day, time_pos)) {
    if (text.length() == 10) {
      return OB_FILE_COL_DATE;
    }
    if (text.length() > 10 && (text.ptr()[10] == ' ' || text.ptr()[10] == 'T' || text.ptr()[10] == 't')) {
      int hour = 0, minute = 0, second = 0;
      if (parse_time_parts(text.ptr() + 11, text.length() - 11, hour, minute, second)) {
        return OB_FILE_COL_DATETIME;
      }
    }
  }
  return OB_FILE_COL_VARCHAR;
}

int widen_kind(int current, int next)
{
  if (current < 0) {
    return next;
  }
  if (next < 0 || current == next) {
    return current;
  }
  if (current == OB_FILE_COL_VARCHAR || next == OB_FILE_COL_VARCHAR) {
    return OB_FILE_COL_VARCHAR;
  }
  if ((current == OB_FILE_COL_BIGINT && next == OB_FILE_COL_DOUBLE) ||
      (current == OB_FILE_COL_DOUBLE && next == OB_FILE_COL_BIGINT)) {
    return OB_FILE_COL_DOUBLE;
  }
  if ((current == OB_FILE_COL_DATE && next == OB_FILE_COL_DATETIME) ||
      (current == OB_FILE_COL_DATETIME && next == OB_FILE_COL_DATE)) {
    return OB_FILE_COL_DATETIME;
  }
  return OB_FILE_COL_VARCHAR;
}

int text_to_obj(const ObString &text, int kind, ObObj &obj, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (text.empty() || (kind != OB_FILE_COL_VARCHAR && text.case_compare("NULL") == 0 && kind != OB_FILE_COL_BOOL)) {
    obj.set_null();
  } else if (OB_FILE_COL_BIGINT == kind) {
    char tmp[128];
    if (text.length() >= static_cast<int64_t>(sizeof(tmp))) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      memcpy(tmp, text.ptr(), text.length());
      tmp[text.length()] = '\0';
      errno = 0;
      char *end = NULL;
      int64_t value = strtoll(tmp, &end, 10);
      if (end != tmp + text.length() || errno == ERANGE) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        obj.set_int(value);
      }
    }
  } else if (OB_FILE_COL_DOUBLE == kind) {
    char tmp[256];
    if (text.length() >= static_cast<int64_t>(sizeof(tmp))) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      memcpy(tmp, text.ptr(), text.length());
      tmp[text.length()] = '\0';
      char *end = NULL;
      double value = strtod(tmp, &end);
      if (end != tmp + text.length()) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        obj.set_double(value);
      }
    }
  } else if (OB_FILE_COL_BOOL == kind) {
    if (text.case_compare("true") == 0) {
      obj.set_tinyint(1);
    } else if (text.case_compare("false") == 0) {
      obj.set_tinyint(0);
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
  } else if (OB_FILE_COL_DATE == kind) {
    int32_t value = 0;
    if (OB_FAIL(ObTimeConverter::str_to_date(text, value))) {
      LOG_WARN("file sql date parse failed", K(ret), K(text));
    } else {
      obj.set_date(value);
    }
  } else if (OB_FILE_COL_DATETIME == kind) {
    ObTimeConvertCtx ctx(NULL, false);
    int64_t value = 0;
    ObString normalized = text;
    char tmp[128];
    if (text.length() > 10 && text.length() < static_cast<int64_t>(sizeof(tmp)) &&
        (text.ptr()[10] == 'T' || text.ptr()[10] == 't')) {
      memcpy(tmp, text.ptr(), text.length());
      tmp[10] = ' ';
      tmp[text.length()] = '\0';
      normalized.assign_ptr(tmp, text.length());
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObTimeConverter::str_to_datetime(normalized, ctx, value, NULL))) {
      LOG_WARN("file sql datetime parse failed", K(ret), K(text));
    } else {
      obj.set_datetime(value);
    }
  } else {
    ObString stored;
    if (OB_FAIL(copy_string(allocator, text, stored))) {
    } else {
      obj.set_varchar(stored);
      obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    }
  }
  return ret;
}

int snappy_uncompress(const unsigned char *src, int64_t src_len, unsigned char *dst, int64_t dst_cap, int64_t &dst_len)
{
  int ret = OB_SUCCESS;
  int64_t i = 0;
  int64_t expected = 0;
  int shift = 0;
  if (src_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
  }
  while (OB_SUCC(ret) && i < src_len) {
    unsigned char b = src[i++];
    expected |= static_cast<int64_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      break;
    }
    shift += 7;
    if (shift > 28) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  dst_len = 0;
  if (OB_FAIL(ret)) {
  } else if (expected < 0 || expected > dst_cap) {
    ret = OB_SIZE_OVERFLOW;
  }
  while (OB_SUCC(ret) && i < src_len && dst_len < expected) {
    unsigned char tag = src[i++];
    int kind = tag & 3;
    if (kind == 0) {
      int ln = tag >> 2;
      int64_t length = 0;
      if (ln < 60) {
        length = ln + 1;
      } else if (ln == 60) {
        if (i >= src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          length = src[i++] + 1;
        }
      } else if (ln == 61) {
        if (i + 1 >= src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          length = src[i] + (src[i + 1] << 8) + 1;
          i += 2;
        }
      } else if (ln == 62) {
        if (i + 2 >= src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          length = src[i] + (src[i + 1] << 8) + (src[i + 2] << 16) + 1;
          i += 3;
        }
      } else if (i + 3 >= src_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        length = static_cast<int64_t>(src[i]) + (static_cast<int64_t>(src[i + 1]) << 8) +
                 (static_cast<int64_t>(src[i + 2]) << 16) + (static_cast<int64_t>(src[i + 3]) << 24) + 1;
        i += 4;
      }
      if (OB_FAIL(ret)) {
      } else if (i + length > src_len || dst_len + length > expected) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        memcpy(dst + dst_len, src + i, length);
        dst_len += length;
        i += length;
      }
    } else {
      int64_t length = 0;
      int64_t offset = 0;
      if (kind == 1) {
        if (i >= src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          length = ((tag >> 2) & 7) + 4;
          offset = ((tag >> 5) << 8) | src[i++];
        }
      } else if (kind == 2) {
        if (i + 1 >= src_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          length = (tag >> 2) + 1;
          offset = src[i] | (src[i + 1] << 8);
          i += 2;
        }
      } else if (i + 3 >= src_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        length = (tag >> 2) + 1;
        offset = static_cast<int64_t>(src[i]) | (static_cast<int64_t>(src[i + 1]) << 8) |
                 (static_cast<int64_t>(src[i + 2]) << 16) | (static_cast<int64_t>(src[i + 3]) << 24);
        i += 4;
      }
      if (OB_FAIL(ret)) {
      } else if (offset <= 0 || offset > dst_len || dst_len + length > expected) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        for (int64_t n = 0; n < length; ++n) {
          dst[dst_len] = dst[dst_len - offset];
          ++dst_len;
        }
      }
    }
  }
  if (OB_SUCC(ret) && dst_len != expected) {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

uint64_t read_uvarint(const unsigned char *buf, int64_t len, int64_t &pos, int &ok)
{
  uint64_t x = 0;
  int shift = 0;
  ok = 1;
  while (pos < len) {
    unsigned char b = buf[pos++];
    x |= static_cast<uint64_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      return x;
    }
    shift += 7;
    if (shift > 63) {
      ok = 0;
      return 0;
    }
  }
  ok = 0;
  return 0;
}

int64_t zigzag(uint64_t n)
{
  return static_cast<int64_t>((n >> 1) ^ (~(n & 1) + 1));
}

struct ParquetCursor
{
  const unsigned char *buf;
  int64_t len;
  int64_t pos;
  int status;
};

uint64_t pq_var(ParquetCursor &c)
{
  int ok = 1;
  uint64_t v = read_uvarint(c.buf, c.len, c.pos, ok);
  if (!ok) {
    c.status = OB_INVALID_ARGUMENT;
  }
  return v;
}

int64_t pq_zig(ParquetCursor &c)
{
  return zigzag(pq_var(c));
}

int pq_skip(ParquetCursor &c, int typ);

int pq_skip_struct(ParquetCursor &c)
{
  int last = 0;
  while (c.status == OB_SUCCESS && c.pos < c.len) {
    unsigned char t = c.buf[c.pos++];
    if (t == 0) {
      return c.status;
    }
    int delta = t >> 4;
    int typ = t & 0x0f;
    int fid = last + delta;
    if (delta == 0) {
      fid = static_cast<int>(pq_zig(c));
    }
    last = fid;
    pq_skip(c, typ);
  }
  if (c.status == OB_SUCCESS) {
    c.status = OB_INVALID_ARGUMENT;
  }
  return c.status;
}

int pq_skip(ParquetCursor &c, int typ)
{
  if (c.status != OB_SUCCESS) {
    return c.status;
  }
  if (typ == 1 || typ == 2) {
    return c.status;
  }
  if (typ >= 3 && typ <= 6) {
    pq_var(c);
    return c.status;
  }
  if (typ == 7) {
    if (c.pos + 8 > c.len) {
      c.status = OB_INVALID_ARGUMENT;
    } else {
      c.pos += 8;
    }
    return c.status;
  }
  if (typ == 8) {
    uint64_t n = pq_var(c);
    if (c.pos + static_cast<int64_t>(n) > c.len) {
      c.status = OB_INVALID_ARGUMENT;
    } else {
      c.pos += static_cast<int64_t>(n);
    }
    return c.status;
  }
  if (typ == 9 || typ == 10) {
    uint64_t header = pq_var(c);
    int et = static_cast<int>(header & 0x0f);
    uint64_t cnt = header >> 4;
    if (cnt == 15) {
      cnt = pq_var(c);
    }
    for (uint64_t i = 0; i < cnt && c.status == OB_SUCCESS; ++i) {
      pq_skip(c, et);
    }
    return c.status;
  }
  if (typ == 11) {
    uint64_t header = pq_var(c);
    int kt = static_cast<int>(header & 0x0f);
    int vt = static_cast<int>((header >> 4) & 0x0f);
    uint64_t cnt = header >> 8;
    if (cnt == 15) {
      cnt = pq_var(c);
    }
    for (uint64_t i = 0; i < cnt && c.status == OB_SUCCESS; ++i) {
      pq_skip(c, kt);
      pq_skip(c, vt);
    }
    return c.status;
  }
  if (typ == 12) {
    return pq_skip_struct(c);
  }
  c.status = OB_NOT_SUPPORTED;
  return c.status;
}

struct PqChunk
{
  int64_t off;
  int64_t sz;
  int codec;
};

struct ParquetColumn
{
  int physical;
  int converted;
  int repetition;
  int type_length;
  char *name;
  PqChunk *chunks;
  int nchunks;
  int chunk_cap;
  int chunk_i;
  int started;
  int64_t page_pos;
  int64_t chunk_end;
  int codec;
  unsigned char *page;
  int64_t page_cap;
  int values_left;
  int encoding;
  int num_nulls;
  const unsigned char *levels;
  int64_t levels_len;
  int64_t levels_pos;
  int run_left;
  int run_val;
  int level_bits;
  int level_mode;
  int64_t level_bitpos;
  int64_t level_endbit;
  unsigned char *level_buf;
  const unsigned char *vals;
  int64_t vals_len;
  int64_t vals_pos;
  int dict_n;
  int dict_kind;
  int64_t *dict_i64;
  double *dict_f64;
  char **dict_ptr;
  int *dict_len;
  int idx_bits;
  const unsigned char *idx;
  int64_t idx_len;
  int64_t idx_pos;
  int irun_left;
  int irun_val;
  int idx_mode;
  int64_t idx_bitpos;
  int64_t idx_endbit;
};

struct ParquetFile
{
  int fd;
  int64_t file_len;
  ParquetColumn *columns;
  int column_count;
  int column_cap;
};

int pread_full(int fd, void *dst, int64_t n, int64_t off)
{
  unsigned char *p = static_cast<unsigned char *>(dst);
  int64_t got = 0;
  while (got < n) {
    ssize_t r = ::pread(fd, p + got, static_cast<size_t>(n - got), off + got);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return OB_IO_ERROR;
    }
    if (r == 0) {
      return OB_IO_ERROR;
    }
    got += r;
  }
  return OB_SUCCESS;
}

void parquet_column_free(ParquetColumn &col)
{
  free(col.name);
  free(col.chunks);
  free(col.page);
  free(col.level_buf);
  free(col.dict_i64);
  free(col.dict_f64);
  if (NULL != col.dict_ptr) {
    for (int i = 0; i < col.dict_n; ++i) {
      free(col.dict_ptr[i]);
    }
  }
  free(col.dict_ptr);
  free(col.dict_len);
  memset(&col, 0, sizeof(col));
}

void parquet_file_free(ParquetFile &file)
{
  if (NULL != file.columns) {
    for (int i = 0; i < file.column_count; ++i) {
      parquet_column_free(file.columns[i]);
    }
    free(file.columns);
    file.columns = NULL;
  }
  file.column_count = 0;
  file.column_cap = 0;
}

int parquet_add_column(ParquetFile &file, ParquetColumn **out)
{
  int ret = OB_SUCCESS;
  if (file.column_count >= file.column_cap) {
    int cap = file.column_cap == 0 ? 8 : file.column_cap * 2;
    if (cap > OB_FILE_SQL_MAX_COLUMNS) {
      cap = OB_FILE_SQL_MAX_COLUMNS;
    }
    if (file.column_count >= cap) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      ParquetColumn *grown = static_cast<ParquetColumn *>(realloc(file.columns, sizeof(ParquetColumn) * cap));
      if (NULL == grown) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        memset(grown + file.column_cap, 0, sizeof(ParquetColumn) * (cap - file.column_cap));
        file.columns = grown;
        file.column_cap = cap;
      }
    }
  }
  if (OB_SUCC(ret)) {
    *out = &file.columns[file.column_count++];
    memset(*out, 0, sizeof(ParquetColumn));
  }
  return ret;
}

int parquet_add_chunk(ParquetColumn &col, int64_t off, int64_t sz, int codec)
{
  int ret = OB_SUCCESS;
  if (col.nchunks >= col.chunk_cap) {
    int cap = col.chunk_cap == 0 ? 4 : col.chunk_cap * 2;
    PqChunk *grown = static_cast<PqChunk *>(realloc(col.chunks, sizeof(PqChunk) * cap));
    if (NULL == grown) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      col.chunks = grown;
      col.chunk_cap = cap;
    }
  }
  if (OB_SUCC(ret)) {
    col.chunks[col.nchunks].off = off;
    col.chunks[col.nchunks].sz = sz;
    col.chunks[col.nchunks].codec = codec;
    ++col.nchunks;
  }
  return ret;
}

int parquet_load(const unsigned char *data, int64_t len, ParquetFile &file)
{
  int ret = OB_SUCCESS;
  file.fd = -1;
  file.file_len = 0;
  file.columns = NULL;
  file.column_count = 0;
  file.column_cap = 0;
  if (len <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ParquetCursor c;
    c.buf = data;
    c.len = len;
    c.pos = 0;
    c.status = OB_SUCCESS;
    int last = 0;
    int schema_idx = 0;
    while (OB_SUCC(ret) && c.status == OB_SUCCESS && c.pos < c.len) {
      unsigned char t = c.buf[c.pos++];
      if (t == 0) {
        break;
      }
      int delta = t >> 4;
      int typ = t & 0x0f;
      int fid = last + delta;
      if (delta == 0) {
        fid = static_cast<int>(pq_zig(c));
      }
      last = fid;
      if (fid == 2 && typ == 9) {
        uint64_t header = pq_var(c);
        uint64_t cnt = header >> 4;
        if (cnt == 15) {
          cnt = pq_var(c);
        }
        for (uint64_t n = 0; n < cnt && c.status == OB_SUCCESS && OB_SUCC(ret); ++n) {
          int slast = 0;
          int physical = -1;
          int converted = -1;
          int repetition = 0;
          int type_length = 0;
          ObString name;
          while (c.status == OB_SUCCESS) {
            unsigned char st = c.buf[c.pos++];
            if (st == 0) {
              break;
            }
            int sd = st >> 4;
            int styp = st & 0x0f;
            int sfid = slast + sd;
            if (sd == 0) {
              sfid = static_cast<int>(pq_zig(c));
            }
            slast = sfid;
            if (styp >= 3 && styp <= 6) {
              int64_t val = pq_zig(c);
              if (sfid == 1) {
                physical = static_cast<int>(val);
              } else if (sfid == 2) {
                type_length = static_cast<int>(val);
              } else if (sfid == 3) {
                repetition = static_cast<int>(val);
              } else if (sfid == 6) {
                converted = static_cast<int>(val);
              }
            } else if (styp == 8) {
              uint64_t blen = pq_var(c);
              if (sfid == 4 && c.pos + static_cast<int64_t>(blen) <= c.len) {
                name.assign_ptr(reinterpret_cast<const char *>(c.buf + c.pos), static_cast<int32_t>(blen));
              }
              c.pos += static_cast<int64_t>(blen);
            } else {
              pq_skip(c, styp);
            }
          }
          if (schema_idx > 0 && !name.empty()) {
            ParquetColumn *col = NULL;
            if (OB_FAIL(parquet_add_column(file, &col))) {
            } else {
              col->physical = physical;
              col->converted = converted;
              col->repetition = repetition;
              col->type_length = type_length;
              col->name = static_cast<char *>(malloc(name.length() + 1));
              if (NULL == col->name) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else {
                memcpy(col->name, name.ptr(), name.length());
                col->name[name.length()] = '\0';
              }
            }
          }
          ++schema_idx;
        }
      } else if (fid == 4 && typ == 9) {
        uint64_t header = pq_var(c);
        uint64_t cnt = header >> 4;
        if (cnt == 15) {
          cnt = pq_var(c);
        }
        for (uint64_t n = 0; n < cnt && c.status == OB_SUCCESS && OB_SUCC(ret); ++n) {
          int rlast = 0;
          int col_i = 0;
          while (c.status == OB_SUCCESS && OB_SUCC(ret)) {
            unsigned char rt = c.buf[c.pos++];
            if (rt == 0) {
              break;
            }
            int rd = rt >> 4;
            int rtyp = rt & 0x0f;
            int rfid = rlast + rd;
            if (rd == 0) {
              rfid = static_cast<int>(pq_zig(c));
            }
            rlast = rfid;
            if (rfid == 1 && rtyp == 9) {
              uint64_t ch = pq_var(c);
              uint64_t ccnt = ch >> 4;
              if (ccnt == 15) {
                ccnt = pq_var(c);
              }
              for (uint64_t k = 0; k < ccnt && c.status == OB_SUCCESS && OB_SUCC(ret); ++k) {
                int clast = 0;
                int physical = -1;
                int codec = 0;
                int64_t data_off = -1;
                int64_t comp_size = 0;
                while (c.status == OB_SUCCESS) {
                  unsigned char ct = c.buf[c.pos++];
                  if (ct == 0) {
                    break;
                  }
                  int cd = ct >> 4;
                  int ctyp = ct & 0x0f;
                  int cfid = clast + cd;
                  if (cd == 0) {
                    cfid = static_cast<int>(pq_zig(c));
                  }
                  clast = cfid;
                  if (ctyp == 12 && cfid == 3) {
                    int mlast = 0;
                    while (c.status == OB_SUCCESS) {
                      unsigned char mt = c.buf[c.pos++];
                      if (mt == 0) {
                        break;
                      }
                      int md = mt >> 4;
                      int mtyp = mt & 0x0f;
                      int mfid = mlast + md;
                      if (md == 0) {
                        mfid = static_cast<int>(pq_zig(c));
                      }
                      mlast = mfid;
                      if (mtyp >= 3 && mtyp <= 6) {
                        int64_t val = pq_zig(c);
                        if (mfid == 1) {
                          physical = static_cast<int>(val);
                        } else if (mfid == 4) {
                          codec = static_cast<int>(val);
                        } else if (mfid == 7) {
                          comp_size = val;
                        } else if (mfid == 9) {
                          data_off = val;
                        }
                      } else {
                        pq_skip(c, mtyp);
                      }
                    }
                  } else {
                    pq_skip(c, ctyp);
                  }
                }
                if (col_i < file.column_count) {
                  if (physical >= 0) {
                    file.columns[col_i].physical = physical;
                  }
                  if (OB_FAIL(parquet_add_chunk(file.columns[col_i], data_off, comp_size, codec))) {
                  }
                  ++col_i;
                }
              }
            } else {
              pq_skip(c, rtyp);
            }
          }
        }
      } else {
        pq_skip(c, typ);
      }
      if (c.status != OB_SUCCESS && OB_SUCC(ret)) {
        ret = c.status;
      }
    }
  }
  return ret;
}

int rle_next(const unsigned char *b, int64_t len, int64_t &pos, int bitw,
             int &mode, int &left, int &val, int64_t &bitpos, int64_t &endbit, int &out)
{
  int ret = OB_SUCCESS;
  if (bitw < 0 || bitw > 32) {
    ret = OB_NOT_SUPPORTED;
  }
  while (OB_SUCC(ret)) {
    if (mode == 1 && left > 0) {
      out = val;
      --left;
      break;
    }
    if (mode == 2 && bitpos + bitw <= endbit) {
      uint32_t v = 0;
      for (int i = 0; i < bitw; ++i) {
        int64_t bp = bitpos + i;
        if ((bp >> 3) >= len) {
          ret = OB_INVALID_ARGUMENT;
          break;
        }
        unsigned char byte = b[bp >> 3];
        v |= static_cast<uint32_t>((byte >> (bp & 7)) & 1) << i;
      }
      if (OB_SUCC(ret)) {
        bitpos += bitw;
        out = static_cast<int>(v);
      }
      break;
    }
    mode = 0;
    if (pos >= len) {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
    int ok = 1;
    uint64_t header = read_uvarint(b, len, pos, ok);
    if (!ok) {
      ret = OB_INVALID_ARGUMENT;
      break;
    }
    if ((header & 1) == 0) {
      left = static_cast<int>(header >> 1);
      int nbytes = (bitw + 7) / 8;
      if (nbytes < 1) {
        nbytes = 1;
      }
      if (left <= 0 || pos + nbytes > len) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
      uint64_t raw = 0;
      for (int i = 0; i < nbytes; ++i) {
        raw |= static_cast<uint64_t>(b[pos++]) << (8 * i);
      }
      val = static_cast<int>(raw);
      mode = 1;
    } else {
      int groups = static_cast<int>(header >> 1);
      int64_t nbytes = static_cast<int64_t>(groups) * bitw;
      if (groups <= 0 || bitw == 0 || pos + nbytes > len) {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
      bitpos = pos * 8;
      endbit = bitpos + static_cast<int64_t>(groups) * 8 * bitw;
      pos += nbytes;
      mode = 2;
    }
  }
  return ret;
}

int plain_one(ParquetColumn &col, int64_t &i64, double &f64, const unsigned char *&bytes, int &blen, int &kind)
{
  int ret = OB_SUCCESS;
  kind = 0;
  bytes = NULL;
  blen = 0;
  if (col.physical == 0) {
    if (col.vals_pos >= col.vals_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      int bit = col.vals_pos;
      int byte = col.vals[bit >> 3];
      i64 = (byte >> (bit & 7)) & 1;
      ++col.vals_pos;
      kind = 4;
    }
  } else if (col.physical == 1) {
    if (col.vals_pos + 4 > col.vals_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      int32_t v = 0;
      memcpy(&v, col.vals + col.vals_pos, 4);
      col.vals_pos += 4;
      i64 = v;
      kind = 1;
    }
  } else if (col.physical == 2) {
    if (col.vals_pos + 8 > col.vals_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      int64_t v = 0;
      memcpy(&v, col.vals + col.vals_pos, 8);
      col.vals_pos += 8;
      i64 = v;
      kind = 1;
    }
  } else if (col.physical == 4) {
    if (col.vals_pos + 4 > col.vals_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      float v = 0;
      memcpy(&v, col.vals + col.vals_pos, 4);
      col.vals_pos += 4;
      f64 = v;
      kind = 2;
    }
  } else if (col.physical == 5) {
    if (col.vals_pos + 8 > col.vals_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      double v = 0;
      memcpy(&v, col.vals + col.vals_pos, 8);
      col.vals_pos += 8;
      f64 = v;
      kind = 2;
    }
  } else if (col.physical == 6 || col.physical == 7) {
    int32_t ln = col.physical == 7 ? col.type_length : 0;
    if (col.physical == 6) {
      if (col.vals_pos + 4 > col.vals_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        memcpy(&ln, col.vals + col.vals_pos, 4);
        col.vals_pos += 4;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (ln < 0 || col.vals_pos + ln > col.vals_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      bytes = col.vals + col.vals_pos;
      blen = ln;
      col.vals_pos += ln;
      kind = 3;
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet physical type");
  }
  return ret;
}

int store_dict(ParquetColumn &col, const unsigned char *p, int64_t n, int count)
{
  int ret = OB_SUCCESS;
  col.dict_n = count;
  col.dict_kind = 0;
  col.vals = p;
  col.vals_len = n;
  col.vals_pos = 0;
  if (col.physical == 0) {
    col.dict_i64 = static_cast<int64_t *>(calloc(count, sizeof(int64_t)));
    col.dict_kind = 1;
    if (NULL == col.dict_i64) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  } else if (col.physical == 1 || col.physical == 2) {
    col.dict_i64 = static_cast<int64_t *>(calloc(count, sizeof(int64_t)));
    col.dict_kind = 1;
    if (NULL == col.dict_i64) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  } else if (col.physical == 4 || col.physical == 5) {
    col.dict_f64 = static_cast<double *>(calloc(count, sizeof(double)));
    col.dict_kind = 2;
    if (NULL == col.dict_f64) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  } else if (col.physical == 6 || col.physical == 7) {
    col.dict_ptr = static_cast<char **>(calloc(count, sizeof(char *)));
    col.dict_len = static_cast<int *>(calloc(count, sizeof(int)));
    col.dict_kind = 3;
    if (NULL == col.dict_ptr || NULL == col.dict_len) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  for (int i = 0; OB_SUCC(ret) && i < count; ++i) {
    int64_t i64 = 0;
    double f64 = 0;
    const unsigned char *bytes = NULL;
    int blen = 0;
    int kind = 0;
    if (OB_FAIL(plain_one(col, i64, f64, bytes, blen, kind))) {
    } else if (kind == 1 || kind == 4) {
      col.dict_i64[i] = i64;
    } else if (kind == 2) {
      col.dict_f64[i] = f64;
    } else if (kind == 3) {
      char *copy = static_cast<char *>(malloc(blen + 1));
      if (NULL == copy) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (blen > 0) {
          memcpy(copy, bytes, blen);
        }
        copy[blen] = '\0';
        col.dict_ptr[i] = copy;
        col.dict_len[i] = blen;
      }
    }
  }
  col.vals = NULL;
  col.vals_len = 0;
  col.vals_pos = 0;
  return ret;
}

int load_page(ParquetFile &file, ParquetColumn &col)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && col.values_left == 0) {
    if (col.chunk_i >= col.nchunks) {
      ret = OB_ITER_END;
    } else {
      if (!col.started) {
        col.page_pos = col.chunks[col.chunk_i].off;
        col.chunk_end = col.page_pos + col.chunks[col.chunk_i].sz;
        col.codec = col.chunks[col.chunk_i].codec;
        col.started = 1;
      }
      if (col.page_pos >= col.chunk_end) {
        ++col.chunk_i;
        col.started = 0;
        continue;
      }
      int64_t remain = col.chunk_end - col.page_pos;
      int64_t peek = remain > 1024 ? 1024 : remain;
      unsigned char hdr[1024];
      if (OB_FAIL(pread_full(file.fd, hdr, peek, col.page_pos))) {
      } else {
        ParquetCursor h;
        h.buf = hdr;
        h.len = peek;
        h.pos = 0;
        h.status = OB_SUCCESS;
        int hlast = 0;
        int type = -1;
        int comp = 0;
        int num_values = 0;
        int num_nulls = 0;
        int encoding = 0;
        int def_bytes = 0;
        int rep_bytes = 0;
        int is_comp = 1;
        int dict_values = 0;
        int dict_encoding = 0;
        while (h.status == OB_SUCCESS && h.pos < h.len) {
          unsigned char t = h.buf[h.pos++];
          if (t == 0) {
            break;
          }
          int delta = t >> 4;
          int typ = t & 0x0f;
          int fid = hlast + delta;
          if (delta == 0) {
            fid = static_cast<int>(pq_zig(h));
          }
          hlast = fid;
          if (typ >= 3 && typ <= 6) {
            int64_t val = pq_zig(h);
            if (fid == 1) {
              type = static_cast<int>(val);
            } else if (fid == 3) {
              comp = static_cast<int>(val);
            }
          } else if (typ == 12 && (fid == 5 || fid == 7 || fid == 8)) {
            int mlast = 0;
            int which = fid;
            while (h.status == OB_SUCCESS) {
              unsigned char mt = h.buf[h.pos++];
              if (mt == 0) {
                break;
              }
              int md = mt >> 4;
              int mtyp = mt & 0x0f;
              int mfid = mlast + md;
              if (md == 0) {
                mfid = static_cast<int>(pq_zig(h));
              }
              mlast = mfid;
              if (mtyp >= 3 && mtyp <= 6) {
                int64_t val = pq_zig(h);
                if (which == 8 && mfid == 1) {
                  num_values = static_cast<int>(val);
                } else if (which == 8 && mfid == 2) {
                  num_nulls = static_cast<int>(val);
                } else if (which == 8 && mfid == 4) {
                  encoding = static_cast<int>(val);
                } else if (which == 8 && mfid == 5) {
                  def_bytes = static_cast<int>(val);
                } else if (which == 8 && mfid == 6) {
                  rep_bytes = static_cast<int>(val);
                } else if (which == 5 && mfid == 1) {
                  num_values = static_cast<int>(val);
                } else if (which == 5 && mfid == 2) {
                  encoding = static_cast<int>(val);
                } else if (which == 7 && mfid == 1) {
                  dict_values = static_cast<int>(val);
                } else if (which == 7 && mfid == 2) {
                  dict_encoding = static_cast<int>(val);
                }
              } else if (which == 8 && mfid == 7 && mtyp == 1) {
                is_comp = 1;
              } else if (which == 8 && mfid == 7 && mtyp == 2) {
                is_comp = 0;
              } else {
                pq_skip(h, mtyp);
              }
            }
          } else {
            pq_skip(h, typ);
          }
        }
        if (h.status != OB_SUCCESS) {
          ret = h.status;
        } else if (comp < 0 || h.pos + static_cast<int64_t>(comp) > remain || comp > 32 * 1024 * 1024) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("parquet page size is invalid", K(ret), K(comp), K(remain), K(h.pos));
        } else {
          int64_t header_len = h.pos;
          unsigned char *body = static_cast<unsigned char *>(malloc(comp > 0 ? comp : 1));
          if (NULL == body) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (comp > 0 && OB_FAIL(pread_full(file.fd, body, comp, col.page_pos + header_len))) {
            free(body);
          } else {
            col.page_pos += header_len + comp;
            unsigned char *owned = NULL;
            const unsigned char *values = NULL;
            int64_t values_len = 0;
            if (type == 3) {
              if (rep_bytes < 0 || def_bytes < 0 || rep_bytes + def_bytes > comp) {
                ret = OB_INVALID_ARGUMENT;
              } else if (def_bytes > 0) {
                free(col.level_buf);
                col.level_buf = static_cast<unsigned char *>(malloc(def_bytes));
                if (NULL == col.level_buf) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else {
                  memcpy(col.level_buf, body + rep_bytes, def_bytes);
                  col.levels = col.level_buf;
                  col.levels_len = def_bytes;
                  col.levels_pos = 0;
                }
              } else {
                col.levels = NULL;
                col.levels_len = 0;
                col.levels_pos = 0;
              }
              if (OB_SUCC(ret)) {
                const unsigned char *src = body + rep_bytes + def_bytes;
                int64_t slen = comp - rep_bytes - def_bytes;
                if (is_comp && col.codec == 1 && slen > 0) {
                  int64_t cap = slen * 8 + 64;
                  if (cap < 256) {
                    cap = 256;
                  }
                  if (cap > 32 * 1024 * 1024) {
                    cap = 32 * 1024 * 1024;
                  }
                  owned = static_cast<unsigned char *>(malloc(cap));
                  int64_t out_len = 0;
                  if (NULL == owned) {
                    ret = OB_ALLOCATE_MEMORY_FAILED;
                  } else if (OB_FAIL(snappy_uncompress(src, slen, owned, cap, out_len))) {
                    free(owned);
                    cap = 32 * 1024 * 1024;
                    owned = static_cast<unsigned char *>(malloc(cap));
                    if (NULL == owned) {
                      ret = OB_ALLOCATE_MEMORY_FAILED;
                    } else if (OB_FAIL(snappy_uncompress(src, slen, owned, cap, out_len))) {
                      free(owned);
                      owned = NULL;
                    }
                  }
                  if (OB_SUCC(ret)) {
                    values = owned;
                    values_len = out_len;
                  }
                } else if (is_comp && col.codec != 0 && col.codec != 1) {
                  ret = OB_NOT_SUPPORTED;
                  LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet compression codec");
                } else {
                  owned = static_cast<unsigned char *>(malloc(slen > 0 ? slen : 1));
                  if (NULL == owned) {
                    ret = OB_ALLOCATE_MEMORY_FAILED;
                  } else {
                    if (slen > 0) {
                      memcpy(owned, src, slen);
                    }
                    values = owned;
                    values_len = slen;
                  }
                }
              }
              free(body);
              body = NULL;
            } else if (type == 0 || type == 2) {
              const unsigned char *raw = body;
              int64_t raw_len = comp;
              unsigned char *unc = NULL;
              if (col.codec == 1 && comp > 0) {
                int64_t cap = comp * 8 + 64;
                if (cap > 32 * 1024 * 1024) {
                  cap = 32 * 1024 * 1024;
                }
                unc = static_cast<unsigned char *>(malloc(cap));
                int64_t out_len = 0;
                if (NULL == unc) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else if (OB_FAIL(snappy_uncompress(body, comp, unc, cap, out_len))) {
                } else {
                  raw = unc;
                  raw_len = out_len;
                }
              } else if (col.codec != 0) {
                ret = OB_NOT_SUPPORTED;
                LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet compression codec");
              }
              if (OB_FAIL(ret)) {
              } else if (type == 2) {
                if (dict_encoding != 0) {
                  ret = OB_NOT_SUPPORTED;
                  LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet dictionary page encoding");
                } else if (OB_FAIL(store_dict(col, raw, raw_len, dict_values))) {
                }
              } else {
                int64_t pos = 0;
                if (col.repetition != 0) {
                  if (pos + 4 > raw_len) {
                    ret = OB_INVALID_ARGUMENT;
                  } else {
                    uint32_t ln = 0;
                    memcpy(&ln, raw + pos, 4);
                    pos += 4;
                    if (pos + ln > raw_len) {
                      ret = OB_INVALID_ARGUMENT;
                    } else {
                      free(col.level_buf);
                      col.level_buf = static_cast<unsigned char *>(malloc(ln > 0 ? ln : 1));
                      if (NULL == col.level_buf) {
                        ret = OB_ALLOCATE_MEMORY_FAILED;
                      } else {
                        if (ln > 0) {
                          memcpy(col.level_buf, raw + pos, ln);
                        }
                        col.levels = col.level_buf;
                        col.levels_len = ln;
                        col.levels_pos = 0;
                        pos += ln;
                      }
                    }
                  }
                }
                if (OB_SUCC(ret)) {
                  values_len = raw_len - pos;
                  owned = static_cast<unsigned char *>(malloc(values_len > 0 ? values_len : 1));
                  if (NULL == owned) {
                    ret = OB_ALLOCATE_MEMORY_FAILED;
                  } else {
                    if (values_len > 0) {
                      memcpy(owned, raw + pos, values_len);
                    }
                    values = owned;
                  }
                }
              }
              free(body);
              free(unc);
              body = NULL;
            } else {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet page type");
              free(body);
              body = NULL;
            }
            if (OB_FAIL(ret)) {
              free(owned);
            } else if (type == 2) {
              free(owned);
            } else if (type == 0 || type == 3) {
              free(col.page);
              col.page = owned;
              col.encoding = encoding;
              col.num_nulls = num_nulls;
              col.values_left = num_values;
              col.level_mode = 0;
              col.run_left = 0;
              col.idx_mode = 0;
              col.irun_left = 0;
              col.level_bits = 1;
              if (encoding == 2 || encoding == 8) {
                if (values_len < 1) {
                  ret = OB_INVALID_ARGUMENT;
                  col.values_left = 0;
                } else {
                  col.idx_bits = values[0];
                  col.idx = values + 1;
                  col.idx_len = values_len - 1;
                  col.idx_pos = 0;
                  col.vals = NULL;
                  col.vals_len = 0;
                  col.vals_pos = 0;
                }
              } else if (encoding == 0) {
                col.vals = values;
                col.vals_len = values_len;
                col.vals_pos = 0;
              } else {
                ret = OB_NOT_SUPPORTED;
                LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet data encoding");
                col.values_left = 0;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int parquet_next_raw(ParquetFile &file, int col_i, int &is_null, int64_t &i64, double &f64,
                     const unsigned char *&bytes, int &blen, int &kind)
{
  int ret = OB_SUCCESS;
  is_null = 0;
  kind = 0;
  bytes = NULL;
  blen = 0;
  if (col_i < 0 || col_i >= file.column_count) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ParquetColumn &col = file.columns[col_i];
    if (col.values_left == 0 && OB_FAIL(load_page(file, col))) {
    } else if (col.values_left <= 0) {
      ret = OB_ITER_END;
    } else {
      --col.values_left;
      int present = 1;
      if (col.repetition != 0 && col.num_nulls > 0) {
        int level = 0;
        if (OB_FAIL(rle_next(col.levels, col.levels_len, col.levels_pos, col.level_bits,
                             col.level_mode, col.run_left, col.run_val, col.level_bitpos,
                             col.level_endbit, level))) {
        } else {
          present = level > 0;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (!present) {
        is_null = 1;
      } else if (col.encoding == 0) {
        if (OB_FAIL(plain_one(col, i64, f64, bytes, blen, kind))) {
        }
      } else if (col.encoding == 2 || col.encoding == 8) {
        int index = 0;
        if (OB_FAIL(rle_next(col.idx, col.idx_len, col.idx_pos, col.idx_bits,
                             col.idx_mode, col.irun_left, col.irun_val, col.idx_bitpos,
                             col.idx_endbit, index))) {
        } else if (index < 0 || index >= col.dict_n) {
          ret = OB_INVALID_ARGUMENT;
        } else if (col.dict_kind == 1) {
          i64 = col.dict_i64[index];
          kind = col.physical == 0 ? 4 : 1;
        } else if (col.dict_kind == 2) {
          f64 = col.dict_f64[index];
          kind = 2;
        } else if (col.dict_kind == 3) {
          bytes = reinterpret_cast<const unsigned char *>(col.dict_ptr[index]);
          blen = col.dict_len[index];
          kind = 3;
        } else {
          ret = OB_ERR_UNEXPECTED;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
      }
    }
  }
  return ret;
}

} // namespace

int ObFileTableDef::deep_copy(const ObFileTableDef &other, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  kind_ = other.kind_;
  header_ = other.header_;
  sample_size_ = other.sample_size_;
  file_size_ = other.file_size_;
  mtime_sec_ = other.mtime_sec_;
  if (OB_FAIL(copy_string(allocator, other.path_, path_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < other.col_names_.count(); ++i) {
    ObString name;
    if (OB_FAIL(copy_string(allocator, other.col_names_.at(i), name))) {
    } else if (OB_FAIL(col_names_.push_back(name))) {
    } else if (OB_FAIL(col_kinds_.push_back(other.col_kinds_.at(i)))) {
    } else if (OB_FAIL(col_nullable_.push_back(other.col_nullable_.at(i)))) {
    }
  }
  return ret;
}

const char *ObFileTableDef::kind_name() const
{
  switch (kind_) {
    case OB_FILE_SQL_CSV: return "read_csv";
    case OB_FILE_SQL_JSONL: return "read_json";
    case OB_FILE_SQL_PARQUET: return "read_parquet";
    case OB_FILE_SQL_GLOB: return "glob";
    default: return "read_file";
  }
}

ObObjType ObFileScan::obj_type(int64_t kind)
{
  switch (kind) {
    case OB_FILE_COL_BIGINT: return ObIntType;
    case OB_FILE_COL_DOUBLE: return ObDoubleType;
    case OB_FILE_COL_BOOL: return ObTinyIntType;
    case OB_FILE_COL_DATE: return ObDateType;
    case OB_FILE_COL_DATETIME: return ObDateTimeType;
    default: return ObVarcharType;
  }
}

const char *ObFileScan::type_name(int64_t kind)
{
  switch (kind) {
    case OB_FILE_COL_BIGINT: return "BIGINT";
    case OB_FILE_COL_DOUBLE: return "DOUBLE";
    case OB_FILE_COL_BOOL: return "BOOLEAN";
    case OB_FILE_COL_DATE: return "DATE";
    case OB_FILE_COL_DATETIME: return "DATETIME";
    default: return "VARCHAR";
  }
}

int ObFileScan::check_enabled(ObSQLSessionInfo *session)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    LOG_WARN("read enable_file_sql failed", K(ret));
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql is disabled, set enable_file_sql");
  }
  return ret;
}

int ObFileScan::resolve_real_path(ObSQLSessionInfo *session,
                                  const ObString &path,
                                  ObIAllocator &allocator,
                                  ObString &real_path,
                                  int64_t &file_size,
                                  int64_t &mtime_sec,
                                  bool allow_missing_file)
{
  int ret = OB_SUCCESS;
  char input[PATH_MAX];
  char resolved[PATH_MAX];
  file_size = -1;
  mtime_sec = -1;
  if (path.empty() || path.length() >= PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path");
  } else {
    memcpy(input, path.ptr(), path.length());
    input[path.length()] = '\0';
    if (NULL == realpath(input, resolved)) {
      if (allow_missing_file && errno == ENOENT) {
        // Caller checks the parent directory. Keep the normalized input for creation.
        if (OB_FAIL(copy_string(allocator, ObString(path.length(), input), real_path))) {
        }
      } else {
        ret = OB_FILE_NOT_EXIST;
        LOG_WARN("realpath failed", K(ret), K(path), K(errno));
      }
    } else {
      struct stat st;
      if (0 != stat(resolved, &st)) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        file_size = static_cast<int64_t>(st.st_size);
        mtime_sec = static_cast<int64_t>(st.st_mtime);
        ObString secure;
        if (OB_ISNULL(session)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(session->get_secure_file_priv(secure))) {
        } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure, ObString(resolved)))) {
          LOG_WARN("file sql path is outside secure_file_priv", K(ret), K(resolved));
        } else if (OB_FAIL(copy_string(allocator, ObString(static_cast<int32_t>(strlen(resolved)), resolved), real_path))) {
        }
      }
    }
  }
  return ret;
}

ObFileRowCursor::ObFileRowCursor()
    : def_(NULL),
      fd_(-1),
      buf_(NULL),
      buf_cap_(0),
      buf_len_(0),
      buf_pos_(0),
      eof_(false),
      row_buf_(NULL),
      row_cap_(0),
      row_len_(0),
      decode_buf_(NULL),
      decode_cap_(0),
      decode_len_(0),
      line_no_(0),
      header_skipped_(false),
      skip_header_(false),
      parquet_(false),
      project_all_(true),
      count_only_(false),
      quoted_(NULL),
      quoted_cap_(0),
      quoted_len_(0),
      owned_(NULL),
      owned_n_(0),
      owned_cap_(0),
      parquet_state_(NULL),
      glob_paths_(NULL),
      glob_count_(0),
      glob_idx_(0),
      glob_root_(NULL)
{
  memset(project_words_, 0, sizeof(project_words_));
}

ObFileRowCursor::~ObFileRowCursor()
{
  close();
}

void ObFileRowCursor::free_owned()
{
  for (int i = 0; i < owned_n_; ++i) {
    free(owned_[i]);
  }
  owned_n_ = 0;
}

void ObFileRowCursor::project_all()
{
  project_all_ = true;
  count_only_ = false;
  memset(project_words_, 0, sizeof(project_words_));
}

void ObFileRowCursor::project_column(int64_t idx)
{
  if (idx >= 0 && idx < 4096) {
    project_all_ = false;
    project_words_[idx >> 6] |= 1ULL << (idx & 63);
  }
}

bool ObFileRowCursor::column_projected(int64_t idx) const
{
  if (project_all_ || idx < 0 || idx >= 4096) {
    return project_all_;
  }
  return 0 != (project_words_[idx >> 6] & (1ULL << (idx & 63)));
}

bool ObFileRowCursor::field_quoted(int64_t idx) const
{
  return NULL != quoted_ && idx >= 0 && idx < quoted_len_ && quoted_[idx] != 0;
}

int ObFileRowCursor::push_quoted(bool quoted)
{
  int ret = OB_SUCCESS;
  if (quoted_len_ >= quoted_cap_) {
    int64_t cap = quoted_cap_ == 0 ? 16 : quoted_cap_ * 2;
    int8_t *grown = static_cast<int8_t *>(realloc(quoted_, cap));
    if (NULL == grown) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      quoted_ = grown;
      quoted_cap_ = cap;
    }
  }
  if (OB_SUCC(ret)) {
    quoted_[quoted_len_++] = quoted ? 1 : 0;
  }
  return ret;
}

int ObFileRowCursor::own_field(const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  dst.reset();
  if (!src.empty()) {
    if (owned_n_ >= owned_cap_) {
      int cap = owned_cap_ == 0 ? 16 : owned_cap_ * 2;
      char **grown = static_cast<char **>(realloc(owned_, sizeof(char *) * cap));
      if (NULL == grown) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        owned_ = grown;
        owned_cap_ = cap;
      }
    }
    char *copy = NULL;
    if (OB_FAIL(ret)) {
    } else if (NULL == (copy = static_cast<char *>(malloc(src.length())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      memcpy(copy, src.ptr(), src.length());
      owned_[owned_n_++] = copy;
      dst.assign_ptr(copy, src.length());
    }
  }
  return ret;
}

int ObFileRowCursor::ensure_decode(int64_t need)
{
  int ret = OB_SUCCESS;
  if (need > OB_FILE_SQL_MAX_FIELD) {
    ret = OB_SIZE_OVERFLOW;
  } else if (need > decode_cap_) {
    int64_t cap = decode_cap_ > 0 ? decode_cap_ : 4096;
    while (cap < need) {
      cap *= 2;
    }
    char *grown = static_cast<char *>(realloc(decode_buf_, cap));
    if (NULL == grown) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      decode_buf_ = grown;
      decode_cap_ = cap;
    }
  }
  return ret;
}

void ObFileRowCursor::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (NULL != buf_) {
    free(buf_);
    buf_ = NULL;
  }
  if (NULL != row_buf_) {
    free(row_buf_);
    row_buf_ = NULL;
  }
  if (NULL != decode_buf_) {
    free(decode_buf_);
    decode_buf_ = NULL;
  }
  if (NULL != quoted_) {
    free(quoted_);
    quoted_ = NULL;
  }
  free_owned();
  if (NULL != owned_) {
    free(owned_);
    owned_ = NULL;
  }
  if (NULL != parquet_state_) {
    ParquetFile *file = static_cast<ParquetFile *>(parquet_state_);
    parquet_file_free(*file);
    free(file);
    parquet_state_ = NULL;
  }
  if (NULL != glob_paths_) {
    for (int i = 0; i < glob_count_; ++i) {
      free(glob_paths_[i]);
    }
    free(glob_paths_);
    glob_paths_ = NULL;
  }
  if (NULL != glob_root_) {
    free(glob_root_);
    glob_root_ = NULL;
  }
  buf_cap_ = buf_len_ = buf_pos_ = 0;
  row_cap_ = row_len_ = 0;
  decode_cap_ = decode_len_ = 0;
  quoted_cap_ = quoted_len_ = 0;
  owned_n_ = owned_cap_ = 0;
  eof_ = false;
  parquet_ = false;
  glob_count_ = glob_idx_ = 0;
}

int ObFileRowCursor::open(const ObFileTableDef &def, bool skip_header)
{
  int ret = OB_SUCCESS;
  bool keep_all = project_all_;
  bool keep_count = count_only_;
  uint64_t keep_words[64];
  memcpy(keep_words, project_words_, sizeof(keep_words));
  close();
  project_all_ = keep_all;
  count_only_ = keep_count;
  memcpy(project_words_, keep_words, sizeof(keep_words));
  def_ = &def;
  skip_header_ = skip_header;
  header_skipped_ = false;
  if (OB_FILE_SQL_PARQUET == def.kind_) {
    parquet_ = true;
    struct stat st;
    fd_ = ::open(def.path_.ptr(), O_RDONLY);
    if (fd_ < 0) {
      ret = OB_FILE_NOT_EXIST;
    } else if (0 != fstat(fd_, &st)) {
      ret = OB_FILE_NOT_EXIST;
    } else if (st.st_size != def.file_size_ || static_cast<int64_t>(st.st_mtime) != def.mtime_sec_) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file changed since schema was inferred");
    } else if (st.st_size < 12) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      unsigned char tail[8];
      int64_t footer_len = 0;
      unsigned char *footer = NULL;
      if (OB_FAIL(pread_full(fd_, tail, 8, st.st_size - 8))) {
      } else if (memcmp(tail + 4, "PAR1", 4) != 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("parquet magic mismatch", K(ret));
      } else {
        footer_len = static_cast<int64_t>(tail[0]) | (static_cast<int64_t>(tail[1]) << 8) |
                     (static_cast<int64_t>(tail[2]) << 16) | (static_cast<int64_t>(tail[3]) << 24);
        if (footer_len <= 0 || footer_len + 8 > st.st_size || footer_len > OB_FILE_SQL_MAX_FIELD) {
          ret = OB_INVALID_ARGUMENT;
        } else if (NULL == (footer = static_cast<unsigned char *>(malloc(footer_len)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(pread_full(fd_, footer, footer_len, st.st_size - 8 - footer_len))) {
        }
      }
      ParquetFile *file = NULL;
      if (OB_FAIL(ret)) {
      } else if (NULL == (file = static_cast<ParquetFile *>(calloc(1, sizeof(ParquetFile))))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(parquet_load(footer, footer_len, *file))) {
        free(file);
      } else {
        file->fd = fd_;
        file->file_len = st.st_size;
        parquet_state_ = file;
      }
      free(footer);
    }
  } else if (OB_FILE_SQL_GLOB == def.kind_) {
    glob_t matches;
    memset(&matches, 0, sizeof(matches));
    const char *slash = def.path_.ptr() == NULL ? NULL : strrchr(def.path_.ptr(), '/');
    int root_len = slash == NULL ? 0 : static_cast<int>(slash - def.path_.ptr());
    int gret = glob(def.path_.ptr(), 0, NULL, &matches);
    if (GLOB_NOMATCH == gret) {
      glob_count_ = 0;
    } else if (0 != gret) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      glob_paths_ = static_cast<char **>(calloc(matches.gl_pathc, sizeof(char *)));
      if (NULL == glob_paths_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      for (size_t i = 0; OB_SUCC(ret) && i < matches.gl_pathc; ++i) {
        char resolved[PATH_MAX];
        if (NULL == realpath(matches.gl_pathv[i], resolved)) {
          ret = OB_FILE_NOT_EXIST;
        } else if (root_len > 0 &&
                   (strncmp(resolved, def.path_.ptr(), root_len) != 0 ||
                    (resolved[root_len] != '/' && resolved[root_len] != '\0'))) {
          ret = OB_ERR_NO_PRIVILEGE;
          LOG_WARN("glob result escaped the pattern directory", K(ret), K(resolved));
        } else if (NULL == (glob_paths_[glob_count_] = strdup(resolved))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          ++glob_count_;
        }
      }
      globfree(&matches);
    }
  } else if (OB_FAIL(open_text())) {
  }
  return ret;
}

int ObFileRowCursor::open_text()
{
  int ret = OB_SUCCESS;
  struct stat st;
  fd_ = ::open(def_->path_.ptr(), O_RDONLY);
  if (fd_ < 0) {
    ret = OB_FILE_NOT_EXIST;
  } else if (0 != fstat(fd_, &st)) {
    ret = OB_FILE_NOT_EXIST;
  } else if (st.st_size != def_->file_size_ || static_cast<int64_t>(st.st_mtime) != def_->mtime_sec_) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file changed since schema was inferred");
  } else {
    buf_ = static_cast<char *>(malloc(OB_FILE_SQL_READ_CHUNK));
    row_buf_ = static_cast<char *>(malloc(4096));
    if (NULL == buf_ || NULL == row_buf_) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      buf_cap_ = OB_FILE_SQL_READ_CHUNK;
      row_cap_ = 4096;
    }
  }
  return ret;
}

int ObFileRowCursor::fill()
{
  int ret = OB_SUCCESS;
  if (buf_pos_ > 0 && buf_pos_ < buf_len_) {
    memmove(buf_, buf_ + buf_pos_, buf_len_ - buf_pos_);
    buf_len_ -= buf_pos_;
    buf_pos_ = 0;
  } else if (buf_pos_ >= buf_len_) {
    buf_len_ = 0;
    buf_pos_ = 0;
  }
  if (!eof_ && buf_len_ < buf_cap_) {
    ssize_t n = ::read(fd_, buf_ + buf_len_, buf_cap_ - buf_len_);
    if (n < 0) {
      ret = OB_IO_ERROR;
    } else if (n == 0) {
      eof_ = true;
    } else {
      buf_len_ += n;
    }
  }
  return ret;
}

int ObFileRowCursor::ensure_row(int64_t need)
{
  int ret = OB_SUCCESS;
  if (need > OB_FILE_SQL_MAX_FIELD) {
    ret = OB_SIZE_OVERFLOW;
  } else if (need > row_cap_) {
    int64_t cap = row_cap_ > 0 ? row_cap_ : 4096;
    while (cap < need) {
      cap *= 2;
    }
    char *grown = static_cast<char *>(realloc(row_buf_, cap));
    if (NULL == grown) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      row_buf_ = grown;
      row_cap_ = cap;
    }
  }
  return ret;
}

int ObFileRowCursor::parse_csv_row(ObIArray<ObString> &fields, bool &got)
{
  int ret = OB_SUCCESS;
  got = false;
  fields.reset();
  row_len_ = 0;
  bool in_quote = false;
  bool any = false;
  while (OB_SUCC(ret) && !got) {
    if (buf_pos_ >= buf_len_) {
      if (OB_FAIL(fill())) {
      } else if (buf_pos_ >= buf_len_ && eof_) {
        if (in_quote) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unterminated CSV quote");
        } else if (any || row_len_ > 0) {
          got = true;
        } else {
          ret = OB_ITER_END;
        }
      }
    }
    if (OB_FAIL(ret) || got || ret == OB_ITER_END) {
      break;
    }
    char c = buf_[buf_pos_++];
    any = true;
    if (in_quote) {
      if (c == '"') {
        if (buf_pos_ >= buf_len_ && OB_FAIL(fill())) {
        } else if (buf_pos_ < buf_len_ && buf_[buf_pos_] == '"') {
          ++buf_pos_;
          if (OB_FAIL(ensure_row(row_len_ + 1))) {
          } else {
            row_buf_[row_len_++] = '"';
          }
        } else {
          in_quote = false;
        }
      } else if (OB_FAIL(ensure_row(row_len_ + 1))) {
      } else {
        row_buf_[row_len_++] = c;
      }
    } else if (c == '"') {
      in_quote = true;
    } else if (c == ',') {
      if (OB_FAIL(fields.push_back(ObString(row_len_, row_buf_ + (row_len_ == 0 ? 0 : 0))))) {
      }
    } else if (c == '\n') {
      got = true;
    } else if (c == '\r') {
      if (buf_pos_ >= buf_len_ && OB_FAIL(fill())) {
      } else if (buf_pos_ < buf_len_ && buf_[buf_pos_] == '\n') {
        ++buf_pos_;
      }
      if (OB_SUCC(ret)) {
        got = true;
      }
    } else if (OB_FAIL(ensure_row(row_len_ + 1))) {
    } else {
      row_buf_[row_len_++] = c;
    }
  }
  return ret;
}

int ObFileRowCursor::next_csv(ObIArray<ObString> &fields)
{
  // The field slices above are wrong because they alias a moving buffer.
  // Rebuild each row into stable field copies inside row_buf_.
  int ret = OB_SUCCESS;
  ObSEArray<int64_t, 16> starts;
  ObSEArray<int64_t, 16> lens;
  ObSEArray<int8_t, 16> qflags;
  bool got = false;
  fields.reset();
  quoted_len_ = 0;
  row_len_ = 0;
  bool in_quote = false;
  bool any = false;
  bool field_quoted = false;
  int64_t field_start = 0;
  while (OB_SUCC(ret) && !got) {
    if (buf_pos_ >= buf_len_) {
      if (OB_FAIL(fill())) {
      } else if (buf_pos_ >= buf_len_ && eof_) {
        if (in_quote) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unterminated CSV quote");
        } else if (any) {
          if (OB_FAIL(starts.push_back(field_start)) ||
              OB_FAIL(lens.push_back(row_len_ - field_start)) ||
              OB_FAIL(qflags.push_back(field_quoted ? 1 : 0))) {
          } else {
            got = true;
          }
        } else {
          ret = OB_ITER_END;
        }
      }
    }
    if (OB_FAIL(ret) || got) {
      break;
    }
    char c = buf_[buf_pos_++];
    any = true;
    if (in_quote) {
      if (c == '"') {
        if (buf_pos_ >= buf_len_ && OB_FAIL(fill())) {
        } else if (buf_pos_ < buf_len_ && buf_[buf_pos_] == '"') {
          ++buf_pos_;
          if (OB_FAIL(ensure_row(row_len_ + 1))) {
          } else {
            row_buf_[row_len_++] = '"';
          }
        } else {
          in_quote = false;
        }
      } else if (OB_FAIL(ensure_row(row_len_ + 1))) {
      } else {
        row_buf_[row_len_++] = c;
      }
    } else if (c == '"') {
      in_quote = true;
      field_quoted = true;
    } else if (c == ',' || c == '\n' || c == '\r') {
      if (OB_FAIL(starts.push_back(field_start)) || OB_FAIL(lens.push_back(row_len_ - field_start)) ||
          OB_FAIL(qflags.push_back(field_quoted ? 1 : 0))) {
      } else {
        field_start = row_len_;
        field_quoted = false;
      }
      if (OB_SUCC(ret) && c == '\r') {
        if (buf_pos_ >= buf_len_ && OB_FAIL(fill())) {
        } else if (buf_pos_ < buf_len_ && buf_[buf_pos_] == '\n') {
          ++buf_pos_;
        }
        got = true;
      } else if (OB_SUCC(ret) && c == '\n') {
        got = true;
      }
    } else if (OB_FAIL(ensure_row(row_len_ + 1))) {
    } else {
      row_buf_[row_len_++] = c;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < starts.count(); ++i) {
    if (OB_FAIL(fields.push_back(ObString(lens.at(i), row_buf_ + starts.at(i))))) {
    } else if (OB_FAIL(push_quoted(qflags.at(i) != 0))) {
    }
  }
  if (OB_SUCC(ret)) {
    ++line_no_;
  }
  return ret;
}

int ObFileRowCursor::json_value(const char *s, const char *end, const char *&out, ObString &text, int &json_type)
{
  int ret = OB_SUCCESS;
  text.reset();
  json_type = 0;
  while (s < end && isspace(static_cast<unsigned char>(*s))) {
    ++s;
  }
  if (s >= end) {
    ret = OB_INVALID_ARGUMENT;
  } else if (*s == '{') {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested JSON object");
  } else if (*s == '[') {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "JSON array");
  } else if (*s == 'n' && end - s >= 4 && memcmp(s, "null", 4) == 0) {
    json_type = 1;
    out = s + 4;
  } else if (*s == 't' && end - s >= 4 && memcmp(s, "true", 4) == 0) {
    json_type = 2;
    text.assign_ptr("true", 4);
    out = s + 4;
  } else if (*s == 'f' && end - s >= 5 && memcmp(s, "false", 5) == 0) {
    json_type = 2;
    text.assign_ptr("false", 5);
    out = s + 5;
  } else if (*s == '"') {
    ++s;
    if (OB_FAIL(ensure_decode(decode_len_ + 1))) {
    }
    int64_t start = decode_len_;
    while (OB_SUCC(ret) && s < end && *s != '"') {
      char c = *s++;
      if (c == '\\') {
        if (s >= end) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          char e = *s++;
          if (e == 'n') {
            c = '\n';
          } else if (e == 't') {
            c = '\t';
          } else if (e == 'r') {
            c = '\r';
          } else if (e == '"' || e == '\\' || e == '/') {
            c = e;
          } else if (e == 'u' && s + 4 <= end) {
            int cp = 0;
            for (int k = 0; k < 4; ++k) {
              unsigned char h = static_cast<unsigned char>(*s++);
              cp <<= 4;
              if (h >= '0' && h <= '9') cp += h - '0';
              else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
              else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
              else ret = OB_INVALID_ARGUMENT;
            }
            if (OB_SUCC(ret) && OB_FAIL(ensure_decode(decode_len_ + 3))) {
            } else if (OB_SUCC(ret)) {
              if (cp < 0x80) {
                decode_buf_[decode_len_++] = static_cast<char>(cp);
              } else if (cp < 0x800) {
                decode_buf_[decode_len_++] = static_cast<char>(0xc0 | (cp >> 6));
                decode_buf_[decode_len_++] = static_cast<char>(0x80 | (cp & 0x3f));
              } else {
                decode_buf_[decode_len_++] = static_cast<char>(0xe0 | (cp >> 12));
                decode_buf_[decode_len_++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                decode_buf_[decode_len_++] = static_cast<char>(0x80 | (cp & 0x3f));
              }
              continue;
            }
          } else {
            ret = OB_INVALID_ARGUMENT;
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(ensure_decode(decode_len_ + 1))) {
      } else if (OB_SUCC(ret)) {
        decode_buf_[decode_len_++] = c;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (s >= end || *s != '"') {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ++s;
      text.assign_ptr(decode_buf_ + start, static_cast<int32_t>(decode_len_ - start));
      json_type = 3;
      out = s;
    }
  } else if (*s == '-' || isdigit(static_cast<unsigned char>(*s))) {
    const char *begin = s++;
    while (s < end && (isdigit(static_cast<unsigned char>(*s)) || *s == '.' || *s == 'e' || *s == 'E' || *s == '+' || *s == '-')) {
      ++s;
    }
    text.assign_ptr(begin, static_cast<int32_t>(s - begin));
    json_type = 4;
    out = s;
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "JSON value");
  }
  return ret;
}

int ObFileRowCursor::next_json(ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  fields.reset();
  free_owned();
  decode_len_ = 0;
  bool got_line = false;
  row_len_ = 0;
  while (OB_SUCC(ret) && !got_line) {
    if (buf_pos_ >= buf_len_ && OB_FAIL(fill())) {
    } else if (buf_pos_ >= buf_len_ && eof_) {
      ret = OB_ITER_END;
    } else {
      while (buf_pos_ < buf_len_ && buf_[buf_pos_] != '\n') {
        if (OB_FAIL(ensure_row(row_len_ + 1))) {
          break;
        }
        row_buf_[row_len_++] = buf_[buf_pos_++];
      }
      if (OB_SUCC(ret) && buf_pos_ < buf_len_ && buf_[buf_pos_] == '\n') {
        ++buf_pos_;
        got_line = true;
      } else if (OB_SUCC(ret) && eof_ && buf_pos_ >= buf_len_) {
        got_line = row_len_ > 0;
        if (!got_line) {
          ret = OB_ITER_END;
        }
      } else if (OB_SUCC(ret) && buf_pos_ >= buf_len_) {
        continue;
      }
    }
  }
  if (OB_SUCC(ret)) {
    const char *s = row_buf_;
    const char *end = row_buf_ + row_len_;
    if (row_len_ > 0 && row_buf_[row_len_ - 1] == '\r') {
      --end;
    }
    while (s < end && isspace(static_cast<unsigned char>(*s))) ++s;
    while (end > s && isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    if (s == end) {
      return next_json(fields);
    }
    if (*s != '{') {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "JSONL row, expected an object");
    } else {
      ++s;
      ++line_no_;
      // Names are discovered by the infer pass. Execution projects by the stored names.
      // Here we return key\0value pairs packed as alternating strings: key, value, key, value.
      while (OB_SUCC(ret) && s < end) {
        while (s < end && (isspace(static_cast<unsigned char>(*s)) || *s == ',')) ++s;
        if (s < end && *s == '}') {
          break;
        }
        ObString key;
        int key_type = 0;
        const char *next = NULL;
        ObString owned_key;
        if (OB_FAIL(json_value(s, end, next, key, key_type)) || key_type != 3) {
          if (OB_SUCC(ret)) {
            ret = OB_INVALID_ARGUMENT;
          }
        } else if (OB_FAIL(own_field(key, owned_key))) {
        } else {
          s = next;
          while (s < end && isspace(static_cast<unsigned char>(*s))) ++s;
          if (s >= end || *s != ':') {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ++s;
            ObString value;
            int value_type = 0;
            ObString owned_value;
            if (OB_FAIL(json_value(s, end, next, value, value_type))) {
            } else if (value_type != 1 && OB_FAIL(own_field(value, owned_value))) {
            } else if (OB_FAIL(fields.push_back(owned_key)) ||
                       OB_FAIL(fields.push_back(value_type == 1 ? ObString() : owned_value))) {
            } else {
              s = next;
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObFileRowCursor::next_glob(ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  fields.reset();
  if (glob_idx_ >= glob_count_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(fields.push_back(ObString(static_cast<int32_t>(strlen(glob_paths_[glob_idx_])), glob_paths_[glob_idx_])))) {
  } else {
    ++glob_idx_;
  }
  return ret;
}

int ObFileRowCursor::next(ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(def_)) {
    ret = OB_NOT_INIT;
  } else if (parquet_) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FILE_SQL_GLOB == def_->kind_) {
    ret = next_glob(fields);
  } else if (OB_FILE_SQL_JSONL == def_->kind_) {
    ret = next_json(fields);
  } else {
    ret = next_csv(fields);
    if (OB_SUCC(ret) && skip_header_ && !header_skipped_) {
      header_skipped_ = true;
      ret = next_csv(fields);
    }
  }
  return ret;
}

int ObFileRowCursor::next_parquet(ObIArray<ObObj> &row, ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  if (NULL == parquet_state_ || NULL == def_) {
    ret = OB_NOT_INIT;
  } else {
    ParquetFile *file = static_cast<ParquetFile *>(parquet_state_);
    bool any_read = false;
    for (int64_t c = 0; OB_SUCC(ret) && c < def_->col_names_.count(); ++c) {
      ObObj obj;
      bool read_col = column_projected(c) || (count_only_ && c == 0) || project_all_;
      if (!read_col) {
        obj.set_null();
      } else if (c >= file->column_count) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int is_null = 0;
        int64_t i64 = 0;
        double f64 = 0;
        const unsigned char *bytes = NULL;
        int blen = 0;
        int kind = 0;
        if (OB_FAIL(parquet_next_raw(*file, static_cast<int>(c), is_null, i64, f64, bytes, blen, kind))) {
          if (OB_ITER_END == ret && !any_read) {
          } else if (OB_ITER_END == ret) {
            ret = OB_INVALID_ARGUMENT;
          }
        } else {
          any_read = true;
          const ParquetColumn &col = file->columns[c];
          if (is_null || count_only_) {
            obj.set_null();
          } else if (col.converted == 6 && (kind == 1 || kind == 4)) {
            obj.set_date(static_cast<int32_t>(i64));
          } else if (col.converted == 9 && kind == 1) {
            obj.set_datetime(i64 * 1000);
          } else if (col.converted == 10 && kind == 1) {
            obj.set_datetime(i64);
          } else if (kind == 4) {
            obj.set_tinyint(static_cast<int8_t>(i64));
          } else if (kind == 1) {
            obj.set_int(i64);
          } else if (kind == 2) {
            obj.set_double(f64);
          } else if (kind == 3) {
            ObString stored;
            ObString src(blen, reinterpret_cast<const char *>(bytes));
            if (OB_FAIL(copy_string(alloc, src, stored))) {
            } else {
              obj.set_varchar(stored);
              obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
            }
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet value");
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(row.push_back(obj))) {
      }
    }
    if (OB_SUCC(ret) && !any_read && def_->col_names_.count() > 0 && !project_all_ && !count_only_) {
      bool projected = false;
      for (int64_t c = 0; c < def_->col_names_.count(); ++c) {
        projected = projected || column_projected(c);
      }
      if (!projected) {
        ret = OB_ITER_END;
      }
    }
  }
  return ret;
}

int ObFileRowCursor::next_objs(ObIArray<ObObj> &row, ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  row.reset();
  if (parquet_) {
    ret = next_parquet(row, alloc);
  } else {
    ObSEArray<ObString, 16> fields;
    if (OB_FAIL(next(fields))) {
    } else if (OB_FILE_SQL_JSONL == def_->kind_) {
      for (int64_t c = 0; OB_SUCC(ret) && c < def_->col_names_.count(); ++c) {
        ObString text;
        bool found = false;
        for (int64_t i = 0; i + 1 < fields.count(); i += 2) {
          if (name_equal(fields.at(i), def_->col_names_.at(c))) {
            text = fields.at(i + 1);
            found = true;
          }
        }
        ObObj obj;
        if (!found || text.empty()) {
          obj.set_null();
        } else if (OB_FAIL(text_to_obj(text, static_cast<int>(def_->col_kinds_.at(c)), obj, alloc))) {
        }
        if (OB_SUCC(ret) && OB_FAIL(row.push_back(obj))) {
        }
      }
    } else if (OB_FILE_SQL_GLOB == def_->kind_) {
      ObObj obj;
      ObString stored;
      if (fields.count() != 1) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(copy_string(alloc, fields.at(0), stored))) {
      } else {
        obj.set_varchar(stored);
        obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        if (OB_FAIL(row.push_back(obj))) {
        }
      }
    } else {
      if (fields.count() != def_->col_names_.count()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "CSV row width");
        LOG_WARN("csv column count mismatch", K(ret), K(fields.count()), K(def_->col_names_.count()), K(line_no_));
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < def_->col_names_.count(); ++c) {
        ObObj obj;
        ObString text = c < fields.count() ? fields.at(c) : ObString();
        bool quoted = field_quoted(c);
        int kind_id = static_cast<int>(def_->col_kinds_.at(c));
        if ((!quoted && text.empty()) || (!quoted && text.case_compare("NULL") == 0)) {
          obj.set_null();
        } else if (text.empty() && kind_id == OB_FILE_COL_VARCHAR) {
          static const char empty_ch = '\0';
          obj.set_varchar(ObString(0, &empty_ch));
          obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        } else if (OB_FAIL(text_to_obj(text, kind_id, obj, alloc))) {
        }
        if (OB_SUCC(ret) && OB_FAIL(row.push_back(obj))) {
        }
      }
    }
  }
  return ret;
}

int ObFileScan::infer(ObSQLSessionInfo *session,
                      ObIAllocator &allocator,
                      int kind,
                      const ObString &path,
                      bool header,
                      int64_t sample_size,
                      ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  UNUSED(sample_size);
  def.kind_ = kind;
  def.header_ = header;
  def.sample_size_ = -1;
  if (OB_FAIL(check_enabled(session))) {
  } else if (OB_FILE_SQL_GLOB == kind) {
    char pattern[PATH_MAX];
    char directory[PATH_MAX];
    if (path.empty() || path.length() >= PATH_MAX) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path");
    } else {
      memcpy(pattern, path.ptr(), path.length());
      pattern[path.length()] = '\0';
      char *wild = strpbrk(pattern, "*?[");
      const char *suffix = "";
      if (NULL == wild) {
        snprintf(directory, sizeof(directory), "%s", pattern);
      } else {
        char *slash = wild;
        while (slash > pattern && *slash != '/') {
          --slash;
        }
        if (*slash == '/') {
          if (slash == pattern) {
            snprintf(directory, sizeof(directory), "/");
          } else {
            int dlen = static_cast<int>(slash - pattern);
            memcpy(directory, pattern, dlen);
            directory[dlen] = '\0';
          }
          suffix = slash;
        } else {
          snprintf(directory, sizeof(directory), ".");
          suffix = pattern;
        }
      }
      char resolved[PATH_MAX];
      ObString secure;
      if (NULL == realpath(directory, resolved)) {
        ret = OB_FILE_NOT_EXIST;
        LOG_WARN("glob directory does not exist", K(ret), K(directory), K(errno));
      } else if (OB_ISNULL(session)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(session->get_secure_file_priv(secure))) {
      } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure, ObString(resolved)))) {
        LOG_WARN("glob directory is outside secure_file_priv", K(ret), K(resolved));
      } else {
        char full[PATH_MAX];
        int n = 0;
        if (suffix[0] == '/') {
          n = snprintf(full, sizeof(full), "%s%s", resolved, suffix);
        } else if (suffix[0] == '\0') {
          n = snprintf(full, sizeof(full), "%s", resolved);
        } else {
          n = snprintf(full, sizeof(full), "%s/%s", resolved, suffix);
        }
        if (n <= 0 || n >= static_cast<int>(sizeof(full))) {
          ret = OB_SIZE_OVERFLOW;
        } else if (OB_FAIL(copy_string(allocator, ObString(n, full), def.path_))) {
        } else {
          def.file_size_ = -1;
          def.mtime_sec_ = -1;
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(append_unique_name(allocator, def.col_names_, ObString("file")))) {
    } else if (OB_FAIL(def.col_kinds_.push_back(OB_FILE_COL_VARCHAR))) {
    } else if (OB_FAIL(def.col_nullable_.push_back(0))) {
    }
  } else if (OB_FAIL(resolve_real_path(session, path, allocator, def.path_, def.file_size_, def.mtime_sec_, false))) {
  } else if (OB_FILE_SQL_PARQUET == kind) {
    ObFileRowCursor cursor;
    if (OB_FAIL(cursor.open(def))) {
    } else {
      ParquetFile *file = static_cast<ParquetFile *>(cursor.parquet_state_);
      if (OB_ISNULL(file)) {
        ret = OB_ERR_UNEXPECTED;
      }
      for (int i = 0; OB_SUCC(ret) && file != NULL && i < file->column_count; ++i) {
        int col_kind = OB_FILE_COL_VARCHAR;
        if (file->columns[i].converted == 6) { // DATE
          col_kind = OB_FILE_COL_DATE;
        } else if (file->columns[i].converted == 9 || file->columns[i].converted == 10) {
          col_kind = OB_FILE_COL_DATETIME;
        } else if (file->columns[i].physical == 0) {
          col_kind = OB_FILE_COL_BOOL;
        } else if (file->columns[i].physical == 1 || file->columns[i].physical == 2) {
          col_kind = OB_FILE_COL_BIGINT;
        } else if (file->columns[i].physical == 4 || file->columns[i].physical == 5) {
          col_kind = OB_FILE_COL_DOUBLE;
        }
        ObString parquet_name(static_cast<int32_t>(strlen(file->columns[i].name)), file->columns[i].name);
        if (OB_FAIL(append_unique_name(allocator, def.col_names_, parquet_name))) {
        } else if (OB_FAIL(def.col_kinds_.push_back(col_kind))) {
        } else if (OB_FAIL(def.col_nullable_.push_back(file->columns[i].repetition == 0 ? 0 : 1))) {
        }
      }
    }
  } else {
    ObFileRowCursor cursor;
    ObSEArray<int64_t, 16> kinds;
    ObSEArray<int8_t, 16> nulls;
    ObSEArray<ObString, 16> header_names;
    bool first = true;
    if (OB_FAIL(cursor.open(def))) {
    }
    while (OB_SUCC(ret)) {
      ObSEArray<ObString, 16> fields;
      if (OB_FAIL(cursor.next(fields))) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_FILE_SQL_JSONL == kind) {
        for (int64_t i = 0; OB_SUCC(ret) && i + 1 < fields.count(); i += 2) {
          int idx = -1;
          for (int64_t c = 0; c < header_names.count(); ++c) {
            if (name_equal(header_names.at(c), fields.at(i))) {
              idx = static_cast<int>(c);
            }
          }
          if (idx < 0) {
            if (OB_FAIL(append_unique_name(allocator, header_names, fields.at(i)))) {
            } else if (OB_FAIL(kinds.push_back(-1)) || OB_FAIL(nulls.push_back(0))) {
            } else {
              idx = static_cast<int>(header_names.count() - 1);
            }
          }
          if (OB_FAIL(ret)) {
          } else if (fields.at(i + 1).empty()) {
            nulls.at(idx) = 1;
          } else {
            kinds.at(idx) = widen_kind(static_cast<int>(kinds.at(idx)), classify_text(fields.at(i + 1)));
          }
        }
        for (int64_t c = 0; OB_SUCC(ret) && c < header_names.count(); ++c) {
          bool present = false;
          for (int64_t i = 0; i + 1 < fields.count(); i += 2) {
            if (name_equal(header_names.at(c), fields.at(i))) {
              present = true;
            }
          }
          if (!present) {
            nulls.at(c) = 1;
          }
        }
      } else if (header && first) {
        first = false;
        for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
          if (OB_FAIL(append_unique_name(allocator, header_names, fields.at(i)))) {
          } else if (OB_FAIL(kinds.push_back(-1)) || OB_FAIL(nulls.push_back(0))) {
          }
        }
      } else {
        first = false;
        if (header_names.empty()) {
          for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
            char generated[32];
            int len = snprintf(generated, sizeof(generated), "column%ld", i);
            if (OB_FAIL(append_unique_name(allocator, header_names, ObString(len, generated)))) {
            } else if (OB_FAIL(kinds.push_back(-1)) || OB_FAIL(nulls.push_back(0))) {
            }
          }
        }
        if (fields.count() != header_names.count()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "CSV row width");
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
          if (fields.at(i).empty()) {
            nulls.at(i) = 1;
          } else {
            kinds.at(i) = widen_kind(static_cast<int>(kinds.at(i)), classify_text(fields.at(i)));
          }
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < header_names.count(); ++i) {
      int kind_id = kinds.at(i) < 0 ? OB_FILE_COL_VARCHAR : static_cast<int>(kinds.at(i));
      int8_t nullable = (nulls.at(i) || kinds.at(i) < 0) ? 1 : 0;
      if (OB_FAIL(def.col_names_.push_back(header_names.at(i)))) {
      } else if (OB_FAIL(def.col_kinds_.push_back(kind_id))) {
      } else if (OB_FAIL(def.col_nullable_.push_back(nullable))) {
      }
    }
  }
  if (OB_SUCC(ret) && def.col_names_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file has no columns");
  } else if (OB_SUCC(ret) && def.col_names_.count() > OB_FILE_SQL_MAX_COLUMNS) {
    ret = OB_SIZE_OVERFLOW;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
