# libwebrtc dependency (Windows only)

if(NOT SUNSHINE_ENABLE_WEBRTC)
    return()
endif()

set(WEBRTC_ROOT "${CMAKE_BINARY_DIR}/libwebrtc"
        CACHE PATH "Path to libwebrtc root (contains include/ and lib/).")
set(WEBRTC_LIBRARY "" CACHE FILEPATH "Path to libwebrtc library file.")
set(WEBRTC_INCLUDE_DIR "" CACHE PATH "Path to libwebrtc include directory.")
set(WEBRTC_EXTRA_LIBRARIES "" CACHE STRING "Extra libraries required by libwebrtc.")
set(WEBRTC_BUILD_DIR "" CACHE PATH "Working directory for the WebRTC build script.")
set(WEBRTC_OUT_DIR "" CACHE PATH "Output directory for the WebRTC build script.")
set(WEBRTC_BRANCH "m125_release" CACHE STRING "WebRTC branch for the build script.")
set(WEBRTC_REPO_URL "" CACHE STRING "WebRTC repo URL for the build script.")
set(WEBRTC_CONFIGURATION "" CACHE STRING "WebRTC build configuration (Debug/Release).")
set(WEBRTC_MSYS2_BIN "" CACHE PATH "MSYS2 ucrt64 bin path for the build script.")
set(WEBRTC_GIT_EXE "" CACHE FILEPATH "Git executable for the build script.")
set(WEBRTC_GIT_BIN "" CACHE PATH "Git bin directory for the build script.")
set(WEBRTC_VS_PATH "" CACHE PATH "Visual Studio install path for the build script.")
set(WEBRTC_WINSDK_DIR "" CACHE PATH "Windows SDK path for the build script.")
set(WEBRTC_CLANG_BASE_PATH "" CACHE PATH "Clang base path for the build script.")
set(WEBRTC_GCLIENT_JOBS "" CACHE STRING "gclient sync jobs for the build script.")
set(WEBRTC_GIT_CACHE_DIR "" CACHE PATH "Git cache directory for the build script.")
set(WEBRTC_DEPOT_TOOLS_DIR "" CACHE PATH "Depot tools directory for the build script.")
set(WEBRTC_VCVARS_PATH "" CACHE FILEPATH "vcvars64.bat path for MSVC configure/build scripts.")
set(WEBRTC_MSVC_BUILD_DIR "" CACHE PATH "MSVC build directory for configure/build scripts.")
set(WEBRTC_BUILD_JOBS "" CACHE STRING "Build jobs for MSVC build script.")

