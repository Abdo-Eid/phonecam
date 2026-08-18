# --- FlatBuffers control-protocol codegen (proto/wire.fbs, proto/control.fbs) ---
# flatc.exe is a standalone compiler (winget Google.flatbuffers); the C++ *runtime*
# (header-only) is vendored as the third_party/flatbuffers submodule since flatc
# doesn't ship it. See docs/build.md.
find_program(FLATC_EXECUTABLE flatc REQUIRED)

set(PHONECAM_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../proto)
set(PHONECAM_PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/proto)
file(MAKE_DIRECTORY ${PHONECAM_PROTO_GEN_DIR})

add_custom_command(
    OUTPUT ${PHONECAM_PROTO_GEN_DIR}/wire_generated.h ${PHONECAM_PROTO_GEN_DIR}/control_generated.h
    COMMAND ${FLATC_EXECUTABLE} --cpp --scoped-enums --cpp-std c++17
            -o ${PHONECAM_PROTO_GEN_DIR} ${PHONECAM_PROTO_DIR}/wire.fbs ${PHONECAM_PROTO_DIR}/control.fbs
    DEPENDS ${PHONECAM_PROTO_DIR}/wire.fbs ${PHONECAM_PROTO_DIR}/control.fbs
    COMMENT "flatc: generating control-protocol C++ headers"
    VERBATIM
)
add_custom_target(phonecam_proto_gen
    DEPENDS ${PHONECAM_PROTO_GEN_DIR}/wire_generated.h ${PHONECAM_PROTO_GEN_DIR}/control_generated.h
)

add_executable(phonecam-host
    main.cpp
    vcam_ctl/VCamControl.h
    vcam_ctl/VCamControl.cpp
    bridge/TestPatternProducer.h
    bridge/TestPatternProducer.cpp
    bridge/LiveVideoBridge.h
    bridge/LiveVideoBridge.cpp
    decode/MFH264Decoder.h
    decode/MFH264Decoder.cpp
    transport/VideoTransport.h
    transport/AdbTransport.h
    transport/AdbTransport.cpp
    transport/AoaTransport.h
    transport/AoaTransport.cpp
    control/ControlTransport.h
    control/ControlTransport.cpp
    control/ControlChannel.h
    control/ControlChannel.cpp
    control/ConsoleControlUi.h
    control/ConsoleControlUi.cpp
    tray/TrayIcon.h
    tray/TrayIcon.cpp
    resources/phonecam.rc
)

add_dependencies(phonecam-host phonecam_proto_gen)

# Phase 7: AOA transport (transport/AoaTransport.h) needs libusb, already
# vendored for windows/tools/aoa_probe -- same include/lib/dll layout.
set(LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libusb)

target_include_directories(phonecam-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PHONECAM_PROTO_GEN_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/flatbuffers/include
    ${LIBUSB_DIR}/include
)
target_link_libraries(phonecam-host PRIVATE phonecam_common mfplat mfuuid ole32 ws2_32 shell32
    ${LIBUSB_DIR}/lib/libusb-1.0.lib)
target_compile_features(phonecam-host PRIVATE cxx_std_20)

# libusb-1.0.dll must sit next to the exe at runtime -- same reasoning as
# aoa_probe's own copy and adb.exe's PostBuildEvent copy for phonecam-vcam.dll.
add_custom_command(TARGET phonecam-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LIBUSB_DIR}/bin/libusb-1.0.dll $<TARGET_FILE_DIR:phonecam-host>
)
