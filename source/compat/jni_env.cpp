#include "compat/loader.h"
#include "compat/jni.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <string>
#include <set>

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

static void* g_jni_funcs[JNI_NUM_SLOTS] = {};
static void* g_vm_funcs[VM_NUM_SLOTS]   = {};
static void* g_jni_inner = nullptr;
static void* g_vm_inner  = nullptr;
static void* g_jni_outer = nullptr;
static void* g_vm_outer  = nullptr;

static std::vector<JNINativeMethod> g_native_methods;

#define DUMMY_CLASS  ((void*)0x1001)
#define DUMMY_METHOD ((void*)0x2001)
#define DUMMY_FIELD  ((void*)0x3001)

// ─── Method registry ─────────────────────────────────────────────────────────
// Each unique (name, sig) pair gets a stable MethodEntry pointer used as jmethodID.
struct MethodEntry { char name[80]; char sig[128]; };
static MethodEntry g_method_pool[512];
static int g_method_count = 0;

static MethodEntry* lookupOrCreateMethod(const char* n, const char* sg) {
    for (int i = 0; i < g_method_count; i++) {
        if (strcmp(g_method_pool[i].name, n ? n : "") == 0 &&
            strcmp(g_method_pool[i].sig,  sg ? sg : "") == 0)
            return &g_method_pool[i];
    }
    if (g_method_count < 512) {
        MethodEntry* e = &g_method_pool[g_method_count++];
        strncpy(e->name, n  ? n  : "", 79);  e->name[79]  = 0;
        strncpy(e->sig,  sg ? sg : "", 127); e->sig[127]  = 0;
        return e;
    }
    return nullptr;
}

// ─── UserDefault in-memory store (Cocos2d-x SharedPreferences emulation) ─────
static std::unordered_map<std::string, int>         g_int_store;
static std::unordered_map<std::string, std::string> g_str_store;
static std::set<std::string> g_logged_int_keys;   // suppress per-call spam

// ─── All JNI stubs (must be defined before jniSetup uses their addresses) ─────

static jint     s_GetVersion(JNIEnv*)            { return JNI_VERSION_1_6; }
static jclass   s_FindClass(JNIEnv*, const char* n) {
    compatLogFmt("JNI FindClass: %s", n ? n : "?");
    return DUMMY_CLASS;
}
static jclass   s_GetSuperclass(JNIEnv*, jclass)        { return DUMMY_CLASS; }
static jboolean s_IsAssignableFrom(JNIEnv*, jclass, jclass) { return JNI_TRUE; }
static jint     s_Throw(JNIEnv*, jthrowable)            { return 0; }
static jint     s_ThrowNew(JNIEnv*, jclass, const char*){ return 0; }
static jthrowable s_ExceptionOccurred(JNIEnv*)          { return nullptr; }
static void     s_ExceptionDescribe(JNIEnv*)             {}
static void     s_ExceptionClear(JNIEnv*)                {}
static void     s_FatalError(JNIEnv*, const char* m) {
    compatLogFmt("JNI FatalError: %s", m ? m : "?");
}
static jint     s_PushLocalFrame(JNIEnv*, jint)         { return 0; }
static jobject  s_PopLocalFrame(JNIEnv*, jobject o)     { return o; }
static jobject  s_NewGlobalRef(JNIEnv*, jobject o)      { return o; }
static void     s_DeleteGlobalRef(JNIEnv*, jobject)      {}
static void     s_DeleteLocalRef(JNIEnv*, jobject)       {}
static jboolean s_IsSameObject(JNIEnv*, jobject a, jobject b) {
    return a == b ? JNI_TRUE : JNI_FALSE;
}
static jobject  s_NewLocalRef(JNIEnv*, jobject o)       { return o; }
static jint     s_EnsureLocalCapacity(JNIEnv*, jint)    { return 0; }
static jobject  s_AllocObject(JNIEnv*, jclass)          { return nullptr; }
static jclass   s_GetObjectClass(JNIEnv*, jobject)      { return DUMMY_CLASS; }
static jboolean s_IsInstanceOf(JNIEnv*, jobject, jclass){ return JNI_TRUE; }

