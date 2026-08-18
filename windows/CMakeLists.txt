cmake_minimum_required(VERSION 3.24)

# Must be set before project() to affect the initial compiler/runtime detection. Release
# configs link the static CRT (/MT) -- matches windows/vcam/PhoneCamVCam.vcxproj's Release
# config (MultiThreaded) -- so the installer (Phase 5) doesn't need to bundle or depend on
# the end user already having the VC++ redistributable installed. Debug stays dynamic
# (/MDd, CMake's default) since debug builds are dev-only and never shipped.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(PhoneCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if (MSVC)
  add_compile_options(/W4 /permissive- /EHsc)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

add_subdirectory(common)
add_subdirectory(host)
add_subdirectory(svc)

# vcam/ is intentionally NOT a CMake target: it's forked from VCamSample
# (MIT), which builds via its own MSBuild project (PhoneCamVCam.vcxproj +
# packages.config, for the WIL/CppWinRT NuGet packages CMake doesn't
# naturally consume). It's process-isolated from phonecam-host, talking only
# through the SharedFrameRing contract in common/shm, so the two build
# systems never need to interoperate. Build it via:
#   msbuild windows\vcam\PhoneCamVCam.vcxproj /p:Configuration=Release /p:Platform=x64
# See docs/build.md.
