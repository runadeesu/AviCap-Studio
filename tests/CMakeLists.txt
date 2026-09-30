# Unit + integration tests (doctest). Each test_*.cpp registers its own cases.
file(GLOB AVICAP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(avicap_tests test_main.cpp test_support.cpp ${AVICAP_TEST_SOURCES})
target_link_libraries(avicap_tests PRIVATE avicap_options AviCap::doctest ${AVICAP_TEST_LINK_LIBS})
set(AVICAP_TEST_MEDIA_DIR "${PROJECT_SOURCE_DIR}/build/test_media" CACHE PATH
    "Generated test media (tools/make_test_media.sh)")
target_compile_definitions(avicap_tests PRIVATE
    AVICAP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    AVICAP_TEST_MEDIA_DIR="${AVICAP_TEST_MEDIA_DIR}")

# Generates the synthetic media used by decode/analysis/export tests.
find_program(AVICAP_FFMPEG_CLI ffmpeg)
find_program(AVICAP_BASH bash)
if(AVICAP_FFMPEG_CLI AND AVICAP_BASH)
    add_custom_target(test_media
        COMMAND "${AVICAP_BASH}" "${PROJECT_SOURCE_DIR}/tools/make_test_media.sh" "${AVICAP_TEST_MEDIA_DIR}"
        COMMENT "Generating test media")
endif()

# Link every engine module.
get_property(_modules GLOBAL PROPERTY AVICAP_MODULE_TARGETS)
target_link_libraries(avicap_tests PRIVATE ${_modules})

add_test(NAME unit_tests COMMAND avicap_tests --exclude-test-case-glob=*[stress]* --duration=false)
add_test(NAME stress_tests COMMAND avicap_tests --test-case-glob=*[stress]*)
set_tests_properties(stress_tests PROPERTIES LABELS "stress" TIMEOUT 1800)
set_tests_properties(unit_tests PROPERTIES TIMEOUT 900)