static jmethodID s_GetMethodID(JNIEnv*, jclass, const char* n, const char* sg) {
    compatLogFmt("JNI GetMethodID: %s %s", n ? n : "?", sg ? sg : "?");
    MethodEntry* e = lookupOrCreateMethod(n, sg);
    return e ? (jmethodID)e : (jmethodID)DUMMY_METHOD;
}
static jmethodID s_GetStaticMethodID(JNIEnv*, jclass, const char* n, const char* sg) {
    compatLogFmt("JNI GetStaticMethodID: %s %s", n ? n : "?", sg ? sg : "?");
    MethodEntry* e = lookupOrCreateMethod(n, sg);
    return e ? (jmethodID)e : (jmethodID)DUMMY_METHOD;
}
static jfieldID s_GetFieldID(JNIEnv*, jclass, const char* n, const char* sg) {
    compatLogFmt("JNI GetFieldID: %s %s", n ? n : "?", sg ? sg : "?");
    return DUMMY_FIELD;
}
static jfieldID s_GetStaticFieldID(JNIEnv*, jclass, const char* n, const char* sg) {
    compatLogFmt("JNI GetStaticFieldID: %s %s", n ? n : "?", sg ? sg : "?");
    return DUMMY_FIELD;
}

// Return-type stubs (generic, for slots that don't need per-call logging)
static jobject  s_RetObj(JNIEnv*, ...)   { return nullptr; }
static jobject  s_RetObjV(JNIEnv*, jobject, jmethodID, va_list) { return nullptr; }
static jboolean s_RetBool(JNIEnv*, ...)  { return JNI_FALSE; }
static jboolean s_RetBoolV(JNIEnv*, jobject, jmethodID, va_list) { return JNI_FALSE; }
static jint     s_RetInt(JNIEnv*, ...)   { return 0; }
static jint     s_RetIntV(JNIEnv*, jobject, jmethodID, va_list) { return 0; }
static jlong    s_RetLong(JNIEnv*, ...)  { return 0LL; }
static jlong    s_RetLongV(JNIEnv*, jobject, jmethodID, va_list) { return 0LL; }
static jfloat   s_RetFloat(JNIEnv*, ...) { return 0.0f; }
static jfloat   s_RetFloatV(JNIEnv*, jobject, jmethodID, va_list) { return 0.0f; }
static jdouble  s_RetDouble(JNIEnv*, ...) { return 0.0; }
static jdouble  s_RetDoubleV(JNIEnv*, jobject, jmethodID, va_list) { return 0.0; }
static void     s_RetVoid(JNIEnv*, ...)   {}
static void     s_RetVoidV(JNIEnv*, jobject, jmethodID, va_list) {}

// ─── Instance-method call stubs ───────────────────────────────────────────────
static jobject s_CallObjectMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallObjectMethod"); return (jobject)"";
}
static jobject s_CallObjectMethodV(JNIEnv*, jobject, jmethodID, va_list) {
    compatLog("JNI CallObjectMethodV"); return (jobject)"";
}
static void s_CallVoidMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallVoidMethod");
}
static void s_CallVoidMethodV(JNIEnv*, jobject, jmethodID, va_list) {
    compatLog("JNI CallVoidMethodV");
}
static jboolean s_CallBoolMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallBooleanMethod"); return JNI_FALSE;
}
static jint s_CallIntMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallIntMethod"); return 0;
}
static jlong s_CallLongMethod(JNIEnv*, jobject, jmethodID, ...) {
    compatLog("JNI CallLongMethod"); return 0LL;
}
static jobject s_NewObject(JNIEnv*, jclass, jmethodID, ...) {
    compatLog("JNI NewObject"); return nullptr;
}

// ─── Static-method dispatching stubs ─────────────────────────────────────────
// Helper: resolve a jmethodID to its MethodEntry (guards against DUMMY_METHOD)
static inline MethodEntry* methodEntry(jmethodID mid) {
    return (mid == (jmethodID)DUMMY_METHOD) ? nullptr : (MethodEntry*)mid;
}

