#include "dobby.h"
#include "dobby_jni.h"

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#if defined(__ANDROID__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace dobby::jni;

// This executable links the same host-test Dobby as the wxshadow suite. JNI
// integration uses the native backend and must never submit a shadow patch.
#if !defined(__ANDROID__)
__attribute__((visibility("default"))) int dobby_wx_test_transport(int, void *) { return -ENOTSUP; }
__attribute__((visibility("default"))) bool dobby_wx_test_failure(int) { return false; }
#endif

static void DescriptorAndAbiTests() {
  MethodDescriptor method;
  assert(ParseMethodDescriptor("(ZBCSIJFDLjava/lang/String;[[I)V", &method) == Status::Ok);
  assert(method.parameters.size() == 10 && method.result.kind == Kind::Void);
  for (const char *bad : {"", "I)V", "(V)V", "([V)V", "(L;)V", "(I)", "()VV",
                           "(Ljava.lang.String;)V", "(Ljava//lang/String;)V", "()", "(I)Vx"}) {
    assert(ParseMethodDescriptor(bad, &method) == Status::InvalidArgument);
    assert(method.parameters.size() == 10); // Failure leaves the output intact.
  }
  std::string excessive = "(" + std::string(256, 'I') + ")V";
  assert(ParseMethodDescriptor(excessive, &method) == Status::InvalidArgument);
  excessive = "(" + std::string(256, '[') + "I)V";
  assert(ParseMethodDescriptor(excessive, &method) == Status::InvalidArgument);

  // Seven integers and nine FP args overflow BOTH register banks. Stack slots
  // follow source argument order, with an eight-byte slot even for float/int.
  assert(ParseMethodDescriptor("(IIIIIIIFDFDFDFDD[B)J", &method) == Status::Ok);
  uint64_t gp[8] = {0x1000, 0x2000, 11, 12, 13, 14, 15, 16};
  uint64_t fp[8]{};
  for (int i = 0; i < 8; ++i) {
    if (i % 2 == 0) { float x = i + 0.25f; std::memcpy(&fp[i], &x, sizeof(x)); }
    else { double x = i + 0.5; std::memcpy(&fp[i], &x, sizeof(x)); }
  }
  uint64_t stack[3] = {static_cast<uint32_t>(-17), 0, 0x12345678};
  double ninth = 99.5;
  std::memcpy(&stack[1], &ninth, sizeof(ninth));
  std::vector<jvalue> values;
  assert(DecodeArm64Arguments(method, gp, fp, stack, sizeof(stack), &values) == Status::Ok);
  assert(values.size() == 17 && values[0].i == 11 && values[5].i == 16 && values[6].i == -17);
  assert(values[7].f == 0.25f && values[8].d == 1.5 && values[15].d == ninth);
  assert(values[16].l == reinterpret_cast<jobject>(0x12345678));
  assert(DecodeArm64Arguments(method, gp, fp, stack, 16, &values) == Status::BufferTooSmall);
  assert(values.size() == 17);
  assert(DecodeArm64Arguments(method, gp, fp, nullptr, 0, &values) == Status::BufferTooSmall);

  assert(ParseMethodDescriptor("(ZBCSJFD)V", &method) == Status::Ok);
  gp[2] = 1; gp[3] = 0x80; gp[4] = 0xffff; gp[5] = 0x8000; gp[6] = UINT64_C(0x8000000000000001);
  float negative = -1.25f;
  double precise = 1.0 / 3.0;
  std::memcpy(fp, &negative, sizeof(negative));
  std::memcpy(fp + 1, &precise, sizeof(precise));
  assert(DecodeArm64Arguments(method, gp, fp, nullptr, 0, &values) == Status::Ok);
  assert(values[0].z == 1 && values[1].b == -128 && values[2].c == 65535 && values[3].s == -32768);
  assert(values[4].j == INT64_MIN + 1 && values[5].f == negative && values[6].d == precise);
}

