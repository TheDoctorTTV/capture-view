cmake_minimum_required(VERSION 3.25)

project(capture_view
  VERSION 0.2.1
  DESCRIPTION "Low-latency HDMI USB capture-card viewer"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CAPTURE_VIEW_WARNINGS /W4 /permissive-)
else()
  set(CAPTURE_VIEW_WARNINGS -Wall -Wextra -Wpedantic -Wconversion)
endif()

find_package(OpenGL REQUIRED)

if(WIN32)
  find_package(SDL3 CONFIG REQUIRED)
  if(TARGET SDL3::SDL3-static)
    set(CAPTURE_VIEW_SDL_TARGET SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3-shared)
    set(CAPTURE_VIEW_SDL_TARGET SDL3::SDL3-shared)
  elseif(TARGET SDL3::SDL3)
    set(CAPTURE_VIEW_SDL_TARGET SDL3::SDL3)
  else()
    message(FATAL_ERROR "SDL3 CMake package did not define an SDL3 target")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
  pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
  pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
  pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
endif()

set(CAPTURE_VIEW_COMMON_SOURCES
  src/app.cpp
  src/cli.cpp
  src/config.cpp
  src/frame.cpp
  src/log.cpp
  src/main.cpp
  src/test_pattern.cpp
)

if(WIN32)
  list(REMOVE_ITEM CAPTURE_VIEW_COMMON_SOURCES src/app.cpp)
  list(APPEND CAPTURE_VIEW_COMMON_SOURCES
    src/app_windows.cpp
    src/renderer_sdl_windows.cpp
  )
else()
  list(APPEND CAPTURE_VIEW_COMMON_SOURCES
    src/audio_monitor.cpp
    src/audio_devices.cpp
    src/audio_selector.cpp
    src/gui_bridge.cpp
    src/embedded_capture.cpp
    src/preview_writer.cpp
    src/h264_decoder.cpp
    src/mjpeg_decoder.cpp
    src/renderer_sdl.cpp
    src/wizard.cpp
  )
endif()

set(CAPTURE_VIEW_LINUX_SOURCES
  src/v4l2_capture.cpp
  src/v4l2_discovery.cpp
  src/v4l2_output.cpp
)