static jint s_CallStaticIntMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (e && strcmp(e->name, "getIntegerForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        jint defval    = va_arg(args, jint);
        if (key) {
            auto it = g_int_store.find(key);
            if (it != g_int_store.end()) {
                compatLogFmt("JNI getIntegerForKey(%s) → %d (stored)", key, it->second);
                return it->second;
            }
            // Log each new key once to avoid log spam during the tight loop
            if (g_logged_int_keys.insert(key).second)
                compatLogFmt("JNI getIntegerForKey(%s) → %d (default)", key, defval);
        }
        return defval;
    }
    return 0;
}
static jint s_CallStaticIntMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jint r = s_CallStaticIntMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

static jboolean s_CallStaticBoolMethodV(JNIEnv*, jclass, jmethodID mid, va_list) {
    MethodEntry* e = methodEntry(mid);
    if (e) {
        // EULA/network checks — return true so the game doesn't wait forever
        if (strcmp(e->name, "eulaHasBeenAccepted") == 0 ||
            strcmp(e->name, "isNetworkAvailable")   == 0 ||
            strcmp(e->name, "hasUserConsented")      == 0) {
            compatLogFmt("JNI %s() → true", e->name);
            return JNI_TRUE;
        }
        compatLogFmt("JNI CallStaticBooleanMethodV: %s → false", e->name);
    }
    return JNI_FALSE;
}
static jboolean s_CallStaticBoolMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jboolean r = s_CallStaticBoolMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

static void s_CallStaticVoidMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (!e) { compatLog("JNI CallStaticVoidMethodV"); return; }

    if (strcmp(e->name, "setIntegerForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        jint val        = va_arg(args, jint);
        compatLogFmt("JNI setIntegerForKey(%s, %d)", key ? key : "?", val);
        if (key) { g_int_store[key] = val; g_logged_int_keys.erase(key); }
        return;
    }
    if (strcmp(e->name, "setStringForKey") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        const char* val = (const char*)va_arg(args, jstring);
        compatLogFmt("JNI setStringForKey(%s, \"%s\")", key ? key : "?", val ? val : "null");
        if (key) g_str_store[key] = val ? val : "";
        return;
    }
    if (strcmp(e->name, "flush") == 0) {
        compatLog("JNI UserDefault.flush (no-op)");
        return;
    }
    compatLogFmt("JNI CallStaticVoidMethodV: %s %s", e->name, e->sig);
}
static void s_CallStaticVoidMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    s_CallStaticVoidMethodV(env, cls, mid, a);
    va_end(a);
}

static jobject s_CallStaticObjectMethodV(JNIEnv*, jclass, jmethodID mid, va_list args) {
    MethodEntry* e = methodEntry(mid);
    if (!e) { compatLog("JNI CallStaticObjectMethodV"); return (jobject)""; }

    if (strcmp(e->name, "getStringForKey") == 0) {
        const char* key    = (const char*)va_arg(args, jstring);
        const char* defval = (const char*)va_arg(args, jstring);
        if (key) {
            auto it = g_str_store.find(key);
            if (it != g_str_store.end()) {
                compatLogFmt("JNI getStringForKey(%s) → stored", key);
                return (jobject)it->second.c_str();
            }
        }
        return (jobject)(defval ? defval : "");
    }
    if (strcmp(e->name, "retrieveDefaultsString") == 0) {
        const char* key = (const char*)va_arg(args, jstring);
        if (key) {
            auto it = g_str_store.find(key);
            if (it != g_str_store.end()) return (jobject)it->second.c_str();
        }
        return (jobject)"";
    }
    if (strcmp(e->name, "aesDecrypt") == 0 || strcmp(e->name, "aesEncrypt") == 0) {
        const char* data = (const char*)va_arg(args, jstring);
        // Return input unchanged — identity cipher so round-trips are consistent
        return (jobject)(data ? data : "");
    }
    compatLogFmt("JNI CallStaticObjectMethodV: %s %s → \"\"", e->name, e->sig);
    return (jobject)"";
}
static jobject s_CallStaticObjectMethod(JNIEnv* env, jclass cls, jmethodID mid, ...) {
    va_list a; va_start(a, mid);
    jobject r = s_CallStaticObjectMethodV(env, cls, mid, a);
    va_end(a);
    return r;
}

