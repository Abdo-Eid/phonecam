add_executable(phonecam-svc
    main.cpp
)

target_link_libraries(phonecam-svc PRIVATE phonecam_common advapi32)
target_compile_features(phonecam-svc PRIVATE cxx_std_20)
