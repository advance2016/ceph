// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#ifndef CEPH_OBJECTSTORE_H
#define CEPH_OBJECTSTORE_H

#include "include/buffer.h"
#include "include/common_fwd.h"
#include "include/Context.h"
#include "include/interval_set.h"
#include "include/stringify.h"
#include "include/types.h"

#include "osd/osd_types.h"
#include "common/TrackedOp.h"
#include "common/WorkQueue.h"
#include "ObjectMap.h"
#include "os/Transaction.h"

#include <errno.h>
#include <sys/stat.h>
#include <map>
#include <memory>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__sun) || defined(_WIN32)
#include <sys/statvfs.h>
#else
#include <sys/vfs.h>    /* or <sys/statfs.h> */
#endif

namespace ceph {
  class Formatter;
}

/*
 * low-level interface to the local OSD file system
 */

class Logger;
class ContextQueue;

static inline void encode(const std::map<std::string,ceph::buffer::ptr> *attrset, ceph::buffer::list &bl) {
  using ceph::encode;
  encode(*attrset, bl);
}

// Flag bits
typedef uint32_t osflagbits_t;
const int SKIP_JOURNAL_REPLAY = 1 << 0;
const int SKIP_MOUNT_OMAP = 1 << 1;

//对象存储系统抽象操作接口。所有的对象存储引擎都有继承并实现它定义的接口
class ObjectStore {
protected:
  std::string path;

public:
  using Transaction = ceph::os::Transaction;

  CephContext* cct;
  /**
   * create - create an ObjectStore instance.
   *
   * This is invoked once at initialization time.
   *
   * @param type type of store. This is a std::string from the configuration file.
   * @param data path (or other descriptor) for data
   * @param journal path (or other descriptor) for journal (optional)
   * @param flags which filestores should check if applicable
   */
#ifndef WITH_SEASTAR
  static std::unique_ptr<ObjectStore> create(
    CephContext *cct,
    const std::string& type,
    const std::string& data,
    const std::string& journal,
    osflagbits_t flags = 0);
#endif
  static std::unique_ptr<ObjectStore> create(
    CephContext *cct,
    const std::string& type,
    const std::string& data);

  /**
   * probe a block device to learn the uuid of the owning OSD
   *
   * @param cct cct
   * @param path path to device
   * @param fsid [out] osd uuid
   */
  static int probe_block_device_fsid(
    CephContext *cct,
    const std::string& path,
    uuid_d *fsid);

  /**
   * Fetch Object Store statistics.
   *
   * Currently only latency of write and apply times are measured.
   *
   * This appears to be called with nothing locked.
   */
    /**
     * Fetch Object Store statistics.
     *
     * 返回延时和响应时间。write latency and apply times
     *
     * 此调用不会获取锁。即调用的瞬间可能 OS 状态发生改变，但获取的结果已经过时
     *
     * @param 
     * @return objectstore_perf_stat_t 实例
     */
  virtual objectstore_perf_stat_t get_cur_stats() = 0;

  /**
   * Fetch Object Store performance counters.
   * 获取 OS 中的 perf_counters 对象指针。
   *
   * This appears to be called with nothing locked.
   */
  virtual const PerfCounters* get_perf_counters() const = 0;

  /**
   * a collection also orders transactions
   *
   * Any transactions queued under a given collection will be applied in
   * sequence.  Transactions queued under different collections may run
   * in parallel.
   *
   * ObjectStore users may get collection handles with open_collection() (or,
   * for bootstrapping a new collection, create_new_collection()).
   */
  struct CollectionImpl : public RefCountedObject {
    const coll_t cid;

    /// wait for any queued transactions to apply
    // block until any previous transactions are visible.  specifically,
    // collection_list and collection_empty need to reflect prior operations.
    virtual void flush() = 0;

    /**
     * Async flush_commit
     *
     * There are two cases:
     * 1) collection is currently idle: the method returns true.  c is
     *    not touched.
     * 2) collection is not idle: the method returns false and c is
     *    called asynchronously with a value of 0 once all transactions
     *    queued on this collection prior to the call have been applied
     *    and committed.
     */
    virtual bool flush_commit(Context *c) = 0;

    const coll_t &get_cid() {
      return cid;
    }
  protected:
    CollectionImpl() = delete;
    CollectionImpl(CephContext* cct, const coll_t& c) : RefCountedObject(cct), cid(c) {}
    ~CollectionImpl() = default;
  };
  using CollectionHandle = ceph::ref_t<CollectionImpl>;