// Fields (get/set)
static jobject  s_GetObjField(JNIEnv*, jobject, jfieldID) { return nullptr; }
static jboolean s_GetBoolField(JNIEnv*, jobject, jfieldID){ return JNI_FALSE; }
static jbyte    s_GetByteField(JNIEnv*, jobject, jfieldID){ return 0; }
static jchar    s_GetCharField(JNIEnv*, jobject, jfieldID){ return 0; }
static jshort   s_GetShortField(JNIEnv*, jobject, jfieldID){ return 0; }
static jint     s_GetIntField(JNIEnv*, jobject, jfieldID) { return 0; }
static jlong    s_GetLongField(JNIEnv*, jobject, jfieldID){ return 0LL; }
static jfloat   s_GetFloatField(JNIEnv*, jobject, jfieldID){ return 0.0f; }
static jdouble  s_GetDoubleField(JNIEnv*, jobject, jfieldID){ return 0.0; }
static void     s_SetField(JNIEnv*, jobject, jfieldID, ...) {}

static jobject  s_GetStaticObjField(JNIEnv*, jclass, jfieldID) { return nullptr; }
static jint     s_GetStaticIntField(JNIEnv*, jclass, jfieldID)  { return 0; }
static jlong    s_GetStaticLongField(JNIEnv*, jclass, jfieldID) { return 0LL; }
static void     s_SetStaticField(JNIEnv*, jclass, jfieldID, ...) {}

// Strings
static jstring s_NewStringUTF(JNIEnv*, const char* str) { return (jstring)str; }
static jsize   s_GetStringUTFLength(JNIEnv*, jstring s) {
    return s ? (jsize)strlen((const char*)s) : 0;
}
static const char* s_GetStringUTFChars(JNIEnv*, jstring s, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return (const char*)s;
}
static void    s_ReleaseStringUTFChars(JNIEnv*, jstring, const char*) {}
static jstring s_NewString(JNIEnv*, const jchar*, jsize) { return nullptr; }
static jsize   s_GetStringLength(JNIEnv*, jstring)       { return 0; }
static const jchar* s_GetStringChars(JNIEnv*, jstring, jboolean* cp) {
    if (cp) *cp = JNI_FALSE; return nullptr;
}
static void    s_ReleaseStringChars(JNIEnv*, jstring, const jchar*) {}
static void    s_GetStringRegion(JNIEnv*, jstring, jsize, jsize, jchar*) {}
static void    s_GetStringUTFRegion(JNIEnv*, jstring, jsize, jsize, char*) {}
static const jchar* s_GetStringCritical(JNIEnv*, jstring, jboolean* cp) {
    if (cp) *cp = JNI_FALSE; return nullptr;
}
static void    s_ReleaseStringCritical(JNIEnv*, jstring, const jchar*) {}

// Arrays
static jsize s_GetArrayLength(JNIEnv*, jarray) { return 0; }
static jbyteArray s_NewByteArray(JNIEnv*, jsize len) {
    uint8_t* p = (uint8_t*)calloc(1, 4 + (size_t)(len > 0 ? len : 0));
    if (p && len > 0) *(jint*)p = len;
    return p;
}
static jobjectArray s_NewObjectArray(JNIEnv*, jsize, jclass, jobject) { return nullptr; }
static jobject      s_GetObjectArrayElement(JNIEnv*, jobjectArray, jsize) { return nullptr; }
static void         s_SetObjectArrayElement(JNIEnv*, jobjectArray, jsize, jobject) {}
static jbooleanArray s_NewBoolArray(JNIEnv*, jsize l)  { return (jbooleanArray)s_NewByteArray(nullptr, l); }
static jcharArray    s_NewCharArray(JNIEnv*, jsize l)  { return (jcharArray)s_NewByteArray(nullptr, l*2); }
static jshortArray   s_NewShortArray(JNIEnv*, jsize l) { return (jshortArray)s_NewByteArray(nullptr, l*2); }
static jintArray     s_NewIntArray(JNIEnv*, jsize l)   { return (jintArray)s_NewByteArray(nullptr, l*4); }
static jlongArray    s_NewLongArray(JNIEnv*, jsize l)  { return (jlongArray)s_NewByteArray(nullptr, l*8); }
static jfloatArray   s_NewFloatArray(JNIEnv*, jsize l) { return (jfloatArray)s_NewByteArray(nullptr, l*4); }
static jdoubleArray  s_NewDoubleArray(JNIEnv*, jsize l){ return (jdoubleArray)s_NewByteArray(nullptr, l*8); }

