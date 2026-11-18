/* SPDX-License-Identifier: BSD-3-Clause */
//
// jni_stub.h — **本机编译检查专用**的最小 JNI 头替身。
//
// ── 为什么需要它（问题的本质）────────────────────────────────────────────
//
//  `platform/android/app/src/main/cpp/*.cpp`（JNI 桥接层）**只**在 Android
//  构建里参与编译（根 `CMakeLists.txt` 的 `if(RECON_PLATFORM_ANDROID)` 内），
//  而本仓库的开发机没有 NDK / `jni.h`。后果是：**桥接 C++ 从未被任何本机构建
//  编译过** —— 任何 C++ 语法 / 类型错误都只能等 Android CI 真编译才能发现。
//
//  这不是理论担忧，而是一个**反复发生**的类别：
//    · CI run #10：`native-lib.cpp:140`
//        error: assigning to 'recon_pipeline_branch' from incompatible type
//               'unsigned int'
//      （C 允许 `unsigned int` 隐式转枚举，**C++ 不允许** —— 恰好只有 C++
//        编译能发现，而这段代码此前只在 Android CI 被 C++ 编译过。）
//
// ── 本文件做什么 ─────────────────────────────────────────────────────────
//
//  提供一个**只够类型检查**的 `jni.h` 替身：把 JNI 的类型别名与 `JNIEnv`
//  成员函数声明补齐到「足以让桥接源文件通过 `-fsyntax-only`」的程度。
//
//  ⚠️ 用途边界（必须说清，否则会误导）：
//    · **只用于 `-fsyntax-only`** —— 声明不需要（也不应该有）实现。
//    · **不可用于链接或运行**（没有 .cpp 实现，链不上）。
//    · **签名必须与真实 ndk 的 jni.h 一致** —— 若某成员签名写错，本机检查
//      会给出与真机 NDK 不同的结论（假绿）。故本文件只声明**桥接层实际用到**
//      的成员（用面可枚举，见 `tools/check_jni_bridge_syntax.py` 的断言），
//      其余一概不声明：**用到的没声明 → 本机报错 → 强迫我们核对真实签名**，
//      这比"多声明一堆猜的签名"更安全。
//
//  ⚠️ 维护方式：桥接层若新增 JNI 成员调用，本机检查会因「未声明」而失败 ——
//     此时**请对着真实 NDK 头文件补齐签名**，不要凭记忆写。
//

#ifndef RECON_TOOLS_JNI_STUB_H
#define RECON_TOOLS_JNI_STUB_H

#include <cstddef>
#include <cstdint>

// ── 基本整型别名（与真实 jni.h 一致）─────────────────────────────────────
typedef int32_t  jint;
typedef int64_t  jlong;
typedef double   jdouble;
typedef float    jfloat;
typedef int8_t   jbyte;
typedef int16_t  jshort;
typedef uint8_t  jboolean;
typedef jint     jsize;

// Android NDK 的 jni.h 定义了 JNI_TRUE / JNI_FALSE（0 / 1）。
#ifndef JNI_FALSE
#define JNI_FALSE 0
#endif
#ifndef JNI_TRUE
#define JNI_TRUE 1
#endif

// JNI 的 `jboolean*` 可空指针约定：`GetStringUTFChars(str, nullptr)` 表示
// 「不关心是否复制」。真实 ndk 头同样是 `jboolean*`，故此处保持一致。
class _jobject {};
class _jclass    : public _jobject {};
class _jstring   : public _jobject {};
class _jthrowable: public _jobject {};
class _jarray    : public _jobject {};
class _jbyteArray  : public _jarray {};
class _jshortArray : public _jarray {};
class _jintArray   : public _jarray {};
class _jfloatArray : public _jarray {};
class _jdoubleArray: public _jarray {};
class _jobjectArray: public _jarray {};

typedef _jobject*      jobject;
typedef _jclass*       jclass;
typedef _jstring*      jstring;
typedef _jarray*       jarray;
typedef _jbyteArray*   jbyteArray;
typedef _jshortArray*  jshortArray;
typedef _jintArray*    jintArray;
typedef _jfloatArray*  jfloatArray;
typedef _jdoubleArray* jdoubleArray;
typedef _jobjectArray* jobjectArray;

struct _jmethodID; typedef _jmethodID* jmethodID;
struct _jfieldID;  typedef _jfieldID*  jfieldID;

// ── JNIEnv ───────────────────────────────────────────────────────────────
//
// 只声明桥接层**实际用到**的成员。参数名省略以减少样板（签名以类型为准）。
// 全部为声明（无定义）—— `-fsyntax-only` 不需要定义，链接也不需要（不链接）。
struct JNIEnv {
    // 类 / 方法 / 对象
    jclass    FindClass(const char*);
    jclass    GetObjectClass(jobject);
    jmethodID GetMethodID(jclass, const char*, const char*);
    void      CallVoidMethod(jobject, jmethodID, ...);
    // ⚠️ 曾在此处预防性地声明过 `jobject NewObject(jclass, jmethodID, ...);`
    //    —— 桥接层**从未调用**它（用的是 `NewObjectArray`）。已删除。
    //    原因：本桩的立场是「只声明被用到的成员」，死声明是"桩悄悄膨胀/过期"
    //    的信号，且一旦将来被用上就可能带着**未经核对的签名**（假绿风险）。
    //    这条约束由 `tools/check_jni_bridge_syntax.py` 的 G-AND-7(a)① 机器看守。

    // 字符串
    const char* GetStringUTFChars(jstring, jboolean*);
    void        ReleaseStringUTFChars(jstring, const char*);
    jstring     NewStringUTF(const char*);

    // 数组（读取）
    jsize GetArrayLength(jarray);
    void  GetByteArrayRegion(jbyteArray, jsize, jsize, jbyte*);
    void  SetByteArrayRegion(jbyteArray, jsize, jsize, const jbyte*);
    void  GetShortArrayRegion(jshortArray, jsize, jsize, jshort*);
    void  GetDoubleArrayRegion(jdoubleArray, jsize, jsize, jdouble*);

    // 数组（写入）
    void SetIntArrayRegion(jintArray, jsize, jsize, const jint*);
    void SetFloatArrayRegion(jfloatArray, jsize, jsize, const jfloat*);
    void SetDoubleArrayRegion(jdoubleArray, jsize, jsize, const jdouble*);

    // 数组（创建）
    jbyteArray   NewByteArray(jsize);
    jintArray    NewIntArray(jsize);
    jfloatArray  NewFloatArray(jsize);
    jdoubleArray NewDoubleArray(jsize);
    jobjectArray NewObjectArray(jsize, jclass, jobject);
    void         SetObjectArrayElement(jobjectArray, jsize, jobject);

    // 异常 / 引用
    jboolean ExceptionCheck();
    void     ExceptionClear();
    void     DeleteLocalRef(jobject);
};

// 真实 jni.h 里这两个宏按平台展开（Windows 上为 __declspec(dllexport)/__stdcall，
// Linux/Android 上为空）。本机检查只关心类型，展开为空即可 —— 但**必须定义**，
// 否则 `JNIEXPORT jint JNICALL Foo(...)` 会因未定义符号而报错。
#define JNIEXPORT
#define JNICALL

// 可变参数宏（真实 jni.h 有，桥接层若用到需一致）。
#define JNI_VERSION_1_6 0x00010006

#endif  // RECON_TOOLS_JNI_STUB_H