  /*********************************
   *
   * Object Contents and semantics
   *
   * All ObjectStore objects are identified as a named object
   * (ghobject_t and hobject_t) in a named collection (coll_t).
   * ObjectStore operations support the creation, mutation, deletion
   * and enumeration of objects within a collection.  Enumeration is
   * in sorted key order (where keys are sorted by hash). Object names
   * are globally unique.
   *
   * Each object has four distinct parts: byte data, xattrs, omap_header
   * and omap entries.
   *
   * The data portion of an object is conceptually equivalent to a
   * file in a file system. Random and Partial access for both read
   * and write operations is required. The ability to have a sparse
   * implementation of the data portion of an object is beneficial for
   * some workloads, but not required. There is a system-wide limit on
   * the maximum size of an object, which is typically around 100 MB.
   *
   * Xattrs are equivalent to the extended attributes of file
   * systems. Xattrs are a std::set of key/value pairs.  Sub-value access
   * is not required. It is possible to enumerate the std::set of xattrs in
   * key order.  At the implementation level, xattrs are used
   * exclusively internal to Ceph and the implementer can expect the
   * total size of all of the xattrs on an object to be relatively
   * small, i.e., less than 64KB. Much of Ceph assumes that accessing
   * xattrs on temporally adjacent object accesses (recent past or
   * near future) is inexpensive.
   *
   * omap_header is a single blob of data. It can be read or written
   * in total.
   *
   * Omap entries are conceptually the same as xattrs
   * but in a different address space. In other words, you can have
   * the same key as an xattr and an omap entry and they have distinct
   * values. Enumeration of xattrs doesn't include omap entries and
   * vice versa. The size and access characteristics of omap entries
   * are very different from xattrs. In particular, the value portion
   * of an omap entry can be quite large (MBs).  More importantly, the
   * interface must support efficient range queries on omap entries even
   * when there are a large numbers of entries.
   *
   *********************************/

  /*******************************
   *
   * Collections
   *
   * A collection is simply a grouping of objects. Collections have
   * names (coll_t) and can be enumerated in order.  Like an
   * individual object, a collection also has a std::set of xattrs.
   *
   *
   */

  /**
   * @param ch 集合句柄，用于获取对应 collection
   * @param t 事务，封装一组 op 操作，可以是不同 hobj
   * @param op 
   * @param handle 
   * @return 0 for success, other for failure
   */
  int queue_transaction(CollectionHandle& ch,
            Transaction&& t,
            TrackedOpRef op = TrackedOpRef(),
            ThreadPool::TPHandle *handle = NULL) {
    std::vector<Transaction> tls;
    tls.push_back(std::move(t));
    return queue_transactions(ch, tls, op, handle);
  }

  /*
  queue_transactions是所有ObjectStore更新操作的接口。更新相关的操作（例如创建
  一个对象，修改属性，写数据等）都是以事务的方式提交给ObjectStore，该函数被重载
  成各种不同的接口。其参数为：
  ● list<Transaction*>& tls要提交的事务，或者事务的列表。
  */
  virtual int queue_transactions(
    CollectionHandle& ch, std::vector<Transaction>& tls,
    TrackedOpRef op = TrackedOpRef(),
    ThreadPool::TPHandle *handle = NULL) = 0;


 public:
  ObjectStore(CephContext* cct,
          const std::string& path_) : path(path_), cct(cct) {}
  virtual ~ObjectStore() {}

  // no copying
  explicit ObjectStore(const ObjectStore& o) = delete;
  const ObjectStore& operator=(const ObjectStore& o) = delete;

  // versioning 仅在 FileStore 中使用
  virtual int upgrade() {
    return 0;
  }

  //打印 OS 的使用情况，可以通过 OSD 使用 dump_objectstore_kv_stats 命令调用。
  //ceph daemon /var/run/ceph/ceph-osd.0.asok dump_objectstore_kv_stats
  virtual void get_db_statistics(ceph::Formatter *f) { }

  //generate_db_histogram @param f 输出流
  //ceph daemon /var/run/ceph/ceph-osd.0.asok calc_objectstore_db_histogram
  virtual void generate_db_histogram(ceph::Formatter *f) { }

