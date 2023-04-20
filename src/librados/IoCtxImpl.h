// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2012 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_LIBRADOS_IOCTXIMPL_H
#define CEPH_LIBRADOS_IOCTXIMPL_H

#include <atomic>

#include "common/Cond.h"
#include "common/ceph_mutex.h"
#include "common/snap_types.h"
#include "common/zipkin_trace.h"
#include "include/types.h"
#include "include/rados/librados.h"
#include "include/rados/librados.hpp"
#include "include/xlist.h"
#include "osd/osd_types.h"
#include "osdc/Objecter.h"

class RadosClient;


/*
类IoCtxImpl是pool相关的上下文信息，一个pool对应一个IoCtxImpl对象。librados中所有
关于IO操作的API都设计在librados::IoCtx中，接口的真正实现在IoCtxImpl中。可以在该pool
里创建，删除对象，完成对象数据读写等各种操作，包括同步和异步的实现。其处理过程都
比较简单，而且过程类似：
1）把请求封装成ObjectOperation类（该类定义在osdc/Objecter.h中）。
2）把相关的pool信息添加到里面，封装成Objecter::Op对像。
3）调用函数objecter->op_submit发送给相应的OSD，如果是同步操作，就等待操作
4）操作完成后，调用相应的回调函数。
完成。如果是异步操作，就不用等待，直接返回。当操作完成后，调用相应的回调函数通知。
*/

struct librados::IoCtxImpl {
  std::atomic<uint64_t> ref_cnt = { 0 };
  RadosClient *client = nullptr;
  int64_t poolid = 0;
  snapid_t snap_seq;
  ::SnapContext snapc;
  uint64_t assert_ver = 0;
  version_t last_objver = 0;
  uint32_t notify_timeout = 30;
  object_locator_t oloc;
  int extra_op_flags = 0;

  ceph::mutex aio_write_list_lock =
    ceph::make_mutex("librados::IoCtxImpl::aio_write_list_lock");
  ceph_tid_t aio_write_seq = 0;
  ceph::condition_variable aio_write_cond;
  xlist<AioCompletionImpl*> aio_write_list;
  std::map<ceph_tid_t, std::list<AioCompletionImpl*> > aio_write_waiters;

  Objecter *objecter = nullptr;

  IoCtxImpl();
  IoCtxImpl(RadosClient *c, Objecter *objecter,
	    int64_t poolid, snapid_t s);

  void dup(const IoCtxImpl& rhs) {
    // Copy everything except the ref count
    client = rhs.client;
    poolid = rhs.poolid;
    snap_seq = rhs.snap_seq;
    snapc = rhs.snapc;
    assert_ver = rhs.assert_ver;
    last_objver = rhs.last_objver;
    notify_timeout = rhs.notify_timeout;
    oloc = rhs.oloc;
    extra_op_flags = rhs.extra_op_flags;
    objecter = rhs.objecter;
  }

  void set_snap_read(snapid_t s);
  int set_snap_write_context(snapid_t seq, std::vector<snapid_t>& snaps);

  void get() {
    ref_cnt++;
  }

  void put() {
    if (--ref_cnt == 0)
      delete this;
  }

  void queue_aio_write(struct AioCompletionImpl *c);
  void complete_aio_write(struct AioCompletionImpl *c);
  void flush_aio_writes_async(AioCompletionImpl *c);
  void flush_aio_writes();

  int64_t get_id() {
    return poolid;
  }

  std::string get_cached_pool_name();

  int get_object_hash_position(const std::string& oid, uint32_t *hash_position);
  int get_object_pg_hash_position(const std::string& oid, uint32_t *pg_hash_position);

  ::ObjectOperation *prepare_assert_ops(::ObjectOperation *op);

  // snaps
  int snap_list(std::vector<uint64_t> *snaps);
  int snap_lookup(const char *name, uint64_t *snapid);
  int snap_get_name(uint64_t snapid, std::string *s);
  int snap_get_stamp(uint64_t snapid, time_t *t);
  int snap_create(const char* snapname);
  int selfmanaged_snap_create(uint64_t *snapid);
  void aio_selfmanaged_snap_create(uint64_t *snapid, AioCompletionImpl *c);
  int snap_remove(const char* snapname);
  int rollback(const object_t& oid, const char *snapName);
  int selfmanaged_snap_remove(uint64_t snapid);
  void aio_selfmanaged_snap_remove(uint64_t snapid, AioCompletionImpl *c);
  int selfmanaged_snap_rollback_object(const object_t& oid,
                                       ::SnapContext& snapc, uint64_t snapid);

