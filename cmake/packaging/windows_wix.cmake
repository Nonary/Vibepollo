# WIX Packaging
# see options at: https://cmake.org/cmake/help/latest/cpack_gen/wix.html

# Use WiX as generator on Windows
set(CPACK_GENERATOR "WIX")

# Product identity and visuals
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/sunshine.ico")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Vibeshine")

# Stable Upgrade GUID to enable in-place upgrades
# NOTE: Do not change once released, or upgrades will break.
set(CPACK_WIX_UPGRADE_GUID "{C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}")

# Add a Start Menu shortcut and optional desktop link
# Pair of <exe-name;friendly-name>
set(CPACK_PACKAGE_EXECUTABLES "sunshine;Vibeshine")
# Uncomment to also create a desktop shortcut
# set(CPACK_CREATE_DESKTOP_LINKS "sunshine")

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
)


set(CPACK_WIX_EXTRA_SOURCES
  "${CMAKE_SOURCE_DIR}/packaging/windows/wix/custom_actions.wxs"
)


# ----------------------------------------------------------------------------
# Sanitize version for WiX: must be x.x.x.x with integers [0,65534]
# Map semver pre-release like 0.0.9-beta.1 -> 0.0.9.1
# If no pre-release, use .0
# ----------------------------------------------------------------------------
set(_RAW_VER "${CMAKE_PROJECT_VERSION}")
set(_WIX_MAJ 0)
set(_WIX_MIN 0)
set(_WIX_PAT 0)
set(_WIX_REV 0)

if(_RAW_VER MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)([-.][0-9A-Za-z.-]+)?$")
  set(_WIX_MAJ "${CMAKE_MATCH_1}")
  set(_WIX_MIN "${CMAKE_MATCH_2}")
  set(_WIX_PAT "${CMAKE_MATCH_3}")
  set(_SUFFIX   "${CMAKE_MATCH_4}")
  if(NOT "${_SUFFIX}" STREQUAL "")
    # Extract first numeric identifier from suffix (e.g., -beta.1 -> 1). If none, default to 1.
    string(REGEX MATCH "([0-9]+)" _NUM_FROM_SUFFIX "${_SUFFIX}")
    if(NOT "${_NUM_FROM_SUFFIX}" STREQUAL "")
      set(_WIX_REV "${CMAKE_MATCH_1}")
    else()
      set(_WIX_REV 1)
    endif()
  else()
    set(_WIX_REV 0)
  endif()
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

# Clamp to WiX allowed maximum 65534
foreach(_v IN ITEMS _WIX_MAJ _WIX_MIN _WIX_PAT _WIX_REV)
  if(${_v} GREATER 65534)
    set(${_v} 65534)
  endif()
endforeach()

set(CPACK_WIX_PRODUCT_VERSION "${_WIX_MAJ}.${_WIX_MIN}.${_WIX_PAT}.${_WIX_REV}")

# Ensure WiX template uses a valid version; some CPack templates reference CPACK_PACKAGE_VERSION
set(CPACK_PACKAGE_VERSION "${CPACK_WIX_PRODUCT_VERSION}")


# Merge our custom actions and sequencing directly into the generated Product
set(CPACK_WIX_PATCH_FILE "${CMAKE_SOURCE_DIR}/packaging/windows/wix/patch_custom_actions.wxs")

# Optional: increase light diagnostics
# set(CPACK_WIX_LIGHT_EXTRA_FLAGS "-dcl:high")
