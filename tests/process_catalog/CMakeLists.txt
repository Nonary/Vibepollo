cmake_minimum_required(VERSION 3.20)

# This same target is included by the application test suite and can be built
# independently without unrelated capture, driver or web dependencies.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ProcessCatalogTests LANGUAGES CXX)
    enable_testing()
endif()
find_package(Threads REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS process)
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

set(_catalog_repository_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(test_component_process_refresh
    "${CMAKE_CURRENT_LIST_DIR}/../unit/test_process_catalog.cpp"
    "${_catalog_repository_root}/src/process_catalog.cpp")
target_compile_features(test_component_process_refresh PRIVATE cxx_std_23)
target_include_directories(test_component_process_refresh PRIVATE "${_catalog_repository_root}")
target_link_libraries(test_component_process_refresh PRIVATE Boost::process Threads::Threads nlohmann_json::nlohmann_json)
add_test(NAME test_component_process_refresh COMMAND test_component_process_refresh)
set_tests_properties(test_component_process_refresh PROPERTIES LABELS "component" TIMEOUT 45)
unset(_catalog_repository_root)
