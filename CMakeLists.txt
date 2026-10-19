cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(compilerconfig)
include(defaults)
include(helpers)

option(BUILD_TESTS "Build unit tests" ON)

find_package(libobs REQUIRED)
# Streaming / recording / virtual-camera state for the auto-pause (spec 004 §4.5).
find_package(obs-frontend-api REQUIRED)

include(FetchContent)

FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.5)
FetchContent_MakeAvailable(ixwebsocket)
# Don't let the third-party IXWebSocket sources fail the build on their own
# warnings. COMPILE_WARNING_AS_ERROR covers CMake's mechanism; the macOS Xcode
# generator applies -Werror via GCC_TREAT_WARNINGS_AS_ERRORS, so disable that too.
set_target_properties(ixwebsocket PROPERTIES
  COMPILE_WARNING_AS_ERROR OFF
  XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS NO)
if(NOT MSVC)
  target_compile_options(ixwebsocket PRIVATE -Wno-error)
endif()

add_library(${CMAKE_PROJECT_NAME} MODULE
  src/plugin-main.cpp
  src/base64.cpp
  src/audio-convert.cpp
  src/filter.cpp
  src/ring-buffer.cpp
  src/backoff.cpp
  src/caption-protocol.cpp
  src/translate-protocol.cpp
  src/caption-composer.cpp
  src/caption-wrap.cpp
  src/caption-session.cpp
  src/caption-output.cpp
  src/pause-policy.cpp
  src/output-state.cpp
  src/live-translate-protocol.cpp
  src/live-caption-buffer.cpp
  src/live-translate-session.cpp
)
# Spec 005 slices arrive independently. Watch only their explicit paths so a
# subsequent build reconfigures when a new implementation is created.
file(GLOB caption_segment_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/caption-segmenter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/translation-scheduler.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/caption-pipeline.cpp"
)
file(GLOB translation_quality_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/translation-quality.cpp"
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/caption-segment.hpp
  src/translation-quality-types.hpp
  ${caption_segment_sources}
  ${translation_quality_sources}
)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
  OBS::libobs
  OBS::obs-frontend-api
  ixwebsocket
  nlohmann_json::nlohmann_json
  # bcrypt is a Windows crypto library needed by mbedTLS there; not on mac/Linux.
  $<$<PLATFORM_ID:Windows>:bcrypt>
)
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
