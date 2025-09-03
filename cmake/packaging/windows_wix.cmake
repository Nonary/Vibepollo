# WIX Packaging
# see options at: https://cmake.org/cmake/help/latest/cpack_gen/wix.html

# Use WiX as generator on Windows
set(CPACK_GENERATOR "WIX")

# Product identity and visuals
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/sunshine.ico")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Sunshine")

# Stable Upgrade GUID to enable in-place upgrades
# NOTE: Do not change once released, or upgrades will break.
set(CPACK_WIX_UPGRADE_GUID "{C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}")

# Add a Start Menu shortcut and optional desktop link
# Pair of <exe-name;friendly-name>
set(CPACK_PACKAGE_EXECUTABLES "sunshine;Sunshine")
# Uncomment to also create a desktop shortcut
# set(CPACK_CREATE_DESKTOP_LINKS "sunshine")

# ARP info
set(CPACK_WIX_PROPERTY_ARPCOMMENTS "${CMAKE_PROJECT_DESCRIPTION}")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "${CMAKE_PROJECT_HOMEPAGE_URL}")

# Localizations/culture
set(CPACK_WIX_CULTURES "en-US")

# License for WiX must be RTF; point to our RTF wrapper
set(CPACK_WIX_LICENSE_RTF "${CMAKE_SOURCE_DIR}/packaging/windows/LICENSE.rtf")

# Enable WiX Util extension for QuietExec custom actions
set(CPACK_WIX_EXTENSIONS WixUtilExtension)

# Point WiX to your source folder so those VBS files can be resolved
set(CPACK_WIX_LIGHT_EXTRA_FLAGS
  "-b" "MyScripts=${CMAKE_SOURCE_DIR}/packaging/windows/wix"
)

# Merge our custom actions and sequencing directly into the generated Product
set(CPACK_WIX_PATCH_FILE "${CMAKE_SOURCE_DIR}/packaging/windows/wix/patch_custom_actions.wxs")

# Optional: increase light diagnostics
# set(CPACK_WIX_LIGHT_EXTRA_FLAGS "-dcl:high")
