add_executable(phonecam-media-bench main.cpp)
target_link_libraries(phonecam-media-bench PRIVATE mf mfplat mfreadwrite mfuuid ole32)

add_executable(phonecam-decoder-recovery DecoderRecovery.cpp RtpDecoderReplay.cpp
    ../../host/decode/MFH264Decoder.cpp ../../host/transport/RtpVideoTransport.cpp)
target_include_directories(phonecam-decoder-recovery PRIVATE ../../host)
target_link_libraries(phonecam-decoder-recovery PRIVATE phonecam_common mfplat mfuuid ole32 ws2_32)
