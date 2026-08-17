add_executable(phonecam-host
    main.cpp
)

target_link_libraries(phonecam-host PRIVATE phonecam_common)
target_compile_features(phonecam-host PRIVATE cxx_std_20)
