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
# keep using wmain's CRT startup despite the GUI subsystem, so argv-based diagnostic flags still
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
    connection/ConnectionManager.h
    connection/ConnectionManager.cpp
    resources/phonecam.rc
)

add_dependencies(phonecam-host phonecam_proto_gen)

target_include_directories(phonecam-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PHONECAM_PROTO_GEN_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/flatbuffers/include
)
target_link_libraries(phonecam-host PRIVATE phonecam_common mfplat mfuuid ole32 ws2_32 shell32
    )
target_compile_features(phonecam-host PRIVATE cxx_std_20)
target_link_options(phonecam-host PRIVATE "/ENTRY:wmainCRTStartup")

add_executable(phonecam-video-tests transport/VideoTimingTests.cpp)

add_executable(phonecam-media-settings-tests bench/MediaSettingsTests.cpp)
target_include_directories(phonecam-media-settings-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