  //清空 onode 和 buffer 缓存，因为缓存从磁盘读取，所以也不需要刷新到磁盘，直接清空缓存即可完成动作。
  /**
   * @param os 输出流
   * @return 0 for success, other for failure
  */
  virtual int flush_cache(std::ostream *os = NULL) { return -1; }

  /**
   * @param f, os 输出流
   * @return
   */
  virtual void dump_perf_counters(ceph::Formatter *f) {}
  virtual void dump_cache_stats(ceph::Formatter *f) {}
  virtual void dump_cache_stats(std::ostream& os) {}

  /** 返回 OS 类型，如 bluestore。
   * @param
   * @return string 类型字符串
   */
  virtual std::string get_type() = 0;

  // mgmt mount 前的预测试，在 OSD::pre_init()中被调用，如若发生错误，则说明 ObjectStore::mount() 不可用。
  virtual bool test_mount_in_use() = 0;

  //加载objectsotre相关的系统信息, 挂载/卸载 OS。在 mkfs() 之后使用。
  virtual int mount() = 0;
  virtual int umount() = 0;

  //对 OS 进行检查或者修复。
  /**
   * @param deep true for FSCK_DEEP, false for FSCK_REGULAR
   * @return 0 for success, other for false
   */
  virtual int fsck(bool deep) {
    return -EOPNOTSUPP;
  }
  virtual int repair(bool deep) {
    return -EOPNOTSUPP;
  }
  virtual int quick_fix() {
    return -EOPNOTSUPP;
  }
  
  /*
  开启缓存。BlueStore 支持自己管理 onode 和 buffer 缓存。此函数用于创建缓存实例，num 是创建的总实例数量。在《Ceph之rados设计原理与实现》p101 页介绍了每个 BlueStore 包含多个 Cache 实例，每个 OSD 相应地会设置多个 PG 工作队列，BlueStore 中的 Cache 实例个数与之对应。
  
  此函数在 BlueStore create() 时被调用。
  */
  /**
   * @param num cache 实例总数
   * @return 
   */
  virtual void set_cache_shards(unsigned num) { }

  /** BlueStore中支持任何长度的 name，所以此函数在 BLueStore 中始终返回0。
   * Returns 0 if the hobject is valid, -error otherwise
   *
   * Errors:
   * -ENAMETOOLONG: locator/namespace/name too large
   */
  virtual int validate_hobject_key(const hobject_t &obj) const = 0;

  //BlueStore 内部对 xattr name 的长度也没有真正限制。这里返回 256。
  virtual unsigned get_max_attr_name_length() = 0;

  //创建objectstore相关的系统信息, OS 格式化。在 create() 之后使用。内部提供检测机制，支持对一个 OSD 目录多次调用 mkfs()。
  virtual int mkfs() = 0;  // wipe

  //BlueStore 不支持。
  virtual int mkjournal() = 0; // journal only
  virtual bool needs_journal() = 0;  //< requires a journal
  virtual bool wants_journal() = 0;  //< prefers a journal
  virtual bool allows_journal() = 0; //< allows a journal

  virtual void prepare_for_fast_shutdown() {}
  virtual bool has_null_manager() const { return false; }

  // return store min allocation size, if applicable
  //返回最小分配空间。默认 4KB。支持配置文件修改：bluestore_min_alloc_size，bluestore_min_alloc_size_hdd，bluestore_min_alloc_size_ssd。
  /**
   * @param 
   * @return uint64_t 最小分配空间的字节长度
   */
  virtual uint64_t get_min_alloc_size() const {
    return 0;
  }

  /// enumerate hardware devices (by 'devname', e.g., 'sda' as in /sys/block/sda)
  // 枚举所有磁盘设备。
  /**
  * @param devls 记录所有磁盘设备的位置
  * @return 0 for success, other for failure
  */
  // ceph daemon /var/run/ceph/ceph-osd.0.asok list_devices
  virtual int get_devices(std::set<std::string> *devls) {
    return -EOPNOTSUPP;
  }

  /// true if a txn is readable immediately after it is queued.
  //BlueStore 不支持。
  virtual bool is_sync_onreadable() const {
    return true;
  }

  /**
   * is_rotational
   *
   * Check whether store is backed by a rotational (HDD) or non-rotational
   * (SSD) device.
   *
   * This must be usable *before* the store is mounted.
   *
   * @return true for HDD, false for SSD
   */
   //验证 SLOW 设备是 HDD 还是 SSD，ture 为 HDD，false 为 SSD。
  virtual bool is_rotational() {
    return true;
  }

