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
#include <ctype.h>
#include <sstream>
#include "ObjectStore.h"
#include "common/Formatter.h"
#include "common/safe_io.h"

#ifndef  WITH_SEASTAR
#include "filestore/FileStore.h"
#endif
#include "memstore/MemStore.h"
#if defined(WITH_BLUESTORE)
#include "bluestore/BlueStore.h"
#endif
#ifndef WITH_SEASTAR
#include "kstore/KStore.h"
#endif

using std::string;

std::unique_ptr<ObjectStore> ObjectStore::create(
  CephContext *cct,
  const string& type,
  const string& data)
{
  if (type == "memstore") {
    return std::make_unique<MemStore>(cct, data);
  }
#if defined(WITH_BLUESTORE)
  if (type == "bluestore" || type == "random") {
    return std::make_unique<BlueStore>(cct, data);
  }
#endif
  return nullptr;
}

#ifndef WITH_SEASTAR
/**
* create - create an ObjectStore instance.
*
* This is invoked once at initialization time.
*
* @param type 存储引擎类型（BLueStore，FileStore）
* @param data OSD 数据路径，如/var/lib/ceph/osd0
* @param journal 日志路径，FileStore 需要提供，BlueStore 不需要提供
* @param flags which filestores should check if applicable，BlueStore 不需要提供
* @return ObjectStore 指针
*/
/*
cct=0x555557368000
, type="bluestore"
, data="/var/lib/ceph/osd/ceph-admin"
, journal="/var/lib/ceph/osd/ceph-admin/journal"
, flags=flags@entry=0
*/
std::unique_ptr<ObjectStore> ObjectStore::create(
  CephContext *cct,
  const string& type,
  const string& data,
  const string& journal,
  osflagbits_t flags)
{
  if (type == "filestore" || (type == "random" && rand() % 2)) {
    return std::make_unique<FileStore>(cct, data, journal, flags);
  }
  if (type == "kstore" &&
      cct->check_experimental_feature_enabled("kstore")) {
    return std::make_unique<KStore>(cct, data);
  }
  return create(cct, type, data);
}
#endif


/**
* 返回一个 OSD 的 fsid号。即 /var/lib/ceph/osd/fsid 文件中保存的 uuid 序列号
* probe a block device to learn the uuid of the owning OSD
*
* @param cct cct
* @param path OSD 路径
* @param fsid [out] OSD fsid 号
* @return 0 for success, other for failure
*/
int ObjectStore::probe_block_device_fsid(
  CephContext *cct,
  const string& path,
  uuid_d *fsid)
{
  int r;

#if defined(WITH_BLUESTORE)
  // first try bluestore -- it has a crc on its header and will fail
  // reliably.
  r = BlueStore::get_block_device_fsid(cct, path, fsid);
  if (r == 0) {
    lgeneric_dout(cct, 0) << __func__ << " " << path << " is bluestore, "
			  << *fsid << dendl;
    return r;
  }
#endif

#ifndef WITH_SEASTAR
  // okay, try FileStore (journal).
  r = FileStore::get_block_device_fsid(cct, path, fsid);
  if (r == 0) {
    lgeneric_dout(cct, 0) << __func__ << " " << path << " is filestore, "
			  << *fsid << dendl;
    return r;
  }
#endif

  return -EINVAL;
}

int ObjectStore::write_meta(const std::string& key,
			    const std::string& value)
{
  string v = value;
  v += "\n";
  int r = safe_write_file(path.c_str(), key.c_str(),
			  v.c_str(), v.length(), 0600);
  if (r < 0)
    return r;
  return 0;
}

int ObjectStore::read_meta(const std::string& key,
			   std::string *value)
{
  char buf[4096];
  int r = safe_read_file(path.c_str(), key.c_str(),
			 buf, sizeof(buf));
  if (r <= 0)
    return r;
  // drop trailing newlines
  while (r && isspace(buf[r-1])) {
    --r;
  }
  *value = string(buf, r);
  return 0;
}
