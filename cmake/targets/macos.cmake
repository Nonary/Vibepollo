# macos specific target definitions

if (SUNSHINE_BUILD_HOMEBREW)
    target_link_options(sunshine PRIVATE LINKER:-sectcreate,__TEXT,__info_plist,${APPLE_PLIST_FILE})
else()
    # The touch helper: a small app bundle inside Vibepollo.app/Contents/Helpers that owns the
    # macOS 27 virtual touchscreen. It's built for both architectures with the project's warning
    # flags. The build signs it like the host, without the HID entitlement; touch needs it signed
    # again with a development profile (scripts/macos_sign_touch_helper.sh).
    configure_file("${SUNSHINE_SOURCE_ASSETS_DIR}/macos/touch_helper/Info.plist.in"
            "${CMAKE_BINARY_DIR}/touch_helper/Info.plist" @ONLY)
    add_executable(vibepollo_touch_helper MACOSX_BUNDLE
            "${CMAKE_SOURCE_DIR}/tools/macos_touch_helper/main.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/macos/touchscreen_policy.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/macos/touchscreen_policy.h")
    set_target_properties(vibepollo_touch_helper PROPERTIES
            OUTPUT_NAME "${TOUCH_HELPER_EXECUTABLE_NAME}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/touch_helper/Info.plist"
            OSX_ARCHITECTURES "arm64;x86_64"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/touch_helper")
    target_include_directories(vibepollo_touch_helper PRIVATE "${CMAKE_SOURCE_DIR}")
    target_include_directories(vibepollo_touch_helper SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third-party")
    target_compile_options(vibepollo_touch_helper PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    target_compile_definitions(vibepollo_touch_helper PRIVATE
            TOUCH_HELPER_IDENTIFIER="${TOUCH_HELPER_IDENTIFIER}"
            TOUCH_HELPER_HOST_IDENTIFIER="${PROJECT_FQDN}")
    target_link_libraries(vibepollo_touch_helper PRIVATE
            "${CORE_FOUNDATION_LIBRARY}" "${IO_KIT_LIBRARY}" "${SECURITY_LIBRARY}")
    add_dependencies(sunshine vibepollo_touch_helper)

    set(_touch_helper_sign_identity "-")
    if(APPLE_CODESIGN_IDENTITY)
        set(_touch_helper_sign_identity "${APPLE_CODESIGN_IDENTITY}")
    endif()

    # .app build
    set_target_properties(sunshine PROPERTIES
            OUTPUT_NAME "${CMAKE_PROJECT_NAME}"
            MACOSX_BUNDLE_BUNDLE_NAME "${CMAKE_PROJECT_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${PROJECT_FQDN}"
            MACOSX_BUNDLE_INFO_PLIST "${APPLE_PLIST_FILE}"
            MACOSX_BUNDLE_ICON_FILE "vibepollo.icns"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")

    # Sign the build-tree bundle with the packaging identity too. With a stable (non ad-hoc)
    # identity both copies satisfy the same designated requirement, so macOS privacy permissions
    # granted to one also cover the other and survive rebuilds.
    set(_sign_build_tree_bundle "")
    if(APPLE_CODESIGN_IDENTITY)
        set(_sign_build_tree_bundle
                COMMAND /usr/bin/codesign --force --sign "${APPLE_CODESIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:sunshine>")
    endif()

    # Populate bundle resources in the build tree for local runs.
    # MACOSX_BUNDLE_ICON_FILE only writes the Info.plist key, so copy the icon it names too.
    set(_bundle_resources_dir "$<TARGET_FILE_DIR:sunshine>/../Resources")
    set(_bundle_launch_agents_dir "$<TARGET_FILE_DIR:sunshine>/../Library/LaunchAgents")
    add_custom_command(TARGET sunshine POST_BUILD
            COMMENT "Copying bundle resources to build tree"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle_resources_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_BINARY_DIR}/assets" "${_bundle_resources_dir}/assets"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SUNSHINE_SOURCE_ASSETS_DIR}/macos/build/vibepollo.icns" "${_bundle_resources_dir}/vibepollo.icns"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle_launch_agents_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${APPLE_LAUNCH_AGENT_FILE}" "${_bundle_launch_agents_dir}/${PROJECT_FQDN}.plist"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:sunshine>/../Helpers"
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "$<TARGET_FILE_DIR:sunshine>/../Helpers/${TOUCH_HELPER_BUNDLE_NAME}.app"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:vibepollo_touch_helper>"
                    "$<TARGET_FILE_DIR:sunshine>/../Helpers/${TOUCH_HELPER_BUNDLE_NAME}.app"
            COMMAND /usr/bin/codesign --force --sign "${_touch_helper_sign_identity}" --options runtime
                    --identifier "${TOUCH_HELPER_IDENTIFIER}"
                    "$<TARGET_FILE_DIR:sunshine>/../Helpers/${TOUCH_HELPER_BUNDLE_NAME}.app"
            ${_sign_build_tree_bundle}
            VERBATIM)
endif()

# Tell linker to dynamically load these symbols at runtime, in case they're unavailable:
target_link_options(sunshine PRIVATE -Wl,-U,_CGPreflightScreenCaptureAccess -Wl,-U,_CGRequestScreenCaptureAccess)
