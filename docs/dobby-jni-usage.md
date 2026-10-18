# Dobby native hook 的 Java 参数解析

日期：2026-10-18。已通过主机 OpenJDK 和 Android 12 / ARM64 实机测试。

## 1. 框架关系与接入位置

本项目的三个层次：

| 层次 | 现有实现 | 职责 |
|---|---|---|
| 内核 | `kpms/wxshadow`、`shared/wxshadow_uapi.h` | 维护原始页/执行影子页，处理读/执行 fault，通过 PATCH_EX、cookie 和 QUERY_PATCH 安装、查询、撤销入口 |
| Native hook | `Dobby/source/InterceptRouting`、`Backend/UserMode/WxShadow` | DobbyHook 创建入口跳板、original 和 relay；按已选后端发布入口 |
| Java 参数 | 新增 `Dobby/source/JniBridge/dobby_jni.cc` | 在当前调用线程通过标准 JNI 解析有效 Java 引用，返回独立的 C++ 数据快照 |

参考 `rustFrida/quickjs-hook/src/jsapi/java/mod.rs` 的字符串和运行时类名读取、`callback/marshal.rs` 的数组转换和 ARM64 GP/FP 参数分配、`callback/signature.rs` 的 JNI 描述符解析，以及 `jni_core.rs` 的线程附件管理。

Dobby 提供可选的 C++17 API，无需运行 Rust agent 或 QuickJS。Java 对象由 JNI 访问；不依赖 ART 内部对象布局、ArtMethod 偏移或内核解引用 Java 堆。原有 `DobbyHook` / `DobbyDestroy` 和 WxShadow 协议继续使用。

## 2. 构建

在仓库根目录执行，替换本机 NDK 路径：

```sh
cmake -S Dobby -B build/dobby-jni-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release -DDOBBY_WITH_JNI=ON \
  -DDOBBY_WITH_WXSHADOW=ON -DDOBBY_WXSHADOW_UAPI_DIR="$PWD/shared"
cmake --build build/dobby-jni-android -j6
```

`DOBBY_WITH_JNI` 默认 OFF，开启后同时编入 `libdobby.a` 和 `libdobby.so`。只用普通后端可以关闭 `DOBBY_WITH_WXSHADOW`。JNI 解析本身不要求 root 或 KPM；WxShadow 的构建、部署和页访问协调条件见 [后端使用文档](dobby-wxshadow-usage.md)。

使用 `#include <dobby_jni.h>`，应用与 Dobby 使用一致的 NDK、STL 和 C++ ABI。CMake 通过两个库目标传递 C++17 要求。Android 的 `jni.h` 来自 NDK；主机测试通过 `find_package(JNI)` 获取 JDK 头文件，Dobby 库本身不链接 JVM。

可编译的完整示例在 `Dobby/examples/jni_example.cc`。构建时增加 `-DDOBBY_BUILD_EXAMPLE=ON`，然后执行 `cmake --build build/dobby-jni-android --target jni_example_lib`。

## 3. 在 DobbyHook replacement 中读取参数

目标 Java 方法为 `native int process(String text, byte[] payload, Object options)`：

```cpp
#include <dobby.h>
#include <dobby_jni.h>

using namespace dobby::jni;
using Target = jint (JNICALL *)(JNIEnv *, jobject, jstring, jbyteArray, jobject);
static Target original;

static jint JNICALL replacement(JNIEnv *env, jobject self, jstring text,
                                jbyteArray payload, jobject options) {
  Inspector inspect(env);
  Value s, bytes, object, mode;
  if (inspect.ReadObject(text, &s) == Status::Ok && !s.is_null) {
    // s.text 是拥有自身存储的 UTF-8 std::string，可在回调结束后保存。
    // 其中可包含 NUL，请使用 data()/size() 处理完整内容。
  }
  if (inspect.ReadObject(payload, &bytes) == Status::Ok && !bytes.is_null) {
    // bytes.elements[i].scalar.b 是有符号 jbyte。
  }
  if (inspect.ReadObject(options, &object) == Status::Ok && !object.is_null) {
    // object.class_name 为运行时类名，如 com.example.Options。
    // 已知 Options 有 int mode 字段时：
    inspect.ReadField(options, "mode", "I", &mode);
  }
  return original(env, self, text, payload, options);
}

int install(void *target) {
  // 使用 WxShadow 时，调用方先协调停止整个源页访问。
  int rc = dobby_set_hook_backend(DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED);
  if (rc) return rc;
  return DobbyHook(target, reinterpret_cast<void *>(replacement),
                   reinterpret_cast<void **>(&original));
}
```

目标是静态 native 方法时，第二个参数为 `jclass`。保留原方法的完整 C/C++ 签名和 `JNICALL`。普通 native 函数若另外传递有效 `jobject`，可以使用当前线程的 `JNIEnv*` 进行相同读取。