  // io
  int nlist(Objecter::NListContext *context, int max_entries);
  uint32_t nlist_seek(Objecter::NListContext *context, uint32_t pos);
  uint32_t nlist_seek(Objecter::NListContext *context, const rados_object_list_cursor& cursor);
  rados_object_list_cursor nlist_get_cursor(Objecter::NListContext *context);
  void object_list_slice(
    const hobject_t start,
    const hobject_t finish,
    const size_t n,
    const size_t m,
    hobject_t *split_start,
    hobject_t *split_finish);

  int create(const object_t& oid, bool exclusive);
  int write(const object_t& oid, bufferlist& bl, size_t len, uint64_t off);
  int append(const object_t& oid, bufferlist& bl, size_t len);
  int write_full(const object_t& oid, bufferlist& bl);
  int writesame(const object_t& oid, bufferlist& bl,
		size_t write_len, uint64_t offset);
  int read(const object_t& oid, bufferlist& bl, size_t len, uint64_t off);
  int mapext(const object_t& oid, uint64_t off, size_t len,
	     std::map<uint64_t,uint64_t>& m);
  int sparse_read(const object_t& oid, std::map<uint64_t,uint64_t>& m,
		  bufferlist& bl, size_t len, uint64_t off);
  int checksum(const object_t& oid, uint8_t type, const bufferlist &init_value,
	       size_t len, uint64_t off, size_t chunk_size, bufferlist *pbl);
  int remove(const object_t& oid);
  int remove(const object_t& oid, int flags);
  int stat(const object_t& oid, uint64_t *psize, time_t *pmtime);
  int stat2(const object_t& oid, uint64_t *psize, struct timespec *pts);
  int trunc(const object_t& oid, uint64_t size);
  int cmpext(const object_t& oid, uint64_t off, bufferlist& cmp_bl);

  int tmap_update(const object_t& oid, bufferlist& cmdbl);

  int exec(const object_t& oid, const char *cls, const char *method, bufferlist& inbl, bufferlist& outbl);

  int getxattr(const object_t& oid, const char *name, bufferlist& bl);
  int setxattr(const object_t& oid, const char *name, bufferlist& bl);
  int getxattrs(const object_t& oid, std::map<std::string, bufferlist>& attrset);
  int rmxattr(const object_t& oid, const char *name);

  int operate(const object_t& oid, ::ObjectOperation *o, ceph::real_time *pmtime, int flags=0);
  int operate_read(const object_t& oid, ::ObjectOperation *o, bufferlist *pbl, int flags=0);
  int aio_operate(const object_t& oid, ::ObjectOperation *o,
		  AioCompletionImpl *c, const SnapContext& snap_context,
		  const ceph::real_time *pmtime, int flags,
		  const blkin_trace_info *trace_info = nullptr);
  int aio_operate_read(const object_t& oid, ::ObjectOperation *o,
		       AioCompletionImpl *c, int flags, bufferlist *pbl, const blkin_trace_info *trace_info = nullptr);

  struct C_aio_stat_Ack : public Context {
    librados::AioCompletionImpl *c;
    time_t *pmtime;
    ceph::real_time mtime;
    C_aio_stat_Ack(AioCompletionImpl *_c, time_t *pm);
    void finish(int r) override;
  };

  struct C_aio_stat2_Ack : public Context {
    librados::AioCompletionImpl *c;
    struct timespec *pts;
    ceph::real_time mtime;
    C_aio_stat2_Ack(AioCompletionImpl *_c, struct timespec *pts);
    void finish(int r) override;
  };

  struct C_aio_Complete : public Context {
#if defined(WITH_EVENTTRACE)
    object_t oid;
#endif
    AioCompletionImpl *c;
    explicit C_aio_Complete(AioCompletionImpl *_c);
    void finish(int r) override;
  };

