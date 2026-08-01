set(SUNSHINE_WEB_SOURCE_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web")
set(SUNSHINE_WEB_OUTPUT_DIR "${CMAKE_BINARY_DIR}/assets/web")
set(SUNSHINE_WEB_STAMP "${SUNSHINE_WEB_OUTPUT_DIR}/.build-stamp")

set(SUNSHINE_WEB_SOURCES
    "${SUNSHINE_WEB_SOURCE_DIR}/package-lock.json"
    "${SUNSHINE_WEB_SOURCE_DIR}/package.json"
)
file(GLOB SUNSHINE_WEB_ROOT_SOURCES
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    "${SUNSHINE_WEB_SOURCE_DIR}/*.html"
    "${SUNSHINE_WEB_SOURCE_DIR}/*.json"
    "${SUNSHINE_WEB_SOURCE_DIR}/*.ts"
    "${SUNSHINE_WEB_SOURCE_DIR}/*.vue")
list(APPEND SUNSHINE_WEB_SOURCES ${SUNSHINE_WEB_ROOT_SOURCES})
foreach(SUNSHINE_WEB_SOURCE_SUBDIR IN ITEMS
        api components configs design generated public scripts services stores styles types utils views)
    file(GLOB_RECURSE SUNSHINE_WEB_SUBDIR_SOURCES
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES FALSE
        "${SUNSHINE_WEB_SOURCE_DIR}/${SUNSHINE_WEB_SOURCE_SUBDIR}/*")
    list(APPEND SUNSHINE_WEB_SOURCES ${SUNSHINE_WEB_SUBDIR_SOURCES})
endforeach()
list(REMOVE_DUPLICATES SUNSHINE_WEB_SOURCES)

find_program(SUNSHINE_NPM_EXECUTABLE NAMES npm.cmd npm)

if(SUNSHINE_NPM_EXECUTABLE)
    add_custom_command(
        OUTPUT "${SUNSHINE_WEB_STAMP}"
        COMMAND "${SUNSHINE_NPM_EXECUTABLE}" ci --ignore-scripts --no-audit --no-fund
        COMMAND "${CMAKE_COMMAND}" -E env
                "SUNSHINE_WEB_OUTPUT_DIR=${SUNSHINE_WEB_OUTPUT_DIR}"
                "${SUNSHINE_NPM_EXECUTABLE}" run build
        COMMAND "${CMAKE_COMMAND}" -E touch "${SUNSHINE_WEB_STAMP}"
        WORKING_DIRECTORY "${SUNSHINE_WEB_SOURCE_DIR}"
        BYPRODUCTS "${SUNSHINE_WEB_OUTPUT_DIR}/index.html"
        DEPENDS ${SUNSHINE_WEB_SOURCES}
        COMMENT "Building the Vibepollo browser interface"
        USES_TERMINAL
    )
    add_custom_target(web_ui DEPENDS "${SUNSHINE_WEB_STAMP}")
else()
    add_custom_target(web_ui
        COMMAND "${CMAKE_COMMAND}" -E echo "npm is required to build the Vibepollo browser interface"
        COMMAND "${CMAKE_COMMAND}" -E false
        COMMENT "Unable to build the Vibepollo browser interface"
    )
endif()
