#!/usr/bin/env bash
# Syntax and type check of the Windows tree on Linux with MinGW-w64 and the
# Linux build of DXC. It cannot link or run anything; it catches typos, wrong
# signatures and API misuse before a Windows build.
#
#   sudo apt-get install g++-mingw-w64-x86-64-posix
#   tools/check-windows-syntax.sh            # SDK headers are cached in .cache/
#
# Not covered: nv/NvOf.cpp (the NVOF D3D12 header ships only with the
# Optical Flow SDK download) and MSVC-only SEH (__try is replaced here).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cache="$root/.cache/sdk"
mkdir -p "$cache"

if [ ! -f "$cache/dlss/include/nvsdk_ngx.h" ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git "$cache/dlss"
  git -C "$cache/dlss" sparse-checkout set include
fi
if [ ! -f "$cache/webview2/build/native/include/WebView2.h" ]; then
  curl -sSL -o "$cache/webview2.zip" https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.3240.44
  mkdir -p "$cache/webview2" && (cd "$cache/webview2" && unzip -q -o ../webview2.zip)
fi
if [ ! -f "$cache/json/nlohmann/json.hpp" ]; then
  mkdir -p "$cache/json/nlohmann"
  curl -sSL -o "$cache/json/nlohmann/json.hpp" https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
fi

# MinGW lacks EventToken.h, which WebView2.h includes; the type is trivial.
mkdir -p "$cache/stubs"
[ -f "$cache/stubs/EventToken.h" ] || printf '#pragma once\ntypedef struct EventRegistrationToken { __int64 value; } EventRegistrationToken;\n' > "$cache/stubs/EventToken.h"

cxx=x86_64-w64-mingw32-g++
flags=(-std=c++20 -fsyntax-only -Wall -Wextra -Wno-unknown-pragmas -Wno-missing-field-initializers
       -Wno-cast-function-type -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00
       -I"$root/win/src" -I"$root/core/include" -I"$cache/dlss/include"
       -I"$cache/webview2/build/native/include" -I"$cache/json" -idirafter "$cache/stubs")
status=0
for f in $(cd "$root/win/src" && find . -name '*.cpp' ! -name NvOf.cpp | sort); do
  if out=$("$cxx" "${flags[@]}" "$root/win/src/$f" 2>&1); then
    echo "ok   $f"
  else
    echo "FAIL $f"; echo "$out" | grep -E "error" | head -20; status=1
  fi
done

# Shaders, when a Linux dxc is available (DirectXShaderCompiler releases).
if [ -n "${DXC:-}" ]; then
  for s in "$root"/win/shaders/*.hlsl; do
    if "$DXC" -T cs_6_0 -E main -O3 -HV 2021 -Fo /dev/null "$s" >/dev/null 2>&1; then echo "ok   $(basename "$s")"
    else echo "FAIL $(basename "$s")"; status=1; fi
  done
fi
exit $status
