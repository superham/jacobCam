# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(qcam
        VERSION 0.1.0
        DESCRIPTION "Modern Windows driver for the Logitech QuickCam Express (STV0600/HDCS-1000)"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(QCAM_BUILD_TESTS "Build the portable unit tests" ON)
option(QCAM_ASAN        "Build with AddressSanitizer (non-MSVC)" OFF)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
add_library(qcam_flags INTERFACE)
if(MSVC)
    target_compile_options(qcam_flags INTERFACE /W4 /permissive- /utf-8)
    target_compile_definitions(qcam_flags INTERFACE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(qcam_flags INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(QCAM_ASAN AND NOT MSVC)
    target_compile_options(qcam_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(qcam_flags INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# qcamcore - portable protocol, framing and imaging
# ---------------------------------------------------------------------------
set(QCAM_CORE_SOURCES
    src/core/types.cpp
    src/core/log.cpp
    src/core/bridge.cpp
    src/core/sensor_hdcs.cpp
    src/core/framer.cpp
    src/core/decode.cpp
    src/core/autoexp.cpp
    src/core/device.cpp
    src/core/transport_mock.cpp)

if(WIN32)
    list(APPEND QCAM_CORE_SOURCES
        src/win/enumerate.cpp
        src/win/transport_winusb.cpp
        src/win/frame_ring.cpp
        src/win/vcam_control.cpp)
else()
    list(APPEND QCAM_CORE_SOURCES
        src/core/enumerate_stub.cpp
        src/core/frame_ring_stub.cpp)
endif()

add_library(qcamcore STATIC ${QCAM_CORE_SOURCES})
target_include_directories(qcamcore PUBLIC include)
target_link_libraries(qcamcore PUBLIC qcam_flags)

if(WIN32)
    # WinUSB gives us vendor control transfers and, since Windows 8.1,
    # isochronous transfers from user mode. setupapi/cfgmgr32 enumerate.
    target_link_libraries(qcamcore PUBLIC
        winusb setupapi cfgmgr32 ole32 oleaut32 advapi32
        mf mfplat mfuuid mfsensorgroup)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(qcamcore PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Tests (portable; they run anywhere, with no camera attached)
# ---------------------------------------------------------------------------
if(QCAM_BUILD_TESTS)
    enable_testing()
    add_executable(qcam_tests
        tests/test_main.cpp
        tests/test_bridge.cpp
        tests/test_framer.cpp
        tests/test_hdcs.cpp
        tests/test_decode.cpp
        tests/test_autoexp.cpp
        tests/test_camera.cpp)
    target_link_libraries(qcam_tests PRIVATE qcamcore)
    target_include_directories(qcam_tests PRIVATE tests)
    add_test(NAME qcam_tests COMMAND qcam_tests)
endif()

# ---------------------------------------------------------------------------
# Windows-only targets
# ---------------------------------------------------------------------------
# The diagnostics CLI builds on any platform. Off Windows it cannot open a
# camera, but `qcamctl selftest` still exercises the protocol stack against
# the mock transport, which keeps the tool itself under test.
add_executable(qcamctl src/qcamctl/main.cpp)
target_link_libraries(qcamctl PRIVATE qcamcore)

if(WIN32)
    # Frame broker service: owns the USB device, publishes to shared memory.
    add_executable(qcamsvc src/qcamsvc/main.cpp src/qcamsvc/service.cpp)
    target_link_libraries(qcamsvc PRIVATE qcamcore advapi32)
    # The service uses wmain; be explicit about the CRT entry point rather
    # than relying on the linker inferring it.
    if(MSVC)
        target_link_options(qcamsvc PRIVATE /ENTRY:wmainCRTStartup)
    endif()

    # Media Foundation virtual camera source.
    add_library(qcamvcam SHARED
        src/qcamvcam/activate.cpp
        src/qcamvcam/dllmain.cpp
        src/qcamvcam/media_source.cpp
        src/qcamvcam/media_stream.cpp)
    target_link_libraries(qcamvcam PRIVATE
        qcamcore mf mfplat mfuuid mfreadwrite mfsensorgroup ole32 oleaut32 shlwapi)
    set_target_properties(qcamvcam PROPERTIES
        OUTPUT_NAME "qcamvcam"
        PREFIX "")
    target_sources(qcamvcam PRIVATE src/qcamvcam/qcamvcam.def)

    # Stage the INF next to the binaries so the install script finds them
    # together.
    add_custom_command(TARGET qcamctl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/driver/qcamusb.inf
                $<TARGET_FILE_DIR:qcamctl>/qcamusb.inf)
endif()

message(STATUS "qcam ${PROJECT_VERSION}: tests=${QCAM_BUILD_TESTS} win32=${WIN32}")