static jbyte*    s_GetByteElements(JNIEnv*, jbyteArray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (jbyte*)((uint8_t*)a + 4) : nullptr;
}
static void* s_GetElements(JNIEnv*, jarray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (uint8_t*)a + 4 : nullptr;
}
static void  s_ReleaseElements(JNIEnv*, jarray, void*, jint) {}
static void  s_GetByteRegion(JNIEnv*, jbyteArray a, jsize st, jsize l, jbyte* buf) {
    if (a && buf) memcpy(buf, (uint8_t*)a + 4 + st, (size_t)l);
}
static void  s_SetByteRegion(JNIEnv*, jbyteArray a, jsize st, jsize l, const jbyte* buf) {
    if (a && buf) memcpy((uint8_t*)a + 4 + st, buf, (size_t)l);
}
static void  s_GetArrayRegion(JNIEnv*, jarray, jsize, jsize, void*) {}
static void  s_SetArrayRegion(JNIEnv*, jarray, jsize, jsize, const void*) {}

static void* s_GetPrimArrayCritical(JNIEnv*, jarray a, jboolean* cp) {
    if (cp) *cp = JNI_FALSE;
    return a ? (uint8_t*)a + 4 : nullptr;
}
static void  s_ReleasePrimArrayCritical(JNIEnv*, jarray, void*, jint) {}

// Misc
static jint s_RegisterNatives(JNIEnv*, jclass, const JNINativeMethod* m, jint n) {
    for (jint i = 0; i < n; i++) {
        compatLogFmt("JNI RegisterNative: %s", m[i].name ? m[i].name : "?");
        g_native_methods.push_back(m[i]);
    }
    return JNI_OK;
}
static jint s_UnregisterNatives(JNIEnv*, jclass) { return JNI_OK; }
static jint s_MonitorEnter(JNIEnv*, jobject) { return 0; }
static jint s_MonitorExit(JNIEnv*, jobject)  { return 0; }
static jint s_GetJavaVM(JNIEnv*, JavaVM** out) {
    if (out) *out = (JavaVM*)g_vm_outer;
    return JNI_OK;
}
static jboolean s_ExceptionCheck(JNIEnv*) { return JNI_FALSE; }
static jobject  s_NewDirectByteBuffer(JNIEnv*, void*, jlong) { return nullptr; }
static void*    s_GetDirectBufferAddress(JNIEnv*, jobject)   { return nullptr; }
static jlong    s_GetDirectBufferCapacity(JNIEnv*, jobject)  { return -1L; }
static jweak    s_NewWeakGlobalRef(JNIEnv*, jobject o)       { return o; }
static void     s_DeleteWeakGlobalRef(JNIEnv*, jweak)        {}
static jobjectRefType s_GetObjectRefType(JNIEnv*, jobject)   { return JNILocalRefType; }

// ─── JavaVM stubs ─────────────────────────────────────────────────────────────
static jint vm_DestroyJavaVM(JavaVM*) { return 0; }
static jint vm_AttachCurrentThread(JavaVM*, JNIEnv** e, void*) {
    if (e) *e = (JNIEnv*)g_jni_outer; return JNI_OK;
}
static jint vm_DetachCurrentThread(JavaVM*) { return 0; }
static jint vm_GetEnv(JavaVM*, void** e, jint) {
    if (e) *e = g_jni_outer; return JNI_OK;
}
static jint vm_AttachDaemon(JavaVM*, JNIEnv** e, void*) {
    if (e) *e = (JNIEnv*)g_jni_outer; return JNI_OK;
}

