add_library(phonecam_common STATIC
    log/Log.h
    log/Log.cpp
)

target_include_directories(phonecam_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(phonecam_common PUBLIC cxx_std_20)