add_executable(capture-view
  ${CAPTURE_VIEW_COMMON_SOURCES}
  $<$<NOT:$<PLATFORM_ID:Windows>>:${CAPTURE_VIEW_LINUX_SOURCES}>
)
if(WIN32)
  set_target_properties(capture-view PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(capture-view PRIVATE src)
target_link_libraries(capture-view PRIVATE OpenGL::GL)

if(WIN32)
  target_link_libraries(capture-view PRIVATE ${CAPTURE_VIEW_SDL_TARGET})
else()
  target_link_libraries(capture-view PRIVATE
    PkgConfig::SDL3
    PkgConfig::TURBOJPEG
    PkgConfig::PIPEWIRE
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
  )
endif()

target_compile_options(capture-view PRIVATE ${CAPTURE_VIEW_WARNINGS})

enable_testing()

add_executable(capture-view-tests
  tests/unit_tests.cpp
  src/config.cpp
  src/cli.cpp
  src/frame.cpp
)
target_include_directories(capture-view-tests PRIVATE src)
target_compile_options(capture-view-tests PRIVATE ${CAPTURE_VIEW_WARNINGS})
add_test(NAME capture-view-tests COMMAND capture-view-tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(capture-view-device-helper src/device_helper.cpp)
  target_compile_options(capture-view-device-helper PRIVATE ${CAPTURE_VIEW_WARNINGS})
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(capture-view-device-helper PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  install(TARGETS capture-view-device-helper RUNTIME DESTINATION bin)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME capture-view-gui-bridge
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/gui_bridge_test.py" $<TARGET_FILE:capture-view>)
    set_tests_properties(capture-view-gui-bridge PROPERTIES TIMEOUT 20)
  endif()

  add_executable(capture-view-audio-startup-tests
    tests/audio_startup_tests.cpp
    src/audio_monitor.cpp
    src/log.cpp
  )
  target_include_directories(capture-view-audio-startup-tests PRIVATE src)
  target_link_libraries(capture-view-audio-startup-tests PRIVATE PkgConfig::PIPEWIRE)
  target_compile_options(capture-view-audio-startup-tests PRIVATE ${CAPTURE_VIEW_WARNINGS})
  target_link_options(capture-view-audio-startup-tests PRIVATE
    -Wl,--wrap=pw_context_new -Wl,--wrap=pw_context_connect -Wl,--wrap=pw_core_disconnect
    -Wl,--wrap=pw_stream_new -Wl,--wrap=pw_stream_new_simple -Wl,--wrap=pw_stream_add_listener
    -Wl,--wrap=pw_stream_connect -Wl,--wrap=pw_stream_destroy
    -Wl,--wrap=pw_stream_get_data_loop -Wl,--wrap=pw_thread_loop_start)
  add_test(NAME capture-view-audio-startup-tests COMMAND capture-view-audio-startup-tests)

  add_executable(capture-view-linux-tests
    tests/linux_tests.cpp
    src/audio_monitor.cpp
    src/config.cpp
    src/frame.cpp
    src/log.cpp
    src/v4l2_capture.cpp
  )
  target_include_directories(capture-view-linux-tests PRIVATE src)
  target_link_libraries(capture-view-linux-tests PRIVATE PkgConfig::PIPEWIRE)
  target_compile_options(capture-view-linux-tests PRIVATE ${CAPTURE_VIEW_WARNINGS})
  target_link_options(capture-view-linux-tests PRIVATE
    -Wl,--wrap=pw_stream_dequeue_buffer -Wl,--wrap=pw_stream_queue_buffer
    -Wl,--wrap=ioctl -Wl,--wrap=mmap -Wl,--wrap=munmap -Wl,--wrap=poll)
  add_test(NAME capture-view-linux-tests COMMAND capture-view-linux-tests)

  add_executable(capture-view-audio-discovery-tests tests/audio_discovery_tests.cpp src/audio_devices.cpp)
  target_include_directories(capture-view-audio-discovery-tests PRIVATE src)
  target_link_libraries(capture-view-audio-discovery-tests PRIVATE PkgConfig::PIPEWIRE)
  target_compile_options(capture-view-audio-discovery-tests PRIVATE ${CAPTURE_VIEW_WARNINGS})
  add_test(NAME capture-view-audio-discovery-tests COMMAND capture-view-audio-discovery-tests)
  set_tests_properties(capture-view-audio-discovery-tests PROPERTIES TIMEOUT 20)
endif()

# The Linux settings interface is Rust + GPUI; the capture engine remains C++.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(CAPTURE_VIEW_BUILD_GUI "Build the Rust / GPUI interface" ON)
  if(CAPTURE_VIEW_BUILD_GUI)
    find_program(CARGO_EXECUTABLE cargo REQUIRED)
    set(CAPTURE_VIEW_RUST_TARGET_DIR "${CMAKE_SOURCE_DIR}/build/rust")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      set(CAPTURE_VIEW_CARGO_PROFILE debug)
      set(CAPTURE_VIEW_CARGO_FLAGS)
    else()
      set(CAPTURE_VIEW_CARGO_PROFILE release)
      set(CAPTURE_VIEW_CARGO_FLAGS --release)
    endif()
    file(GLOB_RECURSE CAPTURE_VIEW_GUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/gui/src/*.rs" "${CMAKE_SOURCE_DIR}/third-party/xattr/src/*.rs" "${CMAKE_SOURCE_DIR}/third-party/gpui/src/*.rs")
    add_custom_command(
      OUTPUT "${CMAKE_BINARY_DIR}/capture-view-gui"
      COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CAPTURE_VIEW_RUST_TARGET_DIR}"
        ${CARGO_EXECUTABLE} build --locked --manifest-path "${CMAKE_SOURCE_DIR}/Cargo.toml" ${CAPTURE_VIEW_CARGO_FLAGS}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CAPTURE_VIEW_RUST_TARGET_DIR}/${CAPTURE_VIEW_CARGO_PROFILE}/capture-view-gui"
        "${CMAKE_BINARY_DIR}/capture-view-gui"
      DEPENDS ${CAPTURE_VIEW_GUI_SOURCES} "${CMAKE_SOURCE_DIR}/Cargo.toml" "${CMAKE_SOURCE_DIR}/Cargo.lock" "${CMAKE_SOURCE_DIR}/third-party/gpui/Cargo.toml"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      USES_TERMINAL VERBATIM)
    add_custom_target(capture-view-gpui ALL DEPENDS "${CMAKE_BINARY_DIR}/capture-view-gui")
    install(PROGRAMS "${CMAKE_BINARY_DIR}/capture-view-gui" DESTINATION bin)
  endif()
endif()

install(TARGETS capture-view RUNTIME DESTINATION bin)
install(FILES data/kirsch.bdf DESTINATION share/capture-view)
install(FILES LICENSE DESTINATION share/licenses/capture-view)
install(FILES data/kirsch.OFL.txt DESTINATION share/licenses/capture-view)

if(NOT WIN32)
  install(FILES data/capture-view.desktop DESTINATION share/applications)
  install(FILES data/capture-view.png DESTINATION share/icons/hicolor/512x512/apps)
  install(FILES docs/capture-view.1 DESTINATION share/man/man1)
endif()