  /**
   * is_journal_rotational
   *
   * Check whether journal is backed by a rotational (HDD) or non-rotational
   * (SSD) device.
   *
   *
   * @return true for HDD, false for SSD
   */
   //BlueStore 中检查 WAL 设备是 HDD 还是 SSD，true 为 HDD，false 为 SSD。
  virtual bool is_journal_rotational() {
    return true;
  }

  //调用 is_rotational() 函数，查询设备的类别：HDD 或者 SSD
  virtual std::string get_default_device_class() {
    return is_rotational() ? "hdd" : "ssd";
  }

  //暂不可用
  virtual int get_numa_node(
    int *numa_node,
    std::set<int> *nodes,
    std::set<std::string> *failed) {
    return -EOPNOTSUPP;
  }

  //BlueStore 不支持
  virtual bool can_sort_nibblewise() {
    return false;   // assume a backend cannot, unless it says otherwise
  }

  //获取objectstore系统信息
  //可以使用 ceph-objectstore-tool 工具查看 statfs 信息
  // ceph-objectstore-tool --data-path /root/ceph/build/dev/osd0/ --op statfs --no-mon-config
  virtual int statfs(struct store_statfs_t *buf,
             osd_alert_list_t* alerts = nullptr) = 0;

  //获取池文件系统信息
  virtual int pool_statfs(uint64_t pool_id, struct store_statfs_t *buf,
              bool *per_pool_omap) = 0;

  virtual void collect_metadata(std::map<std::string,std::string> *pm) { }

  /**
   * write_meta - write a simple configuration key out-of-band
   *
   * Write a simple key/value pair for basic store configuration
   * (e.g., a uuid or magic number) to an unopened/unmounted store.
   * The default implementation writes this to a plaintext file in the
   * path.
   *
   * A newline is appended.
   *
   * @param key key name (e.g., "fsid")
   * @param value value (e.g., a uuid rendered as a std::string)
   * @returns 0 for success, or an error code
   */
   /*
   BlueStore 中向块设备的超级块写入元数据，以键值对形式。
   
   注：BlueStore 的超级块是 SLOW 设备的第一个 4KB 块。BlueFS 的超级块是 DB 设备（当 DB 不存在时，使用 SLOW设备）的第二个 4KB 块。
   可以使用多种工具查看，这里给出 ceph-objectstore-tool 工具的查看命令：
   ceph-objectstore-tool --data-path /var/lib/ceph/osd/ceph-0/ --op dump-super
   */
  virtual int write_meta(const std::string& key,
             const std::string& value);

  /**
   * read_meta - read a simple configuration key out-of-band
   *
   * Read a simple key value to an unopened/mounted store.
   *
   * Trailing whitespace is stripped off.
   *
   * @param key key name
   * @param value pointer to value std::string
   * @returns 0 for success, or an error code
   */
   //读取超级块信息
  virtual int read_meta(const std::string& key,
            std::string *value);

  /**
   * get ideal max value for collection_list()
   *
   * default to some arbitrary values; the implementation will override.
   */
  virtual int get_ideal_list_max() { return 64; }


  /**
   * get a collection handle
   *
   * Provide a trivial handle as a default to avoid converting legacy
   * implementations.
   */
   //获取 collection。BlueStore 会查询 kvdb 中前缀为 C 的所有 kv 键值对，找到 key 值匹配的集合并返回，若未在 kvdb 命中，也会返回一个 colleciton 指针。
    /**
     * get a collection handle
     *
     * Provide a trivial handle as a default to avoid converting legacy
     * implementations.
     *
     * @param cid 集合id、类型的包装
     * @return CollectionHandle 集合句柄
     */
  virtual CollectionHandle open_collection(const coll_t &cid) = 0;

  /**
   * get a collection handle for a soon-to-be-created collection
   *
   * This handle must be used by queue_transaction that includes a
   * create_collection call in order to become valid.  It will become the
   * reference to the created collection.
   */
  // 创建一个集合，实际就是把集合写入 kvdb，此操作需要通过 queue_transaction() 才能生效。
  virtual CollectionHandle create_new_collection(const coll_t &cid) = 0;

