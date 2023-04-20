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

#ifndef CEPH_MSG_EVENT_H
#define CEPH_MSG_EVENT_H

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#endif

// We use epoll, kqueue, evport, select in descending order by performance.
#if defined(__linux__)
#define HAVE_EPOLL 1
#endif

#if (defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#define HAVE_KQUEUE 1
#endif

#ifdef __sun
#include <sys/feature_tests.h>
#ifdef _DTRACE_VERSION
#define HAVE_EVPORT 1
#endif
#endif

#include <atomic>
#include <mutex>
#include <condition_variable>

#include "common/ceph_time.h"
#include "common/dout.h"
#include "net_handler.h"

#define EVENT_NONE 0
#define EVENT_READABLE 1
#define EVENT_WRITABLE 2

class EventCenter;

class EventCallback {

 public:
  virtual void do_request(uint64_t fd_or_id) = 0;
  virtual ~EventCallback() {}       // we want a virtual destructor!!!
};

typedef EventCallback* EventCallbackRef;

struct FiredFileEvent {
  int fd;
  int mask;
};

/*
 * EventDriver is a wrap of event mechanisms depends on different OS.
 * For example, Linux will use epoll(2), BSD will use kqueue(2) and select will
 * be used for worst condition.
 * EventDriver是一个抽象的接口，定义了添加事件监听，删除事件监听，获取触发的事件的接口。
 * 针对不同的IO多路复用机制，实现了不同的类。SelectDriver实现了select的方式。
 * EpollDriver实现了epoll的网络事件处理方式。KqueueDriver是FreeBSD实现kqueue事件处理模型。
 */
class EventDriver {
 public:
  virtual ~EventDriver() {}       // we want a virtual destructor!!!
  virtual int init(EventCenter *center, int nevent) = 0;
  virtual int add_event(int fd, int cur_mask, int mask) = 0;
  virtual int del_event(int fd, int cur_mask, int del_mask) = 0;
  virtual int event_wait(std::vector<FiredFileEvent> &fired_events, struct timeval *tp) = 0;
  virtual int resize_events(int newsize) = 0;
  virtual bool need_wakeup() { return true; }
};

/*
 * EventCenter maintain a set of file descriptor and handle registered events.
 * 保存所有事件，并提供了处理事件的相关函数。
 */
class EventCenter {
 public:
  // should be enough; 进程的EventCenter数量
  static const int MAX_EVENTCENTER = 24;

 private:
  using clock_type = ceph::coarse_mono_clock;

  struct AssociatedCenters {
    EventCenter *centers[MAX_EVENTCENTER];
    AssociatedCenters() {
      // FIPS zeroization audit 20191115: this memset is not security related.
      memset(centers, 0, MAX_EVENTCENTER * sizeof(EventCenter*));
    }
  };

  //FileEvent事件，也就是socket对应的事件。
  struct FileEvent {
    int mask;  //标志
    EventCallbackRef read_cb;  //处理读操作的回调函数
    EventCallbackRef write_cb;  //处理写操作的回调函数
    FileEvent(): mask(0), read_cb(NULL), write_cb(NULL) {}
  };

  struct TimeEvent {
    uint64_t id;  //时间事件的ID号
    EventCallbackRef time_cb;  //事件处理的回调函数

    TimeEvent(): id(0), time_cb(NULL) {}
  };

 public:
  /**
     * A Poller object is invoked once each time through the dispatcher's
     * inner polling loop.
     * 类Poller 用于轮询事件，主要用于DPDK 模式。在PosixStack模式里没有用。
     */
  class Poller {
   public:
    explicit Poller(EventCenter* center, const std::string& pollerName);
    virtual ~Poller();

    /**
     * This method is defined by a subclass and invoked once by the
     * center during each pass through its inner polling loop.
     *
     * \return
     *      1 means that this poller did useful work during this call.
     *      0 means that the poller found no work to do.
     */
    virtual int poll() = 0;

   private:
    /// The EventCenter object that owns this Poller.  NULL means the
    /// EventCenter has been deleted.
    EventCenter* owner;

    /// Human-readable string name given to the poller to make it
    /// easy to identify for debugging. For most pollers just passing
    /// in the subclass name probably makes sense.
    std::string poller_name;

    /// Index of this Poller in EventCenter::pollers.  Allows deletion
    /// without having to scan all the entries in pollers. -1 means
    /// this poller isn't currently in EventCenter::pollers (happens
    /// after EventCenter::reset).
    int slot;
  };