// ─── jniSetup ─────────────────────────────────────────────────────────────────
void jniSetup(CompatLayer* cl) {
    // Reserved slots 0-3 stay null
    g_jni_funcs[4]  = (void*)s_GetVersion;
    g_jni_funcs[5]  = (void*)s_RetObj;       // DefineClass (stub)
    g_jni_funcs[6]  = (void*)s_FindClass;
    g_jni_funcs[7]  = (void*)s_RetObj;       // FromReflectedMethod
    g_jni_funcs[8]  = (void*)s_RetObj;       // FromReflectedField
    g_jni_funcs[9]  = (void*)s_RetObj;       // ToReflectedMethod
    g_jni_funcs[10] = (void*)s_GetSuperclass;
    g_jni_funcs[11] = (void*)s_IsAssignableFrom;
    g_jni_funcs[12] = (void*)s_RetObj;       // ToReflectedField
    g_jni_funcs[13] = (void*)s_Throw;
    g_jni_funcs[14] = (void*)s_ThrowNew;
    g_jni_funcs[15] = (void*)s_ExceptionOccurred;
    g_jni_funcs[16] = (void*)s_ExceptionDescribe;
    g_jni_funcs[17] = (void*)s_ExceptionClear;
    g_jni_funcs[18] = (void*)s_FatalError;
    g_jni_funcs[19] = (void*)s_PushLocalFrame;
    g_jni_funcs[20] = (void*)s_PopLocalFrame;
    g_jni_funcs[21] = (void*)s_NewGlobalRef;
    g_jni_funcs[22] = (void*)s_DeleteGlobalRef;
    g_jni_funcs[23] = (void*)s_DeleteLocalRef;
    g_jni_funcs[24] = (void*)s_IsSameObject;
    g_jni_funcs[25] = (void*)s_NewLocalRef;
    g_jni_funcs[26] = (void*)s_EnsureLocalCapacity;
    g_jni_funcs[27] = (void*)s_AllocObject;
    g_jni_funcs[28] = (void*)s_NewObject;     // NewObject (varargs)
    g_jni_funcs[29] = (void*)s_RetObjV;       // NewObjectV
    g_jni_funcs[30] = (void*)s_NewObject;     // NewObjectA
    g_jni_funcs[31] = (void*)s_GetObjectClass;
    g_jni_funcs[32] = (void*)s_IsInstanceOf;
    g_jni_funcs[33] = (void*)s_GetMethodID;
    // CallObjectMethod 34-36
    g_jni_funcs[34] = (void*)s_CallObjectMethod;
    g_jni_funcs[35] = (void*)s_CallObjectMethodV;
    g_jni_funcs[36] = (void*)s_CallObjectMethod;
    // CallBooleanMethod 37-39
    g_jni_funcs[37] = (void*)s_CallBoolMethod;
    g_jni_funcs[38] = (void*)s_RetBoolV;
    g_jni_funcs[39] = (void*)s_CallBoolMethod;
    // CallByte/Char/ShortMethod 40-48
    for (int i = 40; i <= 48; i++) g_jni_funcs[i] = (void*)s_RetInt;
    // CallIntMethod 49-51
    g_jni_funcs[49] = (void*)s_CallIntMethod;
    g_jni_funcs[50] = (void*)s_RetIntV;
    g_jni_funcs[51] = (void*)s_CallIntMethod;
    // CallLongMethod 52-54
    g_jni_funcs[52] = (void*)s_CallLongMethod;
    g_jni_funcs[53] = (void*)s_RetLongV;
    g_jni_funcs[54] = (void*)s_CallLongMethod;
    // CallFloat 55-57
    g_jni_funcs[55] = (void*)s_RetFloat;
    g_jni_funcs[56] = (void*)s_RetFloatV;
    g_jni_funcs[57] = (void*)s_RetFloat;
    // CallDouble 58-60
    g_jni_funcs[58] = (void*)s_RetDouble;
    g_jni_funcs[59] = (void*)s_RetDoubleV;
    g_jni_funcs[60] = (void*)s_RetDouble;
    // CallVoidMethod 61-63
    g_jni_funcs[61] = (void*)s_CallVoidMethod;
    g_jni_funcs[62] = (void*)s_CallVoidMethodV;
    g_jni_funcs[63] = (void*)s_CallVoidMethod;
    // Nonvirtual 64-93 (all void stubs)
    for (int i = 64; i <= 93; i++) g_jni_funcs[i] = (void*)s_RetVoid;
    // GetFieldID, Get/SetXxxField 94-112
    g_jni_funcs[94]  = (void*)s_GetFieldID;
    g_jni_funcs[95]  = (void*)s_GetObjField;
    g_jni_funcs[96]  = (void*)s_GetBoolField;
    g_jni_funcs[97]  = (void*)s_GetByteField;
    g_jni_funcs[98]  = (void*)s_GetCharField;
    g_jni_funcs[99]  = (void*)s_GetShortField;
    g_jni_funcs[100] = (void*)s_GetIntField;
    g_jni_funcs[101] = (void*)s_GetLongField;
    g_jni_funcs[102] = (void*)s_GetFloatField;
    g_jni_funcs[103] = (void*)s_GetDoubleField;
    for (int i = 104; i <= 112; i++) g_jni_funcs[i] = (void*)s_SetField;
    // GetStaticMethodID 113
    g_jni_funcs[113] = (void*)s_GetStaticMethodID;
    // CallStaticXxxMethod 114-143
    g_jni_funcs[114] = (void*)s_CallStaticObjectMethod;
    g_jni_funcs[115] = (void*)s_CallStaticObjectMethodV;
    g_jni_funcs[116] = (void*)s_CallStaticObjectMethod;  // A variant
    g_jni_funcs[117] = (void*)s_CallStaticBoolMethod;
    g_jni_funcs[118] = (void*)s_CallStaticBoolMethodV;
    g_jni_funcs[119] = (void*)s_CallStaticBoolMethod;    // A variant
    for (int i = 120; i <= 128; i++) g_jni_funcs[i] = (void*)s_RetInt; // byte/char/short
    g_jni_funcs[129] = (void*)s_CallStaticIntMethod;
    g_jni_funcs[130] = (void*)s_CallStaticIntMethodV;
    g_jni_funcs[131] = (void*)s_CallStaticIntMethod;     // A variant
    g_jni_funcs[132] = (void*)s_RetLong;
    g_jni_funcs[133] = (void*)s_RetLongV;
    g_jni_funcs[134] = (void*)s_RetLong;
    g_jni_funcs[135] = (void*)s_RetFloat;
    g_jni_funcs[136] = (void*)s_RetFloatV;
    g_jni_funcs[137] = (void*)s_RetFloat;
    g_jni_funcs[138] = (void*)s_RetDouble;
    g_jni_funcs[139] = (void*)s_RetDoubleV;
    g_jni_funcs[140] = (void*)s_RetDouble;
    g_jni_funcs[141] = (void*)s_CallStaticVoidMethod;
    g_jni_funcs[142] = (void*)s_CallStaticVoidMethodV;
    g_jni_funcs[143] = (void*)s_CallStaticVoidMethod;    // A variant
    // GetStaticFieldID + Get/SetStaticXxxField 144-162
    g_jni_funcs[144] = (void*)s_GetStaticFieldID;
    g_jni_funcs[145] = (void*)s_GetStaticObjField;
    for (int i = 146; i <= 153; i++) g_jni_funcs[i] = (void*)s_GetStaticIntField;
    for (int i = 154; i <= 162; i++) g_jni_funcs[i] = (void*)s_SetStaticField;
    // Strings 163-170
    g_jni_funcs[163] = (void*)s_NewString;
    g_jni_funcs[164] = (void*)s_GetStringLength;
    g_jni_funcs[165] = (void*)s_GetStringChars;
    g_jni_funcs[166] = (void*)s_ReleaseStringChars;
    g_jni_funcs[167] = (void*)s_NewStringUTF;
    g_jni_funcs[168] = (void*)s_GetStringUTFLength;
    g_jni_funcs[169] = (void*)s_GetStringUTFChars;
    g_jni_funcs[170] = (void*)s_ReleaseStringUTFChars;
    // Arrays 171-214
    g_jni_funcs[171] = (void*)s_GetArrayLength;
    g_jni_funcs[172] = (void*)s_NewObjectArray;
    g_jni_funcs[173] = (void*)s_GetObjectArrayElement;
    g_jni_funcs[174] = (void*)s_SetObjectArrayElement;
    g_jni_funcs[175] = (void*)s_NewBoolArray;
    g_jni_funcs[176] = (void*)s_NewByteArray;
    g_jni_funcs[177] = (void*)s_NewCharArray;
    g_jni_funcs[178] = (void*)s_NewShortArray;
    g_jni_funcs[179] = (void*)s_NewIntArray;
    g_jni_funcs[180] = (void*)s_NewLongArray;
    g_jni_funcs[181] = (void*)s_NewFloatArray;
    g_jni_funcs[182] = (void*)s_NewDoubleArray;
    g_jni_funcs[183] = (void*)s_GetElements;  // GetBooleanArrayElements
    g_jni_funcs[184] = (void*)s_GetByteElements;
    g_jni_funcs[185] = (void*)s_GetElements;
    g_jni_funcs[186] = (void*)s_GetElements;
    g_jni_funcs[187] = (void*)s_GetElements;
    g_jni_funcs[188] = (void*)s_GetElements;
    g_jni_funcs[189] = (void*)s_GetElements;
    g_jni_funcs[190] = (void*)s_GetElements;
    for (int i = 191; i <= 198; i++) g_jni_funcs[i] = (void*)s_ReleaseElements;
    g_jni_funcs[199] = (void*)s_GetArrayRegion;
    g_jni_funcs[200] = (void*)s_GetByteRegion;
    for (int i = 201; i <= 206; i++) g_jni_funcs[i] = (void*)s_GetArrayRegion;
    g_jni_funcs[207] = (void*)s_SetArrayRegion;
    g_jni_funcs[208] = (void*)s_SetByteRegion;
    for (int i = 209; i <= 214; i++) g_jni_funcs[i] = (void*)s_SetArrayRegion;
    // Misc 215-232
    g_jni_funcs[215] = (void*)s_RegisterNatives;
    g_jni_funcs[216] = (void*)s_UnregisterNatives;
    g_jni_funcs[217] = (void*)s_MonitorEnter;
    g_jni_funcs[218] = (void*)s_MonitorExit;
    g_jni_funcs[219] = (void*)s_GetJavaVM;
    g_jni_funcs[220] = (void*)s_GetStringRegion;
    g_jni_funcs[221] = (void*)s_GetStringUTFRegion;
    g_jni_funcs[222] = (void*)s_GetPrimArrayCritical;
    g_jni_funcs[223] = (void*)s_ReleasePrimArrayCritical;
    g_jni_funcs[224] = (void*)s_GetStringCritical;
    g_jni_funcs[225] = (void*)s_ReleaseStringCritical;
    g_jni_funcs[226] = (void*)s_NewWeakGlobalRef;
    g_jni_funcs[227] = (void*)s_DeleteWeakGlobalRef;
    g_jni_funcs[228] = (void*)s_ExceptionCheck;
    g_jni_funcs[229] = (void*)s_NewDirectByteBuffer;
    g_jni_funcs[230] = (void*)s_GetDirectBufferAddress;
    g_jni_funcs[231] = (void*)s_GetDirectBufferCapacity;
    g_jni_funcs[232] = (void*)s_GetObjectRefType;

    // JavaVM table
    g_vm_funcs[3] = (void*)vm_DestroyJavaVM;
    g_vm_funcs[4] = (void*)vm_AttachCurrentThread;
    g_vm_funcs[5] = (void*)vm_DetachCurrentThread;
    g_vm_funcs[6] = (void*)vm_GetEnv;
    g_vm_funcs[7] = (void*)vm_AttachDaemon;

    // Build indirection chain
    g_jni_inner = (void*)g_jni_funcs;
    g_jni_outer = (void*)&g_jni_inner;
    g_vm_inner  = (void*)g_vm_funcs;
    g_vm_outer  = (void*)&g_vm_inner;

    cl->vm_outer  = g_vm_outer;
    cl->env_outer = g_jni_outer;
}
