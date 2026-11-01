# PoissonRecon 补丁记录

上游：https://github.com/mkazhdan/PoissonRecon （V7.0 标签）
许可：MIT

本文件记录对上游源码的**每一处**修改。
依据 `docs/06_网格化与后处理.md` §1.1 的工程约束：
> 不得修改上游源码后仍以原许可分发；如需改动须在 `third_party/poisson_recon/PATCHES.md` 记录每条 patch

---

## P1 · FunctionData.inl：BinaryNode 模板实参去除

**文件**：`FunctionData.inl`
**行数**：7 处（70, 98, 103, 151, 154, 163, 165）
**类型**：编译修复（上游版本内不匹配）

### 问题

V7.0 的 `BinaryNode.h` 定义的是**非模板类**：

```cpp
class BinaryNode            // ← 非模板
{
    static inline int CenterCount( int depth ) { ... }
    template< class Real > static inline void CenterAndWidth( ... )  // ← 成员函数是模板
    ...
};
```

但同版本的 `FunctionData.inl` 却以**模板实参**方式调用它：

```cpp
res = BinaryNode<double>::CumulativeCenterCount( depth );   // ← 编译错误
```

GCC / Clang / MSVC 均报 `error: 'BinaryNode' is not a template`。
这说明该 V7.0 标签内部 `FunctionData.inl` 与 `BinaryNode.h` **版本不匹配**
（`FunctionData.inl` 疑似来自更晚的、把 `BinaryNode` 改成模板类的分支）。

### 修改

将 7 处 `BinaryNode< double >` / `BinaryNode<double>` 统一改为 `BinaryNode`：

```diff
-	res = BinaryNode<double>::CumulativeCenterCount( depth );
+	res = BinaryNode::CumulativeCenterCount( depth );
-		BinaryNode< double >::CenterAndWidth( i , c1 , w1 );
+		BinaryNode::CenterAndWidth( i , c1 , w1 );
   （其余 5 处同理）
```

### 语义等价性

`BinaryNode` 的被调用成员分两类：

| 成员 | 形式 | 模板实参的作用 |
|---|---|---|
| `CumulativeCenterCount(int)` | 非模板静态 | 无 |
| `CenterAndWidth(int, Real&, Real&)` | 成员函数模板，`Real` 由**实参类型推导** | `double` 只是**显式指定**，与推导结果一致（调用处 `c1/w1` 均为 `double`） |
| `DepthAndOffset(int, int&, int&)` | 同上 | 同上 |

即：`BinaryNode<double>::f(x, y)` 与 `BinaryNode::f(x, y)` 在调用点的
实参类型均为 `double`，**行为完全一致**，仅是去掉了冗余的显式实参。

### 验证

- GCC 16.2 (MinGW-w64)：编译通过
- MSVC 2019/2022：待目标机型验证（`docs/14_验收测试.md` 回填项）

---

## P2 · Ply.h：`value` 成员笔误修正

**文件**：`Ply.h`
**行数**：2 处（343、389）
**类型**：编译修复（上游笔误）

### 问题

两处 `operator -` 都引用了**不存在的成员** `value`：

```cpp
// L343（PlyOrientedVertex）
PlyOrientedVertex operator - ( PlyOrientedVertex p ) const { return PlyOrientedVertex( point-p.value , normal-p.normal ); }

// L389（PlyColorVertex::_PlyColorVertex）
_PlyColorVertex operator - ( _PlyColorVertex p ) const { return _PlyColorVertex( point-p.value , color-p.color ); }
```

而这两个类的实际数据成员是 `point` / `normal` / `color`，**没有 `value`**。
GCC / Clang / MSVC 均报 `has no member named 'value'`。

对照同文件里的 `operator +`（用的是 `point+p.point`）可见，
`value` 是 `point` 的笔误。

### 修改

```diff
-	PlyOrientedVertex operator - ( PlyOrientedVertex p ) const { return PlyOrientedVertex( point-p.value , normal-p.normal ); }
+	PlyOrientedVertex operator - ( PlyOrientedVertex p ) const { return PlyOrientedVertex( point-p.point , normal-p.normal ); }
-	_PlyColorVertex operator - ( _PlyColorVertex p ) const { return _PlyColorVertex( point-p.value , color-p.color ); }
+	_PlyColorVertex operator - ( _PlyColorVertex p ) const { return _PlyColorVertex( point-p.point , color-p.color ); }
```

### 语义等价性

与同类的 `operator +` 完全对称（`point-p.point` / `color-p.color`），
是**显然的笔误修正**，不改变任何算法行为。

### 验证

- GCC 16.2 (MinGW-w64)：编译通过
- MSVC 2019/2022：待目标机型验证（`docs/14_验收测试.md` 回填项）

---

## 附：未修改上游源码的兼容处理（编译期注入）

以下兼容性问题**未改动上游任何文件**，通过
`third_party/poisson_recon/recon_hash_compat.h` + CMake 的 `-include` / `/FI` 注入解决：

| 问题 | 处理 |
|---|---|
| `hash_map` 三种写法（`<hash_map>` / `<ext/hash_map>`）在现代工具链均不可用 | 垫片用 alias template 统一映射到 `std::unordered_map` |
| `BSplineData.h` / `Ply.h` / `PointStream.inl` 缺 `#include`（依赖上游包含顺序） | 垫片按拓扑顺序预包含 `Array.h` / `BinaryNode.h` / `FunctionData.h` / `MAT.h` / `Polynomial.h` / `Vector.h` |
| `malloc` / `printf` 等 C 标准库符号未显式包含 | 垫片补 `<cstdio>` / `<cstdlib>` / `<cstring>` |
| `DIMENSION` 宏定义在 `Octree.h`，但 `PointStream.inl` 需要它且不 include | 垫片按上游 `Octree.h` 的同值定义 `#define DIMENSION 3`（带 `#ifndef` 保护，不覆盖已定义值） |

这些处理**不改变上游源码**，因此无需逐条 patch 记录；
但垫片本身是集成层新增文件，许可与上游一致（MIT）。
