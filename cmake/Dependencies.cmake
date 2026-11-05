# Third-party dependencies, all pinned. Large binary SDKs are downloaded into the build tree.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- Dear ImGui (Win32 + DX11 backends) -------------------------------------------------------
FetchContent_Declare(imgui_src
  URL https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.zip  # v1.93.0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui_src)
add_library(imgui STATIC
  ${imgui_src_SOURCE_DIR}/imgui.cpp
  ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_src_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_src_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_src_SOURCE_DIR} ${imgui_src_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)

# --- pocketfft (header-only C++ FFT) ----------------------------------------------------------
# SOURCE_SUBDIR points at a directory that doesn't exist so MakeAvailable only downloads (no add_subdirectory).
FetchContent_Declare(pocketfft_src
  URL https://github.com/mreineck/pocketfft/archive/4d865cba24359b05b37da4e75aee24498c3b78e7.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _download_only)
FetchContent_MakeAvailable(pocketfft_src)
add_library(pocketfft INTERFACE)
target_include_directories(pocketfft SYSTEM INTERFACE ${pocketfft_src_SOURCE_DIR})
target_compile_definitions(pocketfft INTERFACE POCKETFFT_NO_MULTITHREADING)

# --- libsamplerate ----------------------------------------------------------------------------
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)  # libsamplerate 0.2.2 declares cmake_minimum_required(3.1)
FetchContent_Declare(libsamplerate
  URL https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(libsamplerate)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- ONNX Runtime (DirectML build) + DirectML redistributable ---------------------------------
FetchContent_Declare(ort_dml
  URL https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4
  DOWNLOAD_NAME ort_dml.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _download_only)
FetchContent_Declare(directml
  URL https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4
  DOWNLOAD_NAME directml.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _download_only)
FetchContent_MakeAvailable(ort_dml directml)
add_library(onnxruntime_dml SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime_dml PROPERTIES
  IMPORTED_IMPLIB ${ort_dml_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime.lib
  IMPORTED_LOCATION ${ort_dml_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime.dll
  INTERFACE_INCLUDE_DIRECTORIES "${ort_dml_SOURCE_DIR}/build/native/include;${directml_SOURCE_DIR}/include")
set(SPATIALL_DIRECTML_DLL ${directml_SOURCE_DIR}/bin/x64-win/DirectML.dll)

# --- Steam Audio (binaural HRTF rendering) ----------------------------------------------------
FetchContent_Declare(steamaudio
  URL https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip
  URL_HASH SHA256=4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _download_only)
FetchContent_MakeAvailable(steamaudio)
find_path(STEAMAUDIO_INCLUDE_DIR phonon.h PATHS ${steamaudio_SOURCE_DIR} PATH_SUFFIXES include steamaudio/include
  NO_DEFAULT_PATH REQUIRED)
get_filename_component(STEAMAUDIO_ROOT ${STEAMAUDIO_INCLUDE_DIR} DIRECTORY)
add_library(steamaudio SHARED IMPORTED GLOBAL)
set_target_properties(steamaudio PROPERTIES
  IMPORTED_IMPLIB ${STEAMAUDIO_ROOT}/lib/windows-x64/phonon.lib
  IMPORTED_LOCATION ${STEAMAUDIO_ROOT}/lib/windows-x64/phonon.dll
  INTERFACE_INCLUDE_DIRECTORIES ${STEAMAUDIO_INCLUDE_DIR})

# --- JetBrains Mono (UI typeface, SIL OFL 1.1; shipped in <exe>/fonts) ----------------------------
FetchContent_Declare(jbmono
  URL https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
  URL_HASH SHA256=6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _download_only)
FetchContent_MakeAvailable(jbmono)
set(SPATIALL_FONT_FILES
  ${jbmono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-ExtraLight.ttf
  ${jbmono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Light.ttf
  ${jbmono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf
  ${jbmono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Medium.ttf
  ${jbmono_SOURCE_DIR}/OFL.txt)

# Copy the native runtime DLLs next to a target's executable.
function(spatiall_copy_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:onnxruntime_dml> ${SPATIALL_DIRECTML_DLL} $<TARGET_FILE:steamaudio>
            $<TARGET_FILE_DIR:${target}>
    VERBATIM)
endfunction()
