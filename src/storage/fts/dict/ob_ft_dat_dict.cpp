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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dat_dict.h"

#include <atomic>
#include <climits>
#include <cstdlib>
#include <pthread.h>

#include "lib/utility/alloc_assist.h"
#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/list/ob_list.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/fts/dict/ob_ft_trie.h"
namespace oceanbase
{
namespace storage
{
template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::init(ObFTTrie<DATA_TYPE> &trie)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    size_t map_size = ObArrayHashMap::estimate_size(trie.node_num());
    size_t array_size = trie.node_num() * 6; // by experience.
    size_t base_size = array_size * sizeof(int32_t);
    size_t check_size = base_size;
    size_t total_size = sizeof(ObFTDAT) + map_size + base_size + check_size;

    if (OB_ISNULL(dat_ = static_cast<ObFTDAT *>(alloc_.alloc(total_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      memset(dat_, 0, total_size);
      dat_->mem_block_size_ = total_size;
      dat_->array_size_ = array_size;
      dat_->base_offset_ = map_size;
      dat_->check_offset_ = dat_->base_offset_ + base_size;
      map_ = dat_->get_map();
      if (OB_FAIL(map_->init(trie.node_num()))) {
      }
    }
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::build_from_trie(ObFTTrie<DATA_TYPE> &trie)
{
  typedef typename ObFTTrieNode<DATA_TYPE>::NodeIndex NodeIndex;

  int ret = OB_SUCCESS;
  if (OB_ISNULL(dat_) || OB_ISNULL(dat_->buff)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int32_t *base = reinterpret_cast<int32_t *>(dat_->buff + dat_->base_offset_);
    int32_t *check = reinterpret_cast<int32_t *>(dat_->buff + dat_->check_offset_);
    base[ObFTDAT::FIRST_INDEX] = 1;
    ObArenaAllocator alloc(lib::ObMemAttr("Tmp dfs"));
    ObList<ObFTTrieNode<DATA_TYPE> *, ObIAllocator> dfs_queue(alloc);

    // Start to build the DAT.
    trie.root()->dat_build_info_.state_index_ = ObFTDAT::FIRST_BASE;
    dfs_queue.push_back(trie.root());

    int next_base_offset = 0;
    int level = 1;

    while (OB_SUCC(ret) && !dfs_queue.empty()) {
      ObFTTrieNode<DATA_TYPE> *node = dfs_queue.get_first();
      dfs_queue.pop_front();

      int rootIdx = node->dat_build_info_.state_index_;

      // mark as leaf
      if (node->is_leaf_) {
        base[rootIdx] = -1;
      }

      if (level != node->dat_build_info_.level_) {
        level = node->dat_build_info_.level_;
        next_base_offset = 0;
      }

      if (node->is_empty()) {
        // nothing
      } else {
        int try_base = 1;
        try_base += next_base_offset;
        int start = try_base;
        bool need_code = true;
        int try_step = 1;

        for (; try_base < dat_->array_size_; try_base += try_step) {
          bool conflict = false;

          if (node->is_empty()) {
          } else {
            for (typename ObList<NodeIndex, ObIAllocator>::iterator iter = node->children_->begin();
                 iter != node->children_->end();
                 ++iter) {
              ObFTWordCode child_code;
              NodeIndex &child_index = *iter;
              if (need_code) {
                if (OB_FAIL(encode(child_index.word_.get_word(), child_code, true))) {
                  LOG_WARN("Failed to encode");
                  break;
                }
                child_index.child_->dat_build_info_.code_ = child_code;
              }
              child_code = child_index.child_->dat_build_info_.code_;
              if (check[try_base + child_code] != 0) {
                conflict = true;
              }
            }
          }

          if (OB_FAIL(ret)) {
          } else {
            need_code = false;

            if (!conflict) {
              if (node->is_leaf_) {
                base[rootIdx] = -try_base;
              } else {
                base[rootIdx] = try_base;
              }
              break;
            }
            try_step = MAX(1, (try_base - start) / 20);
          }
        }

        if (OB_FAIL(ret) || try_base >= dat_->array_size_) {
          break;
        } else {
          // try_average_.add(try_base + 1 - start);
          if (try_base - start > 100) {
            next_base_offset += try_base - start;
          }

          if (node->is_empty()) {
          } else {
            for (typename ObList<NodeIndex, ObIAllocator>::iterator iter = node->children_->begin();
                 iter != node->children_->end();
                 iter++) {
              typename ObFTTrieNode<DATA_TYPE>::NodeIndex &child_index = *iter;
              int my_index = abs(try_base) + child_index.child_->dat_build_info_.code_;
              // max_used = MAX(max_used, my_index);

              if (my_index >= dat_->array_size_) {
                expand();
                base = reinterpret_cast<int32_t *>(dat_->buff + dat_->base_offset_);
                check = reinterpret_cast<int32_t *>(dat_->buff + dat_->check_offset_);
              }
              child_index.child_->dat_build_info_.state_index_ = my_index;
              check[my_index] = rootIdx;
              dfs_queue.push_back(child_index.child_);
            }
          }
        }
      }

    } // while

    if (OB_FAIL(ret)) {
      // already logged
    } else if (OB_FAIL(trie.get_start_word(dat_->start_word_))) {
    } else if (OB_FAIL(trie.get_end_word(dat_->end_word_))) {
    }
    LOG_INFO("build dat finished", K(dat_->start_word_.get_word()), K(dat_->end_word_.get_word()));

    dfs_queue.reset();
    alloc.reset();
  }

  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::expand()
{
  int ret = OB_SUCCESS;
  size_t array_size = dat_->array_size_;
  size_t base_inc = array_size * sizeof(int32_t);
  size_t check_inc = array_size * sizeof(int32_t);
  size_t buffer_size = dat_->mem_block_size_ + base_inc + check_inc;

  ObFTDAT *new_dat = nullptr;
  if (OB_ISNULL(new_dat = static_cast<ObFTDAT *>(alloc_.alloc(buffer_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    memset(new_dat, 0, buffer_size);
    new_dat->mem_block_size_ = buffer_size;
    new_dat->array_size_ = array_size * 2;
    new_dat->base_offset_ = dat_->base_offset_;
    new_dat->check_offset_ = dat_->check_offset_ + base_inc;
    // map and base
    MEMCPY(new_dat->buff, dat_->buff, dat_->check_offset_ - 0);
    // check
    size_t check_size = array_size * sizeof(int32_t);
    MEMCPY(new_dat->buff + new_dat->check_offset_, dat_->buff + dat_->check_offset_, check_size);
    alloc_.free(dat_);
    dat_ = new_dat;
    map_ = dat_->get_map();
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::encode(const ObString &word, ObFTWordCode &code, bool add)
{
  int ret = OB_SUCCESS;
  ObFTWordCode code_value;
  ObFTSingleWord single_word;
  single_word.set_word(word.ptr(), word.length());
  ret = map_->find(single_word, code);
  if (OB_ENTRY_NOT_EXIST == ret) {
    if (add) {
      code = next_code_++;
      ret = map_->insert(word, code);
      if (OB_SUCCESS != ret) {
      }
    } else {
      // do nothing
    }
  } else {
    // nothing
  }
  return ret;
}

static bool canonical_utf8_bmp(const char *p, int32_t len, uint32_t &cp)
{
  if (nullptr == p || len <= 0 || len > 3) {
    return false;
  }
  const unsigned char c0 = static_cast<unsigned char>(p[0]);
  if (len == 1) {
    cp = c0;
    return c0 < 0x80;
  } else if (len == 2) {
    const unsigned char c1 = static_cast<unsigned char>(p[1]);
    if ((c0 & 0xE0) != 0xC0 || (c1 & 0xC0) != 0x80) {
      return false;
    }
    cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
    return cp >= 0x80 && cp < 0x800;
  }
  const unsigned char c1 = static_cast<unsigned char>(p[1]);
  const unsigned char c2 = static_cast<unsigned char>(p[2]);
  if ((c0 & 0xF0) != 0xE0 || (c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
    return false;
  }
  cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
  return cp >= 0x800 && cp < 0x10000;
}

namespace {

struct DatCodeCache
{
  const void *dat;
  uint64_t fingerprint;
  int32_t *slots;
};

constexpr int MAX_DAT_CODE_CACHES = 64;
DatCodeCache g_dat_code_caches[MAX_DAT_CODE_CACHES];
std::atomic<int> g_dat_code_cache_cnt{0};
pthread_mutex_t g_dat_code_cache_mu = PTHREAD_MUTEX_INITIALIZER;

uint64_t dat_fingerprint(const ObFTDAT *dat)
{
  uint64_t fp = static_cast<uint64_t>(dat->mem_block_size_);
  fp ^= dat->word_num_ * 0x9E3779B185EBCA87ULL;
  fp ^= dat->node_num_ * 0xC2B2AE3D27D4EB4FULL;
  fp ^= dat->array_size_ * 0x165667B19E3779F9ULL;
  return fp;
}

// One zero-filled table per dictionary block, kept for the process lifetime.
// Readers are rebuilt for every document, so the table cannot live on the reader.
// The fingerprint rejects a recycled address that now holds a different block.
int32_t *shared_code_slots(const ObFTDAT *dat)
{
  if (nullptr == dat) {
    return nullptr;
  }
  const uint64_t fp = dat_fingerprint(dat);
  int n = g_dat_code_cache_cnt.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    if (g_dat_code_caches[i].dat == dat) {
      return g_dat_code_caches[i].fingerprint == fp ? g_dat_code_caches[i].slots : nullptr;
    }
  }
  pthread_mutex_lock(&g_dat_code_cache_mu);
  n = g_dat_code_cache_cnt.load(std::memory_order_relaxed);
  int32_t *slots = nullptr;
  int found = -1;
  for (int i = 0; i < n; ++i) {
    if (g_dat_code_caches[i].dat == dat) {
      found = i;
      break;
    }
  }
  if (found >= 0) {
    slots = g_dat_code_caches[found].fingerprint == fp ? g_dat_code_caches[found].slots : nullptr;
  } else if (n >= MAX_DAT_CODE_CACHES) {
    // Fall back to the hashmap. A miss here only skips the cache.
  } else if (OB_NOT_NULL(slots = static_cast<int32_t *>(calloc(65536, sizeof(int32_t))))) {
    g_dat_code_caches[n].slots = slots;
    g_dat_code_caches[n].fingerprint = fp;
    g_dat_code_caches[n].dat = dat;
    g_dat_code_cache_cnt.store(n + 1, std::memory_order_release);
  }
  pthread_mutex_unlock(&g_dat_code_cache_mu);
  return slots;
}

} // namespace

template <typename DATA_TYPE>
int32_t *ObFTDATReader<DATA_TYPE>::code_cache() const
{
  if (nullptr != code_slots_) {
    return code_slots_;
  }
  int32_t *slots = shared_code_slots(dat_);
  if (nullptr != slots) {
    code_slots_ = slots;
  }
  return slots;
}

template <typename DATA_TYPE>
int ObFTDATReader<DATA_TYPE>::match_with_hit(const ObString &ft_char,
                                             const ObDATrieHit &last_hit,
                                             ObDATrieHit &hit) const
{
  int ret = OB_SUCCESS;
  ObFTWordCode code = 0;
  hit.set_unmatch();
  int32_t *base = reinterpret_cast<int32_t *>(dat_->buff + dat_->base_offset_);
  int32_t *check = reinterpret_cast<int32_t *>(dat_->buff + dat_->check_offset_);
  uint32_t cp = 0;
  int32_t *slots = nullptr;
  int32_t cached = 0;
  bool use_cache = canonical_utf8_bmp(ft_char.ptr(), ft_char.length(), cp);
  if (use_cache) {
    slots = code_cache();
    if (nullptr != slots) {
      cached = __atomic_load_n(slots + cp, __ATOMIC_RELAXED);
    }
  }

  if (cached > 0) {
    code = cached - 1;
  } else if (cached < 0) {
    return ret;
  } else {
    ObFTSingleWord word;
    word.set_word(ft_char.ptr(), ft_char.length());
    ret = dat_->get_map()->find(word, code);
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
      if (nullptr != slots && code >= 0) {
        __atomic_store_n(slots + cp, -1, __ATOMIC_RELAXED);
      }
      return ret;
    } else if (OB_FAIL(ret)) {
      return ret;
    } else if (nullptr != slots && code >= 0 && code < INT32_MAX) {
      __atomic_store_n(slots + cp, code + 1, __ATOMIC_RELAXED);
    }
  }
  {
    if (&last_hit == &hit) {
      // sameone
    } else {
      hit = last_hit;
    }
    if (0 == hit.base_idx_) {
      hit.base_idx_ = 1; // from current to do check;
    }
    // from current place to do check
    int32_t idx = abs(base[hit.base_idx_]) + code;

    if (check[idx] != hit.base_idx_) {
      hit.set_unmatch();
    } else if (base[idx] < 0) {
      hit.base_idx_ = idx;
      hit.end_pos_ = hit.end_pos_ + ft_char.length();
      hit.char_cnt_++;
      hit.set_match();
      hit.set_prefix(); // to be checked next time
    } else {
      hit.base_idx_ = idx;
      hit.end_pos_ = hit.end_pos_ + ft_char.length();
      hit.char_cnt_++;
      hit.set_prefix();
    }
  }
  return ret;
}

template class ObFTDATBuilder<void>;
template class ObFTDATReader<void>;

ObArrayHashMap *ObFTDAT::get_map() { return reinterpret_cast<ObArrayHashMap *>(buff); }

int ObArrayHashMap::find(const ObFTSingleWord &word, ObFTWordCode &code) const
{
  int ret = OB_ENTRY_NOT_EXIST;
  uint64_t hash = word.get_word().hash();
  uint64_t index = hash % header_.capacity_;
  while (header_.data[index].used) {
    if (header_.data[index].word == word) {
      code = header_.data[index].code;
      ret = OB_SUCCESS;
      break;
    } else {
      index = (index + 3) % header_.capacity_;
    }
  }
  return ret;
}

int ObArrayHashMap::insert(const ObString &key, ObFTWordCode code)
{
  int ret = OB_SUCCESS;
  uint64_t hash = key.hash();
  uint64_t index = hash % header_.capacity_;
  while (header_.data[index].used) {
    index = (index + 3) % header_.capacity_;
  }

  header_.data[index].code = code;
  header_.data[index].word.set_word(key.ptr(), key.length());
  header_.data[index].used = true;
  header_.count_++;
  return ret;
}

int ObArrayHashMap::init(size_t word_num)
{
  int ret = OB_SUCCESS;
  size_t capacity = estimate_capacity(word_num);
  size_t size = estimate_size(word_num);

  memset(this, 0, size);
  header_.buffer_size_ = size;
  header_.capacity_ = capacity;
  header_.count_ = 0;
  return OB_SUCCESS;
}

size_t ObArrayHashMap::estimate_size(size_t word_num)
{
  size_t capacity = estimate_capacity(word_num);
  size_t size = sizeof(Header) + capacity * sizeof(Entry);
  return size;
}

size_t ObArrayHashMap::estimate_capacity(size_t word_num)
{
  constexpr size_t MIN_CAPACITY = 101;                   // prime
  size_t capacity = MAX(word_num * 4 / 3, MIN_CAPACITY); // 75%
  return capacity;
}
} //  namespace storage
} //  namespace oceanbase
