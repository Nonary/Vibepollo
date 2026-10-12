include_guard(GLOBAL)

# This is the upstream Microsoft-signed release, never a locally rebuilt
# kernel driver. These artifacts must remain byte-for-byte unchanged during
# application signing; their catalogs already attest to the INF and SYS files.
set(SUNSHINE_USBIP_RELEASE_TAG "v.0.9.8.1")
set(SUNSHINE_USBIP_INSTALLER_SHA256
    "38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a")
set(SUNSHINE_USBIP_DRIVER_DESTINATION "drivers/usbip-win2")
set(SUNSHINE_USBIP_REQUIRED_FILES
    usbip2_ude.inf usbip2_ude.sys usbip2_ude.cat
    usbip2_filter.inf usbip2_filter.sys usbip2_filter.cat
    release-lock.json)

# Bundling makes the option available. It never selects installation: that
# requires INSTALL_USBIP_TRANSPORT=1 or the bootstrapper's unchecked checkbox.
set(_sunshine_usbip_default OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|x64)$")
    set(_sunshine_usbip_default ON)
endif()
option(SUNSHINE_BUNDLE_USBIP_TRANSPORT
    "Bundle signed USB/IP transport for optional Windows DualSense USB waveform haptics."
    ${_sunshine_usbip_default})
unset(_sunshine_usbip_default)

if(SUNSHINE_BUNDLE_USBIP_TRANSPORT AND
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|x64)$")
    message(FATAL_ERROR "The pinned USB/IP haptics package currently supports Windows x64 only.")
endif()