### 支持的值

| Java 值 | `Value` 中的内容 |
|---|---|
| boolean / byte / char / short / int / long / float / double | `scalar.z/b/c/s/i/j/f/d`；通过 ReadValue / ReadArguments 读取 |
| String | `class_name == "java.lang.String"`，`text` 为标准 UTF-8 |
| 八种基本类型数组 | `kind == Kind::Array`，`elements` 中为对应的基本类型值 |
| 对象数组、多维数组 | 递归的 `elements`，支持 null 和实际类型不同的元素 |
| 声明为 Object 的 String 或数组 | 根据运行时类型继续转换 |
| 其他对象，包括装箱类型 | `class_name`；通过 ReadField 读取已知实例字段 |
| null 引用 | `is_null == true` |

`ReadField(object, name, descriptor, &out)` 支持基本类型、对象、数组，以及可由 GetFieldID 查到的继承实例字段。返回值不包含 JNI 引用，不枚举所有字段，也不自动调用对象的 `toString()` 或 getter。装箱 Integer 可显式读取其 `value` 字段；静态字段、List/Map 展开和 Java 方法调用不属于此 API。

### 有界读取

```cpp
Inspector inspect(env, Limits{8, 256, 4096});
```

三个参数分别为最大数组递归深度、单次读取累计数组元素数、每个字符串最多读取的 UTF-16 代码单元数。达到限制时仍返回 Ok，并设置 `truncated`；嵌套元素的截断会传递到外层。ReadArguments 的所有参数共享元素预算。循环数组按相同预算终止。

字符串通过 GetStringRegion 复制 UTF-16 后转换成标准 UTF-8，保留中文、NUL 和 emoji；孤立代理项转换为 U+FFFD。字符串截断可能在代理对中间结束，此时末尾同样使用替代字符。

## 4. 按 JNI 描述符批量解析

```cpp
MethodDescriptor method;
Status rc = ParseMethodDescriptor("(Ljava/lang/String;[BIFDJ)V", &method);
// method.parameters 对应 String、byte[]、int、float、double、long。

jvalue args[6]{};
args[0].l = text;
args[1].l = payload;
args[2].i = number;
args[3].f = fraction;
args[4].d = precise;
args[5].j = wide;
std::vector<Value> values;
rc = Inspector(env).ReadArguments(method, args, 6, &values);
```

描述符解析拒绝缺失括号、void 参数、void 数组、未终止或空对象类名、尾部垃圾、超过 255 维的数组和超过 255 个 JVM 参数槽的签名。输出仅在成功时改变。`jvalue` 必须写入与描述符匹配的联合体成员。

### ARM64 入口寄存器解码

`DecodeArm64Arguments(method, gp, fp, stack, stack_size, &args)` 把标准 Android ARM64 JNI 调用现场转换为 `jvalue[]`，随后可交给 ReadArguments：

- `gp[0..7]` 为入口 x0..x7，跳过 x0 的 JNIEnv* 和 x1 的 receiver/class。
- `fp[0..7]` 为入口 v0..v7 的低 64 位；float 取低 32 位，double 取低 64 位。
- GP 与 FP 独立分配，溢出参数按参数顺序使用共享的八字节栈槽。
- `stack` 指向**原函数入口 SP** 开始的可读内存，`stack_size` 是调用方保证可读的字节数。空间不足返回 BufferTooSmall，并保持原输出。

普通后端的 DobbyInstrument 若用于函数入口，可以从 DobbyRegisterContext 拷贝 `general.x[0..7]` 和 `floating.q[0..7]` 的低 64 位。调用方仍需提供有效的入口栈范围。WxShadow 当前通过具备真实签名的 DobbyHook replacement 接入；其 DobbyInstrument 仍返回不支持。

此解码器不识别 ART quick ABI、@CriticalNative、省略 JNIEnv/receiver 的特殊入口、可变参数和 Darwin ABI。原始寄存器无法自行推导 Java 类型，必须提供真实 JNI 描述符。

## 5. 线程、引用和异常

JNI native 方法直接使用传入的 env。在没有 env 参数的 native pthread 中，使用先前从 `JNI_OnLoad` 或 `env->GetJavaVM()` 获得的 JavaVM：

```cpp
ScopedEnv thread(vm, true); // true 允许为 detached pthread 创建附件。
if (thread.status() == Status::Ok) {
  Inspector inspect(thread.get());
  // 跨线程传入的 Java 对象须使用有效 global ref。
}
```

ScopedEnv 只释放自己创建的附件，嵌套作用域不会误 detach 已附加线程；默认不自动 attach。它和 Inspector 必须在所属线程使用，ScopedEnv 在同一线程析构且先于 JVM 销毁。