if(WIN32)
    if(NOT WEBRTC_VS_PATH)
        if(DEFINED ENV{VSINSTALLDIR} AND EXISTS "$ENV{VSINSTALLDIR}")
            set(WEBRTC_VS_PATH "$ENV{VSINSTALLDIR}" CACHE PATH "Visual Studio install path for the build script." FORCE)
        else()
            set(VSWHERE_PATHS "")
            if(DEFINED ENV{ProgramFiles})
                list(APPEND VSWHERE_PATHS "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer")
            endif()
            if(DEFINED ENV{ProgramW6432})
                list(APPEND VSWHERE_PATHS "$ENV{ProgramW6432}/Microsoft Visual Studio/Installer")
            endif()
            list(APPEND VSWHERE_PATHS
                "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
                "C:/Program Files/Microsoft Visual Studio/Installer")

            find_program(VSWHERE_EXECUTABLE vswhere PATHS ${VSWHERE_PATHS})
            if(VSWHERE_EXECUTABLE)
                execute_process(
                    COMMAND "${VSWHERE_EXECUTABLE}" -latest -products * -property installationPath
                    OUTPUT_VARIABLE VSWHERE_PATH
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                if(VSWHERE_PATH AND EXISTS "${VSWHERE_PATH}")
                    set(WEBRTC_VS_PATH "${VSWHERE_PATH}" CACHE PATH "Visual Studio install path for the build script." FORCE)
                endif()
            endif()
        endif()
    endif()

    if(NOT WEBRTC_WINSDK_DIR)
        if(DEFINED ENV{WindowsSdkDir} AND EXISTS "$ENV{WindowsSdkDir}")
            set(WEBRTC_WINSDK_DIR "$ENV{WindowsSdkDir}" CACHE PATH "Windows SDK path for the build script." FORCE)
        else()
            execute_process(
                COMMAND reg query "HKLM\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots" /v KitsRoot10
                OUTPUT_VARIABLE WINSDK_QUERY
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            string(REGEX MATCH "KitsRoot10[ \t]+REG_SZ[ \t]+([^\\r\\n]+)" WINSDK_MATCH "${WINSDK_QUERY}")
            if(CMAKE_MATCH_1 AND EXISTS "${CMAKE_MATCH_1}")
                set(WEBRTC_WINSDK_DIR "${CMAKE_MATCH_1}" CACHE PATH "Windows SDK path for the build script." FORCE)
            endif()
        endif()
    endif()
endif()

if(NOT WEBRTC_INCLUDE_DIR)
    if(WEBRTC_ROOT)
        if(EXISTS "${WEBRTC_ROOT}/include/libwebrtc.h")
            set(WEBRTC_INCLUDE_DIR "${WEBRTC_ROOT}/include")
        endif()
        find_path(WEBRTC_INCLUDE_DIR
                NAMES webrtc/api/peer_connection_interface.h api/peer_connection_interface.h libwebrtc.h
                PATHS "${WEBRTC_ROOT}"
                PATH_SUFFIXES include)
    else()
        find_path(WEBRTC_INCLUDE_DIR
                NAMES webrtc/api/peer_connection_interface.h api/peer_connection_interface.h libwebrtc.h)
    endif()
endif()

if(NOT WEBRTC_LIBRARY)
    if(WEBRTC_ROOT)
        if(WIN32)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                foreach(candidate
                        "${WEBRTC_ROOT}/lib/libwebrtc.dll.a"
                        "${WEBRTC_ROOT}/lib/libwebrtc.a"
                        "${WEBRTC_ROOT}/lib/libwebrtc.dll.lib"
                        "${WEBRTC_ROOT}/lib/libwebrtc.lib"
                        "${WEBRTC_ROOT}/lib/webrtc.lib")
                    if(EXISTS "${candidate}")
                        set(WEBRTC_LIBRARY "${candidate}")
                        break()
                    endif()
                endforeach()
                find_file(WEBRTC_LIBRARY
                        NAMES libwebrtc.dll.a libwebrtc.a libwebrtc.dll.lib libwebrtc.lib webrtc.lib
                        PATHS "${WEBRTC_ROOT}"
                        PATH_SUFFIXES lib)
            else()
                foreach(candidate
                        "${WEBRTC_ROOT}/lib/libwebrtc.dll.lib"
                        "${WEBRTC_ROOT}/lib/libwebrtc.lib"
                        "${WEBRTC_ROOT}/lib/webrtc.lib")
                    if(EXISTS "${candidate}")
                        set(WEBRTC_LIBRARY "${candidate}")
                        break()
                    endif()
                endforeach()
                find_file(WEBRTC_LIBRARY
                        NAMES libwebrtc.dll.lib libwebrtc.lib webrtc.lib
                        PATHS "${WEBRTC_ROOT}"
                        PATH_SUFFIXES lib)
            endif()
        else()
            find_library(WEBRTC_LIBRARY
                    NAMES webrtc libwebrtc
                    PATHS "${WEBRTC_ROOT}"
                    PATH_SUFFIXES lib)
        endif()
    else()
        if(WIN32)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                find_file(WEBRTC_LIBRARY
                        NAMES libwebrtc.dll.a libwebrtc.a libwebrtc.dll.lib libwebrtc.lib webrtc.lib)
            else()
                find_file(WEBRTC_LIBRARY
                        NAMES libwebrtc.dll.lib libwebrtc.lib webrtc.lib)
            endif()
        else()
            find_library(WEBRTC_LIBRARY
                    NAMES webrtc libwebrtc)
        endif()
    endif()
endif()

if(NOT WEBRTC_INCLUDE_DIR OR NOT WEBRTC_LIBRARY)
    message(FATAL_ERROR
            "libwebrtc not found. Set WEBRTC_ROOT to the libwebrtc install root, or "
            "set WEBRTC_INCLUDE_DIR and WEBRTC_LIBRARY explicitly.")
endif()

set(WEBRTC_RUNTIME_DLL "")
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(WEBRTC_ROOT AND NOT WEBRTC_RUNTIME_DLL)
        if(EXISTS "${WEBRTC_ROOT}/lib/libwebrtc.dll")
            set(WEBRTC_RUNTIME_DLL "${WEBRTC_ROOT}/lib/libwebrtc.dll")
        elseif(EXISTS "${WEBRTC_ROOT}/bin/libwebrtc.dll")
            set(WEBRTC_RUNTIME_DLL "${WEBRTC_ROOT}/bin/libwebrtc.dll")
        endif()
    endif()
    if(WEBRTC_LIBRARY MATCHES "\\.dll\\.lib$" OR WEBRTC_LIBRARY MATCHES "\\.lib$")
        find_program(GENDEF_EXECUTABLE gendef)
        find_program(DLLTOOL_EXECUTABLE dlltool)
        if(NOT GENDEF_EXECUTABLE OR NOT DLLTOOL_EXECUTABLE)
            message(FATAL_ERROR
                    "libwebrtc uses MSVC import libs. Install gendef and dlltool "
                    "(MSYS2 binutils) to generate a MinGW import library.")
        endif()
        if(NOT WEBRTC_RUNTIME_DLL)
            message(FATAL_ERROR
                    "libwebrtc.dll not found under WEBRTC_ROOT. "
                    "Expected WEBRTC_ROOT/lib/libwebrtc.dll or WEBRTC_ROOT/bin/libwebrtc.dll.")
        endif()

        set(WEBRTC_IMPORT_DIR "${CMAKE_BINARY_DIR}/libwebrtc")
        file(MAKE_DIRECTORY "${WEBRTC_IMPORT_DIR}")
        set(WEBRTC_DEF "${WEBRTC_IMPORT_DIR}/libwebrtc.def")
        set(WEBRTC_IMPORT_LIB "${WEBRTC_IMPORT_DIR}/libwebrtc.dll.a")

        execute_process(
                COMMAND "${GENDEF_EXECUTABLE}" "${WEBRTC_RUNTIME_DLL}"
                WORKING_DIRECTORY "${WEBRTC_IMPORT_DIR}"
                RESULT_VARIABLE GENDEF_RESULT
        )
        if(NOT GENDEF_RESULT EQUAL 0)
            message(FATAL_ERROR "gendef failed to generate ${WEBRTC_DEF}")
        endif()
        if(NOT EXISTS "${WEBRTC_DEF}")
            message(FATAL_ERROR "gendef did not create ${WEBRTC_DEF}")
        endif()

        execute_process(
                COMMAND "${DLLTOOL_EXECUTABLE}" -d "${WEBRTC_DEF}" -l "${WEBRTC_IMPORT_LIB}" -D libwebrtc.dll
                RESULT_VARIABLE DLLTOOL_RESULT
        )
        if(NOT DLLTOOL_RESULT EQUAL 0)
            message(FATAL_ERROR "dlltool failed to generate ${WEBRTC_IMPORT_LIB}")
        endif()

        set(WEBRTC_LIBRARY "${WEBRTC_IMPORT_LIB}")
    endif()
endif()

list(APPEND WEBRTC_INCLUDE_DIRS "${WEBRTC_INCLUDE_DIR}")
list(APPEND WEBRTC_LIBRARIES "${WEBRTC_LIBRARY}" ${WEBRTC_EXTRA_LIBRARIES})
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES ${WEBRTC_LIBRARIES})
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ENABLE_WEBRTC=1)
