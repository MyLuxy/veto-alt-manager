// VetoAltManager - Injected DLL
// Communicates with injector via named pipe, uses JNI to replace Minecraft Session

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <jni.h>
#include <jvmti.h>
// <string> removed — all std::string replaced with C char arrays
#include <vector>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

#define PIPE_NAME "\\\\.\\pipe\\VetoAltMgr"
#define MAX_LOG 256

// Forward declarations
void WINAPI PipeThread(LPVOID lpParam);
JavaVM* FindJavaVM();
jobject GetMinecraft(JNIEnv* env);
jfieldID FindSessionFieldID(JNIEnv* env, jobject mc);
jobject CreateOfflineSession(JNIEnv* env, jclass sessionClass, const char* username);
const char* ReplaceSession(const char* username);
const char* RestoreSession();

static HMODULE g_hModule = NULL;
static JavaVM* g_jvm = NULL;
static char g_log[4096] = {0};  // bigger buffer for log history
static CRITICAL_SECTION g_logLock;

// For Badlion: session might be in weird place, store instance directly
static jclass g_sessionContainer = NULL;
static bool g_sessionIsStatic = false;
static jobject g_foundSessionInstance = NULL;

// Original session backup — set on first CHANGE, used for RESTORE
static jobject   g_originalSession   = NULL;  // global ref to original session object
static jfieldID  g_savedField        = NULL;  // field we set (NULL if sentinel/in-place path)
static bool      g_savedIsStatic     = false;
static jclass    g_savedContainer    = NULL;  // global ref (only when g_savedIsStatic)

// Helper: copy all String field values from source object to target object (same class)
void CopyStringFieldsInPlace(JNIEnv* env, jobject target, jobject source) {
    jclass cls = env->GetObjectClass(target);
    jclass clsClass = env->FindClass("java/lang/Class");
    jmethodID clsGetName = env->GetMethodID(clsClass, "getName", "()Ljava/lang/String;");
    jmethodID getDF = env->GetMethodID(clsClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    if (!getDF || !clsGetName || env->ExceptionCheck()) { env->ExceptionClear(); return; }

    jobjectArray fields = (jobjectArray)env->CallObjectMethod(cls, getDF);
    if (!fields || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cls); return; }

    jclass fClass = env->FindClass("java/lang/reflect/Field");
    jmethodID fGetType = env->GetMethodID(fClass, "getType", "()Ljava/lang/Class;");
    jmethodID fSetAcc = env->GetMethodID(fClass, "setAccessible", "(Z)V");
    jmethodID fGet = env->GetMethodID(fClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    jmethodID fSet = env->GetMethodID(fClass, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V");
    if (!fGetType || !fSetAcc || !fGet || !fSet || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cls); return; }

    jsize count = env->GetArrayLength(fields);
    for (jsize i = 0; i < count; i++) {
        jobject field = env->GetObjectArrayElement(fields, i);
        if (!field) continue;
        jclass ft = (jclass)env->CallObjectMethod(field, fGetType);
        if (!ft || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        jstring ftn = (jstring)env->CallObjectMethod(ft, clsGetName);
        const char* ftnStr = env->GetStringUTFChars(ftn, NULL);
        bool isString = ftnStr && strcmp(ftnStr, "java.lang.String") == 0;
        env->ReleaseStringUTFChars(ftn, ftnStr);
        env->DeleteLocalRef(ftn);
        env->DeleteLocalRef(ft);
        if (!isString) continue;

        env->CallVoidMethod(field, fSetAcc, JNI_TRUE);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        jobject srcVal = env->CallObjectMethod(field, fGet, source);
        if (!env->ExceptionCheck() && srcVal) {
            env->CallVoidMethod(field, fSet, target, srcVal);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(srcVal);
    }
    env->DeleteLocalRef(cls);
}

void DebugLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf)-1, fmt, args);
    va_end(args);

    // OutputDebugString (visible via DebugView)
    OutputDebugStringA("[VetoAlt-DLL] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    // Also write to a file
    FILE* f = fopen("C:\\Users\\alexa\\Desktop\\VetoAlts\\dll_log.txt", "a");
    if (f) {
        fprintf(f, "[%lu] %s\n", GetCurrentThreadId(), buf);
        fclose(f);
    }
}

// ========== DLL Entry Point ==========
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);

        // Clear log file on new injection
        FILE* f = fopen("C:\\Users\\alexa\\Desktop\\VetoAlts\\dll_log.txt", "w");
        if (f) fclose(f);

        // Try to find JVM now — if it fails, PipeThread will retry later
        g_jvm = FindJavaVM();
        if (g_jvm) {
            DebugLog("Found Java VM at %p", (void*)g_jvm);
        } else {
            DebugLog("JVM not loaded yet — will retry in pipe thread");
        }

        // ALWAYS start the pipe listener thread (it retries FindJavaVM as needed)
        HANDLE hThread = CreateThread(NULL, 0,
            (LPTHREAD_START_ROUTINE)PipeThread, NULL, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}

// ========== Find JVM in the current process ==========
JavaVM* FindJavaVM() {
    // Try to find jvm.dll and get JNI_GetCreatedJavaVMs
    HMODULE hJVM = GetModuleHandleA("jvm.dll");
    if (hJVM == NULL) {
        // Try alternate names
        const char* jvmDlls[] = {
            "jvm.dll", "hotspot.dll", "server/jvm.dll", "client/jvm.dll"
        };
        for (const char* name : jvmDlls) {
            hJVM = GetModuleHandleA(name);
            if (hJVM) break;
        }
    }

    if (hJVM == NULL) {
        DebugLog("jvm.dll not loaded in this process yet");
        return NULL;
    }

    typedef jint (JNICALL *GetCreatedJVMsFn)(JavaVM**, jsize, jsize*);
    GetCreatedJVMsFn JNI_GetCreatedJavaVMs =
        (GetCreatedJVMsFn)GetProcAddress(hJVM, "JNI_GetCreatedJavaVMs");

    if (JNI_GetCreatedJavaVMs == NULL) {
        DebugLog("JNI_GetCreatedJavaVMs not found in jvm.dll");
        return NULL;
    }

    JavaVM* jvm = NULL;
    jsize count = 0;
    jint result = JNI_GetCreatedJavaVMs(&jvm, 1, &count);

    if (result != JNI_OK || count == 0 || jvm == NULL) {
        DebugLog("JNI_GetCreatedJavaVMs returned %d, count=%d", result, count);
        return NULL;
    }

    return jvm;
}

