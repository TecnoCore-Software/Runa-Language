// Runa — platform helpers shared by the import/native subsystems.
//
// Two services the toolchain needs at run time:
//   1. the directory of the RUNNING EXECUTABLE (independent of the current
//      working directory) — the anchor for the runalibs search paths;
//   2. the native TARGET KEY of the current machine, e.g. "linux-x86_64",
//      used to select the right binary from a native package manifest.
//
// Supported: Linux (POSIX), Windows, macOS (best-effort). The key names follow
// the package manifest convention: <os>-<arch>.
#pragma once

#include <string>

namespace runa::platform {

// Directory of the running executable (no trailing separator). Falls back to
// "." when the platform lookup fails — documented, deterministic behaviour.
std::string executableDir();

// Native target key of the current machine: <os>-<arch>.
// os: windows | linux | macos     arch: x86_64 | arm64 | armv7
std::string nativeTargetKey();

}  // namespace runa::platform
