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

# WIN32: links with the GUI subsystem so double-clicking phonecam-host.exe (the real end-user
# path -- it's a tray-icon background app, not a CLI tool) doesn't pop a console window. main.cpp
# still defines wmain(argc, argv), not WinMain -- /ENTRY:wmainCRTStartup below tells the linker to
# keep using wmain's CRT startup despite the GUI subsystem, so argv-based flags (--aoa etc.) still
# work unchanged. Logging (std::printf/phonecam::log::*) is unaffected either way: with no console
# attached (a real double-click launch) those writes just go nowhere, same as before this had a
# console to write into; run from an existing terminal (as in dev/test) and they still show up,
# since a GUI-subsystem child process launched from an interactive console inherits its handles.
add_executable(phonecam-host WIN32
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
    transport/NetTransport.h
    transport/NetTransport.cpp
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
target_link_options(phonecam-host PRIVATE "/ENTRY:wmainCRTStartup")

# libusb-1.0.dll must sit next to the exe at runtime -- same reasoning as
# aoa_probe's own copy and adb.exe's PostBuildEvent copy for phonecam-vcam.dll.
add_custom_command(TARGET phonecam-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${LIBUSB_DIR}/bin/libusb-1.0.dll $<TARGET_FILE_DIR:phonecam-host>
)

# RunUsbDriverHelperElevated (main.cpp) locates phonecam-usbdriver.exe next to the running
# phonecam-host.exe -- true in the installed layout (PhoneCam.iss puts both in {app}), but in this
# CMake dev build tree each target lands in its own windows/build/<target>/<config>/ directory.
# Copy it alongside phonecam-host.exe after every build so the tray's driver-setup/revert items
# work from a dev build too, not just an installed one.
add_dependencies(phonecam-host phonecam-usbdriver)
add_custom_command(TARGET phonecam-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:phonecam-usbdriver> $<TARGET_FILE_DIR:phonecam-host>
)