// ========== Named Pipe Server (receives commands from injector) ==========
void WINAPI PipeThread(LPVOID lpParam) {
    DebugLog("Pipe thread started");

    // Retry FindJavaVM until it succeeds (JVM may not be loaded yet)
    for (int tries = 0; tries < 90 && g_jvm == NULL; tries++) {
        g_jvm = FindJavaVM();
        if (g_jvm == NULL) {
            if (tries % 10 == 0) DebugLog("Waiting for JVM... (%ds)", tries);
            Sleep(1000);
        }
    }
    if (g_jvm) {
        DebugLog("JVM ready in pipe thread");
    } else {
        DebugLog("JVM NOT FOUND after 60s — pipe will refuse CHANGE commands");
    }

    DebugLog("Waiting for pipe commands...");

    while (true) {
        HANDLE hPipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            1024, 1024, 0, NULL
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            DebugLog("CreateNamedPipe failed: %d", GetLastError());
            Sleep(1000);
            continue;
        }

        // Wait for injector to connect
        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE :
            (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        // Read command
        char buffer[1024] = {0};
        DWORD bytesRead = 0;
        if (!ReadFile(hPipe, buffer, sizeof(buffer)-1, &bytesRead, NULL)) {
            CloseHandle(hPipe);
            continue;
        }
        buffer[bytesRead] = '\0';

        DebugLog("Received: %s", buffer);

        // Process command
        const char* response = "FAIL: unknown command";

        if (strncmp(buffer, "CHANGE ", 7) == 0) {
            const char* username = buffer + 7;
            // Trim whitespace
            while (*username == ' ') username++;
            if (strlen(username) > 0) {
                const char* err = ReplaceSession(username);
                if (err == NULL) {
                    response = "OK";
                    DebugLog("Session changed to: %s", username);
                } else {
                    response = err;
                    DebugLog("Failed: %s for user: %s", err, username);
                }
            } else {
                response = "FAIL: empty username";
            }
        } else if (strcmp(buffer, "RESTORE") == 0) {
            const char* err = RestoreSession();
            if (err == NULL) {
                response = "OK";
                DebugLog("Session restored to original");
            } else {
                response = err;
                DebugLog("Restore failed: %s", err);
            }
        } else if (strcmp(buffer, "PING") == 0) {
            response = "PONG";
        } else if (strcmp(buffer, "GETLOG") == 0) {
            response = g_log;
        }

        // Send response
        DWORD bytesWritten = 0;
        WriteFile(hPipe, response, (DWORD)strlen(response), &bytesWritten, NULL);
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

// ========== Verify class is really Minecraft ==========
// Badlion can have a bogus "ave" class visible via FindClass.
// Check for real Minecraft methods/fields before trusting it.
bool IsMinecraftClass(JNIEnv* env, jclass c) {
    const char* methods[] = {"getMinecraft", "func_71410_x"};
    const char* methodSigs[] = {
        "()Lnet/minecraft/client/Minecraft;",
        "()Lave;",
        "()Ljava/lang/Object;",
    };
    for (const char* m : methods) {
        for (const char* s : methodSigs) {
            jmethodID mid = env->GetStaticMethodID(c, m, s);
            if (mid && !env->ExceptionCheck()) return true;
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    const char* fields[] = {"theMinecraft", "field_71432_P", "instance"};
    const char* fieldSigs[] = {
        "Lnet/minecraft/client/Minecraft;",
        "Lave;",
        "Ljava/lang/Object;",
    };
    for (const char* f : fields) {
        for (const char* s : fieldSigs) {
            jfieldID fid = env->GetStaticFieldID(c, f, s);
            if (fid && !env->ExceptionCheck()) return true;
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    return false;
}

// ========== Find Minecraft class ==========
// Uses FindClass (works for Vanilla). For Lunar/Badlion, falls back to
// scanning the context class loader of every live thread.
jclass FindMinecraftClass(JNIEnv* env) {
    // 1) Direct FindClass (Vanilla) — verify it's really Minecraft
    const char* jniNames[] = {"net/minecraft/client/Minecraft", "ave"};
    for (const char* n : jniNames) {
        jclass c = env->FindClass(n);
        if (c && !env->ExceptionCheck()) {
            if (IsMinecraftClass(env, c)) {
                DebugLog("FindMC: FindClass %s (verified)", n);
                return c;
            }
            DebugLog("FindMC: FindClass %s found but NOT Minecraft — ignoring", n);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // 2) Lunar/Badlion: try context class loader of every thread
    //    Thread.enumerate and getName are bootstrap APIs, always safe.
    DebugLog("FindMC: scanning thread context class loaders...");
    jclass thrClass = env->FindClass("java/lang/Thread");
    if (!thrClass) return NULL;

    jmethodID getName = env->GetMethodID(thrClass, "getName", "()Ljava/lang/String;");
    jmethodID getCL = env->GetMethodID(thrClass, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    jmethodID enumerate = env->GetStaticMethodID(thrClass, "enumerate", "([Ljava/lang/Thread;)I");
    jclass classClass = env->FindClass("java/lang/Class");
    jmethodID forName = env->GetStaticMethodID(classClass, "forName",
    "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    if (!thrClass || !getName || !getCL || !enumerate || !classClass || !forName) {
        DebugLog("FindMC: failed to get bootstrap method IDs");
        return NULL;
    }

    // Try the current thread's context class loader first (simplest)
    jmethodID curThread = env->GetStaticMethodID(thrClass, "currentThread", "()Ljava/lang/Thread;");
    jobject me = env->CallStaticObjectMethod(thrClass, curThread);
    jobject myCL = env->CallObjectMethod(me, getCL);
    if (myCL) {
        for (const char* n : jniNames) {
            char jn[256]; strncpy(jn, n, 255); jn[255] = '\0';
            for (char* cp = jn; *cp; cp++) if (*cp == '/') *cp = '.';
            jstring ns = env->NewStringUTF(jn);
            // Try without initializing first (Badlion might have broken static init)
            jclass c = (jclass)env->CallStaticObjectMethod(classClass, forName, ns, JNI_FALSE, myCL);
            env->DeleteLocalRef(ns);
            if (!env->ExceptionCheck() && c) {
                if (IsMinecraftClass(env, c)) {
                    DebugLog("FindMC: via own CL (verified, noinit)");
                    return c;
                }
                DebugLog("FindMC: own CL class '%s' not Minecraft — continuing", n);
            } else {
                // Try with initialization if noinit failed
                if (env->ExceptionCheck()) env->ExceptionClear();
                ns = env->NewStringUTF(jn);
                c = (jclass)env->CallStaticObjectMethod(classClass, forName, ns, JNI_TRUE, myCL);
                env->DeleteLocalRef(ns);
                if (!env->ExceptionCheck() && c && IsMinecraftClass(env, c)) {
                    DebugLog("FindMC: via own CL (verified, init)");
                    return c;
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // Enumerate all threads and try each one's context class loader
    jobjectArray threads = env->NewObjectArray(50, thrClass, NULL);
    jint count = env->CallStaticIntMethod(thrClass, enumerate, threads);
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            jobject t = env->GetObjectArrayElement(threads, i);
            if (!t) continue;
            jobject cl = env->CallObjectMethod(t, getCL);
            if (cl == myCL) continue; // already tried
            if (cl) {
                for (const char* n : jniNames) {
                    char jn2[256]; strncpy(jn2, n, 255); jn2[255] = '\0';
                    for (char* cp = jn2; *cp; cp++) if (*cp == '/') *cp = '.';
                    jstring ns = env->NewStringUTF(jn2);
                    jclass c = (jclass)env->CallStaticObjectMethod(classClass, forName, ns, JNI_TRUE, cl);
                    env->DeleteLocalRef(ns);
                    if (!env->ExceptionCheck() && c) {
                        if (IsMinecraftClass(env, c)) {
                            DebugLog("FindMC: via thread %d CL (verified)", i);
                            return c;
                        }
                        DebugLog("FindMC: thread %d class '%s' not Minecraft — continuing", i, n);
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
            }
        }
    }

    // 3) Fallback for Badlion: scan thread's private "target" field
    //    Minecraft's Client thread has its Minecraft instance as the Runnable target
    DebugLog("FindMC: scanning thread targets...");
    jfieldID targetFid = env->GetFieldID(thrClass, "target", "Ljava/lang/Runnable;");
    jclass objClass2 = env->FindClass("java/lang/Object");
    jmethodID runnableGetClass = env->GetMethodID(objClass2, "getClass", "()Ljava/lang/Class;");
    if (targetFid && objClass2 && runnableGetClass) {
        for (int i = 0; i < count; i++) {
            jobject t = env->GetObjectArrayElement(threads, i);
            if (!t) continue;
            jobject target = env->GetObjectField(t, targetFid);
            if (target) {
                jclass targetClass = (jclass)env->CallObjectMethod(target, runnableGetClass);
                if (targetClass && !env->ExceptionCheck()) {
                    if (IsMinecraftClass(env, targetClass)) {
                        DebugLog("FindMC: via thread %d target (Runnable)", i);
                        return targetClass;
                    }
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // 4) Fallback: scan all thread stack traces for Minecraft classes
    DebugLog("FindMC: scanning thread stack traces...");
    jclass stackFrameClass = env->FindClass("java/lang/StackTraceElement");
    jmethodID stackGetClass = env->GetMethodID(stackFrameClass, "getClassName", "()Ljava/lang/String;");
    jmethodID threadGetStack = env->GetMethodID(thrClass, "getStackTrace", "()[Ljava/lang/StackTraceElement;");
    if (stackFrameClass && stackGetClass && threadGetStack) {
        for (int i = 0; i < count; i++) {
            jobject t = env->GetObjectArrayElement(threads, i);
            if (!t) continue;
            jobjectArray stack = (jobjectArray)env->CallObjectMethod(t, threadGetStack);
            if (!stack || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            jsize stackLen = env->GetArrayLength(stack);
            for (jsize si = 0; si < stackLen && si < 5; si++) {
                jobject frame = env->GetObjectArrayElement(stack, si);
                if (!frame) continue;
                jstring clsStr = (jstring)env->CallObjectMethod(frame, stackGetClass);
                if (!clsStr || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                const char* clsName = env->GetStringUTFChars(clsStr, NULL);
                if (!clsName) { env->ReleaseStringUTFChars(clsStr, clsName); env->DeleteLocalRef(clsStr); continue; }
                // Look for any Minecraft-related class
                if (strstr(clsName, "inecraft") || strstr(clsName, "net.minecraft")) {
                    DebugLog("FindMC: found Minecraft class in stack: %s", clsName);
                    // Convert dots to slashes for FindClass
                    char jniName[512]; strncpy(jniName, clsName, 511); jniName[511] = '\0';
                    for (char* ch = jniName; *ch; ch++) if (*ch == '.') *ch = '/';
                    jclass found = env->FindClass(jniName);
                    if (found && !env->ExceptionCheck() && IsMinecraftClass(env, found)) {
                        env->ReleaseStringUTFChars(clsStr, clsName);
                        DebugLog("FindMC: via stack trace from thread %d", i);
                        return found;
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                env->ReleaseStringUTFChars(clsStr, clsName);
                env->DeleteLocalRef(clsStr);
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // 5) JVMTI: enumerate ALL loaded classes in the entire JVM
    //    Bypasses ClassLoader encapsulation issues that break the classes field approach
    DebugLog("FindMC: trying JVMTI GetLoadedClasses...");
    jvmtiEnv* jvmti = NULL;
    jint jvmtierr = g_jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0);
    if (jvmtierr == JNI_OK && jvmti) {
        jint classCount = 0;
        jclass* classes = NULL;
        jvmtiError jerr = jvmti->GetLoadedClasses(&classCount, &classes);
        DebugLog("FindMC: JVMTI returned %d classes, err=%d", classCount, jerr);
        if (jerr == JVMTI_ERROR_NONE && classes) {
            for (jint i = 0; i < classCount; i++) {
                if (classes[i] && IsMinecraftClass(env, classes[i])) {
                    jclass result = (jclass)env->NewGlobalRef(classes[i]);
                    jvmti->Deallocate((unsigned char*)classes);
                    DebugLog("FindMC: via JVMTI GetLoadedClasses");
                    return result;
                }
            }
            DebugLog("FindMC: JVMTI scanned %d classes, no match", classCount);
            jvmti->Deallocate((unsigned char*)classes);
        }
    } else {
        DebugLog("FindMC: JVMTI not available (err=%d)", jvmtierr);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    DebugLog("FindMC: ALL FAILED");
    return NULL;
}

// JVMTI heap iteration: tag the first instance found, then retrieve it
static jvmtiIterationControl JNICALL HeapFindFirstCb(
    jlong class_tag, jlong size, jlong* tag_ptr, void* user_data)
{
    (void)class_tag; (void)size; (void)user_data;
    *tag_ptr = 1;
    return JVMTI_ITERATION_ABORT;
}

// ========== JNI: Get Minecraft singleton ==========
jobject GetMinecraft(JNIEnv* env) {
    jclass mcClass = FindMinecraftClass(env);
    if (!mcClass) {
        // Strategy A: vanilla 1.8.9 — "ave" is the Minecraft class but IsMinecraftClass()
        // fails because all method/field names are obfuscated.  The singleton is stored in
        // a static field of type "ave" inside "ave" itself (theMinecraft → single-letter name).
        // Find it by type, not by name.
        DebugLog("GetMinecraft: trying vanilla static self-ref scan on 'ave'...");
        {
            jclass aveClass = env->FindClass("ave");
            if (!env->ExceptionCheck() && aveClass) {
                jclass clsC = env->FindClass("java/lang/Class");
                jmethodID gDF   = env->GetMethodID(clsC, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
                jclass fC       = env->FindClass("java/lang/reflect/Field");
                jmethodID gMods = env->GetMethodID(fC, "getModifiers", "()I");
                jmethodID gType = env->GetMethodID(fC, "getType", "()Ljava/lang/Class;");
                jmethodID setAcc= env->GetMethodID(fC, "setAccessible", "(Z)V");
                jmethodID fGet  = env->GetMethodID(fC, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
                if (gDF && gMods && gType && setAcc && fGet && !env->ExceptionCheck()) {
                    jobjectArray fields = (jobjectArray)env->CallObjectMethod(aveClass, gDF);
                    if (fields && !env->ExceptionCheck()) {
                        jsize n = env->GetArrayLength(fields);
                        DebugLog("GetMinecraft: ave has %d declared fields", (int)n);
                        for (jsize i = 0; i < n; i++) {
                            jobject field = env->GetObjectArrayElement(fields, i);
                            if (!field) continue;
                            jint mods = env->CallIntMethod(field, gMods);
                            if (!(mods & 0x0008)) { env->DeleteLocalRef(field); continue; } // skip non-static
                            jclass ft = (jclass)env->CallObjectMethod(field, gType);
                            bool selfType = ft && env->IsSameObject(ft, aveClass);
                            if (ft) env->DeleteLocalRef(ft);
                            if (!selfType) { env->DeleteLocalRef(field); continue; }
                            // Static field of type ave — this is theMinecraft
                            env->CallVoidMethod(field, setAcc, JNI_TRUE);
                            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(field); continue; }
                            jobject val = env->CallObjectMethod(field, fGet, (jobject)NULL);
                            env->DeleteLocalRef(field);
                            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                            if (val) {
                                DebugLog("GetMinecraft: found via ave self-ref static field");
                                env->DeleteLocalRef(aveClass);
                                return val;
                            }
                        }
                        env->DeleteLocalRef(fields);
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                env->DeleteLocalRef(aveClass);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // Strategy B: JVMTI heap scan — find a live instance of the obfuscated Minecraft class.
        // This is the most direct approach for pure vanilla where no static getter exists.
        DebugLog("GetMinecraft: trying JVMTI heap instance scan...");
        {
            jvmtiEnv* jvmtiH = NULL;
            if (g_jvm->GetEnv((void**)&jvmtiH, JVMTI_VERSION_1_0) == JNI_OK && jvmtiH) {
                jvmtiCapabilities caps = {0};
                caps.can_tag_objects = 1;
                jvmtiError capErr = jvmtiH->AddCapabilities(&caps);
                DebugLog("GetMinecraft: can_tag_objects AddCapabilities=%d", capErr);
                if (capErr == JVMTI_ERROR_NONE || capErr == JVMTI_ERROR_NOT_AVAILABLE) {
                    const char* klNames[] = { "ave", "net/minecraft/client/Minecraft", NULL };
                    for (int ki = 0; klNames[ki]; ki++) {
                        jclass kl = env->FindClass(klNames[ki]);
                        if (!kl || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                        jvmtiError itErr = jvmtiH->IterateOverInstancesOfClass(
                            kl, JVMTI_HEAP_OBJECT_EITHER, HeapFindFirstCb, NULL);
                        env->DeleteLocalRef(kl);
                        DebugLog("GetMinecraft: IterateOverInstances('%s')=%d", klNames[ki], itErr);
                        if (itErr != JVMTI_ERROR_NONE) continue;
                        jlong searchTags[] = { 1 };
                        jobject* objs = NULL;
                        jint cnt = 0;
                        jvmtiError gtErr = jvmtiH->GetObjectsWithTags(1, searchTags, &cnt, &objs, NULL);
                        DebugLog("GetMinecraft: GetObjectsWithTags=%d cnt=%d", gtErr, cnt);
                        if (gtErr == JVMTI_ERROR_NONE && objs && cnt > 0) {
                            jobject mc = env->NewLocalRef(objs[0]);
                            jvmtiH->Deallocate((unsigned char*)objs);
                            if (mc) {
                                DebugLog("GetMinecraft: found via heap scan (class '%s')", klNames[ki]);
                                return mc;
                            }
                        }
                        if (objs) jvmtiH->Deallocate((unsigned char*)objs);
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        DebugLog("GetMinecraft: FindMinecraftClass failed, trying 'Client thread' target...");
        // Fallback B: Pure vanilla: Minecraft implements Runnable and runs on the thread named "Client thread".
        // The thread's 'target' field IS the Minecraft instance. This name is never obfuscated.
        // In vanilla 1.8.9 launched via the official launcher, Minecraft IS the thread
        // (it extends Thread or its target is cleared). Strategy:
        // 1. Find "Client thread" by name
        // 2. Try its Runnable target field (may be null if thread subclass pattern)
        // 3. If null, scan ALL object fields of the thread object itself for one with >50 fields
        //    (Minecraft stores itself as fields inherited from Thread or the thread IS Minecraft)
        jvmtiEnv* jvmtiT = NULL;
        g_jvm->GetEnv((void**)&jvmtiT, JVMTI_VERSION_1_0);

        jclass thrCls = env->FindClass("java/lang/Thread");
        if (thrCls && !env->ExceptionCheck()) {
            jmethodID enumerate = env->GetStaticMethodID(thrCls, "enumerate", "([Ljava/lang/Thread;)I");
            jmethodID getName   = env->GetMethodID(thrCls, "getName", "()Ljava/lang/String;");
            jfieldID  targetFid = env->GetFieldID(thrCls, "target", "Ljava/lang/Runnable;");
            if (env->ExceptionCheck()) env->ExceptionClear();

            if (enumerate && getName) {
                jobjectArray threads = env->NewObjectArray(200, thrCls, NULL);
                jint cnt = env->CallStaticIntMethod(thrCls, enumerate, threads);
                DebugLog("GetMinecraft: scanning %d threads", cnt);

                const char* mcNames[] = { "Client thread", "main", "Minecraft main thread", NULL };
                jobject mcThread = NULL; // the thread object named "Client thread"

                for (jint i = 0; i < cnt; i++) {
                    jobject t = env->GetObjectArrayElement(threads, i);
                    if (!t) continue;
                    jstring jn = (jstring)env->CallObjectMethod(t, getName);
                    const char* ns = jn ? env->GetStringUTFChars(jn, NULL) : NULL;
                    DebugLog("GetMinecraft: thread[%d] = '%s'", i, ns ? ns : "?");
                    bool match = false;
                    if (ns) for (int k = 0; mcNames[k]; k++) if (strcmp(ns, mcNames[k]) == 0) { match = true; break; }
                    if (ns) env->ReleaseStringUTFChars(jn, ns);
                    if (jn) env->DeleteLocalRef(jn);
                    if (match && !mcThread) mcThread = t;
                    else env->DeleteLocalRef(t);
                }
                env->DeleteLocalRef(threads);

                if (mcThread) {
                    // Step 1: try Runnable target field
                    jobject bestTarget = NULL;
                    if (targetFid) {
                        bestTarget = env->GetObjectField(mcThread, targetFid);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); bestTarget = NULL; }
                        if (bestTarget) DebugLog("GetMinecraft: Client thread target non-null");
                    }

                    // Step 2: if target null, scan all fields of the thread object itself
                    // (Minecraft may extend Thread, so the thread IS the mc instance)
                    if (!bestTarget && jvmtiT) {
                        DebugLog("GetMinecraft: target null — scanning thread's own fields...");
                        jclass tCls = env->GetObjectClass(mcThread);
                        // Walk class hierarchy (Thread -> Object)
                        while (tCls && !bestTarget) {
                            jint fc = 0; jfieldID* fids = NULL;
                            if (jvmtiT->GetClassFields(tCls, &fc, &fids) == JVMTI_ERROR_NONE && fids) {
                                for (jint fi = 0; fi < fc && !bestTarget; fi++) {
                                    jint mods = 0;
                                    jvmtiT->GetFieldModifiers(tCls, fids[fi], &mods);
                                    if (mods & 0x0008) continue; // skip static
                                    char* fsig = NULL;
                                    jvmtiT->GetFieldName(tCls, fids[fi], NULL, &fsig, NULL);
                                    bool isRef = fsig && fsig[0] == 'L';
                                    if (fsig) jvmtiT->Deallocate((unsigned char*)fsig);
                                    if (!isRef) continue;
                                    jobject fval = env->GetObjectField(mcThread, fids[fi]);
                                    if (!fval || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                                    jclass fvc = env->GetObjectClass(fval);
                                    jint ffc = 0; jfieldID* ffids = NULL;
                                    if (jvmtiT->GetClassFields(fvc, &ffc, &ffids) == JVMTI_ERROR_NONE && ffids) {
                                        if (ffc > 50) {
                                            DebugLog("GetMinecraft: found field with %d fields in thread obj", ffc);
                                            bestTarget = fval;
                                        }
                                        jvmtiT->Deallocate((unsigned char*)ffids);
                                    }
                                    env->DeleteLocalRef(fvc);
                                    if (!bestTarget) env->DeleteLocalRef(fval);
                                }
                                jvmtiT->Deallocate((unsigned char*)fids);
                            }
                            // Check if the thread ITSELF has >80 fields (thread IS Minecraft)
                            if (!bestTarget && fc > 80) {
                                DebugLog("GetMinecraft: thread itself has %d fields — IS Minecraft", fc);
                                bestTarget = env->NewLocalRef(mcThread);
                            }
                            jclass superCls = env->GetSuperclass(tCls);
                            env->DeleteLocalRef(tCls);
                            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
                            tCls = superCls;
                            if (!tCls) break;
                            jstring scname = (jstring)env->CallObjectMethod(tCls,
                                env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;"));
                            const char* scnstr = scname ? env->GetStringUTFChars(scname, NULL) : NULL;
                            bool isObj = scnstr && strcmp(scnstr, "java.lang.Object") == 0;
                            if (scnstr) env->ReleaseStringUTFChars(scname, scnstr);
                            if (scname) env->DeleteLocalRef(scname);
                            if (isObj) { env->DeleteLocalRef(tCls); tCls = NULL; }
                        }
                        if (tCls) env->DeleteLocalRef(tCls);
                    }

                    env->DeleteLocalRef(mcThread);
                    if (bestTarget) {
                        env->DeleteLocalRef(thrCls);
                        DebugLog("GetMinecraft: found via Client thread");
                        return bestTarget;
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(thrCls);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        DebugLog("GetMinecraft: class not found");
        return NULL;
    }

    // Log the actual class name to verify what we found
    jclass objClass = env->FindClass("java/lang/Object");
    jmethodID getClassMethod = env->GetMethodID(objClass, "getClass", "()Ljava/lang/Class;");
    jmethodID getNameMethod = env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
    if (getClassMethod && getNameMethod) {
        jstring clsName = (jstring)env->CallObjectMethod(mcClass, getNameMethod);
        const char* cstr = env->GetStringUTFChars(clsName, NULL);
        DebugLog("GetMinecraft: found class '%s'", cstr);
        env->ReleaseStringUTFChars(clsName, cstr);
        env->DeleteLocalRef(clsName);
    }

    // Try getMinecraft() static method with various return types
    bool anyMethodFound = false;
    jmethodID getMc = NULL;
    struct { const char* name; const char* ret; } mcMethods[] = {
        {"getMinecraft",  "()Lnet/minecraft/client/Minecraft;"},
        {"getMinecraft",  "()Lave;"},
        {"func_71410_x",  "()Lnet/minecraft/client/Minecraft;"},
        {"func_71410_x",  "()Lave;"},
        {"getMinecraft",  "()Ljava/lang/Object;"},
        {"func_71410_x",  "()Ljava/lang/Object;"},
    };
    for (auto& m : mcMethods) {
        getMc = env->GetStaticMethodID(mcClass, m.name, m.ret);
        if (getMc != NULL && !env->ExceptionCheck()) {
            anyMethodFound = true;
            DebugLog("GetMinecraft: found method %s with sig %s", m.name, m.ret);
            break;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        getMc = NULL;
    }

    if (getMc) {
        jobject mc = env->CallStaticObjectMethod(mcClass, getMc);
        if (!env->ExceptionCheck() && mc) {
            env->DeleteLocalRef(mcClass);
            DebugLog("GetMinecraft: OK via getMinecraft()");
            return mc;
        }
        if (env->ExceptionCheck()) {
            DebugLog("GetMinecraft: getMinecraft() threw exception");
            env->ExceptionClear();
        } else {
            DebugLog("GetMinecraft: getMinecraft() returned NULL");
        }
    } else {
        DebugLog("GetMinecraft: no getMinecraft method found (anyMethodFound=%d)", anyMethodFound);
    }

    // Fallback: scan static fields for the singleton
    const char* fieldNames[] = {"theMinecraft", "field_71432_P", "instance", "a", "mc", "INSTANCE"};
    const char* fieldTypes[] = {"Lnet/minecraft/client/Minecraft;", "Lave;", "Ljava/lang/Object;"};
    for (const char* fn : fieldNames) {
        for (const char* ft : fieldTypes) {
            jfieldID fid = env->GetStaticFieldID(mcClass, fn, ft);
            if (fid != NULL && !env->ExceptionCheck()) {
                jobject mc = env->GetStaticObjectField(mcClass, fid);
                bool hadExc = env->ExceptionCheck();
                if (!hadExc) env->ExceptionClear();
                DebugLog("GetMinecraft: field '%s'/'%s' found, value=%s", fn, ft, mc ? "NON-NULL" : "NULL");
                if (mc) { env->DeleteLocalRef(mcClass); return mc; }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // Last resort: try to find Minecraft instance via MinecraftServer.getServer()
    DebugLog("GetMinecraft: trying MinecraftServer.getServer() fallback...");
    jclass serverClass = env->FindClass("net/minecraft/server/MinecraftServer");
    if (!serverClass && env->ExceptionCheck()) {
        env->ExceptionClear();
        serverClass = env->FindClass("nc");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (serverClass) {
        jmethodID getServer = env->GetStaticMethodID(serverClass, "getServer", "()Lnet/minecraft/server/MinecraftServer;");
        if (!getServer || env->ExceptionCheck()) {
            env->ExceptionClear();
            getServer = env->GetStaticMethodID(serverClass, "getServer", "()Lnc;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!getServer || env->ExceptionCheck()) {
            env->ExceptionClear();
            getServer = env->GetStaticMethodID(serverClass, "getServer", "()Ljava/lang/Object;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (getServer) {
            jobject server = env->CallStaticObjectMethod(serverClass, getServer);
            if (!env->ExceptionCheck() && server) {
                DebugLog("GetMinecraft: found MinecraftServer");
                // From the server, look for a 'mc' or 'minecraft' field pointing to Minecraft
                const char* srvFields[] = {"mc", "field_71349_r", "minecraftServer", "server"};
                const char* srvTypes[] = {"Lnet/minecraft/client/Minecraft;", "Lave;", "Ljava/lang/Object;"};
                for (const char* sf : srvFields) {
                    for (const char* st : srvTypes) {
                        jfieldID sfid = env->GetFieldID(serverClass, sf, st);
                        if (sfid && !env->ExceptionCheck()) {
                            jobject mc = env->GetObjectField(server, sfid);
                            if (!env->ExceptionCheck() && mc) {
                                DebugLog("GetMinecraft: via MinecraftServer.%s", sf);
                                env->DeleteLocalRef(mcClass);
                                env->DeleteLocalRef(serverClass);
                                env->DeleteLocalRef(server);
                                return mc;
                            }
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->DeleteLocalRef(serverClass);
    }

    env->DeleteLocalRef(mcClass);
    DebugLog("GetMinecraft: ALL FAILED");
    return NULL;
}

// ========== Find Session field ID in Minecraft ==========
// Returns the jfieldID for the Session field, or NULL.
// Uses reflection to find the field by type when direct name lookup fails.
jfieldID FindSessionFieldID(JNIEnv* env, jobject mc) {
    jclass mcClass = env->GetObjectClass(mc);
    if (!mcClass) return NULL;

    // DIAGNOSTIC: log ALL field names and type names on Minecraft class
    {
        jclass clsC = env->FindClass("java/lang/Class");
        jmethodID gDF = env->GetMethodID(clsC, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
        jmethodID cGN = env->GetMethodID(clsC, "getName", "()Ljava/lang/String;");
        jclass fC = env->FindClass("java/lang/reflect/Field");
        jmethodID fGN = env->GetMethodID(fC, "getName", "()Ljava/lang/String;");
        jmethodID fGT = env->GetMethodID(fC, "getType", "()Ljava/lang/Class;");
        if (gDF && cGN && fGN && fGT) {
            jobjectArray flds = (jobjectArray)env->CallObjectMethod(mcClass, gDF);
            if (flds) {
                jsize n = env->GetArrayLength(flds);
                DebugLog("FindSessionField: Minecraft class has %d fields", n);
                for (jsize i = 0; i < n && i < 120; i++) {
                    jobject f = env->GetObjectArrayElement(flds, i); if (!f) continue;
                    jstring fn = (jstring)env->CallObjectMethod(f, fGN);
                    jclass ft = (jclass)env->CallObjectMethod(f, fGT);
                    jstring ftn = ft ? (jstring)env->CallObjectMethod(ft, cGN) : NULL;
                    const char* fnStr = fn ? env->GetStringUTFChars(fn, NULL) : "?";
                    const char* ftnStr = ftn ? env->GetStringUTFChars(ftn, NULL) : "?";
                    DebugLog("  field[%d]: %s : %s", (int)i, fnStr ? fnStr : "?", ftnStr ? ftnStr : "?");
                    if (fnStr) env->ReleaseStringUTFChars(fn, fnStr);
                    if (ftnStr) env->ReleaseStringUTFChars(ftn, ftnStr);
                    if (ftn) { env->DeleteLocalRef(ftn); env->DeleteLocalRef(ft); }
                    if (fn) env->DeleteLocalRef(fn);
                    env->DeleteLocalRef(f);
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Try direct name/type combinations (fast path)
    // vanilla 1.8.9: Minecraft=ave, Session=bft, field theSession=ao
    struct { const char* name; const char* type; } attempts[] = {
        {"ao",            "Lbft;"},                          // vanilla 1.8.9
        {"field_71449_p", "Lnet/minecraft/util/Session;"},
        {"field_71449_p", "Lbft;"},
        {"session",       "Lnet/minecraft/util/Session;"},
        {"session",       "Lbft;"},
    };

    for (auto& a : attempts) {
        jfieldID fid = env->GetFieldID(mcClass, a.name, a.type);
        if (fid != NULL && !env->ExceptionCheck()) {
            jobject val = env->GetObjectField(mc, fid);
            if (!env->ExceptionCheck() && val) {
                env->DeleteLocalRef(val);
                env->DeleteLocalRef(mcClass);
                DebugLog("FindSessionField: %s %s", a.name, a.type);
                return fid;
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // NEW PRIMARY: JVMTI direct instance-field value scan.
    // Walks mc's own fields and tests each object-typed value for getUsername().
    // Works for any obfuscation variant (vanilla, OptiFine, Lunar, etc.)
    // because we check the live value rather than the class name.
    DebugLog("FindSessionField: JVMTI direct value scan on mc instance...");
    {
        jvmtiEnv* jvmtiV = NULL;
        if (g_jvm->GetEnv((void**)&jvmtiV, JVMTI_VERSION_1_0) == JNI_OK && jvmtiV) {
            jclass mcClsV = env->GetObjectClass(mc);
            jint fcV = 0;
            jfieldID* fidsV = NULL;
            if (jvmtiV->GetClassFields(mcClsV, &fcV, &fidsV) == JVMTI_ERROR_NONE && fidsV) {
                for (jint fi = 0; fi < fcV; fi++) {
                    jint mods = 0;
                    jvmtiV->GetFieldModifiers(mcClsV, fidsV[fi], &mods);
                    if (mods & 0x0008) continue; // skip static
                    char* fsig = NULL;
                    if (jvmtiV->GetFieldName(mcClsV, fidsV[fi], NULL, &fsig, NULL) != JVMTI_ERROR_NONE) continue;
                    bool isRef = fsig && fsig[0] == 'L';
                    if (fsig) jvmtiV->Deallocate((unsigned char*)fsig);
                    if (!isRef) continue;
                    jobject fval = env->GetObjectField(mc, fidsV[fi]);
                    if (!fval || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                    jclass fvc = env->GetObjectClass(fval);
                    // Check 1: getUsername() — works on LunarClient (not obfuscated there)
                    jmethodID guM = env->GetMethodID(fvc, "getUsername", "()Ljava/lang/String;");
                    bool hasGU = (guM != NULL && !env->ExceptionCheck());
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    // Check 2: 4-String constructor — works on vanilla (constructor names are never obfuscated)
                    // Session(String username, String playerID, String token, String type) is unique to Session
                    bool has4Str = false;
                    if (!hasGU) {
                        jmethodID c4 = env->GetMethodID(fvc, "<init>",
                            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
                        has4Str = (c4 != NULL && !env->ExceptionCheck());
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    env->DeleteLocalRef(fvc);
                    env->DeleteLocalRef(fval);
                    if (hasGU || has4Str) {
                        jfieldID found = fidsV[fi];
                        char* fname = NULL;
                        jvmtiV->GetFieldName(mcClsV, found, &fname, NULL, NULL);
                        DebugLog("FindSessionField: direct value scan found field='%s' (via %s)",
                            fname ? fname : "?", hasGU ? "getUsername" : "4-str-ctor");
                        if (fname) jvmtiV->Deallocate((unsigned char*)fname);
                        jvmtiV->Deallocate((unsigned char*)fidsV);
                        env->DeleteLocalRef(mcClsV);
                        return found;
                    }
                }
                jvmtiV->Deallocate((unsigned char*)fidsV);
            }
            env->DeleteLocalRef(mcClsV);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Reflection fallback: enumerate ALL declared fields, find one with Session type
    // Walk up the superclass chain since getDeclaredFields() doesn't return inherited fields
    DebugLog("FindSessionField: trying reflection enumeration (scanning hierarchy)...");
    {
        jclass clsClass = env->FindClass("java/lang/Class");
        jmethodID getDF = env->GetMethodID(clsClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
        jmethodID clsGetName = env->GetMethodID(clsClass, "getName", "()Ljava/lang/String;");
        jmethodID getSuper = env->GetMethodID(clsClass, "getSuperclass", "()Ljava/lang/Class;");
        if (getDF && clsGetName && getSuper && !env->ExceptionCheck()) {

            // Walk hierarchy: current class and all superclasses
            jclass curClass = (jclass)env->NewGlobalRef(mcClass);
            int hierarchyLevel = 0;
            while (curClass) {
                // Log current class name
                jstring curName = (jstring)env->CallObjectMethod(curClass, clsGetName);
                const char* curNameStr = env->GetStringUTFChars(curName, NULL);
                DebugLog("FindSessionField: hierarchy[%d] class='%s'", hierarchyLevel, curNameStr ? curNameStr : "?");
                env->ReleaseStringUTFChars(curName, curNameStr);
                env->DeleteLocalRef(curName);

                jobjectArray fields = (jobjectArray)env->CallObjectMethod(curClass, getDF);
                if (fields && !env->ExceptionCheck()) {

                    jclass fClass = env->FindClass("java/lang/reflect/Field");
                    jmethodID fGetType = env->GetMethodID(fClass, "getType", "()Ljava/lang/Class;");
                    jmethodID fGetName = env->GetMethodID(fClass, "getName", "()Ljava/lang/String;");
                    jmethodID fSetAcc = env->GetMethodID(fClass, "setAccessible", "(Z)V");
                    if (fGetType && fGetName && fSetAcc && !env->ExceptionCheck()) {

                        jsize count = env->GetArrayLength(fields);
                        for (jsize i = 0; i < count; i++) {
                            jobject field = env->GetObjectArrayElement(fields, i);
                            if (!field) continue;

                            jclass fType = (jclass)env->CallObjectMethod(field, fGetType);
                            if (!fType || env->ExceptionCheck()) { env->ExceptionClear(); continue; }

                            jstring tn = (jstring)env->CallObjectMethod(fType, clsGetName);
                            const char* tnStr = env->GetStringUTFChars(tn, NULL);

                            bool isSession = tnStr && (
                                strcmp(tnStr, "net.minecraft.util.Session") == 0 ||
                                strcmp(tnStr, "bft") == 0
                            );

                            if (isSession) {
                                DebugLog("FindSessionField: found field type '%s' at hierarchy[%d]", tnStr ? tnStr : "?", hierarchyLevel - 1);

                                char sig[512]; sig[0] = 'L'; int sigLen = 1;
                                for (const char* p = tnStr; *p && sigLen < 510; p++) sig[sigLen++] = (*p == '.') ? '/' : *p;
                                sig[sigLen++] = ';'; sig[sigLen] = '\0';

                                env->ReleaseStringUTFChars(tn, tnStr);
                                env->DeleteLocalRef(tn);
                                env->DeleteLocalRef(fType);

                                env->CallVoidMethod(field, fSetAcc, JNI_TRUE);
                                jstring fn = (jstring)env->CallObjectMethod(field, fGetName);
                                const char* fnStr = env->GetStringUTFChars(fn, NULL);

                                DebugLog("FindSessionField: found field '%s' with sig '%s'", fnStr, sig);

                                jfieldID fid = env->GetFieldID(curClass, fnStr, sig);
                                env->ReleaseStringUTFChars(fn, fnStr);
                                env->DeleteLocalRef(fn);

                                if (fid && !env->ExceptionCheck()) {
                                    jobject val = env->GetObjectField(mc, fid);
                                    if (!env->ExceptionCheck() && val) {
                                        env->DeleteLocalRef(val);
                                        env->DeleteLocalRef(fields);
                                        env->DeleteLocalRef(curClass);
                                        env->DeleteLocalRef(mcClass);
                                        DebugLog("FindSessionField: via reflection type match");
                                        return fid;
                                    }
                                    if (env->ExceptionCheck()) env->ExceptionClear();
                                }
                                if (env->ExceptionCheck()) env->ExceptionClear();
                                break; // Found field of right type but couldn't verify — don't scan more
                            }
                            env->ReleaseStringUTFChars(tn, tnStr);
                            env->DeleteLocalRef(tn);
                            env->DeleteLocalRef(fType);
                        }
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                if (env->ExceptionCheck()) env->ExceptionClear();

                // Move to superclass
                jobject parent = env->CallObjectMethod(curClass, getSuper);
                env->DeleteLocalRef(curClass);
                if (env->ExceptionCheck()) { DebugLog("FindSessionField: exception getting super"); env->ExceptionClear(); break; }
                if (!parent) { DebugLog("FindSessionField: no super (null)"); break; }
                // Check if we've hit java.lang.Object
                jstring objName = (jstring)env->CallObjectMethod(parent, clsGetName);
                const char* objNameStr = env->GetStringUTFChars(objName, NULL);
                DebugLog("FindSessionField: superclass='%s'", objNameStr ? objNameStr : "?");
                bool isObject = strcmp(objNameStr, "java.lang.Object") == 0;
                env->ReleaseStringUTFChars(objName, objNameStr);
                env->DeleteLocalRef(objName);
                if (isObject) { DebugLog("FindSessionField: reached Object, stopping"); env->DeleteLocalRef(parent); break; }

                curClass = (jclass)parent;
            }
            if (curClass) env->DeleteLocalRef(curClass);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(mcClass);

    // 3) JVMTI cross-class scan: find session class via getUsername(), then scan
    //    ALL loaded classes' fields for one whose TYPE matches the session class.
    //    Completely different approach: searches EVERY field in the JVM, not just
    //    Minecraft's fields or static fields. Works for Badlion's obfuscated storage.
    g_sessionIsStatic = false;
    if (g_sessionContainer) { env->DeleteGlobalRef(g_sessionContainer); g_sessionContainer = NULL; }
    if (g_foundSessionInstance) { env->DeleteGlobalRef(g_foundSessionInstance); g_foundSessionInstance = NULL; }
    DebugLog("FindSessionField: trying JVMTI cross-class field scan...");
    do {
        jvmtiEnv* jvmti = NULL;
        if (g_jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0) != JNI_OK || !jvmti) break;

        // Get loaded classes
        jvmtiCapabilities caps = {0};
        caps.can_get_synthetic_attribute = 1;
        jvmtiError capErr = jvmti->AddCapabilities(&caps);
        DebugLog("FindSessionField: AddCapabilities = %d", capErr);

        jint classCount = 0;
        jclass* classes = NULL;
        jvmtiError jerr = jvmti->GetLoadedClasses(&classCount, &classes);
        if (jerr != JVMTI_ERROR_NONE || !classes) { DebugLog("FindSessionField: GetLoadedClasses failed"); break; }
        DebugLog("FindSessionField: JVMTI returned %d classes", classCount);

        // Find the session class (has getUsername() — or known class names)
        jclass sessionClass = NULL;
        // First try getUsername() method (works on un-obfuscated/Lunar)
        for (jint i = 0; i < classCount; i++) {
            if (!classes[i]) continue;
            env->GetMethodID(classes[i], "getUsername", "()Ljava/lang/String;");
            if (!env->ExceptionCheck()) { sessionClass = classes[i]; break; }
            env->ExceptionClear();
        }
        // Fallback: try known obfuscated session class names
        if (!sessionClass) {
            const char* knownSessionClasses[] = {"bft", "net/minecraft/util/Session", "net/badlion/a/aGg"};
            for (const char* sc : knownSessionClasses) {
                for (jint i = 0; i < classCount; i++) {
                    if (!classes[i]) continue;
                    // Get class name from JVMTI
                    char* cname = NULL;
                    if (jvmti->GetClassSignature(classes[i], &cname, NULL) == JVMTI_ERROR_NONE && cname) {
                        // JVMTI signature is "Lfully/qualified/Class;" — extract class part
                        // Extract inner name: strip leading 'L' and trailing ';'
                        const char* inner = (cname[0] == 'L') ? cname + 1 : cname;
                        size_t innerLen = strlen(inner);
                        char sigBuf[512]; strncpy(sigBuf, inner, 511); sigBuf[511] = '\0';
                        if (innerLen > 0 && sigBuf[innerLen - 1] == ';') sigBuf[innerLen - 1] = '\0';
                        if (strcmp(sigBuf, sc) == 0) {
                            sessionClass = classes[i];
                            jvmti->Deallocate((unsigned char*)cname);
                            break;
                        }
                        jvmti->Deallocate((unsigned char*)cname);
                    }
                }
                if (sessionClass) break;
            }
        }
        if (!sessionClass) {
            DebugLog("FindSessionField: no session class found via getUsername or known names");
            jvmti->Deallocate((unsigned char*)classes);
            break;
        }

        jclass clsCls = env->FindClass("java/lang/Class");
        jmethodID clsGetName = env->GetMethodID(clsCls, "getName", "()Ljava/lang/String;");
        jstring sn = (jstring)env->CallObjectMethod(sessionClass, clsGetName);
        const char* snStr = sn ? env->GetStringUTFChars(sn, NULL) : "?";
        jstring threadName = NULL; // will use later if needed
        DebugLog("FindSessionField: session class = '%s'", snStr ? snStr : "?");

        char sessionSig[512]; sessionSig[0] = 'L'; int ssLen = 1;
        if (snStr && snStr[0] != '?') {
            for (const char* p = snStr; *p && ssLen < 510; p++) sessionSig[ssLen++] = (*p == '.') ? '/' : *p;
            sessionSig[ssLen++] = ';';
        }
        sessionSig[ssLen] = '\0';
        if (snStr && snStr[0] != '?') env->ReleaseStringUTFChars(sn, snStr);
        env->DeleteLocalRef(sn);
        DebugLog("FindSessionField: session field sig = '%s'", sessionSig);

        // Step B: Scan ALL classes for a field whose sig matches sessionSig
        jfieldID foundField = NULL;
        jclass mcClassTmp = mc ? env->GetObjectClass(mc) : NULL;

        for (jint c = 0; c < classCount && foundField == NULL; c++) {
            if (!classes[c]) continue;
            jint fieldCount = 0;
            jfieldID* fields = NULL;
            if (jvmti->GetClassFields(classes[c], &fieldCount, &fields) != JVMTI_ERROR_NONE || !fields) continue;

            for (jint f = 0; f < fieldCount; f++) {
                char* name = NULL;
                char* sig = NULL;
                if (jvmti->GetFieldName(classes[c], fields[f], &name, &sig, NULL) != JVMTI_ERROR_NONE || !sig) {
                    if (name) jvmti->Deallocate((unsigned char*)name);
                    if (sig) jvmti->Deallocate((unsigned char*)sig);
                    continue;
                }

                bool sigMatch = (strcmp(sessionSig, sig) == 0);
                if (!sigMatch) {
                    if (name) jvmti->Deallocate((unsigned char*)name);
                    if (sig) jvmti->Deallocate((unsigned char*)sig);
                    continue;
                }

                DebugLog("FindSessionField: field match in class[%d] field='%s' sig='%s'", c, name ? name : "?", sig);

                // Check if on Minecraft class
                if (mc && mcClassTmp && env->IsSameObject(classes[c], mcClassTmp)) {
                    jobject val = env->GetObjectField(mc, fields[f]);
                    if (!env->ExceptionCheck() && val) { env->DeleteLocalRef(val); foundField = fields[f]; }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }

                // Check if static
                if (!foundField) {
                    jint fieldAccess = 0;
                    jvmti->GetFieldModifiers(classes[c], fields[f], &fieldAccess);
                    if (fieldAccess & 0x0008) {
                        jobject val = env->GetStaticObjectField(classes[c], fields[f]);
                        if (!env->ExceptionCheck() && val) {
                            env->DeleteLocalRef(val);
                            g_sessionContainer = (jclass)env->NewGlobalRef(classes[c]);
                            g_sessionIsStatic = true;
                            foundField = fields[f];
                        }
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                }

                // Instance field on non-Minecraft class: try getInstance() singleton
                if (!foundField) {
                    jmethodID getInst = env->GetStaticMethodID(classes[c], "getInstance", "()Ljava/lang/Object;");
                    if (!getInst || env->ExceptionCheck()) { env->ExceptionClear(); getInst = NULL; }
                    if (getInst) {
                        jobject container = env->CallStaticObjectMethod(classes[c], getInst);
                        if (!env->ExceptionCheck() && container) {
                            jobject val = env->GetObjectField(container, fields[f]);
                            if (!env->ExceptionCheck() && val) {
                                env->DeleteLocalRef(val);
                                g_sessionContainer = (jclass)env->NewGlobalRef(container);
                                g_sessionIsStatic = false;
                                foundField = fields[f];
                            }
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                }

                if (foundField) {
                    DebugLog("FindSessionField: found via JVMTI scan");
                    jvmti->Deallocate((unsigned char*)name);
                    jvmti->Deallocate((unsigned char*)sig);
                    jvmti->Deallocate((unsigned char*)fields);
                    jvmti->Deallocate((unsigned char*)classes);
                    if (mcClassTmp) env->DeleteLocalRef(mcClassTmp);
                    env->DeleteLocalRef(clsCls);
                    return foundField;
                }

                if (name) jvmti->Deallocate((unsigned char*)name);
                if (sig) jvmti->Deallocate((unsigned char*)sig);
            }
            jvmti->Deallocate((unsigned char*)fields);
        }

        jvmti->Deallocate((unsigned char*)classes);
        if (mcClassTmp) env->DeleteLocalRef(mcClassTmp);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(clsCls);
    } while(0);

    // NEW SECONDARY: JVMTI static field value scan.
    // For Badlion, the session may not live in the Minecraft class at all but as a
    // static field in some other obfuscated class. We scan static object fields of
    // all non-system classes and test each value for getUsername().
    DebugLog("FindSessionField: JVMTI static value scan (Badlion)...");
    {
        jvmtiEnv* jvmtiSV = NULL;
        if (g_jvm->GetEnv((void**)&jvmtiSV, JVMTI_VERSION_1_0) == JNI_OK && jvmtiSV) {
            jint svCount = 0;
            jclass* svCls = NULL;
            if (jvmtiSV->GetLoadedClasses(&svCount, &svCls) == JVMTI_ERROR_NONE && svCls) {
                jfieldID foundSV = NULL;
                for (jint c = 0; c < svCount && !foundSV; c++) {
                    if (!svCls[c]) continue;
                    char* cn = NULL;
                    if (jvmtiSV->GetClassSignature(svCls[c], &cn, NULL) != JVMTI_ERROR_NONE || !cn) continue;
                    bool skip =
                        strstr(cn, "Ljava/") || strstr(cn, "Lsun/") || strstr(cn, "Ljavax/") ||
                        strstr(cn, "Lcom/sun/") || strstr(cn, "Lorg/apache/") ||
                        strstr(cn, "Lorg/lwjgl/") || strstr(cn, "Lio/netty/") ||
                        strstr(cn, "Lcom/google/") || strstr(cn, "Lorg/objectweb/") ||
                        strstr(cn, "Ljdk/") || strstr(cn, "Lorg/slf4j/");
                    jvmtiSV->Deallocate((unsigned char*)cn);
                    if (skip) continue;

                    jint sfcSV = 0;
                    jfieldID* sfidsSV = NULL;
                    if (jvmtiSV->GetClassFields(svCls[c], &sfcSV, &sfidsSV) != JVMTI_ERROR_NONE || !sfidsSV) continue;
                    for (jint f = 0; f < sfcSV; f++) {
                        jint mods = 0;
                        jvmtiSV->GetFieldModifiers(svCls[c], sfidsSV[f], &mods);
                        if (!(mods & 0x0008)) continue; // only static
                        char* fsig = NULL;
                        if (jvmtiSV->GetFieldName(svCls[c], sfidsSV[f], NULL, &fsig, NULL) != JVMTI_ERROR_NONE) continue;
                        bool isRef = fsig && fsig[0] == 'L';
                        if (fsig) jvmtiSV->Deallocate((unsigned char*)fsig);
                        if (!isRef) continue;
                        jobject fval = env->GetStaticObjectField(svCls[c], sfidsSV[f]);
                        if (!fval || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                        jclass fvc = env->GetObjectClass(fval);
                        jmethodID guM = env->GetMethodID(fvc, "getUsername", "()Ljava/lang/String;");
                        bool hasGU = (guM != NULL && !env->ExceptionCheck());
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        bool has4Str = false;
                        if (!hasGU) {
                            jmethodID c4 = env->GetMethodID(fvc, "<init>",
                                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
                            has4Str = (c4 != NULL && !env->ExceptionCheck());
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        env->DeleteLocalRef(fvc);
                        env->DeleteLocalRef(fval);
                        if (hasGU || has4Str) {
                            if (g_sessionContainer) env->DeleteGlobalRef(g_sessionContainer);
                            g_sessionContainer = (jclass)env->NewGlobalRef(svCls[c]);
                            g_sessionIsStatic = true;
                            foundSV = sfidsSV[f];
                            char* fname = NULL;
                            jvmtiSV->GetFieldName(svCls[c], foundSV, &fname, NULL, NULL);
                            DebugLog("FindSessionField: static value scan found field='%s'", fname ? fname : "?");
                            if (fname) jvmtiSV->Deallocate((unsigned char*)fname);
                            break;
                        }
                    }
                    jvmtiSV->Deallocate((unsigned char*)sfidsSV);
                }
                jvmtiSV->Deallocate((unsigned char*)svCls);
                if (foundSV) return foundSV;
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // 4) ThreadLocal scan: session might be stored in a ThreadLocal by Badlion
    DebugLog("FindSessionField: trying ThreadLocal scan...");
    {
        jclass thrClass = env->FindClass("java/lang/Thread");
        jmethodID enumerate = env->GetStaticMethodID(thrClass, "enumerate", "([Ljava/lang/Thread;)I");
        jmethodID thrGetName = env->GetMethodID(thrClass, "getName", "()Ljava/lang/String;");
        if (thrClass && enumerate && thrGetName) {
            jobjectArray threads = env->NewObjectArray(100, thrClass, NULL);
            jint thrCount = env->CallStaticIntMethod(thrClass, enumerate, threads);
            DebugLog("FindSessionField: scanning %d threads for ThreadLocals", thrCount);

            // Get ThreadLocal.ThreadLocalMap's table Entry[] via reflection
            // Thread class has field 'threadLocals' of type ThreadLocal.ThreadLocalMap
            // ThreadLocalMap has field 'table' of type Entry[]
            // Entry has field 'value'
            jclass objClass = env->FindClass("java/lang/Object");
            jmethodID objGetClass = env->GetMethodID(objClass, "getClass", "()Ljava/lang/Class;");

            for (jint t = 0; t < thrCount; t++) {
                jobject thread = env->GetObjectArrayElement(threads, t);
                if (!thread) continue;

                // Access Thread.threadLocals via reflection
                jclass thrClass2 = env->GetObjectClass(thread);
                jfieldID tlsFid = env->GetFieldID(thrClass2, "threadLocals", "Ljava/lang/ThreadLocal$ThreadLocalMap;");
                if (!tlsFid || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(thrClass2); continue; }

                jobject tlm = env->GetObjectField(thread, tlsFid);
                env->DeleteLocalRef(thrClass2);
                if (!tlm || env->ExceptionCheck()) { env->ExceptionClear(); continue; }

                // Access ThreadLocalMap.table
                jclass tlmClass = env->GetObjectClass(tlm);
                jfieldID tableFid = env->GetFieldID(tlmClass, "table", "[Ljava/lang/ThreadLocal$ThreadLocalMap$Entry;");
                if (!tableFid || env->ExceptionCheck()) {
                    env->ExceptionClear();
                    // Try alternative: the table is of type Entry[]
                    tableFid = env->GetFieldID(tlmClass, "table", "[Ljava/lang/Object;");
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                if (!tableFid) { env->DeleteLocalRef(tlmClass); env->DeleteLocalRef(tlm); continue; }

                jobjectArray table = (jobjectArray)env->GetObjectField(tlm, tableFid);
                env->DeleteLocalRef(tlmClass);
                if (!table || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(tlm); continue; }

                jsize tableLen = env->GetArrayLength(table);
                for (jsize e = 0; e < tableLen; e++) {
                    jobject entry = env->GetObjectArrayElement(table, e);
                    if (!entry) continue;

                    // Entry has 'value' field (inherited)
                    jclass entryClass = env->GetObjectClass(entry);
                    jfieldID valFid = env->GetFieldID(entryClass, "value", "Ljava/lang/Object;");
                    if (!valFid || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(entryClass); continue; }

                    jobject val = env->GetObjectField(entry, valFid);
                    env->DeleteLocalRef(entryClass);
                    if (!val || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(entry); continue; }

                    jclass valClass = (jclass)env->CallObjectMethod(val, objGetClass);
                    if (!valClass || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(val); env->DeleteLocalRef(entry); continue; }

                    // Check if this value has getUsername()
                    env->GetMethodID(valClass, "getUsername", "()Ljava/lang/String;");
                    bool hasGetUsername = !env->ExceptionCheck();
                    if (env->ExceptionCheck()) env->ExceptionClear();

                    if (hasGetUsername) {
                        DebugLog("FindSessionField: found session in ThreadLocal!");
                        g_foundSessionInstance = env->NewGlobalRef(val);
                        env->DeleteLocalRef(valClass);
                        env->DeleteLocalRef(val);
                        env->DeleteLocalRef(entry);
                        env->DeleteLocalRef(table);
                        env->DeleteLocalRef(tlm);
                        env->DeleteLocalRef(threads);
                        // Try to also find the field on the owning thread's class to save for replacement
                        DebugLog("FindSessionField: via ThreadLocal scan");
                        return (jfieldID)(intptr_t)-1; // sentinel
                    }
                    env->DeleteLocalRef(valClass);
                    env->DeleteLocalRef(val);
                    env->DeleteLocalRef(entry);
                }
                env->DeleteLocalRef(table);
                env->DeleteLocalRef(tlm);
                env->DeleteLocalRef(thread);
            }

            if (env->ExceptionCheck()) env->ExceptionClear();
        } else {
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    DebugLog("FindSessionField: ALL FAILED");
    return NULL;
}

// ========== Create new offline-mode Session ==========
// Builds a UUID from username, then constructs a Session object.
// Tries multiple constructor signatures for different obfuscation levels.
jobject CreateOfflineSession(JNIEnv* env, jclass sessionClass, const char* username) {
    // UUID: nameUUIDFromBytes("OfflinePlayer:username")
    char seed[256]; snprintf(seed, sizeof(seed), "OfflinePlayer:%s", username);
    jclass uuidClass = env->FindClass("java/util/UUID");
    jmethodID nameUUIDFromBytes = env->GetStaticMethodID(uuidClass, "nameUUIDFromBytes",
        "([B)Ljava/util/UUID;");
    jsize seedLen = (jsize)strlen(seed);
    jbyteArray bytes = env->NewByteArray(seedLen);
    env->SetByteArrayRegion(bytes, 0, seedLen, (const jbyte*)seed);
    jobject uuid = env->CallStaticObjectMethod(uuidClass, nameUUIDFromBytes, bytes);
    jmethodID toString = env->GetMethodID(uuidClass, "toString", "()Ljava/lang/String;");
    jstring uuidJStr = (jstring)env->CallObjectMethod(uuid, toString);

    jstring userJStr = env->NewStringUTF(username);
    jstring zeroJStr = env->NewStringUTF("0");
    jstring legacyJStr = env->NewStringUTF("legacy");

    // 1) Try 4-String constructor: (String, String, String, String)
    jmethodID ctor = env->GetMethodID(sessionClass, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (ctor && !env->ExceptionCheck()) {
        jobject s = env->NewObject(sessionClass, ctor, userJStr, uuidJStr, zeroJStr, legacyJStr);
        if (!env->ExceptionCheck() && s) {
            DebugLog("CreateSession: 4-String ctor OK");
            return s;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // 2) Try 3-String constructor: (String, String, String)
    ctor = env->GetMethodID(sessionClass, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (ctor && !env->ExceptionCheck()) {
        jobject s = env->NewObject(sessionClass, ctor, userJStr, uuidJStr, zeroJStr);
        if (!env->ExceptionCheck() && s) {
            DebugLog("CreateSession: 3-String ctor OK");
            return s;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // 3) Try with Session.Type enum: (String, String, String, Session.Type)
    jclass typeClass = env->FindClass("net/minecraft/util/Session$Type");
    if (!typeClass || env->ExceptionCheck()) {
        env->ExceptionClear();
        typeClass = env->FindClass("bft$a");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (typeClass) {
        jmethodID valueOf = env->GetStaticMethodID(typeClass, "valueOf",
            "(Ljava/lang/String;)Lnet/minecraft/util/Session$Type;");
        if (!valueOf || env->ExceptionCheck()) {
            env->ExceptionClear();
            valueOf = env->GetStaticMethodID(typeClass, "valueOf", "(Ljava/lang/String;)Lbft$a;");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (valueOf) {
            jstring legacyEnum = env->NewStringUTF("LEGACY");
            jobject typeVal = env->CallStaticObjectMethod(typeClass, valueOf, legacyEnum);

            ctor = env->GetMethodID(sessionClass, "<init>",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lnet/minecraft/util/Session$Type;)V");
            if (!ctor || env->ExceptionCheck()) {
                env->ExceptionClear();
                ctor = env->GetMethodID(sessionClass, "<init>",
                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lbft$a;)V");
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            if (ctor) {
                jobject s = env->NewObject(sessionClass, ctor, userJStr, uuidJStr, zeroJStr, typeVal);
                if (!env->ExceptionCheck() && s) {
                    DebugLog("CreateSession: enum ctor OK");
                    return s;
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
    }

    DebugLog("CreateSession: ALL ctors FAILED");
    return NULL;
}

// ========== Main session replacement logic ==========
const char* ReplaceSession(const char* username) {
    if (g_jvm == NULL) {
        g_jvm = FindJavaVM();
        if (g_jvm == NULL) return "no JVM";
    }

    JNIEnv* env = NULL;
    jint attachResult = g_jvm->AttachCurrentThread((void**)&env, NULL);
    if (attachResult != JNI_OK || env == NULL) {
        DebugLog("AttachCurrentThread failed: %d", attachResult);
        return "attach fail";
    }

    // Get Minecraft singleton
    jobject mc = GetMinecraft(env);
    if (mc == NULL) {
        DebugLog("ReplaceSession: Minecraft not found");
        g_jvm->DetachCurrentThread();
        return "no minecraft";
    }

    // Find the Session field in Minecraft (or elsewhere for Badlion)
    jfieldID sessionField = FindSessionFieldID(env, mc);
    if (sessionField == NULL) {
        DebugLog("ReplaceSession: Session field not found");
        env->DeleteLocalRef(mc);
        g_jvm->DetachCurrentThread();
        return "no session field";
    }

    // Handle sentinel: in-place modification via g_foundSessionInstance (ThreadLocal path)
    if (sessionField == (jfieldID)(intptr_t)-1) {
        DebugLog("ReplaceSession: using in-place modification via g_foundSessionInstance");
        if (!g_foundSessionInstance) {
            env->DeleteLocalRef(mc);
            g_jvm->DetachCurrentThread();
            return "no instance";
        }

        jclass sessionClass = env->GetObjectClass(g_foundSessionInstance);
        if (!sessionClass) {
            env->DeleteLocalRef(mc);
            g_jvm->DetachCurrentThread();
            return "no session class";
        }

        // Save original by creating a snapshot session with the same string field values
        if (g_originalSession == NULL) {
            // We'll save a freshly-constructed copy of the current session using its
            // string fields, so we can restore them later via CopyStringFieldsInPlace.
            // Read all String fields from the live session object first.
            jclass clsC = env->FindClass("java/lang/Class");
            jmethodID gDF  = env->GetMethodID(clsC, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
            jclass fC      = env->FindClass("java/lang/reflect/Field");
            jmethodID gType= env->GetMethodID(fC, "getType", "()Ljava/lang/Class;");
            jmethodID setA = env->GetMethodID(fC, "setAccessible", "(Z)V");
            jmethodID fGet = env->GetMethodID(fC, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
            jmethodID cGN  = env->GetMethodID(clsC, "getName", "()Ljava/lang/String;");
            char s0[256]="", s1[256]="", s2[256]="", s3[256]=""; int found = 0;
            if (gDF && gType && setA && fGet && cGN) {
                jobjectArray flds = (jobjectArray)env->CallObjectMethod(sessionClass, gDF);
                if (flds && !env->ExceptionCheck()) {
                    jsize n = env->GetArrayLength(flds);
                    for (jsize i = 0; i < n && found < 4; i++) {
                        jobject f = env->GetObjectArrayElement(flds, i); if (!f) continue;
                        jclass ft = (jclass)env->CallObjectMethod(f, gType);
                        jstring ftn = ft ? (jstring)env->CallObjectMethod(ft, cGN) : NULL;
                        const char* fts = ftn ? env->GetStringUTFChars(ftn, NULL) : NULL;
                        bool isSt = fts && strcmp(fts, "java.lang.String") == 0;
                        if (fts) env->ReleaseStringUTFChars(ftn, fts);
                        if (ftn) env->DeleteLocalRef(ftn);
                        if (ft) env->DeleteLocalRef(ft);
                        if (isSt) {
                            env->CallVoidMethod(f, setA, JNI_TRUE);
                            if (!env->ExceptionCheck()) {
                                jobject v = env->CallObjectMethod(f, fGet, g_foundSessionInstance);
                                if (!env->ExceptionCheck() && v) {
                                    jstring jv = (jstring)v;
                                    const char* cs = env->GetStringUTFChars(jv, NULL);
                                    if (cs) {
                                        if      (found == 0) { strncpy(s0, cs, 255); s0[255]='\0'; }
                                        else if (found == 1) { strncpy(s1, cs, 255); s1[255]='\0'; }
                                        else if (found == 2) { strncpy(s2, cs, 255); s2[255]='\0'; }
                                        else if (found == 3) { strncpy(s3, cs, 255); s3[255]='\0'; }
                                        found++;
                                        env->ReleaseStringUTFChars(jv, cs);
                                    }
                                    env->DeleteLocalRef(v);
                                }
                            }
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        env->DeleteLocalRef(f);
                    }
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            // Build a snapshot session using the collected strings
            jstring js0 = env->NewStringUTF(s0);
            jstring js1 = env->NewStringUTF(s1[0] ? s1 : "0");
            jstring js2 = env->NewStringUTF(s2[0] ? s2 : "0");
            jstring js3 = env->NewStringUTF(s3[0] ? s3 : "legacy");
            jmethodID snapshotCtor = env->GetMethodID(sessionClass, "<init>",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
            if (snapshotCtor && !env->ExceptionCheck()) {
                jobject snap = env->NewObject(sessionClass, snapshotCtor, js0, js1, js2, js3);
                if (!env->ExceptionCheck() && snap) {
                    g_originalSession = env->NewGlobalRef(snap);
                    g_savedField = NULL; // sentinel path
                    env->DeleteLocalRef(snap);
                    DebugLog("ReplaceSession: original sentinel session saved (%d strings)", found);
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(js0); env->DeleteLocalRef(js1);
            env->DeleteLocalRef(js2); env->DeleteLocalRef(js3);
        }

        jobject newSession = CreateOfflineSession(env, sessionClass, username);
        env->DeleteLocalRef(sessionClass);

        if (!newSession) {
            DebugLog("ReplaceSession: CreateOfflineSession failed for in-place");
            env->DeleteLocalRef(mc);
            g_jvm->DetachCurrentThread();
            return "create fail";
        }

        CopyStringFieldsInPlace(env, g_foundSessionInstance, newSession);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            DebugLog("ReplaceSession: CopyStringFieldsInPlace had exception");
            env->DeleteLocalRef(newSession);
            env->DeleteLocalRef(mc);
            g_jvm->DetachCurrentThread();
            return "copy fail";
        }

        DebugLog("ReplaceSession: in-place OK -> %s", username);
        env->DeleteLocalRef(newSession);
        env->DeleteLocalRef(mc);
        g_jvm->DetachCurrentThread();
        return NULL;
    }

    // Normal case: read current session, save it as original if this is the first change
    jobject oldSession = NULL;
    if (g_sessionIsStatic && g_sessionContainer) {
        oldSession = env->GetStaticObjectField(g_sessionContainer, sessionField);
    } else {
        oldSession = env->GetObjectField(mc, sessionField);
    }
    if (oldSession == NULL) {
        DebugLog("ReplaceSession: current session is null");
        env->DeleteLocalRef(mc);
        g_jvm->DetachCurrentThread();
        return "null session";
    }

    // First time: save the original premium session so it can be restored later
    if (g_originalSession == NULL) {
        g_originalSession = env->NewGlobalRef(oldSession);
        g_savedField      = sessionField;
        g_savedIsStatic   = g_sessionIsStatic;
        if (g_sessionIsStatic && g_sessionContainer)
            g_savedContainer = (jclass)env->NewGlobalRef(g_sessionContainer);
        DebugLog("ReplaceSession: original session saved");
    }

    jclass sessionClass = env->GetObjectClass(oldSession);
    env->DeleteLocalRef(oldSession);

    if (sessionClass == NULL) {
        env->DeleteLocalRef(mc);
        g_jvm->DetachCurrentThread();
        return "no session class";
    }

    // Create a new offline Session (cleaner than modifying final fields)
    jobject newSession = CreateOfflineSession(env, sessionClass, username);
    env->DeleteLocalRef(sessionClass);

    if (newSession == NULL) {
        DebugLog("ReplaceSession: CreateOfflineSession failed");
        env->DeleteLocalRef(mc);
        g_jvm->DetachCurrentThread();
        return "create fail";
    }

    // Set the new session on its container
    if (g_sessionIsStatic && g_sessionContainer) {
        env->SetStaticObjectField(g_sessionContainer, sessionField, newSession);
    } else {
        env->SetObjectField(mc, sessionField, newSession);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        DebugLog("ReplaceSession: SetObjectField failed");
        env->DeleteLocalRef(newSession);
        env->DeleteLocalRef(mc);
        g_jvm->DetachCurrentThread();
        return "set fail";
    }

    DebugLog("ReplaceSession: OK -> %s", username);
    env->DeleteLocalRef(newSession);
    env->DeleteLocalRef(mc);
    g_jvm->DetachCurrentThread();
    return NULL; // success
}

// ========== Restore original premium session ==========
const char* RestoreSession() {
    if (g_originalSession == NULL)
        return "no original session (change alt first)";
    if (g_jvm == NULL) return "no JVM";

    JNIEnv* env = NULL;
    if (g_jvm->AttachCurrentThread((void**)&env, NULL) != JNI_OK) return "attach fail";

    // Sentinel / in-place path: copy original string fields back onto the live session object
    if (g_savedField == NULL) {
        if (!g_foundSessionInstance) {
            g_jvm->DetachCurrentThread();
            return "no live session instance";
        }
        CopyStringFieldsInPlace(env, g_foundSessionInstance, g_originalSession);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            g_jvm->DetachCurrentThread();
            return "copy fail";
        }
        DebugLog("RestoreSession: in-place OK");
        g_jvm->DetachCurrentThread();
        return NULL;
    }

    // Normal path: set the saved session object back onto the field
    jobject mc = GetMinecraft(env);
    if (!mc) { g_jvm->DetachCurrentThread(); return "no minecraft"; }

    if (g_savedIsStatic && g_savedContainer) {
        env->SetStaticObjectField(g_savedContainer, g_savedField, g_originalSession);
    } else {
        env->SetObjectField(mc, g_savedField, g_originalSession);
    }

    bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();

    env->DeleteLocalRef(mc);
    g_jvm->DetachCurrentThread();

    if (!ok) return "set fail";
    DebugLog("RestoreSession: OK");
    return NULL;
}
