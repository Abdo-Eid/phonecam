cmake_minimum_required(VERSION 3.24)
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