  /**
   * std::set ContextQueue for a collection
   *
   * After that, oncommits of Transaction will queue into commit_queue.
   * And osd ShardThread will call oncommits.
   */
   //为 collection 设置一个 on_commit 回调函数队列，每个 collection 只有一个该队列。在 /src/osd/OSD.cc 中被调用，只在新建集合、载入集合、分裂集合时才会创建。
  virtual void set_collection_commit_queue(const coll_t &cid, ContextQueue *commit_queue) = 0;

  /**
   * Synchronous read operations
   */

  /**
   * exists -- Test for existence of object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @returns true if object exists, false otherwise
   */
   // 判断集合中是否存在该对象。
  virtual bool exists(CollectionHandle& c, const ghobject_t& oid) = 0;
  /**
   * set_collection_opts -- std::set pool options for a collectioninformation for an object
   *
   * @param cid collection
   * @param opts new collection options
   * @returns 0 on success, negative error code on failure.
   */
   //设置存储池 pool 选项。在 src/osd/PG.cc 中被调用。
  virtual int set_collection_opts(
    CollectionHandle& c,
    const pool_opts_t& opts) = 0;

  /**
   * stat -- get information for an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param st output information for the object
   * @param allow_eio if false, assert on -EIO operation failure
   * @returns 0 on success, negative error code on failure.
   */
   //获取对象文件属性信息。stat 信息并非全部填写，而是只获取部分。
  virtual int stat(
    CollectionHandle &c,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio = false) = 0;
  /**
   * read -- read a byte range of data from an object
   *
   * Note: if reading from an offset past the end of the object, we
   * return 0 (not, say, -EINVAL).
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param offset location offset of first byte to be read
   * @param len number of bytes to be read
   * @param bl output ceph::buffer::list
   * @param op_flags is CEPH_OSD_OP_FLAG_*
   * @returns number of bytes read on success, or negative error code on failure.
   */
   // 读取对象数据，可以设置 offset 和 length，默认为读取整个对象。
   virtual int read(
     CollectionHandle &c,
     const ghobject_t& oid,
     uint64_t offset,
     size_t len,
     ceph::buffer::list& bl,
     uint32_t op_flags = 0) = 0;

  /**
   * fiemap -- get extent std::map of data of an object
   *
   * Returns an encoded std::map of the extents of an object's data portion
   * (std::map<offset,size>).
   *
   * A non-enlightened implementation is free to return the extent (offset, len)
   * as the sole extent.
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param offset location offset of first byte to be read
   * @param len number of bytes to be read
   * @param bl output ceph::buffer::list for extent std::map information.
   * @returns 0 on success, negative error code on failure.
   */
   /*
   分段加载 extent_map，为了支持 readv() 函数。
   
   此函数作用是把 object 对应范围的 extent_map 读取到内存中。返回一个 set，保存了读取的范围。
   extent_map 在 BlueStore 中是分片保存在磁盘上，因此需要读取对应分片。此函数可以把一定范围的对象转为 分片范围，读取到内存中。
   
   注意：返回的只是读取范围 [start, end]，并不是 extent_map 内容。
   */
   virtual int fiemap(CollectionHandle& c, const ghobject_t& oid,
              uint64_t offset, size_t len, ceph::buffer::list& bl) = 0;
   virtual int fiemap(CollectionHandle& c, const ghobject_t& oid,
              uint64_t offset, size_t len, std::map<uint64_t, uint64_t>& destmap) = 0;

  /**
   * readv -- read specfic intervals from an object;
   * caller must call fiemap to fill in the extent-map first.
   *
   * Note: if reading from an offset past the end of the object, we
   * return 0 (not, say, -EINVAL). Also the default version of readv
   * reads each extent separately synchronously, which can become horribly
   * inefficient if the physical layout of the pushing object get massively
   * fragmented and hence should be overridden by any real os that
   * cares about the performance..
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param m intervals to be read
   * @param bl output ceph::buffer::list
   * @param op_flags is CEPH_OSD_OP_FLAG_*
   * @returns number of bytes read on success, or negative error code on failure.
   */
   //同 read() 类似。区别在于 read() 只能读取一段数据，readv() 支持读取多段数据。
   virtual int readv(
     CollectionHandle &c,
     const ghobject_t& oid,
     interval_set<uint64_t>& m,
     ceph::buffer::list& bl,
     uint32_t op_flags = 0) {
     int total = 0;
     for (auto p = m.begin(); p != m.end(); p++) {
       ceph::buffer::list t;
       int r = read(c, oid, p.get_start(), p.get_len(), t, op_flags);
       if (r < 0)
         return r;
       total += r;
       // prune fiemap, if necessary
       if (p.get_len() != t.length()) {
          auto save = p++;
          if (t.length() == 0) {
            m.erase(save); // Remove this empty interval
          } else {
            save.set_len(t.length()); // fix interval length
            bl.claim_append(t);
          }
          // Remove any other follow-up intervals present too
          while (p != m.end()) {
            save = p++;
            m.erase(save);
          }
          break;
       }
       bl.claim_append(t);
     }
     return total;
   }

