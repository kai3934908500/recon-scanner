#ifndef HASH_INCLUDED
#define HASH_INCLUDED

// SPDX-License-Identifier: BSD-3-Clause
//
// **本文件已被修改**（见 PATCHES.md 的 P3）。
//
// 上游原文按 `#ifdef WIN32` 在两种 hash_map 之间切换：
//   WIN32 → <hash_map> + stdext::hash_map   （VC++ 专有，MinGW 无此头）
//   否则 → <ext/hash_map> + __gnu_cxx::hash_map （GCC 已移除的废弃扩展）
//
// 两条路在现代工具链（GCC 16 / MinGW-w64 / MSVC 2019+）上都会失败：
//   · MinGW 没有 <hash_map>
//   · GCC 16 已移除 <ext/hash_map>
//
// 本项目统一改用标准 C++11 的 std::unordered_map，
// 并为 long long / unsigned long long 提供标准 hash 特化
// （C++14 起 std::hash 已原生支持 64 位整型）。

#include <cstddef>
#include <functional>
#include <unordered_map>

// 全局 hash_map 别名（上游 Geometry.h 直接使用 `hash_map<K,V>`）。
template <class K, class V, class H = std::hash<K>,
          class E = std::allocator<std::pair<const K, V>>,
          class A = std::allocator<std::pair<const K, V>>>
using hash_map = std::unordered_map<K, V, H, E, A>;

// 兼容上游 Hash.h 可能引用的 stdext:: 名字。
namespace stdext {
template <class K, class V, class H = std::hash<K>,
          class E = std::allocator<std::pair<const K, V>>,
          class A = std::allocator<std::pair<const K, V>>>
using hash_map = std::unordered_map<K, V, H, E, A>;
}  // namespace stdext

#endif  // HASH_INCLUDED
