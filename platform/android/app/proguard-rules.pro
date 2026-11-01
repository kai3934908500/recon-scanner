# SPDX-License-Identifier: BSD-3-Clause
#
# proguard-rules.pro — R8 混淆规则。
# 契约：docs/12_构建与打包.md §3.6。

# ===== native 库与 JNI 桥接：必须保留 =====
# JNI 方法由 C++ 侧按名字查找（RegisterNatives 或命名约定），R8 不得改名/移除
-keep class com.recon.scan.NativeBridge { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}

# ===== ARCore 可选依赖 =====
# ARCore 以 optional 方式引入；保留类名，避免 ClassNotFoundException 的可读性下降
-keep class com.google.ar.core.** { *; }
-dontwarn com.google.ar.core.**

# ===== 零网络断言辅助 =====
# 保留崩溃栈可读性所需的行号信息（不隐藏，便于用户上报 bug）
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
