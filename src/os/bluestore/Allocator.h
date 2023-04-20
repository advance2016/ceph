// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#ifndef CEPH_OS_BLUESTORE_ALLOCATOR_H
#define CEPH_OS_BLUESTORE_ALLOCATOR_H

#include <functional>
#include <ostream>
#include "include/ceph_assert.h"
#include "bluestore_types.h"

/*
extent：offset + length，表示一段连续的物理磁盘地址空间。
PExtentVector：存放空间分配结果，可能是一个或者多个extent。
bdev_block_size：磁盘块大小，IO的最小单元，默认4K。
min_alloc_size：最小分配单元，SSD默认16K，HDD默认64K。
max_alloc_size：最大分配单元，默认0不限制，即一次连续的分配结果只包含一个extent。
alloc_unit：分配单元(AU)，一般设置为min_alloc_size。
slot：uint64类型，64 bit，位操作的基本单元。
children：slot的分配单元，可能占1 bit，也可能占2 bit。
slot_set：8个slot构成一个slot集合(512bit)，构成上层Level的 children。
slot_vector：slot数组，也即uint64的数组。每一层都用一个slot_vector组织。

Allocator是基类，BlueStore抽象了该基类，定义了分配器需要的方法，如果要实现一
个分配器，就需要实现该Allocator基类中定义个方法。
*/
class Allocator {
public:
  Allocator(std::string_view name,
	    int64_t _capacity,
	    int64_t _block_size);
  virtual ~Allocator();

  /*
  * returns allocator type name as per names in config
  */
  virtual const char* get_type() const = 0;

  /*
   * Allocate required number of blocks in n number of extents.
   * Min and Max number of extents are limited by:
   * a. alloc unit
   * b. max_alloc_size.
   * as no extent can be lesser than block_size and greater than max_alloc size.
   * Apart from that extents can vary between these lower and higher limits according
   * to free block search algorithm and availability of contiguous space.
   */
  virtual int64_t allocate(uint64_t want_size, uint64_t block_size,
			   uint64_t max_alloc_size, int64_t hint,
			   PExtentVector *extents) = 0;

  int64_t allocate(uint64_t want_size, uint64_t block_size,
		   int64_t hint, PExtentVector *extents) {
    return allocate(want_size, block_size, want_size, hint, extents);
  }

  /* Bulk release. Implementations may override this method to handle the whole
   * set at once. This could save e.g. unnecessary mutex dance. */
  virtual void release(const interval_set<uint64_t>& release_set) = 0;
  void release(const PExtentVector& release_set);

  virtual void dump() = 0;
  virtual void foreach(
    std::function<void(uint64_t offset, uint64_t length)> notify) = 0;

  virtual void init_add_free(uint64_t offset, uint64_t length) = 0;
  virtual void init_rm_free(uint64_t offset, uint64_t length) = 0;

  virtual uint64_t get_free() = 0;
  virtual double get_fragmentation()
  {
    return 0.0;
  }
  virtual double get_fragmentation_score();
  virtual void shutdown() = 0;

  static Allocator *create(
    CephContext* cct,
    std::string_view type,
    int64_t size,
    int64_t block_size,
    int64_t zone_size = 0,
    int64_t firs_sequential_zone = 0,
    const std::string_view name = ""
    );


  const std::string& get_name() const;
  int64_t get_capacity() const
  {
    return device_size;
  }
  int64_t get_block_size() const
  {
    return block_size;
  }

private:
  class SocketHook;
  SocketHook* asok_hook = nullptr;
protected:
  const int64_t device_size = 0;  //device size
  const int64_t block_size = 0;   //alloc_min_size 默认HDD 64K，SSD 16K
};

#endif
