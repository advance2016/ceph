// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_BUFFER_RAW_H
#define CEPH_BUFFER_RAW_H

#include <map>
#include <utility>
#include <type_traits>
#include "common/ceph_atomic.h"
#include "include/buffer.h"
#include "include/mempool.h"
#include "include/spinlock.h"

namespace ceph::buffer {

/*
内联名字控件可以让不同名字空间之间相互访问，但是这样会破坏名字空间的分割性

代码中为每个版本的类库定义了命名空间，同时将最新版本定义为内联命名空间。有了这
样的准备之后，假设我们队类库进行了升级， 就可以实现：

1.使用者代码不受影响，除非使用者自己想改。

2.可以自由使用新类库的功能

3.如果有需要仍然可以使用原来的类库

*/
inline namespace v15_2_0 {

  class raw {
  public:
    // In the future we might want to have a slab allocator here with few
    // embedded slots. This would allow to avoid the "if" in dtor of ptr_node.
    /*
    * 大小为sizeof(ptr_node)，alignof(ptr_node)对齐的类型。用于ptr_node的构
    * 造(new placement方式)，实际并未使用
    */
    std::aligned_storage<sizeof(ptr_node),
			 alignof(ptr_node)>::type bptr_storage;
  protected:
    //数据指针
    char *data;
    //数据长度
    unsigned len;
  public:
    //引用计数
    ceph::atomic<unsigned> nref { 0 };

    /*
    mempool用于跟踪容器使用的内存大小
    */
    int mempool;

    /*
    * 指定数据段的校验值
    */
    std::pair<size_t, size_t> last_crc_offset {std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()};
    std::pair<uint32_t, uint32_t> last_crc_val;

    /*
    * spinlock: std::atomic_flag(原子布尔类型)，实现自旋互斥，免锁结构
    * class spinlock{
        std::atomoc_flag af = ATOMIC_FLAG_INIT;
        void lock(){
            while(af.test_and_set(std::memory_order_acquire));
        }
        void unlock(){
            af.clear(std::memory_order_release);
        }
    };
    */
    //读写锁
    mutable ceph::spinlock crc_spinlock;

    /*
    * raw构造函数，raw是作为基类，实际数据分配方式是由子类实现
    */
    explicit raw(unsigned l, int mempool=mempool::mempool_buffer_anon)
      : data(nullptr), len(l), nref(0), mempool(mempool) {
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
    }
    raw(char *c, unsigned l, int mempool=mempool::mempool_buffer_anon)
      : data(c), len(l), nref(0), mempool(mempool) {
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
    }
    virtual ~raw() {
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
	-1, -(int)len);
    }

    void _set_len(unsigned l) {
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
	-1, -(int)len);
      len = l;
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(1, len);
    }

    void reassign_to_mempool(int pool) {
      if (pool == mempool) {
	return;
      }
      mempool::get_pool(mempool::pool_index_t(mempool)).adjust_count(
	-1, -(int)len);
      mempool = pool;
      mempool::get_pool(mempool::pool_index_t(pool)).adjust_count(1, len);
    }

    void try_assign_to_mempool(int pool) {
      if (mempool == mempool::mempool_buffer_anon) {
	reassign_to_mempool(pool);
      }
    }

private:
    // no copying.
    // cppcheck-suppress noExplicitConstructor
    /*
    * 禁止拷贝复制
    */
    raw(const raw &other) = delete;
    const raw& operator=(const raw &other) = delete;
public:
    /*
    * 获取原始数据
    */
    char *get_data() const {
      return data;
    }
    unsigned get_len() const {
      return len;
    }
    virtual raw* clone_empty() = 0;
    ceph::unique_leakable_ptr<raw> clone() {
      raw* const c = clone_empty();
      memcpy(c->data, data, len);
      return ceph::unique_leakable_ptr<raw>(c);
    }
    /*
    * crc相关设置
    */
    bool get_crc(const std::pair<size_t, size_t> &fromto,
		 std::pair<uint32_t, uint32_t> *crc) const {
      std::lock_guard lg(crc_spinlock);
      if (last_crc_offset == fromto) {
        *crc = last_crc_val;
        return true;
      }
      return false;
    }
    void set_crc(const std::pair<size_t, size_t> &fromto,
		 const std::pair<uint32_t, uint32_t> &crc) {
      std::lock_guard lg(crc_spinlock);
      last_crc_offset = fromto;
      last_crc_val = crc;
    }
    void invalidate_crc() {
      std::lock_guard lg(crc_spinlock);
      last_crc_offset.first = std::numeric_limits<size_t>::max();
      last_crc_offset.second = std::numeric_limits<size_t>::max();
    }
  };

} // inline namespace v15_2_0
} // namespace ceph::buffer

#endif // CEPH_BUFFER_RAW_H
