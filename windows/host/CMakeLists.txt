add_executable(phonecam-host
    main.cpp
    vcam_ctl/VCamControl.h
    vcam_ctl/VCamControl.cpp
    bridge/TestPatternProducer.h
    bridge/TestPatternProducer.cpp
    decode/MFH264Decoder.h
    decode/MFH264Decoder.cpp
)

target_include_directories(phonecam-host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(phonecam-host PRIVATE phonecam_common mfplat mfuuid ole32)
target_compile_features(phonecam-host PRIVATE cxx_std_20)