  /**
   * dump_onode -- dumps onode metadata in human readable form,
     intended primiarily for debugging
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param section_name section name to create and print under
   * @param f Formatter class instance to print to
   * @returns 0 on success, negative error code on failure.
   */
  //目前仅在 ceph-objectstore-tool 中被调用到。
  // ceph-objectstore-tool --data-path /var/lib/ceph/osd/ceph-0/ rbd_header.20e5ff0224ec0 dump
  virtual int dump_onode(
    CollectionHandle &c,
    const ghobject_t& oid,
    const std::string& section_name,
    ceph::Formatter *f) {
    return -ENOTSUP;
  }

  /**
   * getattr -- get an xattr of an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param name name of attr to read
   * @param value place to put output result.
   * @returns 0 on success, negative error code on failure.
   */
  /*
  查询对象的 xattr 属性。通过 kvdb 获取 onode 信息，xattr 保存在每个对象的 onode 中。
  
  支持多种返回类型：ptr、buffer、map<string, ptr>、map<string, list>。
  */
  virtual int getattr(CollectionHandle &c, const ghobject_t& oid,
              const char *name, ceph::buffer::ptr& value) = 0;

  /**
   * getattr -- get an xattr of an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param name name of attr to read
   * @param value place to put output result.
   * @returns 0 on success, negative error code on failure.
   */
  int getattr(
    CollectionHandle &c, const ghobject_t& oid,
    const std::string& name, ceph::buffer::list& value) {
    ceph::buffer::ptr bp;
    int r = getattr(c, oid, name.c_str(), bp);
    value.push_back(bp);
    return r;
  }

  /**
   * getattrs -- get all of the xattrs of an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param aset place to put output result.
   * @returns 0 on success, negative error code on failure.
   */
  virtual int getattrs(CollectionHandle &c, const ghobject_t& oid,
               std::map<std::string,ceph::buffer::ptr, std::less<>>& aset) = 0;

  /**
   * getattrs -- get all of the xattrs of an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param aset place to put output result.
   * @returns 0 on success, negative error code on failure.
   */
  int getattrs(CollectionHandle &c, const ghobject_t& oid,
           std::map<std::string,ceph::buffer::list,std::less<>>& aset) {
    std::map<std::string,ceph::buffer::ptr,std::less<>> bmap;
    int r = getattrs(c, oid, bmap);
    for (auto i = bmap.begin(); i != bmap.end(); ++i) {
      aset[i->first].append(i->second);
    }
    return r;
  }


  // collections

  /** 查询此 OSD 的所有集合。
   * list_collections -- get all of the collections known to this ObjectStore
   *
   * @param ls std::list of the collections in sorted order.
   * @returns 0 on success, negative error code on failure.
   */
  virtual int list_collections(std::vector<coll_t>& ls) = 0;

  /** 检查 OSD 中是否有该集合。
   * does a collection exist?
   *
   * @param c collection
   * @returns true if it exists, false otherwise
   */
  virtual bool collection_exists(const coll_t& c) = 0;

  /** 检查集合是否为空（没有对象）？
   * is a collection empty?
   *
   * @param c collection
   * @param empty true if the specified collection is empty, false otherwise
   * @returns 0 on success, negative error code on failure.
   */
  virtual int collection_empty(CollectionHandle& c, bool *empty) = 0;

  /**
   * return the number of significant bits of the coll_t::pgid.
   *
   * This should return what the last create_collection or split_collection
   * std::set.  A legacy backend may return -EAGAIN if the value is unavailable
   * (because we upgraded from an older version, e.g., FileStore).
   */

  /*
  对象在进行 crush 运算映射到某个 pg 时，因为 pg 的数量总是有限的，因此不需要
  对整个对象 id 进行 hash 映射，只需要取最后的 n 位（2^n = pg 数量），即 n 位表
  示对象通过 stable_mod 映射至 pg 时，其32位全精度哈希值（从最低位开始）有多少
  位是有效的。这一概念在《Ceph之Rados设计原理与实现》一书中P11 ~ P13页有详细介绍。
  */
  virtual int collection_bits(CollectionHandle& c) = 0;


