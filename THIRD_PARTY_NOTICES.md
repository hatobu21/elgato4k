# Third-party notices

## Veyra (GPL-3.0)

https://github.com/Likely7/Veyra-NRVideo, release 2.1.0. Vela is a derivative work and is distributed under the same licence (see `LICENSE`). Adapted files:

| Vela | Veyra |
|---|---|
| `win/src/nv/NgxCore.*` | `src/ngx/NgxCoreHost.cpp`, `include/veyra/ngx/NgxCoreHost.h` |
| `win/src/nv/DlssFg.*` | `src/ngx/DlssFgBackend.cpp`, `include/veyra/ngx/DlssFgBackend.h`, `include/veyra/ngx/DlssgLite.h` |
| `win/src/nv/DlssSr.*` | `src/ngx/DlssSrBackend.cpp` |
| `win/src/nv/RtxVideo.*` | `src/ngx/VideoSrBackend.cpp`, `src/ngx/TrueHdrBackend.cpp` |
| `win/src/nv/NvOf.*` | `src/ngx/NvOfSession.cpp`, `include/veyra/ngx/NvOfSession.h` |
| `win/src/nv/Reflex.h` | `include/veyra/gfx/ReflexSession.h` |
| `win/src/capture/Elgato.*` | `src/source/ElgatoHdrControl.cpp` |

## Code that Veyra itself adapted

- **elgatosf/capture-device-support** (MIT, Corsair Memory, Inc. 2022): Elgato vendor property set and InfoFrame layout, via `ElgatoHdrControl`.
- **Nitlink** (MIT, NitLink Contributors 2026): Elgato device identity rule and the non-MK.2 P010 full-range chroma policy.
- **NVIDIA/nvapi** (MIT): Reflex sleep-mode and marker ABI subset in `Reflex.h`. No NvAPI header or library is included.

## Fetched at build time, not stored in this repository

- **Microsoft.Web.WebView2** SDK (NuGet), Microsoft licence: headers and `WebView2LoaderStatic.lib`.
- **nlohmann/json** 3.11.3 (MIT).

## NVIDIA SDKs (user supplied)

DLSS SDK (`github.com/NVIDIA/DLSS`), Optical Flow SDK and RTX Video SDK are used under the NVIDIA RTX SDKs licence. Their headers, libraries and runtime DLLs are not part of this repository.
