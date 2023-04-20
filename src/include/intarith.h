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

#ifndef CEPH_INTARITH_H
#define CEPH_INTARITH_H

#include <type_traits>

template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> div_round_up(T n, U d) {
  return (n + d - 1) / d;
}


template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> round_up_to(T n, U d) {
  return (n % d ? (n + d - n % d) : n);
}

template<typename T, typename U>
constexpr inline std::make_unsigned_t<std::common_type_t<T, U>> shift_round_up(T x, U y) {
  return (x + (1 << y) - 1) >> y;
}

/*
 * Wrapper to determine if value is a power of 2
 */
template<typename T>
constexpr inline bool isp2(T x) {
  return (x & (x - 1)) == 0;
}

/*
 * Wrappers for various sorts of alignment and rounding.  The "align" must
 * be a power of 2.  Often times it is a block, sector, or page.
 */

/*
 * return x rounded down to an align boundary
 * eg, p2align(1200, 1024) == 1024 (1*align)
 * eg, p2align(1024, 1024) == 1024 (1*align)
 * eg, p2align(0x1234, 0x100) == 0x1200 (0x12*align)
 * eg, p2align(0x5600, 0x100) == 0x5600 (0x56*align)
 p2align(512, 4096) = 0
 p2align(2048, 4096) = 0

 */
//返回往前最近按照alloc对齐的地址
template<typename T>
constexpr inline T p2align(T x, T align) {
  return x & -align;
}

/*
 * return x % (mod) align
 * eg, p2phase(0x1234, 0x100) == 0x34 (x-0x12*align)
 * eg, p2phase(0x5600, 0x100) == 0x00 (x-0x56*align)
 */
template<typename T>
constexpr inline T p2phase(T x, T align) {
  return x & (align - 1);
}

/*
 * return how much space is left in this block (but if it's perfectly
 * aligned, return 0).
 * eg, p2nphase(0x1234, 0x100) == 0xcc (0x13*align-x)
 * eg, p2nphase(0x5600, 0x100) == 0x00 (0x56*align-x)
 * p2nphase(512, 4096) = -512 & (4096 - 1) = 3584
   p2nphase(4608, 4096) = -4608 & (4096 - 1) = 3584
 */
template<typename T>
constexpr inline T p2nphase(T x, T align) {
  return -x & (align - 1);
}

/*
 * return x rounded up to an align boundary
 * eg, p2roundup(0x1234, 0x100) == 0x1300 (0x13*align)
 * eg, p2roundup(0x5600, 0x100) == 0x5600 (0x56*align)
 p2roundup(1, 4096) = 4096
 */
template<typename T>
constexpr inline T p2roundup(T x, T align) {
  return -(-x & -align);
}

// count trailing zeros.
// NOTE: the builtin is nondeterministic on 0 input
/* 
函数作用是返回输入数二进制表示从最低位开始(右起)的连续的0的个数；如果传入0则行为未定义
int __builtin_ctz (unsigned int x)
Returns the number of trailing 0-bits in x, starting at the least significant bit 
 position. If x is 0, the result is undefined.

 int __builtin_ctzl (unsigned long)
 Similar to __builtin_ctz, except the argument type is unsigned long.

 int __builtin_ctzll (unsigned long long)
 Similar to __builtin_ctz, except the argument type is unsigned long long.
*/
template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) <= sizeof(unsigned)),
  unsigned>::type ctz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_ctz(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned int) &&
   sizeof(T) <= sizeof(unsigned long)),
  unsigned>::type ctz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_ctzl(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned long) &&
   sizeof(T) <= sizeof(unsigned long long)),
  unsigned>::type ctz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_ctzll(v);
}

// count leading zeros
// NOTE: the builtin is nondeterministic on 0 input
/*
函数作用是返回输入数二进制表示从最高位开始(左起)的连续的0的个数；如果传入0则行为未定义。

int __builtin_clz (unsigned int x)
Returns the number of leading 0-bits in x, starting at the most significant bit position. If x is 0, the result is undefined.

int __builtin_clzl (unsigned long)
Similar to __builtin_clz, except the argument type is unsigned long.

int __builtin_clzll (unsigned long long)
Similar to __builtin_clz, except the argument type is unsigned long long.

*/
template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) <= sizeof(unsigned)),
  unsigned>::type clz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_clz(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned int) &&
   sizeof(T) <= sizeof(unsigned long)),
  unsigned>::type clz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_clzl(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned long) &&
   sizeof(T) <= sizeof(unsigned long long)),
  unsigned>::type clz(T v) {
  if (v == 0)
    return sizeof(v) * 8;
  return __builtin_clzll(v);
}

// count bits (set + any 0's that follow)
template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) <= sizeof(unsigned)),
  unsigned>::type cbits(T v) {
  if (v == 0)
    return 0;
  return (sizeof(v) * 8) - __builtin_clz(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned int) &&
   sizeof(T) <= sizeof(unsigned long)),
  unsigned>::type cbits(T v) {
  if (v == 0)
    return 0;
  return (sizeof(v) * 8) - __builtin_clzl(v);
}

template<class T>
  inline typename std::enable_if<
  (std::is_integral<T>::value &&
   sizeof(T) > sizeof(unsigned long) &&
   sizeof(T) <= sizeof(unsigned long long)),
  unsigned>::type cbits(T v) {
  if (v == 0)
    return 0;
  return (sizeof(v) * 8) - __builtin_clzll(v);
}

// count the bits set to 1, a.k.a. population count
/*
函数作用是返回输入的二进制表示中1的个数；如果传入0则返回 0 。
int __builtin_popcount (unsigned int x)
Returns the number of 1-bits in x.

int __builtin_popcountl (unsigned long)
Similar to __builtin_popcount, except the argument type is unsigned long.

int __builtin_popcountll (unsigned long long)
Similar to __builtin_popcount, except the argument type is unsigned long long.
*/
template<class T>
unsigned popcount(T v) {
  static_assert(sizeof(T) <= sizeof(unsigned long long), "type too large");
  if constexpr (sizeof(T) <= sizeof(unsigned int)) {
    return __builtin_popcount(v);
  } else if constexpr (sizeof(T) <= sizeof(unsigned long)) {
    return __builtin_popcountl(v);
  } else {
    return __builtin_popcountll(v);
  }
}
#endif
