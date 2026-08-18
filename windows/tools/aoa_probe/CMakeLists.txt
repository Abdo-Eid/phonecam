# Phase 7 throwaway diagnostic -- see main.cpp's header comment. Not linked into phonecam-host;
# deleted once the real AoaTransport lands and this question is answered for good.
add_executable(aoa_probe main.cpp)

set(LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/libusb)
target_include_directories(aoa_probe PRIVATE ${LIBUSB_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
target_link_libraries(aoa_probe PRIVATE ${LIBUSB_DIR}/lib/libusb-1.0.lib)
target_compile_features(aoa_probe PRIVATE cxx_std_20)

# libusb-1.0.dll must sit next to the exe (or be on PATH) at runtime -- same reasoning as
# adb.exe's PostBuildEvent copy for phonecam-vcam.dll.
add_custom_command(TARGET aoa_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LIBUSB_DIR}/bin/libusb-1.0.dll $<TARGET_FILE_DIR:aoa_probe>
)
