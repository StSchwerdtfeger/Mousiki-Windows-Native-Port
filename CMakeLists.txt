cmake_minimum_required(VERSION 3.16)
project(mousiki VERSION 2.3.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Third-party code — vendored in third_party/, so configuring needs no network.
#   miniaudio.h         : single-file audio library (public domain / MIT-0), v0.11.25
#   kissfft/            : small real-input FFT for the spectrum visualizer (BSD-3-Clause)
#   chromaprint/        : AcoustID fingerprinting library, 1.6.1 (MIT/LGPL-2.1) —
#                         compiled into the fpcalc helper below, which is why the
#                         project needs neither a prebuilt fpcalc.exe nor any
#                         download to produce one.
# ---------------------------------------------------------------------------
set(MINIAUDIO_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(MINIAUDIO_HEADER "${MINIAUDIO_DIR}/miniaudio.h")
set(KISSFFT_DIR "${CMAKE_SOURCE_DIR}/third_party/kissfft")
set(KISSFFT_FILES kiss_fft.c kiss_fft.h _kiss_fft_guts.h kiss_fft_log.h kiss_fftr.c kiss_fftr.h)
set(CHROMAPRINT_DIR "${CMAKE_SOURCE_DIR}/third_party/chromaprint")
set(CHROMAPRINT_KISSFFT_DIR "${CHROMAPRINT_DIR}/kissfft")

if(NOT EXISTS "${MINIAUDIO_HEADER}")
  message(FATAL_ERROR "Missing ${MINIAUDIO_HEADER} — third_party/ is part of the repository; restore it (e.g. `git checkout -- third_party`).")
endif()
foreach(kf IN LISTS KISSFFT_FILES)
  if(NOT EXISTS "${KISSFFT_DIR}/${kf}")
    message(FATAL_ERROR "Missing ${KISSFFT_DIR}/${kf} — third_party/ is part of the repository; restore it (e.g. `git checkout -- third_party`).")
  endif()
endforeach()
if(NOT EXISTS "${CHROMAPRINT_DIR}/chromaprint.h")
  message(FATAL_ERROR "Missing ${CHROMAPRINT_DIR}/chromaprint.h — third_party/ is part of the repository; restore it (e.g. `git checkout -- third_party`).")
endif()

# The Chromaprint library, exactly the file list upstream's src/CMakeLists.txt
# uses — minus the FFT backends that need FFmpeg/FFTW/Accelerate, which are not
# vendored (see third_party/chromaprint/README.md). The glob is safe because
# those files are simply not in the tree.
file(GLOB CHROMAPRINT_SOURCES CONFIGURE_DEPENDS
     "${CHROMAPRINT_DIR}/*.cpp" "${CHROMAPRINT_DIR}/utils/*.cpp")
file(GLOB CHROMAPRINT_KISSFFT_SOURCES CONFIGURE_DEPENDS
     "${CHROMAPRINT_KISSFFT_DIR}/kiss_fft.c" "${CHROMAPRINT_KISSFFT_DIR}/kiss_fftr.c")
if(NOT CHROMAPRINT_SOURCES OR NOT CHROMAPRINT_KISSFFT_SOURCES)
  message(FATAL_ERROR "Chromaprint sources missing under ${CHROMAPRINT_DIR} — third_party/ is part of the repository.")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MUISC_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
set(KISSFFT_SOURCES "${KISSFFT_DIR}/kiss_fft.c" "${KISSFFT_DIR}/kiss_fftr.c")
set_source_files_properties(${KISSFFT_SOURCES} PROPERTIES LANGUAGE C)

add_executable(mousiki ${MUISC_SOURCES} ${KISSFFT_SOURCES})
target_include_directories(mousiki PRIVATE "${CMAKE_SOURCE_DIR}/src" "${MINIAUDIO_DIR}" "${KISSFFT_DIR}")

# ---------------------------------------------------------------------------
# App icon (Windows only): mousiki.rc + mousiki.ico in the source root -- the
# packaging pipeline copies them in before building. Guarded so a plain
# source checkout without an icon still builds.
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/mousiki.rc")
    target_sources(mousiki PRIVATE "${CMAKE_SOURCE_DIR}/mousiki.rc")
endif()

# ---------------------------------------------------------------------------
# fpcalc — the fingerprint helper the meta editor's AcoustID fetch calls
# (scripts/fetch_meta.py runs `fpcalc -json -length 120 <file>`).
#
# Built from the vendored Chromaprint sources, so a fresh checkout builds the
# whole feature: no prebuilt binary to download, nothing to install. The only
# difference from upstream's tool is that this one *spawns* ffmpeg for decoding
# (already a hard mousiki dependency) instead of linking FFmpeg's development
# libraries — see third_party/chromaprint/README.md.
# ---------------------------------------------------------------------------
add_executable(fpcalc_tool ${CMAKE_SOURCE_DIR}/tools/fpcalc.cpp
                           ${CHROMAPRINT_SOURCES} ${CHROMAPRINT_KISSFFT_SOURCES})
target_include_directories(fpcalc_tool PRIVATE "${CHROMAPRINT_DIR}" "${CHROMAPRINT_KISSFFT_DIR}")
# The target is called fpcalc_tool so the CMake name is unambiguous, but the
# program must be called fpcalc: fetch_meta.py looks it up by file name.
set_target_properties(fpcalc_tool PROPERTIES OUTPUT_NAME fpcalc)
target_compile_definitions(fpcalc_tool PRIVATE
    # Chromaprint is compiled straight into this executable, so its API must not
    # be __declspec(dllimport) — that is what CHROMAPRINT_NODLL switches off.
    CHROMAPRINT_NODLL
    # Selects the vendored config.h, which upstream's CMake would generate.
    HAVE_CONFIG_H
    # utils.h computes window functions with M_PI, which MSVC hides otherwise.
    _USE_MATH_DEFINES
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS)
set_source_files_properties(${CHROMAPRINT_KISSFFT_SOURCES} PROPERTIES LANGUAGE C)
if(MSVC)
  # Vendored upstream code, not ours: fingerprint_matcher.cpp trips C4244 while
  # instantiating std::pair inside MSVC's own <utility>. Silencing it on the
  # vendored files only keeps warnings from tools/fpcalc.cpp visible.
  set_source_files_properties(${CHROMAPRINT_SOURCES} PROPERTIES COMPILE_OPTIONS "/wd4244")
endif()
if(WIN32)
  # CommandLineToArgvW(): the helper parses its own UTF-16 command line so that
  # non-ASCII file names survive (main()'s char** goes through the ANSI code page).
  target_link_libraries(fpcalc_tool PRIVATE shell32)
endif()

# ---------------------------------------------------------------------------
# Platform-specific audio backend wiring (compile-time selected)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(mousiki PRIVATE Threads::Threads)

# The lyrics / metadata / search helpers are invoked as script paths, so
# scripts/ has to sit next to the executable (on every platform -- the Linux
# and macOS builds look for it there too). Multi-config generators (Visual
# Studio, Xcode) put the exe in build/Release/, one level deeper than a
# single-config build. This has to be declared before the fpcalc copy step at
# the bottom, which drops the helper into that same scripts/ folder.
if(EXISTS "${CMAKE_SOURCE_DIR}/scripts")
  add_custom_command(TARGET mousiki POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_SOURCE_DIR}/scripts"
              "$<TARGET_FILE_DIR:mousiki>/scripts")
endif()

if(ANDROID)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_ANDROID=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_OPENSL=1)
  target_link_libraries(mousiki PRIVATE OpenSLES log android)
