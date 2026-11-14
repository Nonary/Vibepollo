#
# Loads FFmpeg pre-compiled binaries from GitHub releases or a user-specified path
#
include_guard(GLOBAL)

# ffmpeg pre-compiled binaries
if(NOT DEFINED FFMPEG_PREPARED_BINARIES)
    # Set platform-specific libraries
    if(WIN32)
        set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
    elseif(FREEBSD)
        # numa is not available on FreeBSD
        set(FFMPEG_PLATFORM_LIBRARIES va va-drm va-x11 X11)
    elseif(APPLE)
        set(FFMPEG_PLATFORM_LIBRARIES
            "-framework CoreFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework VideoToolbox"
            "-framework AudioToolbox")
    elseif(UNIX AND NOT APPLE)
        set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
    endif()

    # Pin the owned maintenance package independently of mutable submodule tags.
    include("${CMAKE_CURRENT_LIST_DIR}/ffmpeg-release.cmake")
    set(FFMPEG_ARCHIVE_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-ffmpeg.tar.gz")
    set(FFMPEG_HASH_VARIABLE "FFMPEG_SHA256_${CMAKE_SYSTEM_NAME}_${CMAKE_SYSTEM_PROCESSOR}")
    if(NOT DEFINED ${FFMPEG_HASH_VARIABLE})
        message(FATAL_ERROR "No pinned FFmpeg archive for ${FFMPEG_ARCHIVE_NAME}; provide FFMPEG_PREPARED_BINARIES for a custom build")
    endif()
    set(FFMPEG_ARCHIVE_SHA256 "${${FFMPEG_HASH_VARIABLE}}")
    set(FFMPEG_DOWNLOAD_URL "${FFMPEG_RELEASE_URL}/${FFMPEG_ARCHIVE_NAME}")

    # Include the archive identity in both the download and extraction cache.
    # A previous _deps/ffmpeg tree must never satisfy this new package pin.
    set(FFMPEG_VERSION_DIR "${CMAKE_BINARY_DIR}/_deps/ffmpeg-${FFMPEG_ARCHIVE_SHA256}")
    set(FFMPEG_ARCHIVE_PATH "${FFMPEG_VERSION_DIR}/${FFMPEG_ARCHIVE_NAME}")
    set(FFMPEG_PREPARED_BINARIES "${FFMPEG_VERSION_DIR}/ffmpeg")
    file(MAKE_DIRECTORY "${FFMPEG_VERSION_DIR}")

    if(NOT EXISTS "${FFMPEG_ARCHIVE_PATH}")
        message(STATUS "Downloading pinned FFmpeg binaries from ${FFMPEG_DOWNLOAD_URL}")
        set(FFMPEG_DOWNLOAD_PATH "${FFMPEG_ARCHIVE_PATH}.download")
        file(DOWNLOAD
            "${FFMPEG_DOWNLOAD_URL}"
            "${FFMPEG_DOWNLOAD_PATH}"
            EXPECTED_HASH "SHA256=${FFMPEG_ARCHIVE_SHA256}"
            TLS_VERIFY ON
            SHOW_PROGRESS
            STATUS FFMPEG_DOWNLOAD_STATUS
            TIMEOUT 300
        )
        list(GET FFMPEG_DOWNLOAD_STATUS 0 FFMPEG_DOWNLOAD_STATUS_CODE)
        list(GET FFMPEG_DOWNLOAD_STATUS 1 FFMPEG_DOWNLOAD_STATUS_MESSAGE)
        if(NOT FFMPEG_DOWNLOAD_STATUS_CODE EQUAL 0)
            message(FATAL_ERROR "Failed to download FFmpeg binaries: ${FFMPEG_DOWNLOAD_STATUS_MESSAGE}")
        endif()
        file(RENAME "${FFMPEG_DOWNLOAD_PATH}" "${FFMPEG_ARCHIVE_PATH}")
    endif()

    # Check cached archives too, rather than trusting their filenames.
    file(SHA256 "${FFMPEG_ARCHIVE_PATH}" FFMPEG_CACHED_SHA256)
    if(NOT FFMPEG_CACHED_SHA256 STREQUAL FFMPEG_ARCHIVE_SHA256)
        message(FATAL_ERROR "FFmpeg archive checksum mismatch: ${FFMPEG_ARCHIVE_PATH}")
    endif()
    if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a")
        file(ARCHIVE_EXTRACT  # cmake-lint: disable=E1126
            INPUT "${FFMPEG_ARCHIVE_PATH}"
            DESTINATION "${FFMPEG_VERSION_DIR}"
        )
        if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a")
            message(FATAL_ERROR "FFmpeg extraction failed or unexpected directory structure")
        endif()
    endif()
    message(STATUS "Using pinned FFmpeg ${FFMPEG_RELEASE_TAG}: ${FFMPEG_ARCHIVE_SHA256}")

    # Set FFmpeg libraries
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        set(HDR10_PLUS_LIBRARY "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()

    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a"
        ${HDR10_PLUS_LIBRARY}
        ${FFMPEG_PLATFORM_LIBRARIES}
    )
else()
    # User provided FFMPEG_PREPARED_BINARIES path
    message(STATUS "Using user-specified FFmpeg binaries at ${FFMPEG_PREPARED_BINARIES}")

    # Set platform-specific libraries
    if(NOT DEFINED FFMPEG_PLATFORM_LIBRARIES)
        if(WIN32)
            set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
        elseif(FREEBSD)
            set(FFMPEG_PLATFORM_LIBRARIES va va-drm va-x11 X11)
        elseif(APPLE)
            set(FFMPEG_PLATFORM_LIBRARIES
                "-framework CoreFoundation"
                "-framework CoreMedia"
                "-framework CoreVideo"
                "-framework VideoToolbox"
                "-framework AudioToolbox")
        elseif(UNIX AND NOT APPLE)
            set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
        endif()
    endif()

    # Set base FFmpeg libraries (always required)
    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
    )

    # Add optional libraries if they exist (e.g., from prebuilt packages)
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()

    # Add platform libraries
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_PLATFORM_LIBRARIES})
endif()

if(WIN32)
    # Static libavutil uses BCrypt for av_random_bytes(). Keep its system
    # dependency with FFmpeg so focused consumers do not rely on the host's
    # larger Windows link list, including with user-provided FFmpeg binaries.
    list(APPEND FFMPEG_LIBRARIES bcrypt)
endif()

set(FFMPEG_INCLUDE_DIRS "${FFMPEG_PREPARED_BINARIES}/include")
