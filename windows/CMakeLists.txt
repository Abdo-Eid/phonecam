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
add_subdirectory(vcam)
