#include "dobby_jni.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace dobby {
namespace jni {
namespace {

bool ParseType(std::string_view input, size_t &pos, bool allow_void, Type &out) {
  const size_t start = pos;
  size_t dimensions = 0;
  while (pos < input.size() && input[pos] == '[') {
    ++pos;
    if (++dimensions > 255) return false;
  }
  if (pos == input.size()) return false;
  char code = input[pos++];
  switch (code) {
  case 'V':
    if (!allow_void || dimensions) return false;
    break;
  case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J': case 'F': case 'D':
    break;
  case 'L': {
    size_t component = pos;
    for (; pos < input.size() && input[pos] != ';'; ++pos) {
      char c = input[pos];
      if (c == '.' || c == '[' || c == '(' || c == ')' || c == '\0') return false;
      if (c == '/') {
        if (pos == component) return false;
        component = pos + 1;
      }
    }
    if (pos == component || pos == input.size()) return false;
    ++pos;
    break;
  }
  default: return false;
  }
  out.kind = dimensions ? Kind::Array : static_cast<Kind>(code);
  out.descriptor.assign(input.substr(start, pos - start));
  return true;
}

bool ValidType(const Type &type, bool allow_void = false) {
  size_t pos = 0;
  Type parsed;
  return ParseType(type.descriptor, pos, allow_void, parsed) &&
         pos == type.descriptor.size() && parsed.kind == type.kind;
}

bool ValidMethod(const MethodDescriptor &method) {
  if (!ValidType(method.result, true)) return false;
  size_t slots = 0;
  for (const auto &type : method.parameters) {
    if (!ValidType(type)) return false;
    slots += type.kind == Kind::Long || type.kind == Kind::Double ? 2 : 1;
    if (slots > 255) return false;
  }
  return true;
}

thread_local bool inspecting = false;
struct InspectionGuard {
  bool entered = false;
  explicit InspectionGuard(JNIEnv *env) {
    if (env && !inspecting && !env->ExceptionCheck()) entered = inspecting = true;
  }
  ~InspectionGuard() { if (entered) inspecting = false; }
};

Status CheckEntry(JNIEnv *env) {
  if (!env) return Status::InvalidArgument;
  if (inspecting) return Status::Reentrant;
  return env->ExceptionCheck() ? Status::PendingException : Status::Ok;
}

Status CheckException(JNIEnv *env) {
  if (!env->ExceptionCheck()) return Status::Ok;
  env->ExceptionClear();
  return Status::JavaException;
}

struct LocalFrame {
  JNIEnv *env;
  bool pushed;
  explicit LocalFrame(JNIEnv *env) : env(env), pushed(env->PushLocalFrame(16) == JNI_OK) {}
  ~LocalFrame() { if (pushed) env->PopLocalFrame(nullptr); }
};

void AppendUtf8(uint32_t code, std::string &out) {
  if (code < 0x80) out.push_back(static_cast<char>(code));
  else if (code < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else if (code < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  }
}

Status ReadString(JNIEnv *env, jstring string, size_t limit, std::string &out, bool &truncated) {
  jsize length = env->GetStringLength(string);
  Status status = CheckException(env);
  if (status != Status::Ok) return status;
  size_t count = std::min(static_cast<size_t>(length), limit);
  truncated = count < static_cast<size_t>(length);
  std::vector<jchar> chars(count);
  if (count) env->GetStringRegion(string, 0, static_cast<jsize>(count), chars.data());
  status = CheckException(env);
  if (status != Status::Ok) return status;
  for (size_t i = 0; i < count; ++i) {
    uint32_t code = chars[i];
    if (code >= 0xd800 && code <= 0xdbff) {
      if (i + 1 < count && chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (chars[++i] - 0xdc00);
      } else code = 0xfffd;
    } else if (code >= 0xdc00 && code <= 0xdfff) code = 0xfffd;
    AppendUtf8(code, out);
  }
  return Status::Ok;
}

Status ClassName(JNIEnv *env, jobject object, std::string &out) {
  jclass cls = env->GetObjectClass(object);
  Status status = CheckException(env);
  if (status != Status::Ok || !cls) return status == Status::Ok ? Status::Unavailable : status;
  jclass class_cls = env->GetObjectClass(cls);
  status = CheckException(env);
  if (status != Status::Ok || !class_cls) return status == Status::Ok ? Status::Unavailable : status;
  jmethodID get_name = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
  status = CheckException(env);
  if (status != Status::Ok || !get_name) return status == Status::Ok ? Status::Unavailable : status;
  auto name = static_cast<jstring>(env->CallObjectMethodA(cls, get_name, nullptr));
  status = CheckException(env);
  if (status != Status::Ok || !name) return status == Status::Ok ? Status::Unavailable : status;
  bool truncated = false;
  // Class names determine dispatch, so they must not be truncated.
  return ReadString(env, name, 65535, out, truncated);
}

struct Reader {
  JNIEnv *env;
  const Limits &limits;
  size_t remaining;

  Status Read(Kind kind, jvalue input, Value &out, size_t depth = 0) {
    out.kind = kind;
    if (kind != Kind::Object && kind != Kind::Array) {
      out.scalar = input;
      return Status::Ok;
    }
    if (!input.l) {
      out.is_null = true;
      return Status::Ok;
    }
    LocalFrame frame(env);
    Status status = CheckException(env);
    if (status != Status::Ok || !frame.pushed) return status == Status::Ok ? Status::Unavailable : status;
    status = ClassName(env, input.l, out.class_name);
    if (status != Status::Ok) return status;
    if (out.class_name == "java.lang.String") {
      return ReadString(env, static_cast<jstring>(input.l), limits.max_string_chars, out.text, out.truncated);
    }
    if (out.class_name.empty() || out.class_name[0] != '[') return Status::Ok;
    out.kind = Kind::Array;
    auto array = static_cast<jarray>(input.l);
    jsize length = env->GetArrayLength(array);
    status = CheckException(env);
    if (status != Status::Ok) return status;
    if (depth >= limits.max_depth) {
      out.truncated = length != 0;
      return Status::Ok;
    }
    size_t count = std::min(static_cast<size_t>(length), remaining);
    out.truncated = count < static_cast<size_t>(length);
    remaining -= count;
    char element = out.class_name[1];
    out.elements.reserve(count);
    if (element == 'L' || element == '[') {
      for (size_t i = 0; i < count; ++i) {
        jvalue item{};
        item.l = env->GetObjectArrayElement(static_cast<jobjectArray>(array), static_cast<jsize>(i));
        status = CheckException(env);
        if (status != Status::Ok) return status;
        Value value;
        status = Read(element == '[' ? Kind::Array : Kind::Object, item, value, depth + 1);
        if (item.l) env->DeleteLocalRef(item.l);
        if (status != Status::Ok) return status;
        out.truncated = out.truncated || value.truncated;
        out.elements.push_back(std::move(value));
      }
      return Status::Ok;
    }
    // Region APIs copy values without pinning the Java heap.
#define READ_REGION(code, suffix, type, member) \
    case code: { \
      std::vector<type> values(count); \
      if (count) env->Get##suffix##ArrayRegion(static_cast<type##Array>(array), 0, \
                                              static_cast<jsize>(count), values.data()); \
      status = CheckException(env); \
      if (status != Status::Ok) return status; \
      for (type item : values) { \
        Value value; value.kind = static_cast<Kind>(code); value.scalar.member = item; \
        out.elements.push_back(std::move(value)); \
      } \
      return Status::Ok; \
    }
    switch (element) {
      READ_REGION('Z', Boolean, jboolean, z)
      READ_REGION('B', Byte, jbyte, b)
      READ_REGION('C', Char, jchar, c)
      READ_REGION('S', Short, jshort, s)
      READ_REGION('I', Int, jint, i)
      READ_REGION('J', Long, jlong, j)
      READ_REGION('F', Float, jfloat, f)
      READ_REGION('D', Double, jdouble, d)
    default: return Status::InvalidArgument;
    }
#undef READ_REGION
  }
};

// Only primitive snapshots copy a scalar. Never leave a borrowed jobject in it.
jvalue ScalarFromBits(Kind kind, uint64_t bits) {
  jvalue value{};
  switch (kind) {
  case Kind::Boolean: value.z = static_cast<jboolean>(bits & 0xff); break;
  case Kind::Byte: value.b = static_cast<jbyte>(bits); break;
  case Kind::Char: value.c = static_cast<jchar>(bits); break;
  case Kind::Short: value.s = static_cast<jshort>(bits); break;
  case Kind::Int: value.i = static_cast<jint>(bits); break;
  case Kind::Long: std::memcpy(&value.j, &bits, sizeof(value.j)); break;
  case Kind::Float: {
    uint32_t low = static_cast<uint32_t>(bits);
    std::memcpy(&value.f, &low, sizeof(value.f));
    break;
  }
  case Kind::Double: std::memcpy(&value.d, &bits, sizeof(value.d)); break;
  case Kind::Object: case Kind::Array: value.l = reinterpret_cast<jobject>(static_cast<uintptr_t>(bits)); break;
  default: break;
  }
  return value;
}

} // namespace

Status ParseMethodDescriptor(std::string_view descriptor, MethodDescriptor *out) {
  if (!out || descriptor.empty() || descriptor[0] != '(') return Status::InvalidArgument;
  MethodDescriptor parsed;
  size_t pos = 1, slots = 0;
  while (pos < descriptor.size() && descriptor[pos] != ')') {
    Type type;
    if (!ParseType(descriptor, pos, false, type)) return Status::InvalidArgument;
    slots += type.kind == Kind::Long || type.kind == Kind::Double ? 2 : 1;
    if (slots > 255) return Status::InvalidArgument;
    parsed.parameters.push_back(std::move(type));
  }
  if (pos == descriptor.size() || descriptor[pos++] != ')' ||
      !ParseType(descriptor, pos, true, parsed.result) || pos != descriptor.size()) return Status::InvalidArgument;
  *out = std::move(parsed);
  return Status::Ok;
}

Status DecodeArm64Arguments(const MethodDescriptor &method, const uint64_t gp[8], const uint64_t fp[8],
                            const void *stack, size_t stack_size, std::vector<jvalue> *out) {
  if (!out || !gp || !fp || (!stack && stack_size) || !ValidMethod(method)) return Status::InvalidArgument;
  size_t g = 2, f = 0, offset = 0;
  std::vector<jvalue> values;
  for (const auto &type : method.parameters) {
    bool floating = type.kind == Kind::Float || type.kind == Kind::Double;
    size_t &index = floating ? f : g;
    uint64_t bits;
    if (index < 8) bits = floating ? fp[index] : gp[index];
    else {
      if (offset > stack_size || stack_size - offset < sizeof(bits)) return Status::BufferTooSmall;
      std::memcpy(&bits, static_cast<const uint8_t *>(stack) + offset, sizeof(bits));
      offset += sizeof(bits);
    }
    ++index;
    values.push_back(ScalarFromBits(type.kind, bits));
  }
  *out = std::move(values);
  return Status::Ok;
}

ScopedEnv::ScopedEnv(JavaVM *vm, bool attach_if_detached) : vm_(vm) {
  if (!vm_) return;
  void *raw = nullptr;
  jint rc = vm_->GetEnv(&raw, JNI_VERSION_1_6);
  if (rc == JNI_EDETACHED && attach_if_detached) {
    // The C++ wrapper takes JNIEnv** on Android and void** on OpenJDK.
#if defined(__ANDROID__)
    JNIEnv *attached_env = nullptr;
    rc = vm_->AttachCurrentThread(&attached_env, nullptr);
    raw = attached_env;
#else
    rc = vm_->AttachCurrentThread(&raw, nullptr);
#endif
    attached_ = rc == JNI_OK;
  }
  if (rc == JNI_OK && raw) {
    env_ = static_cast<JNIEnv *>(raw);
    status_ = Status::Ok;
  } else status_ = rc == JNI_EDETACHED ? Status::Detached : Status::Unavailable;
}

ScopedEnv::~ScopedEnv() {
  if (attached_) vm_->DetachCurrentThread();
}

Status Inspector::ReadObject(jobject object, Value *out) const {
  Type type{Kind::Object, "Ljava/lang/Object;"};
  jvalue value{};
  value.l = object;
  return ReadValue(type, value, out);
}

Status Inspector::ReadValue(const Type &type, jvalue value, Value *out) const {
  if (!out || !ValidType(type)) return Status::InvalidArgument;
  Status status = CheckEntry(env_);
  if (status != Status::Ok) return status;
  InspectionGuard guard(env_);
  Reader reader{env_, limits_, limits_.max_elements};
  Value result;
  status = reader.Read(type.kind, value, result);
  if (status == Status::Ok) *out = std::move(result);
  return status;
}

Status Inspector::ReadField(jobject object, const char *name, std::string_view descriptor, Value *out) const {
  size_t pos = 0;
  Type type;
  if (!out || !object || !name || !*name || !ParseType(descriptor, pos, false, type) || pos != descriptor.size())
    return Status::InvalidArgument;
  Status status = CheckEntry(env_);
  if (status != Status::Ok) return status;
  InspectionGuard guard(env_);
  LocalFrame frame(env_);
  status = CheckException(env_);
  if (status != Status::Ok || !frame.pushed) return status == Status::Ok ? Status::Unavailable : status;
  jclass cls = env_->GetObjectClass(object);
  status = CheckException(env_);
  if (status != Status::Ok || !cls) return status == Status::Ok ? Status::Unavailable : status;
  jfieldID field = env_->GetFieldID(cls, name, type.descriptor.c_str());
  status = CheckException(env_);
  if (status != Status::Ok || !field) return status == Status::Ok ? Status::Unavailable : status;
  jvalue value{};
  switch (type.kind) {
  case Kind::Boolean: value.z = env_->GetBooleanField(object, field); break;
  case Kind::Byte: value.b = env_->GetByteField(object, field); break;
  case Kind::Char: value.c = env_->GetCharField(object, field); break;
  case Kind::Short: value.s = env_->GetShortField(object, field); break;
  case Kind::Int: value.i = env_->GetIntField(object, field); break;
  case Kind::Long: value.j = env_->GetLongField(object, field); break;
  case Kind::Float: value.f = env_->GetFloatField(object, field); break;
  case Kind::Double: value.d = env_->GetDoubleField(object, field); break;
  case Kind::Object: case Kind::Array: value.l = env_->GetObjectField(object, field); break;
  default: return Status::InvalidArgument;
  }
  status = CheckException(env_);
  if (status != Status::Ok) return status;
  Reader reader{env_, limits_, limits_.max_elements};
  Value result;
  status = reader.Read(type.kind, value, result);
  if (status == Status::Ok) *out = std::move(result);
  return status;
}

Status Inspector::ReadArguments(const MethodDescriptor &method, const jvalue *args, size_t count,
                                std::vector<Value> *out) const {
  if (!out || count != method.parameters.size() || (count && !args) || !ValidMethod(method))
    return Status::InvalidArgument;
  Status status = CheckEntry(env_);
  if (status != Status::Ok) return status;
  InspectionGuard guard(env_);
  Reader reader{env_, limits_, limits_.max_elements};
  std::vector<Value> result;
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    Value value;
    status = reader.Read(method.parameters[i].kind, args[i], value);
    if (status != Status::Ok) return status;
    result.push_back(std::move(value));
  }
  *out = std::move(result);
  return Status::Ok;
}

} // namespace jni
} // namespace dobby