static void InspectionTests(JNIEnv *env, JavaVM *vm) {
  jclass cls = env->FindClass("DobbyJniFixture");
  assert(cls && !env->ExceptionCheck());
  jmethodID constructor = env->GetMethodID(cls, "<init>", "()V");
  assert(constructor && !env->ExceptionCheck());
  jobject instance = env->NewObjectA(cls, constructor, nullptr);
  assert(instance && !env->ExceptionCheck());
  Inspector inspect(env);
  Value value;
  assert(inspect.ReadObject(instance, &value) == Status::Ok);
  assert(value.class_name == "DobbyJniFixture" && value.scalar.l == nullptr);
  assert(inspect.ReadField(instance, "text", "Ljava/lang/String;", &value) == Status::Ok);
  const char expected[] = u8"中文\0😀";
  assert(value.text == std::string(expected, sizeof(expected) - 1));
  assert(!value.truncated && value.scalar.l == nullptr);
  assert(inspect.ReadField(instance, "number", "I", &value) == Status::Ok && value.scalar.i == -123);
  assert(inspect.ReadField(instance, "wide", "J", &value) == Status::Ok && value.scalar.j == INT64_MIN + 17);
  assert(inspect.ReadField(instance, "anything", "Ljava/lang/Object;", &value) == Status::Ok);
  assert(value.kind == Kind::Array && value.class_name == "[I" && value.elements[2].scalar.i == INT32_MIN);
  assert(inspect.ReadField(instance, "bytes", "[B", &value) == Status::Ok && value.elements[0].scalar.b == -128);
  assert(inspect.ReadField(instance, "booleans", "[Z", &value) == Status::Ok && value.elements[0].scalar.z == JNI_TRUE);
  assert(inspect.ReadField(instance, "chars", "[C", &value) == Status::Ok && value.elements[1].scalar.c == 65535);
  assert(inspect.ReadField(instance, "shorts", "[S", &value) == Status::Ok && value.elements[0].scalar.s == -32768);
  assert(inspect.ReadField(instance, "longs", "[J", &value) == Status::Ok && value.elements[1].scalar.j == INT64_MAX);
  assert(inspect.ReadField(instance, "floats", "[F", &value) == Status::Ok && std::isnan(value.elements[1].scalar.f));
  assert(inspect.ReadField(instance, "doubles", "[D", &value) == Status::Ok && std::isinf(value.elements[1].scalar.d));
  assert(inspect.ReadField(instance, "nested", "[Ljava/lang/Object;", &value) == Status::Ok);
  assert(value.elements.size() == 3 && value.elements[0].text == std::string(expected, sizeof(expected) - 1));
  assert(value.elements[1].is_null && value.elements[2].elements[1].scalar.i == 2);
  assert(inspect.ReadField(instance, "malformed", "Ljava/lang/String;", &value) == Status::Ok);
  assert(value.text == u8"�x�");
  assert(inspect.ReadObject(nullptr, &value) == Status::Ok && value.is_null);

  Inspector limited(env, Limits{2, 4, 2});
  assert(limited.ReadField(instance, "text", "Ljava/lang/String;", &value) == Status::Ok);
  assert(value.text == u8"中文" && value.truncated);
  assert(limited.ReadField(instance, "cycle", "[Ljava/lang/Object;", &value) == Status::Ok);
  assert(value.truncated && value.elements[0].elements[0].elements.empty());
  assert(limited.ReadField(instance, "nested", "[Ljava/lang/Object;", &value) == Status::Ok);
  assert(value.truncated && value.elements[2].elements.size() == 1);
  Inspector zero(env, Limits{0, 0, 0});
  assert(zero.ReadField(instance, "bytes", "[B", &value) == Status::Ok && value.truncated && value.elements.empty());

  value.text = "unchanged";
  assert(inspect.ReadField(instance, "missing", "I", &value) == Status::JavaException);
  assert(!env->ExceptionCheck() && value.text == "unchanged");
  assert(inspect.ReadField(instance, "number", "J", &value) == Status::JavaException);
  assert(!env->ExceptionCheck());
  jclass error = env->FindClass("java/lang/IllegalStateException");
  assert(error && !env->ExceptionCheck());
  assert(env->ThrowNew(error, "original pending exception") == JNI_OK);
  jthrowable pending = env->ExceptionOccurred();
  assert(inspect.ReadObject(instance, &value) == Status::PendingException);
  jthrowable after = env->ExceptionOccurred();
  env->ExceptionClear();
  assert(env->IsSameObject(pending, after));
  env->DeleteLocalRef(pending);
  env->DeleteLocalRef(after);

  // Returned snapshots own all data. Repeated reads must not exhaust the
  // native thread's local-reference table, including on failed field lookups.
  for (int i = 0; i < 1000; ++i) {
    assert(inspect.ReadField(instance, "nested", "[Ljava/lang/Object;", &value) == Status::Ok);
    assert(inspect.ReadField(instance, "missing", "I", &value) == Status::JavaException);
  }
  jobject global = env->NewGlobalRef(instance);
  assert(global && !env->ExceptionCheck());
  std::thread worker([&] {
    ScopedEnv detached(vm);
    assert(detached.status() == Status::Detached && !detached.get());
    {
      ScopedEnv attached(vm, true);
      assert(attached.status() == Status::Ok && attached.get());
      {
        ScopedEnv nested(vm, true);
        assert(nested.get() == attached.get());
      }
      Inspector thread_inspect(attached.get());
      Value snapshot;
      assert(thread_inspect.ReadField(global, "text", "Ljava/lang/String;", &snapshot) == Status::Ok);
      assert(snapshot.text == std::string(expected, sizeof(expected) - 1));
    }
    ScopedEnv detached_again(vm);
    assert(detached_again.status() == Status::Detached);
  });
  worker.join();
  env->DeleteGlobalRef(global);
  env->DeleteLocalRef(error);
  env->DeleteLocalRef(instance);
  env->DeleteLocalRef(cls);
  assert(!env->ExceptionCheck());
}