Inspector 为临时引用创建局部帧，逐个释放对象数组元素，不消费传入引用。Value 的字符串、数组和基本类型均是独立快照，不保留 jobject。需要跨回调或跨线程保留原对象时，由调用方使用 NewGlobalRef / DeleteGlobalRef。

| Status | 含义 |
|---|---|
| Ok | 读取完成；另检查 is_null / truncated |
| InvalidArgument | 空输出、无效描述符、参数个数不匹配等 |
| PendingException | 调用前已有 Java 异常；原异常保持不变，输出不变 |
| JavaException | 本次 JNI 读取产生异常，已清除该异常，输出不变 |
| Detached / Unavailable | 当前线程未附加或无法取得 JNI 资源 |
| Reentrant | 当前线程已在进行一次 Inspector 操作，拒绝递归进入 |
| BufferTooSmall | ARM64 参数解码需要更多栈字节 |

只传入当前线程可用的有效 JNI 引用。jobject 是不透明句柄，不能把 ART 堆指针、任意 native 指针或已经失效的 local ref 交给本接口；CheckJNI 对这些错误可能直接终止进程。JNI critical region 内以及 ART quick/runnable 等内部状态不明的回调不能直接使用 Inspector。[Android JNI 说明](https://developer.android.com/ndk/guides/jni-tips)、[JNI 函数规范](https://docs.oracle.com/en/java/javase/24/docs/specs/jni/functions.html)。

## 6. 测试和实测结果

### 主机

```sh
JAVA_HOME=/usr/lib/jvm/java-11-openjdk-amd64 \
  cmake -S tests -B build/tests-jni -DCMAKE_BUILD_TYPE=Release -DDOBBY_JNI_TESTS=ON
cmake --build build/tests-jni -j6
ctest --test-dir build/tests-jni --output-on-failure
```

OpenJDK 11、`-Xcheck:jni` 下共 6/6 通过：新增 JNI static/shared 两项，以及现有四项 WxShadow 回归。JNI 测试包含八种基本类型数组、运行时类型、字段、嵌套/循环数组、字符串编码与截断、失败时输出不变、原异常对象保留、1000 次重复读取/错误路径、pthread 附件归属、ARM64 GP/FP/栈参数合成现场，以及真实 `Java → Dobby replacement → original → DobbyDestroy` 调用链。主机 hook fixture 预留 NOP 入口，不用于验证 x86 SIMD 指令重定位。

### Android ART

```sh
cmake -S tests/jni/android -B build/jni-device -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
cmake --build build/jni-device -j6
mkdir -p build/jni-device-classes build/jni-device-dex
javac --release 8 -d build/jni-device-classes tests/jni/DobbyJniFixture.java
"$ANDROID_SDK_ROOT/build-tools/35.0.1/d8" --min-api 24 \
  --output build/jni-device-dex build/jni-device-classes/DobbyJniFixture.class
```

Windows 使用同一 NDK/CMake 参数和 SDK 的 `d8.bat`。用环境变量提供设备凭据后：

```powershell
./tests/jni/run-device-tests.ps1 -Serial <adb设备序列号>
```

脚本以 app_process64 / `-Xcheck:jni` 运行真实 Java 调用，通过 RegisterNatives 注册进程自有的独立匿名 RX 入口页。分别测试静态库/动态库 × 普通/WxShadow 后端。仅当原先没有 wxshadow 时加载测试模块，并在所有测试进程退出后 QUIESCE、卸载；已有模块仅用于测试，不由脚本卸载。

2026-10-18 实测环境：Redmi 22041211AC / rubens，Android 12 API 31，ARM64，Linux `5.10.66-android12-9-00001-gd262b276948e-ab8291727`，4KB 页，NDK 25.1.8937393。四组全部通过；WxShadow 查询 capabilities=5f，验证回调读取、mixed GP/FP 参数传递、original 返回、活动源页 4096 字节保持原内容、Destroy 后原行为恢复。模块完成 QUIESCE / 卸载。

```text
PASS ART JNI + native: snapshots, ABI, original, destroy
PASS ART JNI + native: snapshots, ABI, original, destroy
PASS ART JNI + WxShadow: snapshots, ABI, original, hidden page, destroy
PASS ART JNI + WxShadow: snapshots, ABI, original, hidden page, destroy
VM notifiers drained. The KPM can now be unloaded.
```

实机发现 Clang 对 `jvalue{}` 可只初始化首个 jboolean 成员，已为 Value 显式初始化完整指针成员并通过四组复测，保证引用快照的 scalar.l 为空。本机执行日志位于忽略的 `build/jni-host-tests.log` 和 `build/jni-device-tests.log`，不包含设备密钥。

验收范围为有效 JNI 句柄和测试进程自有入口页。Java 方法的自动查找/注册监控、ART managed 方法 hook、任意地址插桩，以及 WxShadow 既有文档中排除的 BTI/PAC/JIT/自读页面均未扩大支持范围。