  /** 列出集合中指定范围的对象。
   * std::list contents of a collection that fall in the range [start, end) and no more than a specified many result
   *
   * @param c collection
   * @param start list object that sort >= this value
   * @param end list objects that sort < this value
   * @param max return no more than this many results
   * @param seq return no objects with snap < seq
   * @param ls [out] result
   * @param next [out] next item sorts >= this value
   * @return zero on success, or negative error
   */
  virtual int collection_list(CollectionHandle &c,
                  const ghobject_t& start, const ghobject_t& end,
                  int max,
                  std::vector<ghobject_t> *ls, ghobject_t *next) = 0;

  virtual int collection_list_legacy(CollectionHandle &c,
                                     const ghobject_t& start,
                                     const ghobject_t& end, int max,
                                     std::vector<ghobject_t> *ls,
                                     ghobject_t *next) {
    return collection_list(c, start, end, max, ls, next);
  }

  /// OMAP
  /// Get omap contents
  /*
  查询指定对象的 omap 属性。
  
  omap 在kvdb 的保存形式为：prefix ： key ：value。每个对象的 omap 单独保存在 kvdb 中，前缀为 M。
  */
  virtual int omap_get(
    CollectionHandle &c,     ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    ceph::buffer::list *header,      ///< [out] omap header
    std::map<std::string, ceph::buffer::list> *out /// < [out] Key to value std::map
    ) = 0;

  /// Get omap header
  virtual int omap_get_header(
    CollectionHandle &c,     ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    ceph::buffer::list *header,      ///< [out] omap header
    bool allow_eio = false ///< [in] don't assert on eio
    ) = 0;

  /// Get keys defined on oid
  virtual int omap_get_keys(
    CollectionHandle &c,   ///< [in] Collection containing oid
    const ghobject_t &oid, ///< [in] Object containing omap
    std::set<std::string> *keys      ///< [out] Keys defined on oid
    ) = 0;

  /// Get key values
  virtual int omap_get_values(
    CollectionHandle &c,         ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const std::set<std::string> &keys,     ///< [in] Keys to get
    std::map<std::string, ceph::buffer::list> *out ///< [out] Returned keys and values
    ) = 0;

#ifdef WITH_SEASTAR
  virtual int omap_get_values(
    CollectionHandle &c,         ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const std::optional<std::string> &start_after,     ///< [in] Keys to get
    std::map<std::string, ceph::buffer::list> *out ///< [out] Returned keys and values
    ) = 0;
#endif

  /// Filters keys into out which are defined on oid
  virtual int omap_check_keys(
    CollectionHandle &c,     ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    const std::set<std::string> &keys, ///< [in] Keys to check
    std::set<std::string> *out         ///< [out] Subset of keys defined on oid
    ) = 0;

  /**
   * Returns an object map iterator
   *
   * Warning!  The returned iterator is an implicit lock on filestore
   * operations in c.  Do not use filestore methods on c while the returned
   * iterator is live.  (Filling in a transaction is no problem).
   *
   * @return iterator, null on error
   */
  virtual ObjectMap::ObjectMapIterator get_omap_iterator(
    CollectionHandle &c,   ///< [in] collection
    const ghobject_t &oid  ///< [in] object
    ) = 0;

  virtual int flush_journal() { return -EOPNOTSUPP; }

  virtual int dump_journal(std::ostream& out) { return -EOPNOTSUPP; }

  virtual int snapshot(const std::string& name) { return -EOPNOTSUPP; }

  /**
   * Set and get internal fsid for this instance. No external data is modified
   */
  virtual void set_fsid(uuid_d u) = 0;
  virtual uuid_d get_fsid() = 0;

  /**
  * Estimates additional disk space used by the specified amount of objects and caused by file allocation granularity and metadata store
  * - num objects - total (including witeouts) object count to measure used space for.
  */
  virtual uint64_t estimate_objects_overhead(uint64_t num_objects) = 0;


  // DEBUG
  virtual void inject_data_error(const ghobject_t &oid) {}
  virtual void inject_mdata_error(const ghobject_t &oid) {}

  virtual void compact() {}
  virtual bool has_builtin_csum() const {
    return false;
  }
};

#endif
