cmake_minimum_required(VERSION 3.24)
project(Spatiall VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Spatiall targets Windows x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

include(cmake/Dependencies.cmake)

set(SPATIALL_DEFS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
set(SPATIALL_WARN /W4 /permissive- /utf-8 /wd4100)

# Everything except the UI: capture/render, routing, separation, spatialization, head tracking.
add_library(spatiall_core STATIC
  src/audio/Wasapi.cpp
  src/audio/Routing.cpp
  src/dsp/Resampler.cpp
  src/engine/Engine.cpp
  src/sep/HtDemucs.cpp
  src/sep/StreamingSeparator.cpp
  src/spatial/Binaural.cpp
  src/spatial/Room.cpp
  src/spatial/Upmix.cpp
  src/track/HeadTracker.cpp
  src/track/HidHeadSensor.cpp
  src/util/Log.cpp
)
target_include_directories(spatiall_core PUBLIC src)
target_compile_definitions(spatiall_core PUBLIC ${SPATIALL_DEFS})
target_compile_options(spatiall_core PRIVATE ${SPATIALL_WARN})
target_link_libraries(spatiall_core PUBLIC
  pocketfft samplerate onnxruntime_dml steamaudio
  dxgi ole32 avrt mmdevapi runtimeobject shell32 uuid propsys hid setupapi cfgmgr32 ws2_32)

add_executable(Spatiall WIN32
  src/main.cpp
  src/app/App.cpp
  src/app/Hotkey.cpp
  src/app/Ini.cpp
  src/app/Layouts.cpp
  src/app/Settings.cpp
  src/app/Tray.cpp
  src/app/VideoDetector.cpp
  src/ui/QuickMenu.cpp
  src/ui/Theme.cpp
  src/ui/Ui.cpp
  res/spatiall.rc
)
target_compile_definitions(Spatiall PRIVATE SPATIALL_VERSION="${PROJECT_VERSION}")
target_compile_options(Spatiall PRIVATE ${SPATIALL_WARN})
target_link_options(Spatiall PRIVATE /MANIFEST:NO)  # res/spatiall.rc embeds res/spatiall.manifest
target_link_libraries(Spatiall PRIVATE spatiall_core imgui d3d11 dcomp shlwapi iphlpapi)

# Runtime files next to the exe: native DLLs, separation models (*.onnx + *.json metadata), UI fonts.
spatiall_copy_runtime(Spatiall)
add_custom_command(TARGET Spatiall POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/models $<TARGET_FILE_DIR:Spatiall>/models
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:Spatiall>/fonts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPATIALL_FONT_FILES} $<TARGET_FILE_DIR:Spatiall>/fonts
  VERBATIM)

# Developer checks (not shipped): offline separator/spatial parity and a headless end-to-end device test.
foreach(tool selftest enginetest)
  add_executable(spatiall_${tool} tools/${tool}.cpp)
  target_compile_options(spatiall_${tool} PRIVATE ${SPATIALL_WARN})
  target_link_libraries(spatiall_${tool} PRIVATE spatiall_core)
  spatiall_copy_runtime(spatiall_${tool})
endforeach()
