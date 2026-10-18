## Dobby

[![Contact me Telegram](https://img.shields.io/badge/Contact%20me-Telegram-blue.svg)](https://t.me/IOFramebuffer) [![Join group Telegram](https://img.shields.io/badge/Join%20group-Telegram-brightgreen.svg)](https://t.me/dobby_group)

Dobby a lightweight, multi-platform, multi-architecture exploit hook framework.

- Minimal and modular library
- Multi-platform support(Windows/macOS/iOS/Android/Linux)
- Multiple architecture support(X86, X86-64, ARM, ARM64)

## Compile

[docs/compile.md](docs/compile.md)

### Optional WxShadow backend (Android ARM64)

This checkout adds `DOBBY_WITH_WXSHADOW=ON` and `DOBBY_WXSHADOW_UAPI_DIR` for
the strict kernel-backed target patch backend. Select
`dobby_set_hook_backend(DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED)` before DobbyHook.
The default backend remains native. A matching wxshadow KPM and its exact
Linux 5.10 VM adapter are required; missing capabilities return an error.

See [integration and usage](../docs/dobby-wxshadow-usage.md) and
[device validation](../docs/dobby-wxshadow-validation.md) in the containing
mkpms repository. Coordinate source-page access during installation/removal;
destroy every hook and run the administrator QUIESCE command before unloading
the KPM. DobbyInstrument and BTI/PAC are outside the current backend's scope.

## Download

[download latest library](https://github.com/jmpews/Dobby/releases/tag/latest)

## Credits

1. [frida-gum](https://github.com/frida/frida-gum)
2. [minhook](https://github.com/TsudaKageyu/minhook)
3. [substrate](https://github.com/jevinskie/substrate).
4. [v8](https://github.com/v8/v8)
5. [dart](https://github.com/dart-lang/sdk)
6. [vixl](https://git.linaro.org/arm/vixl.git)
