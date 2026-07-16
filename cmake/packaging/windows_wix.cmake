# WIX Packaging
# see options at: https://cmake.org/cmake/help/latest/cpack_gen/wix.html

# Use WiX as generator on Windows
set(CPACK_GENERATOR "WIX")

# Product identity and visuals
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/images/apollo.ico")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Vibepollo")

# Stable Upgrade GUID to enable in-place upgrades
# NOTE: Do not change once released, or upgrades will break.
set(CPACK_WIX_UPGRADE_GUID "{E3FA501A-85F8-4187-85A7-D6E6BDC7EDA1}")

# Generate a fresh sortable ProductCode for every CPack WiX invocation.  The
# Upgrade GUID above intentionally remains stable so Windows Installer still
# treats all Vibepollo MSIs as the same product family.
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/packaging/windows/wix/generate_sortable_product_guid.cmake")

# Start Menu shortcut is now defined in custom_actions.wxs with --shortcut argument
# to ensure users launch the web UI instead of running the service binary directly

# ARP info
set(CPACK_WIX_PROPERTY_ARPCOMMENTS "${CMAKE_PROJECT_DESCRIPTION}")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "${CMAKE_PROJECT_HOMEPAGE_URL}")

# Localizations/culture
set(CPACK_WIX_CULTURES "en-US")

# License for WiX must be RTF; point to our RTF wrapper
set(CPACK_WIX_LICENSE_RTF "${CMAKE_SOURCE_DIR}/packaging/windows/LICENSE.rtf")

# Enable WiX extensions
# - WixUtilExtension: QuietExec and other utilities for custom actions
# - WixFirewallExtension: Declarative Windows Firewall rules
set(CPACK_WIX_EXTENSIONS WixUtilExtension;WixFirewallExtension)

# Point WiX to your source folder so those VBS files can be resolved
set(CPACK_WIX_LIGHT_EXTRA_FLAGS
  "-b" "MyScripts=${CMAKE_SOURCE_DIR}/packaging/windows/wix"
  "-b" "PayloadRoot=${CMAKE_BINARY_DIR}/wix_payload/"
)

# Define preprocessor variables for WiX sources
# BinDir: directory containing built binaries (sunshine.exe) at packaging time
set(CPACK_WIX_CANDLE_EXTRA_FLAGS
  "-dBinDir=${CMAKE_BINARY_DIR}"
  "-dVibepolloAppId=${WINDOWS_APP_USER_MODEL_ID}"
  # Human-readable version for ARP DisplayVersion; ProductVersion itself is
  # ordinal-encoded (see below) and no longer matches the semver.
  "-dVibeshineSemVer=${PROJECT_VERSION_FULL}"
)


set(CPACK_WIX_EXTRA_SOURCES
  "${CMAKE_SOURCE_DIR}/packaging/windows/wix/custom_actions.wxs"
)

# Override CPack's default WiX template to keep RemoveExistingProducts inside
# the MSI transaction. A failed upgrade must roll back to the previous install.
set(CPACK_WIX_TEMPLATE "${CMAKE_SOURCE_DIR}/packaging/windows/wix/WIX.template.in")

# uninstall.exe is packed into the MSI unsigned and signed as a nested PE by the
# SignPath "msi-file" deep-sign (see docs/signpath/). There is intentionally no
# CPACK_PRE_BUILD_SCRIPTS uninstaller-signing hook: a runner-local signature is
# non-origin-verified and was a no-op in CI anyway (the token is deliberately
# withheld from the MSI build step).


# ----------------------------------------------------------------------------
# Sanitize version for WiX: must be x.x.x.x with integers [0,65534]
# ----------------------------------------------------------------------------
# Windows Installer only compares the FIRST THREE ProductVersion fields when
# deciding whether a package upgrades an installed one (the 4th field is
# ignored).  Stripping the prerelease suffix therefore collapsed every
# beta/stable build of the same patch to one ProductVersion, which made
# beta->beta, beta->stable, and downgrade transitions invisible to MSI and
# forced the bootstrapper into non-transactional uninstall-then-install
# workarounds.  Encode a strictly monotonic prerelease ordinal into the third
# field instead:
#
#   third field = patch * 100 + ordinal
#     -alpha.N  -> N        (1..29)
#     -beta.N   -> 30 + N   (31..59)
#     -rc.N     -> 60 + N   (61..89)
#     other pre -> 90
#     stable    -> 99
#
#   1.18.0-beta.2 -> 1.18.32.0 < 1.18.0 -> 1.18.99.0 < 1.18.1-beta.1 -> 1.18.131.0
#
# Every release is now a real MSI major upgrade, handled inside a single
# transaction.  The human-readable version is preserved for ARP via the
# VibeshineSemVer candle define below; a third field >= 100 is also how the
# bootstrapper detects a payload that supports transactional downgrades.
set(_RAW_VER "${PROJECT_VERSION_NUMERIC}")
set(_WIX_MAJ 0)
set(_WIX_MIN 0)
set(_WIX_PAT 0)
set(_WIX_REV 0)

