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

#ifndef CEPH_TIMER_H
#define CEPH_TIMER_H

#include <map>
#include "include/common_fwd.h"
#include "ceph_time.h"
#include "ceph_mutex.h"
#include "fair_mutex.h"
#include <condition_variable>

class Context;

template <class Mutex> class CommonSafeTimerThread;

//定时器的功能
template <class Mutex>
class CommonSafeTimer
{
  CephContext *cct;
  Mutex& lock;
  std::condition_variable_any cond;

  //是否是safe_callbacks
  bool safe_callbacks;

  //定时器执行线程
  friend class CommonSafeTimerThread<Mutex>;
  class CommonSafeTimerThread<Mutex> *thread;

  /*
  检查scheduler中的任务是否到期，其循环检查任务是否到期执行。任务在schedule
  中是按照时间升序排列的。首先检查，如果第一任务没有到时间，后面的任务就不用检
  查了，直接终止循环。如果第一任务到了定时时间，就调用callback函数执行，如果是
  safe_callbacks，就必须在获取lock的情况下执行Callback任务。
  */
  void timer_thread();
  void _shutdown();

  using clock_t = ceph::mono_clock;
  using scheduled_map_t = std::multimap<clock_t::time_point, Context*>;
  scheduled_map_t schedule;
  using event_lookup_map_t = std::map<Context*, scheduled_map_t::iterator>;
  event_lookup_map_t events;
  bool stopping;

  void dump(const char *caller = 0) const;

public:
  // This class isn't supposed to be copied
  CommonSafeTimer(const CommonSafeTimer&) = delete;
  CommonSafeTimer& operator=(const CommonSafeTimer&) = delete;

  /* Safe callbacks determines whether callbacks are called with the lock
   * held.
   *
   * safe_callbacks = true (default option) guarantees that a cancelled
   * event's callback will never be called.
   *
   * Under some circumstances, holding the lock can cause lock cycles.
   * If you are able to relax requirements on cancelled callbacks, then
   * setting safe_callbacks = false eliminates the lock cycle issue.
   * */
  CommonSafeTimer(CephContext *cct, Mutex &l, bool safe_callbacks=true);
  virtual ~CommonSafeTimer();

  /* Call with the event_lock UNLOCKED.
   *
   * Cancel all events and stop the timer thread.
   *
   * If there are any events that still have to run, they will need to take
   * the event_lock first. */
  void init();
  void shutdown();

  /* Schedule an event in the future
   * Call with the event_lock LOCKED */
  Context* add_event_after(ceph::timespan duration, Context *callback);
  Context* add_event_after(double seconds, Context *callback);

  //添加定时任务的命令
  Context* add_event_at(clock_t::time_point when, Context *callback);
  Context* add_event_at(ceph::real_clock::time_point when, Context *callback);
  /* Cancel an event.
   * Call with the event_lock LOCKED
   *
   * Returns true if the callback was cancelled.
   * Returns false if you never added the callback in the first place.
   * 取消定时任务的命令
   */
  bool cancel_event(Context *callback);

  /* Cancel all events.
   * Call with the event_lock LOCKED
   *
   * When this function returns, all events have been cancelled, and there are no
   * more in progress.
   */
  void cancel_all_events();

};

extern template class CommonSafeTimer<ceph::mutex>;
extern template class CommonSafeTimer<ceph::fair_mutex>;
using SafeTimer = class CommonSafeTimer<ceph::mutex>;

#endif
