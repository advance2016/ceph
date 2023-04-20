// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 XSKY <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_MSG_ASYNC_POSIXSTACK_H
#define CEPH_MSG_ASYNC_POSIXSTACK_H

#include <thread>

#include "msg/msg_types.h"
#include "msg/async/net_handler.h"

#include "Stack.h"

/*
类PosixWorker实现了 Worker接口。
Worker可以理解为工作者线程，其一一对应一个thread线程。为了兼容其它协议的设计，
对应线程定义在了PosixNetworkStack类里。通过上述分析可知，一个Worker对应一个线程，
同时对应一个 事件处理中心EventCenter类。
*/
class PosixWorker : public Worker {
  ceph::NetHandler net;
  void initialize() override;
 public:
  PosixWorker(CephContext *c, unsigned i)
      : Worker(c, i), net(c) {}
  /*
  实现了Server端的sock的功能：底层调用了NetHandler的功能，实现了socket 的 bind ，
  listen等操作，最后返回ServerSocket对象。
  */
  int listen(entity_addr_t &sa,
	     unsigned addr_slot,
	     const SocketOptions &opt,
	     ServerSocket *socks) override;

  /*
  实现了主动连接请求。返回ConnectedSocket对象。
  */
  int connect(const entity_addr_t &addr, const SocketOptions &opts, ConnectedSocket *socket) override;
};

class PosixNetworkStack : public NetworkStack {
  //线程池
  std::vector<std::thread> threads;

  virtual Worker* create_worker(CephContext *c, unsigned worker_id) override {
    return new PosixWorker(c, worker_id);
  }

 public:
  explicit PosixNetworkStack(CephContext *c);

  void spawn_worker(std::function<void ()> &&func) override {
    threads.emplace_back(std::move(func));
  }
  void join_worker(unsigned i) override {
    ceph_assert(threads.size() > i && threads[i].joinable());
    threads[i].join();
  }
};

#endif //CEPH_MSG_ASYNC_POSIXSTACK_H