 private:
  CephContext *cct;
  std::string type;
  int nevent;
  // Used only to external event
  // 归属的Worker线程
  pthread_t owner = 0;
  //外部事件
  std::mutex external_lock;

  //外部事件个数
  std::atomic_ulong external_num_events;
  std::deque<EventCallbackRef> external_events;
  
  //socket事件， 其下标是socket对应的fd
  std::vector<FileEvent> file_events;

  //底层事件监控机制, 异步机制抽象类对象，在EventCenter::init()中根据配置确定，
  // 比如支持EPOLL，就会使用EpollDriver
  EventDriver *driver;

  //时间事件 [expire time point， TimeEvent]
  std::multimap<clock_type::time_point, TimeEvent> time_events;
  // Keeps track of all of the pollers currently defined.  We don't
  // use an intrusive list here because it isn't reentrant: we need
  // to add/remove elements while the center is traversing the list.
  std::vector<Poller*> pollers;

  //时间事件的map [id，  iterator of [expire time point，time_event]]
  std::map<uint64_t, std::multimap<clock_type::time_point, TimeEvent>::iterator> event_map;
  uint64_t time_event_next_id;

  //触发执行外部事件的fd
  int notify_receive_fd;
  int notify_send_fd;
  ceph::NetHandler net;

  // notify_receive_fd的READABLE时的handler，作为external events在file events中
  // 的代理，这个handler只是将字符从管道中取出而已
  EventCallbackRef notify_handler;
  unsigned center_id;
  AssociatedCenters *global_centers = nullptr;

  int process_time_events();
  FileEvent *_get_file_event(int fd) {
    ceph_assert(fd < nevent);
    return &file_events[fd];
  }

 public:
  explicit EventCenter(CephContext *c):
    cct(c), nevent(0),
    external_num_events(0),
    driver(NULL), time_event_next_id(1),
    notify_receive_fd(-1), notify_send_fd(-1), net(c),
    notify_handler(NULL), center_id(0) { }
  ~EventCenter();
  std::ostream& _event_prefix(std::ostream *_dout);

  int init(int nevent, unsigned center_id, const std::string &type);
  void set_owner();
  pthread_t get_owner() const { return owner; }
  unsigned get_id() const { return center_id; }

  EventDriver *get_driver() { return driver; }

  // Used by internal thread
  //创建file event
  int create_file_event(int fd, int mask, EventCallbackRef ctxt);

  //创建time event
  uint64_t create_time_event(uint64_t milliseconds, EventCallbackRef ctxt);

  //删除file event
  void delete_file_event(int fd, int mask);

  //删除 time event
  void delete_time_event(uint64_t id);

  //处理事件
  int process_events(unsigned timeout_microseconds, ceph::timespan *working_dur = nullptr);

  //唤醒Worker，本质就是写写fd，这样读fd可读，线程得以唤醒
  void wakeup();

  // Used by external thread
  //直接投递EventCallback类型的事件处理函数
  void dispatch_event_external(EventCallbackRef e);

  //判断调用者是Worker线程
  inline bool in_thread() const {
    return pthread_equal(pthread_self(), owner);
  }

 private:
  template <typename func>
  class C_submit_event : public EventCallback {
    std::mutex lock;
    std::condition_variable cond;
    bool done = false;
    func f;
    bool nonwait;
   public:
    C_submit_event(func &&_f, bool nowait)
      : f(std::move(_f)), nonwait(nowait) {}
    void do_request(uint64_t id) override {
      f();
      lock.lock();
      cond.notify_all();
      done = true;
      bool del = nonwait;
      lock.unlock();
      if (del)
        delete this;
    }
    void wait() {
      ceph_assert(!nonwait);
      std::unique_lock<std::mutex> l(lock);
      while (!done)
        cond.wait(l);
    }
  };

 public:
  //处理func类型的事件处理函数
  //使用f构造EventCallback
  template <typename func>
  void submit_to(int i, func &&f, bool always_async = false) {
    ceph_assert(i < MAX_EVENTCENTER && global_centers);
    EventCenter *c = global_centers->centers[i];
    ceph_assert(c);
    if (always_async) {
      C_submit_event<func> *event = new C_submit_event<func>(std::move(f), true);
      c->dispatch_event_external(event);
    } else if (c->in_thread()) { // c->in_thread()就是判断是否是自己的线程
      f();
      return;
    } else {
      C_submit_event<func> event(std::move(f), false); // 创建回调类

      //去唤醒epoll_wait然后去执行回调函数event->do_request
      c->dispatch_event_external(&event);
      event.wait();
    }
  };
};

#endif
