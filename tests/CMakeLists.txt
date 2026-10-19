FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2)
FetchContent_MakeAvailable(Catch2)

include(CTest)
include(Catch)

add_executable(unit-tests
  test-main.cpp
  test-audio-convert.cpp
  test-base64.cpp
  test-backoff.cpp
  test-ring-buffer.cpp
  test-smoke.cpp
  test-caption-protocol.cpp
  test-translate-protocol.cpp
  test-caption-composer.cpp
  test-caption-wrap.cpp
  test-pause-policy.cpp
  test-live-translate-protocol.cpp
  test-live-caption-buffer.cpp
  ${CMAKE_SOURCE_DIR}/src/live-translate-protocol.cpp
  ${CMAKE_SOURCE_DIR}/src/live-caption-buffer.cpp
  ${CMAKE_SOURCE_DIR}/src/pause-policy.cpp
  ${CMAKE_SOURCE_DIR}/src/audio-convert.cpp
  ${CMAKE_SOURCE_DIR}/src/backoff.cpp
  ${CMAKE_SOURCE_DIR}/src/base64.cpp
  ${CMAKE_SOURCE_DIR}/src/ring-buffer.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-protocol.cpp
  ${CMAKE_SOURCE_DIR}/src/translate-protocol.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-composer.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
)
target_include_directories(unit-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unit-tests PRIVATE Catch2::Catch2 nlohmann_json::nlohmann_json)
catch_discover_tests(unit-tests)

# Per-module test binaries for the caption pipeline (docs/specs/001 §4.8). Each
# compiles only its own slice so parallel slice work can build and run its tests
# without depending on the state of sibling slices. `unit-tests` above remains
# the full suite.
function(add_module_test name)
  add_executable(${name}
    test-main.cpp
    test-${name}.cpp
    ${ARGN}
  )
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(${name} PRIVATE Catch2::Catch2 nlohmann_json::nlohmann_json)
  catch_discover_tests(${name} TEST_PREFIX "${name}/")
endfunction()

add_module_test(caption-protocol
  ${CMAKE_SOURCE_DIR}/src/base64.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-protocol.cpp
)
add_module_test(translate-protocol
  ${CMAKE_SOURCE_DIR}/src/translate-protocol.cpp
)
add_module_test(caption-composer
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-composer.cpp
)
add_module_test(caption-wrap
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
)
# Spec 004 slices S1 / S2.
add_module_test(pause-policy
  ${CMAKE_SOURCE_DIR}/src/pause-policy.cpp
)
add_module_test(ring-buffer
  ${CMAKE_SOURCE_DIR}/src/ring-buffer.cpp
)
# Spec 007: two-way live translate engine.
add_module_test(live-translate-protocol
  ${CMAKE_SOURCE_DIR}/src/live-translate-protocol.cpp
)
add_module_test(live-caption-buffer
  ${CMAKE_SOURCE_DIR}/src/live-caption-buffer.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
)

# Watch planned files even while absent; never create a test executable until
# its test and every implementation dependency exist (spec 005 S0 / §5).
file(GLOB caption_segment_tests CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/test-caption-segmenter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/test-translation-scheduler.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/test-caption-pipeline.cpp"
)
target_sources(unit-tests PRIVATE ${caption_segment_sources} ${translation_quality_sources})

function(add_caption_segment_test name)
  set(test_file "${CMAKE_CURRENT_SOURCE_DIR}/test-${name}.cpp")
  if(NOT EXISTS "${test_file}")
    return()
  endif()
  foreach(source IN LISTS ARGN)
    if(NOT EXISTS "${source}")
      return()
    endif()
  endforeach()
  add_module_test(${name} ${ARGN})
  target_sources(unit-tests PRIVATE "${test_file}")
endfunction()

add_caption_segment_test(caption-segmenter
  ${CMAKE_SOURCE_DIR}/src/caption-segmenter.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
)
add_caption_segment_test(translation-scheduler
  ${CMAKE_SOURCE_DIR}/src/translation-scheduler.cpp
)
add_caption_segment_test(caption-pipeline
  ${CMAKE_SOURCE_DIR}/src/caption-pipeline.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-segmenter.cpp
  ${CMAKE_SOURCE_DIR}/src/translation-scheduler.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-composer.cpp
  ${CMAKE_SOURCE_DIR}/src/caption-wrap.cpp
  ${CMAKE_SOURCE_DIR}/src/translate-protocol.cpp
  ${CMAKE_SOURCE_DIR}/src/translation-quality.cpp
)
add_caption_segment_test(translation-quality
  ${CMAKE_SOURCE_DIR}/src/translation-quality.cpp
)