using Probe = jlong (JNICALL *)(JNIEnv *, jclass, jstring, jbyteArray, jobject, jint, jfloat, jdouble, jlong);
static Probe original_probe;
static int hook_hits;

__attribute__((noinline, aligned(64)))
static jlong JNICALL ProbeTarget(JNIEnv *, jclass, jstring, jbyteArray, jobject,
                                 jint number, jfloat fraction, jdouble precise, jlong wide) {
  // Give the host x86 relocator a predictable entry, as in the existing native
  // backend tests. This fixture tests JNI marshaling rather than SIMD decode.
  __asm__ __volatile__(".rept 32\n nop\n .endr" ::: "memory");
  volatile jlong result = wide + number;
  result += static_cast<jlong>(fraction + precise);
  return result;
}

static jlong JNICALL ProbeReplacement(JNIEnv *env, jclass cls, jstring text, jbyteArray bytes, jobject object,
                                      jint number, jfloat fraction, jdouble precise, jlong wide) {
  ++hook_hits;
  Inspector inspect(env);
  MethodDescriptor signature;
  assert(ParseMethodDescriptor("(Ljava/lang/String;[BLjava/lang/Object;IFDJ)J", &signature) == Status::Ok);
  jvalue args[7]{};
  args[0].l = text; args[1].l = bytes; args[2].l = object;
  args[3].i = number; args[4].f = fraction; args[5].d = precise; args[6].j = wide;
  std::vector<Value> values;
  assert(inspect.ReadArguments(signature, args, 7, &values) == Status::Ok);
  assert(values[0].class_name == "java.lang.String" && values[1].elements[0].scalar.b == -128);
  assert(values[2].class_name == "DobbyJniFixture" && values[3].scalar.i == -123);
  assert(values[4].scalar.f == 1.25f && values[5].scalar.d == 2.5 && values[6].scalar.j == INT64_C(9007199254740993));
  return original_probe(env, cls, text, bytes, object, number, fraction, precise, wide) + 100;
}