  int aio_read(const object_t oid, AioCompletionImpl *c,
	       bufferlist *pbl, size_t len, uint64_t off, uint64_t snapid,
	       const blkin_trace_info *info = nullptr);
  int aio_read(object_t oid, AioCompletionImpl *c,
	       char *buf, size_t len, uint64_t off, uint64_t snapid,
	       const blkin_trace_info *info = nullptr);
  int aio_sparse_read(const object_t oid, AioCompletionImpl *c,
		      std::map<uint64_t,uint64_t> *m, bufferlist *data_bl,
		      size_t len, uint64_t off, uint64_t snapid);
  int aio_cmpext(const object_t& oid, AioCompletionImpl *c, uint64_t off,
		      bufferlist& cmp_bl);
  int aio_cmpext(const object_t& oid, AioCompletionImpl *c,
		      const char *cmp_buf, size_t cmp_len, uint64_t off);
  int aio_write(const object_t &oid, AioCompletionImpl *c,
		const bufferlist& bl, size_t len, uint64_t off,
		const blkin_trace_info *info = nullptr);
  int aio_append(const object_t &oid, AioCompletionImpl *c,
		 const bufferlist& bl, size_t len);
  int aio_write_full(const object_t &oid, AioCompletionImpl *c,
		     const bufferlist& bl);
  int aio_writesame(const object_t &oid, AioCompletionImpl *c,
		    const bufferlist& bl, size_t write_len, uint64_t off);
  int aio_remove(const object_t &oid, AioCompletionImpl *c, int flags=0);
  int aio_exec(const object_t& oid, AioCompletionImpl *c, const char *cls,
	       const char *method, bufferlist& inbl, bufferlist *outbl);
  int aio_exec(const object_t& oid, AioCompletionImpl *c, const char *cls,
	       const char *method, bufferlist& inbl, char *buf, size_t out_len);
  int aio_stat(const object_t& oid, AioCompletionImpl *c, uint64_t *psize, time_t *pmtime);
  int aio_stat2(const object_t& oid, AioCompletionImpl *c, uint64_t *psize, struct timespec *pts);
  int aio_getxattr(const object_t& oid, AioCompletionImpl *c,
		   const char *name, bufferlist& bl);
  int aio_setxattr(const object_t& oid, AioCompletionImpl *c,
		   const char *name, bufferlist& bl);
  int aio_getxattrs(const object_t& oid, AioCompletionImpl *c,
		    std::map<std::string, bufferlist>& attrset);
  int aio_rmxattr(const object_t& oid, AioCompletionImpl *c,
		  const char *name);
  int aio_cancel(AioCompletionImpl *c);

  int hit_set_list(uint32_t hash, AioCompletionImpl *c,
		   std::list< std::pair<time_t, time_t> > *pls);
  int hit_set_get(uint32_t hash, AioCompletionImpl *c, time_t stamp,
		  bufferlist *pbl);

  int get_inconsistent_objects(const pg_t& pg,
			       const librados::object_id_t& start_after,
			       uint64_t max_to_get,
			       AioCompletionImpl *c,
			       std::vector<inconsistent_obj_t>* objects,
			       uint32_t* interval);

  int get_inconsistent_snapsets(const pg_t& pg,
				const librados::object_id_t& start_after,
				uint64_t max_to_get,
				AioCompletionImpl *c,
				std::vector<inconsistent_snapset_t>* snapsets,
				uint32_t* interval);

  void set_sync_op_version(version_t ver);
  int watch(const object_t& oid, uint64_t *cookie, librados::WatchCtx *ctx,
	    librados::WatchCtx2 *ctx2, bool internal = false);
  int watch(const object_t& oid, uint64_t *cookie, librados::WatchCtx *ctx,
	    librados::WatchCtx2 *ctx2, uint32_t timeout, bool internal = false);
  int aio_watch(const object_t& oid, AioCompletionImpl *c, uint64_t *cookie,
                librados::WatchCtx *ctx, librados::WatchCtx2 *ctx2,
                bool internal = false);
  int aio_watch(const object_t& oid, AioCompletionImpl *c, uint64_t *cookie,
                librados::WatchCtx *ctx, librados::WatchCtx2 *ctx2,
                uint32_t timeout, bool internal = false);
  int watch_check(uint64_t cookie);
  int unwatch(uint64_t cookie);
  int aio_unwatch(uint64_t cookie, AioCompletionImpl *c);
  int notify(const object_t& oid, bufferlist& bl, uint64_t timeout_ms,
	     bufferlist *preplybl, char **preply_buf, size_t *preply_buf_len);
  int notify_ack(const object_t& oid, uint64_t notify_id, uint64_t cookie,
		 bufferlist& bl);
  int aio_notify(const object_t& oid, AioCompletionImpl *c, bufferlist& bl,
                 uint64_t timeout_ms, bufferlist *preplybl, char **preply_buf,
                 size_t *preply_buf_len);

  int set_alloc_hint(const object_t& oid,
                     uint64_t expected_object_size,
                     uint64_t expected_write_size,
		     uint32_t flags);

  version_t last_version();
  void set_assert_version(uint64_t ver);
  void set_notify_timeout(uint32_t timeout);

  int cache_pin(const object_t& oid);
  int cache_unpin(const object_t& oid);

  int application_enable(const std::string& app_name, bool force);
  void application_enable_async(const std::string& app_name, bool force,
                                PoolAsyncCompletionImpl *c);
  int application_list(std::set<std::string> *app_names);
  int application_metadata_get(const std::string& app_name,
                               const std::string &key,
                               std::string* value);
  int application_metadata_set(const std::string& app_name,
                               const std::string &key,
                               const std::string& value);
  int application_metadata_remove(const std::string& app_name,
                                  const std::string &key);
  int application_metadata_list(const std::string& app_name,
                                std::map<std::string, std::string> *values);

};

#endif
