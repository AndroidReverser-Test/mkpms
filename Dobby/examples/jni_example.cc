#include "dobby.h"
#include "dobby_jni.h"
#include <cstdio>

// Example target: native int process(String text, byte[] payload, Object options)
// JNI descriptor: (Ljava/lang/String;[BLjava/lang/Object;)I
using Process = jint (JNICALL *)(JNIEnv *, jobject, jstring, jbyteArray, jobject);
static Process original_process;

static jint JNICALL replacement_process(JNIEnv *env, jobject receiver, jstring text,
                                         jbyteArray payload, jobject options) {
  dobby::jni::Inspector inspect(env);
  dobby::jni::Value string_value, array_value, object_value;
  using dobby::jni::Status;
  if (inspect.ReadObject(text, &string_value) == Status::Ok &&
      inspect.ReadObject(payload, &array_value) == Status::Ok &&
      inspect.ReadObject(options, &object_value) == Status::Ok) {
    // string_value.text owns standard UTF-8 (may contain NUL).
    // array_value.elements[i].scalar.b contains each copied jbyte.
    // For known object fields: inspect.ReadField(options, "mode", "I", &field).
    std::fprintf(stderr, "JNI text=%zu UTF-8 bytes, payload=%zu copied bytes, options=%s\n",
                 string_value.text.size(), array_value.elements.size(), object_value.class_name.c_str());
  }
  return original_process(env, receiver, text, payload, options);
}

// Choose the Dobby backend before installation. WxShadow installation/removal
// require the caller to coordinate access to the entire source page.
extern "C" JNIEXPORT int InstallDobbyJniExample(void *target) {
  return DobbyHook(target, reinterpret_cast<void *>(replacement_process),
                   reinterpret_cast<void **>(&original_process));
}

extern "C" JNIEXPORT int RemoveDobbyJniExample(void *target) {
  return DobbyDestroy(target);
}
