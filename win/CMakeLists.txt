# Vela Windows application. Configured from the repository root (WIN32 only).
#
# External SDKs (not redistributed in this repository, see docs/03):
#   VELA_DLSS_SDK  NVIDIA DLSS SDK root (github.com/NVIDIA/DLSS): include/, lib/Windows_x86_64/
#   VELA_NVOF_SDK  NVIDIA Optical Flow SDK root: NvOFInterface/
# Fetched automatically: WebView2 SDK (NuGet), nlohmann/json (if vcpkg does not provide it).

set(VELA_DLSS_SDK "" CACHE PATH "NVIDIA DLSS SDK root")
set(VELA_NVOF_SDK "" CACHE PATH "NVIDIA Optical Flow SDK root")
set(VELA_WEBVIEW2_VERSION "1.0.3240.44" CACHE STRING "Microsoft.Web.WebView2 NuGet version")

if(NOT EXISTS "${VELA_DLSS_SDK}/include/nvsdk_ngx.h")
  message(FATAL_ERROR "Set VELA_DLSS_SDK to the NVIDIA DLSS SDK (git clone https://github.com/NVIDIA/DLSS)")
endif()
if(NOT EXISTS "${VELA_NVOF_SDK}/NvOFInterface/nvOpticalFlowD3D12.h")
  message(FATAL_ERROR "Set VELA_NVOF_SDK to the NVIDIA Optical Flow SDK root (contains NvOFInterface/)")
endif()

include(FetchContent)
FetchContent_Declare(webview2
  URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${VELA_WEBVIEW2_VERSION}"
  DOWNLOAD_NAME webview2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(webview2)

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(json)
endif()

add_executable(vela WIN32
  src/main.cpp
  src/app/App.cpp
  src/app/Protocol.cpp
  src/app/UiOverlay.cpp
  src/audio/AudioPassthrough.cpp
  src/base/Log.cpp
  src/capture/Capture.cpp
  src/capture/Elgato.cpp
  src/engine/Engine.cpp
  src/engine/Pipeline.cpp
  src/engine/Presenter.cpp
  src/gfx/Compute.cpp
  src/gfx/Device.cpp
  src/gfx/GpuTimer.cpp
  src/gfx/Swapchain.cpp
  src/nv/DlssFg.cpp
  src/nv/DlssSr.cpp
  src/nv/NgxCore.cpp
  src/nv/NvOf.cpp
  src/nv/RtxVideo.cpp
  vela.manifest)

target_include_directories(vela PRIVATE
  src
  "${VELA_DLSS_SDK}/include"
  "${VELA_NVOF_SDK}/NvOFInterface"
  "${webview2_SOURCE_DIR}/build/native/include")
target_compile_definitions(vela PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# NGX static library matching the CRT (/MD by default in CMake).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(VELA_NGX_LIB "${VELA_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib")
else()
  set(VELA_NGX_LIB "${VELA_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
endif()

target_link_libraries(vela PRIVATE
  vela_core
  nlohmann_json::nlohmann_json
  "${VELA_NGX_LIB}"
  "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
  d3d12 dxgi dxguid dcomp
  mfplat mf mfreadwrite mfuuid
  ole32 avrt version shlwapi)

# ---- shaders: compiled with the Windows SDK's dxc into bin/shaders/*.cso
file(GLOB VELA_KITS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/dxc.exe")
list(SORT VELA_KITS ORDER DESCENDING)
list(GET VELA_KITS 0 VELA_DXC)
if(NOT VELA_DXC)
  message(FATAL_ERROR "dxc.exe not found in the Windows SDK")
endif()
set(VELA_SHADERS CaptureToWorking Analysis ColorApply FlowToMotion Encode Compose)
set(VELA_SHADER_OUTPUTS "")
foreach(shader IN LISTS VELA_SHADERS)
  set(out "$<TARGET_FILE_DIR:vela>/shaders/${shader}.cso")
  set(stamp "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.stamp")
  add_custom_command(OUTPUT "${stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vela>/shaders"
    COMMAND "${VELA_DXC}" -T cs_6_0 -E main -O3 -HV 2021 -Fo "${out}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl"
    COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Common.hlsli"
    COMMENT "dxc ${shader}.hlsl" VERBATIM)
  list(APPEND VELA_SHADER_OUTPUTS "${stamp}")
endforeach()
add_custom_target(vela_shaders DEPENDS ${VELA_SHADER_OUTPUTS})
add_dependencies(vela vela_shaders)

# ---- the interface is served from bin/ui by WebView2's virtual host mapping
add_custom_command(TARGET vela POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:vela>/ui"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vela>/runtime"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${VELA_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll"
          "${VELA_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlssg.dll"
          "$<TARGET_FILE_DIR:vela>/runtime"
  COMMENT "stage ui/ and the DLSS runtimes (RTX Video SDK DLLs are copied by hand, see docs/03)")