elseif(WIN32)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_WINDOWS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_WASAPI=1)
  target_link_libraries(mousiki PRIVATE ole32 winmm)

  target_compile_definitions(mousiki PRIVATE
      # <windows.h> otherwise defines min/max as macros, which collide with
      # std::min/std::max all over app.cpp.
      NOMINMAX
      WIN32_LEAN_AND_MEAN
      # M_PI is not in the C++ standard; MSVC only exposes it behind this,
      # and fft_visualizer.cpp's Hann window needs it.
      _USE_MATH_DEFINES
      # getenv/putenv/localtime are "deprecated" by MSVC in favour of the _s
      # variants; the code uses them deliberately and portably.
      _CRT_SECURE_NO_WARNINGS
      _CRT_NONSTDC_NO_WARNINGS)

  if(MSVC)
    # /utf-8 tells MSVC that the sources are UTF-8 *and* that narrow string
    # literals should stay UTF-8. Without it the box-drawing and block
    # characters baked into the UI get re-encoded to the build machine's ANSI
    # code page and render as mojibake at runtime.
    target_compile_options(mousiki PRIVATE /utf-8 /EHsc /W3
                           # app.cpp is ~3500 lines and exceeds the default
                           # section limit in debug builds.
                           /bigobj)
  endif()
elseif(APPLE)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_MACOS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_COREAUDIO=1)
  target_link_libraries(mousiki PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation" "-framework AudioUnit")
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_LINUX=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_PULSEAUDIO=1 MA_ENABLE_ALSA=1)
  # Note: miniaudio talks to PipeWire through its PulseAudio *and* ALSA
  # compatibility shims (pipewire-pulse / pipewire-alsa) — there is no
  # separate native "ma_backend_pipewire" enum in upstream miniaudio.
  target_link_libraries(mousiki PRIVATE dl m)
endif()

# fetch_meta.py looks for fpcalc in its own directory first, so the freshly
# built helper goes next to the scripts copied above — same place the old
# prebuilt scripts/fpcalc.exe used to live, now a build product instead of a
# checked-in binary. add_dependencies() guarantees it is built before mousiki.
if(TARGET fpcalc_tool)
  add_dependencies(mousiki fpcalc_tool)
  add_custom_command(TARGET mousiki POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:fpcalc_tool>"
              "$<TARGET_FILE_DIR:mousiki>/scripts/$<TARGET_FILE_NAME:fpcalc_tool>")
endif()

message(STATUS "mousiki: building for ${CMAKE_SYSTEM_NAME}")