static void HookTests(JNIEnv *env, bool shadow = false) {
  void *target = reinterpret_cast<void *>(ProbeTarget);
#if defined(__ANDROID__)
  // Isolate the entire source page from ART, the reader and replacement code.
  // The thunk uses only instructions, with no literal data reads from itself.
  assert(sysconf(_SC_PAGESIZE) == 4096);
  auto *source = static_cast<uint32_t *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(source != MAP_FAILED);
  for (size_t i = 0; i < 1024; ++i) source[i] = 0xd503201f; // nop
  uintptr_t body = reinterpret_cast<uintptr_t>(ProbeTarget);
  source[1] = 0xd2800010 | ((body & 0xffff) << 5); // movz x16
  for (int i = 1; i < 4; ++i)
    source[i + 1] = 0xf2800010 | (i << 21) | (((body >> (i * 16)) & 0xffff) << 5); // movk x16
  source[5] = 0xd61f0200; // br x16, preserving all JNI argument registers
  __builtin___clear_cache(reinterpret_cast<char *>(source), reinterpret_cast<char *>(source) + 4096);
  assert(mprotect(source, 4096, PROT_READ | PROT_EXEC) == 0);
  std::vector<uint8_t> saved(4096);
  std::memcpy(saved.data(), source, saved.size());
  target = source;
#else
  assert(!shadow);
#endif
  jclass cls = env->FindClass("DobbyJniFixture");
  assert(cls && !env->ExceptionCheck());
  JNINativeMethod native{const_cast<char *>("probe"),
                         const_cast<char *>("(Ljava/lang/String;[BLjava/lang/Object;IFDJ)J"),
                         target};
  assert(env->RegisterNatives(cls, &native, 1) == JNI_OK && !env->ExceptionCheck());
  jmethodID invoke = env->GetStaticMethodID(cls, "invokeProbe", "()J");
  assert(invoke && !env->ExceptionCheck());
  jlong before = env->CallStaticLongMethodA(cls, invoke, nullptr);
  assert(!env->ExceptionCheck());
  assert(dobby_set_hook_backend(shadow ? DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED : DOBBY_HOOK_BACKEND_NATIVE) == 0);
  assert(DobbyHook(target, reinterpret_cast<void *>(ProbeReplacement),
                   reinterpret_cast<void **>(&original_probe)) == 0);
  jlong during = env->CallStaticLongMethodA(cls, invoke, nullptr);
  assert(!env->ExceptionCheck() && during == before + 100 && hook_hits == 1);
  if (shadow) {
    DobbyHookStatus status{};
    assert(dobby_get_hook_status(target, &status) == 0 && status.state == DOBBY_HOOK_ACTIVE);
#if defined(__ANDROID__)
    assert(std::memcmp(source, saved.data(), saved.size()) == 0);
#endif
  }
  assert(DobbyDestroy(target) == 0);
  jlong after = env->CallStaticLongMethodA(cls, invoke, nullptr);
  assert(!env->ExceptionCheck() && after == before && hook_hits == 1);
  env->DeleteLocalRef(cls);
#if defined(__ANDROID__)
  assert(std::memcmp(source, saved.data(), saved.size()) == 0);
  assert(munmap(source, 4096) == 0);
#endif
}

#if !defined(__ANDROID__)
int main(int argc, char **argv) {
  assert(argc == 2);
  DescriptorAndAbiTests();
  JavaVMOption options[2]{};
  std::string classpath = std::string("-Djava.class.path=") + argv[1];
  options[0].optionString = const_cast<char *>(classpath.c_str());
  options[1].optionString = const_cast<char *>("-Xcheck:jni");
  JavaVMInitArgs args{};
  args.version = JNI_VERSION_1_6;
  args.nOptions = 2;
  args.options = options;
  JavaVM *vm = nullptr;
  void *raw_env = nullptr;
  assert(JNI_CreateJavaVM(&vm, &raw_env, &args) == JNI_OK);
  auto env = static_cast<JNIEnv *>(raw_env);
  {
    ScopedEnv existing(vm, true);
    assert(existing.get() == env && existing.status() == Status::Ok);
  }
  InspectionTests(env, vm);
  HookTests(env);
  assert(vm->DestroyJavaVM() == JNI_OK);
  std::puts("PASS JNI snapshots, UTF-8, arrays, fields, exceptions, thread ownership, ARM64 decoding and Java->DobbyHook->original");
  return 0;
}
#else
extern "C" JNIEXPORT void JNICALL Java_DobbyJniFixture_runChecks(JNIEnv *env, jclass, jboolean shadow) {
  JavaVM *vm = nullptr;
  assert(env->GetJavaVM(&vm) == JNI_OK);
  DescriptorAndAbiTests();
  InspectionTests(env, vm);
  HookTests(env, shadow != JNI_FALSE);
  std::puts(shadow ? "PASS ART JNI + WxShadow: snapshots, ABI, original, hidden page, destroy" :
                     "PASS ART JNI + native: snapshots, ABI, original, destroy");
}
#endif
