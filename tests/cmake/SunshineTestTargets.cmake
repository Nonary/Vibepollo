include_guard(GLOBAL)

function(sunshine_add_test_target)
    set(options)
    set(one_value_args NAME CATEGORY)
    set(multi_value_args TEST_SOURCES SUPPORT_SOURCES PRODUCT_SOURCES INCLUDE_DIRECTORIES DEFINITIONS LINK_LIBRARIES DEPENDENCIES)
    cmake_parse_arguments(SUNSHINE_TEST "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT SUNSHINE_TEST_NAME)
        message(FATAL_ERROR "sunshine_add_test_target requires NAME")
    endif()
    if(NOT SUNSHINE_TEST_CATEGORY)
        message(FATAL_ERROR "sunshine_add_test_target(${SUNSHINE_TEST_NAME}) requires CATEGORY")
    endif()
    if(NOT SUNSHINE_TEST_TEST_SOURCES)
        message(FATAL_ERROR "sunshine_add_test_target(${SUNSHINE_TEST_NAME}) requires TEST_SOURCES")
    endif()

    set(_SUNSHINE_TEST_VALID_CATEGORIES FAST COMPONENT)
    string(TOUPPER "${SUNSHINE_TEST_CATEGORY}" SUNSHINE_TEST_CATEGORY_UPPER)
    if(NOT SUNSHINE_TEST_CATEGORY_UPPER IN_LIST _SUNSHINE_TEST_VALID_CATEGORIES)
        message(FATAL_ERROR
            "sunshine_add_test_target(${SUNSHINE_TEST_NAME}) has unsupported CATEGORY "
            "${SUNSHINE_TEST_CATEGORY}; use fast or component")
    endif()

    # Categories describe a test's seam, never a broad dependency bundle. A
    # target receives only the sources, definitions, libraries, and generated
    # prerequisites passed in this call. Former runtime behavior belongs in a
    # component target only after its real external edge has a deterministic
    # production-default seam.
    set(_sunshine_test_enable_option "SUNSHINE_TEST_ENABLE_${SUNSHINE_TEST_CATEGORY_UPPER}")
    if(NOT ${_sunshine_test_enable_option})
        message(STATUS "Skipping ${SUNSHINE_TEST_NAME}: ${SUNSHINE_TEST_CATEGORY} tests are disabled")
        return()
    endif()

    add_executable(${SUNSHINE_TEST_NAME}
        ${SUNSHINE_TEST_TEST_SOURCES}
        ${SUNSHINE_TEST_SUPPORT_SOURCES}
        ${SUNSHINE_TEST_PRODUCT_SOURCES}
    )
    # Keep the repository root universal for the established <src/...> test
    # includes.  Every non-repository include directory is supplied by the
    # registration that needs it; this prevents an unrelated SDK header from
    # becoming a recompilation dependency of every component target.
    target_include_directories(${SUNSHINE_TEST_NAME} PRIVATE
        "${SUNSHINE_TEST_REPOSITORY_ROOT}"
        ${SUNSHINE_TEST_INCLUDE_DIRECTORIES})
    target_compile_definitions(${SUNSHINE_TEST_NAME} PRIVATE ${SUNSHINE_TEST_DEFINITIONS})
    target_link_libraries(${SUNSHINE_TEST_NAME} PRIVATE ${SUNSHINE_TEST_LINK_LIBRARIES})
    target_link_options(${SUNSHINE_TEST_NAME} PRIVATE)
    set_target_properties(${SUNSHINE_TEST_NAME} PROPERTIES
        FOLDER "tests/${SUNSHINE_TEST_CATEGORY}"
    )

    foreach(dependency IN LISTS SUNSHINE_TEST_DEPENDENCIES)
        add_dependencies(${SUNSHINE_TEST_NAME} ${dependency})
    endforeach()

    add_test(NAME ${SUNSHINE_TEST_NAME} COMMAND ${SUNSHINE_TEST_NAME})
    set_tests_properties(${SUNSHINE_TEST_NAME} PROPERTIES LABELS "${SUNSHINE_TEST_CATEGORY}")
endfunction()
