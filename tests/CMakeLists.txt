# Unit + integration tests (doctest). Each test_*.cpp registers its own cases.
file(GLOB AVICAP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(avicap_tests test_main.cpp test_support.cpp ${AVICAP_TEST_SOURCES})
target_link_libraries(avicap_tests PRIVATE avicap_options AviCap::doctest ${AVICAP_TEST_LINK_LIBS})
target_compile_definitions(avicap_tests PRIVATE
    AVICAP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Link every engine module that has been built.
foreach(_module IN LISTS AVICAP_ENGINE_MODULES)
    if(TARGET avicap_${_module})
        target_link_libraries(avicap_tests PRIVATE avicap_${_module})
    endif()
endforeach()

add_test(NAME unit_tests COMMAND avicap_tests --exclude-test-case-glob=*[stress]* --duration=false)
add_test(NAME stress_tests COMMAND avicap_tests --test-case-glob=*[stress]*)
set_tests_properties(stress_tests PROPERTIES LABELS "stress" TIMEOUT 1800)
set_tests_properties(unit_tests PROPERTIES TIMEOUT 900)
