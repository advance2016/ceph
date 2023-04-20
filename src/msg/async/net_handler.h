// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 UnitedStack <haomai@unitedstack.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_COMMON_NET_UTILS_H
#define CEPH_COMMON_NET_UTILS_H
#include "common/config.h"

// 类NetHandler 封装了Socket的基本的功能。
namespace ceph {
  class NetHandler {
    int generic_connect(const entity_addr_t& addr, const entity_addr_t& bind_addr, bool nonblock);

    CephContext *cct;
   public:
    //创建socket
    int create_socket(int domain, bool reuse_addr=false);
    explicit NetHandler(CephContext *c): cct(c) {}
    
    //设置socket 为非阻塞
    int set_nonblock(int sd);

    //设置socket的选项：nodelay，buffer size
    int set_socket_options(int sd, bool nodelay, int size);

    //连接
    int connect(const entity_addr_t &addr, const entity_addr_t& bind_addr);
    
    /**
     * Try to reconnect the socket.
     *
     * @return    0         success
     *            > 0       just break, and wait for event
     *            < 0       need to goto fail
     * 重连
     */
    int reconnect(const entity_addr_t &addr, int sd);

    //非阻塞connect
    int nonblock_connect(const entity_addr_t &addr, const entity_addr_t& bind_addr);

    //设置优先级
    void set_priority(int sd, int priority, int domain);
  };
}

#endif
