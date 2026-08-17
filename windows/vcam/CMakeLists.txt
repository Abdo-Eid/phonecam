add_library(phonecam-vcam SHARED
    dllmain.cpp
)

target_link_libraries(phonecam-vcam PRIVATE phonecam_common)
target_compile_features(phonecam-vcam PRIVATE cxx_std_20)
set_target_properties(phonecam-vcam PROPERTIES OUTPUT_NAME "phonecam-vcam")
