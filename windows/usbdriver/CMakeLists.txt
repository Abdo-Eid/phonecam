# Phase 2: elevated helper that binds a libusbK driver for AOA, replacing
# the manual Zadig step -- see main.cpp's header comment and
# docs/architecture.md's driver-auto-install sections.
#
# Deliberately a separate executable from phonecam-host.exe: libwdi's own
# internal elevation (it spawns an embedded installer_x64.exe per
# wdi_install_driver call) only prompts once this way, already inside an
# already-elevated process, instead of once per call.
add_executable(phonecam-usbdriver main.cpp)

set(LIBWDI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libwdi)
target_include_directories(phonecam-usbdriver PRIVATE ${LIBWDI_DIR}/include)
target_link_libraries(phonecam-usbdriver PRIVATE
    ${LIBWDI_DIR}/lib/libwdi.lib
    setupapi
    newdev
    ntdll
    crypt32
    cfgmgr32
)
target_compile_features(phonecam-usbdriver PRIVATE cxx_std_20)

# The vendored libwdi.lib was built /MT (Release only, see
# third_party/libwdi/NOTICE.txt) -- force this target to match in every
# configuration, or Debug builds (which the rest of this project links /MTd)
# fail with a CRT mismatch at link time. Deliberately does NOT link
# phonecam_common, which builds /MTd in Debug, for the same reason -- this
# helper has no dependency on it anyway (its own printf-based logging is
# enough for a small, mostly-noninteractive tool).
set_property(TARGET phonecam-usbdriver PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