if(_RAW_VER MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  set(_WIX_MAJ "${CMAKE_MATCH_1}")
  set(_WIX_MIN "${CMAKE_MATCH_2}")
  set(_WIX_PAT "${CMAKE_MATCH_3}")
  set(_WIX_REV 0)
else()
  # Fallback: try separate vars or leave 0.0.0.0
  if(DEFINED CMAKE_PROJECT_VERSION_MAJOR)
    set(_WIX_MAJ "${CMAKE_PROJECT_VERSION_MAJOR}")
  endif()
  if(DEFINED CMAKE_PROJECT_VERSION_MINOR)
    set(_WIX_MIN "${CMAKE_PROJECT_VERSION_MINOR}")
  endif()
  if(DEFINED CMAKE_PROJECT_VERSION_PATCH)
    set(_WIX_PAT "${CMAKE_PROJECT_VERSION_PATCH}")
  endif()
  set(_WIX_REV 0)
endif()

# Derive the prerelease ordinal from the full version (e.g. 1.18.0-beta.2).
set(_WIX_PRERELEASE_ORDINAL 99)
if(PROJECT_VERSION_FULL MATCHES "-([A-Za-z]+)(\\.([0-9]+))?")
  set(_pre_tag "${CMAKE_MATCH_1}")
  set(_pre_num "${CMAKE_MATCH_3}")
  string(TOLOWER "${_pre_tag}" _pre_tag)
  if(_pre_num STREQUAL "")
    set(_pre_num 1)
  endif()
  if(_pre_num GREATER 29)
    message(WARNING "Prerelease number ${_pre_num} exceeds the encodable range (1..29); clamping to 29. ProductVersion ordering vs later ${_pre_tag} builds is no longer guaranteed.")
    set(_pre_num 29)
  endif()
  if(_pre_tag STREQUAL "alpha")
    set(_WIX_PRERELEASE_ORDINAL "${_pre_num}")
  elseif(_pre_tag STREQUAL "beta")
    math(EXPR _WIX_PRERELEASE_ORDINAL "30 + ${_pre_num}")
  elseif(_pre_tag STREQUAL "rc")
    math(EXPR _WIX_PRERELEASE_ORDINAL "60 + ${_pre_num}")
  else()
    # Unknown prerelease tag: rank below stable but above rc.
    set(_WIX_PRERELEASE_ORDINAL 90)
  endif()
endif()

if(_WIX_PAT GREATER 654)
  # 654 * 100 + 99 = 65499 <= 65534 (WiX field limit); anything larger overflows.
  message(FATAL_ERROR "Patch version ${_WIX_PAT} cannot be ordinal-encoded into the WiX ProductVersion third field (max 654).")
endif()
math(EXPR _WIX_PAT "${_WIX_PAT} * 100 + ${_WIX_PRERELEASE_ORDINAL}")

# Clamp to WiX allowed maximum 65534
foreach(_v IN ITEMS _WIX_MAJ _WIX_MIN _WIX_PAT _WIX_REV)
  if(${_v} GREATER 65534)
    set(${_v} 65534)
  endif()
endforeach()

set(CPACK_WIX_PRODUCT_VERSION "${_WIX_MAJ}.${_WIX_MIN}.${_WIX_PAT}.${_WIX_REV}")

# Ensure WiX uses a valid numeric version; some templates reference CPACK_PACKAGE_VERSION
set(CPACK_PACKAGE_VERSION "${CPACK_WIX_PRODUCT_VERSION}")

# Helpful for diagnostics in CI/local logs
message(STATUS "CPACK_WIX_PRODUCT_VERSION = ${CPACK_WIX_PRODUCT_VERSION} (from ${PROJECT_VERSION_FULL})")


# Merge our custom actions and sequencing directly into the generated Product
set(CPACK_WIX_PATCH_FILE "${CMAKE_SOURCE_DIR}/packaging/windows/wix/patch_custom_actions.wxs")

# Optional: increase light diagnostics
# set(CPACK_WIX_LIGHT_EXTRA_FLAGS "-dcl:high")
