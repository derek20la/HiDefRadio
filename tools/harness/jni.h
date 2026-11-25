// Harness only (macOS port, step 0a): a stand-in for the JDK's <jni.h>, so the harness builds
// with no JDK (on macOS and on Linux). The app itself is built with the NDK's own jni.h.
//
// It has only what native-lib.cpp and apptest.cpp use: the JNI types (with the JDK's class tree,
// so a jbyteArray can be passed where a jarray is wanted), JNIEXPORT / JNICALL, JNI_TRUE /
// JNI_FALSE, and the 12 JNIEnv functions the engine calls. apptest.cpp fills in the table.
// Harness and engine are compiled against this same file, so the table's layout need not
// match the JDK's.
#pragma once
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t  jboolean;
typedef int8_t   jbyte;
typedef uint16_t jchar;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;

#define JNI_FALSE 0
#define JNI_TRUE  1
#define JNIEXPORT __attribute__((visibility("default")))
#define JNICALL

class _jobject {};
class _jclass : public _jobject {};
class _jstring : public _jobject {};
class _jarray : public _jobject {};
class _jbooleanArray : public _jarray {};
class _jbyteArray : public _jarray {};
class _jcharArray : public _jarray {};
class _jshortArray : public _jarray {};
class _jintArray : public _jarray {};
class _jlongArray : public _jarray {};
class _jfloatArray : public _jarray {};
class _jdoubleArray : public _jarray {};
typedef _jobject *jobject;
typedef _jclass *jclass;
typedef _jstring *jstring;
typedef _jarray *jarray;
typedef _jbooleanArray *jbooleanArray;
typedef _jbyteArray *jbyteArray;
typedef _jcharArray *jcharArray;
typedef _jshortArray *jshortArray;
typedef _jintArray *jintArray;
typedef _jlongArray *jlongArray;
typedef _jfloatArray *jfloatArray;
typedef _jdoubleArray *jdoubleArray;

struct JNIEnv_;
typedef JNIEnv_ JNIEnv;

struct JNINativeInterface_ {
    jstring (JNICALL *NewStringUTF)(JNIEnv *, const char *);
    jstring (JNICALL *NewString)(JNIEnv *, const jchar *, jsize);
    const char *(JNICALL *GetStringUTFChars)(JNIEnv *, jstring, jboolean *);
    void (JNICALL *ReleaseStringUTFChars)(JNIEnv *, jstring, const char *);
    jsize (JNICALL *GetArrayLength)(JNIEnv *, jarray);
    jbyteArray (JNICALL *NewByteArray)(JNIEnv *, jsize);
    jintArray (JNICALL *NewIntArray)(JNIEnv *, jsize);
    jlongArray (JNICALL *NewLongArray)(JNIEnv *, jsize);
    void (JNICALL *SetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, const jbyte *);
    void (JNICALL *SetIntArrayRegion)(JNIEnv *, jintArray, jsize, jsize, const jint *);
    void (JNICALL *SetLongArrayRegion)(JNIEnv *, jlongArray, jsize, jsize, const jlong *);
    void (JNICALL *SetShortArrayRegion)(JNIEnv *, jshortArray, jsize, jsize, const jshort *);
};

struct JNIEnv_ {
    const JNINativeInterface_ *functions;
    jstring NewStringUTF(const char *s) { return functions->NewStringUTF(this, s); }
    jstring NewString(const jchar *c, jsize n) { return functions->NewString(this, c, n); }
    const char *GetStringUTFChars(jstring s, jboolean *copy) { return functions->GetStringUTFChars(this, s, copy); }
    void ReleaseStringUTFChars(jstring s, const char *c) { functions->ReleaseStringUTFChars(this, s, c); }
    jsize GetArrayLength(jarray a) { return functions->GetArrayLength(this, a); }
    jbyteArray NewByteArray(jsize n) { return functions->NewByteArray(this, n); }
    jintArray NewIntArray(jsize n) { return functions->NewIntArray(this, n); }
    jlongArray NewLongArray(jsize n) { return functions->NewLongArray(this, n); }
    void SetByteArrayRegion(jbyteArray a, jsize st, jsize n, const jbyte *b) { functions->SetByteArrayRegion(this, a, st, n, b); }
    void SetIntArrayRegion(jintArray a, jsize st, jsize n, const jint *b) { functions->SetIntArrayRegion(this, a, st, n, b); }
    void SetLongArrayRegion(jlongArray a, jsize st, jsize n, const jlong *b) { functions->SetLongArrayRegion(this, a, st, n, b); }
    void SetShortArrayRegion(jshortArray a, jsize st, jsize n, const jshort *b) { functions->SetShortArrayRegion(this, a, st, n, b); }
};
