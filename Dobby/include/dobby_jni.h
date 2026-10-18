#pragma once

// Optional C++17 helpers for native hooks receiving valid JNI references.
// Build Dobby with DOBBY_WITH_JNI=ON. All operations run on the calling thread.
#include <jni.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define DOBBY_JNI_API
#else
#define DOBBY_JNI_API __attribute__((visibility("default")))
#endif

namespace dobby {
namespace jni {

enum class Status {
  Ok = 0,
  InvalidArgument = -1,
  PendingException = -2, // Existing Java exception is left untouched.
  JavaException = -3,   // Exception raised by inspection was cleared.
  Detached = -4,
  Unavailable = -5,
  Reentrant = -6,
  BufferTooSmall = -7
};

enum class Kind : char {
  Void = 'V', Boolean = 'Z', Byte = 'B', Char = 'C', Short = 'S',
  Int = 'I', Long = 'J', Float = 'F', Double = 'D', Object = 'L', Array = '['
};

struct Type {
  Kind kind = Kind::Void;
  std::string descriptor;
};

struct MethodDescriptor {
  std::vector<Type> parameters;
  Type result;
};

DOBBY_JNI_API Status ParseMethodDescriptor(std::string_view descriptor, MethodDescriptor *out);

// Standard Android AAPCS64 JNI ABI: x0=JNIEnv*, x1=receiver/class,
// x2..x7 and the low bits of v0..v7 have independent allocation counters.
// stack is a caller-provided readable span starting at the ORIGINAL entry SP.
// Values contain borrowed JNI references, valid only on the source thread.
// Not an ART quick, @CriticalNative, variadic, or Darwin calling convention.
DOBBY_JNI_API Status DecodeArm64Arguments(const MethodDescriptor &method,
                                        const uint64_t gp[8], const uint64_t fp[8],
                                        const void *stack, size_t stack_size,
                                        std::vector<jvalue> *out);

// Optional attachment for a native pthread. Never detaches an already attached
// thread. Construct, use and destroy on the same thread, before VM destruction.
class DOBBY_JNI_API ScopedEnv {
public:
  explicit ScopedEnv(JavaVM *vm, bool attach_if_detached = false);
  ~ScopedEnv();
  ScopedEnv(const ScopedEnv &) = delete;
  ScopedEnv &operator=(const ScopedEnv &) = delete;
  JNIEnv *get() const { return env_; }
  Status status() const { return status_; }

private:
  JavaVM *vm_ = nullptr;
  JNIEnv *env_ = nullptr;
  bool attached_ = false;
  Status status_ = Status::Unavailable;
};

struct Limits {
  size_t max_depth = 8;
  size_t max_elements = 256;     // Total array elements across one read.
  size_t max_string_chars = 4096; // UTF-16 code units per string.
};

// Owned snapshot: contains no JNI references and can outlive the callback.
// scalar is used only for primitives; scalar.l is null for reference values.
// Arbitrary objects expose their class name. ReadField inspects a named field.
// text is standard UTF-8, including embedded NUL; unpaired surrogates -> U+FFFD.
struct Value {
  Kind kind = Kind::Void;
  jvalue scalar{};
  bool is_null = false;
  bool truncated = false;
  std::string class_name;
  std::string text;
  std::vector<Value> elements;
  // jvalue{} may initialize only the first (jboolean) union member on Clang.
  // Explicitly initialize the pointer-sized member for reference snapshots.
  Value() { scalar.l = nullptr; }
};

class DOBBY_JNI_API Inspector {
public:
  explicit Inspector(JNIEnv *env, Limits limits = {}) : env_(env), limits_(limits) {}
  // out is changed only on success. Does not consume incoming local refs.
  Status ReadObject(jobject object, Value *out) const;
  Status ReadValue(const Type &type, jvalue value, Value *out) const;
  Status ReadField(jobject object, const char *name, std::string_view descriptor, Value *out) const;
  Status ReadArguments(const MethodDescriptor &method, const jvalue *args, size_t count,
                       std::vector<Value> *out) const;

private:
  JNIEnv *env_;
  Limits limits_;
};

} // namespace jni
} // namespace dobby
