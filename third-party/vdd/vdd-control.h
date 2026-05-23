#pragma once

#include <Windows.h>
#include <winioctl.h>
#include <stdint.h>

// VDD-native temporary-display control interface.
// Device interface: {bc4e6328-82d7-4844-9455-0b1b708f498f}
// API namespace:    {710ecd21-e23c-44c3-8cf3-d20276561d59}
#ifdef VDD_CONTROL_DEFINE_GUIDS
EXTERN_C const GUID GUID_DEVINTERFACE_VDD_CONTROL =
{ 0xbc4e6328, 0x82d7, 0x4844, { 0x94, 0x55, 0x0b, 0x1b, 0x70, 0x8f, 0x49, 0x8f } };

EXTERN_C const GUID GUID_VDD_CONTROL_API_NAMESPACE =
{ 0x710ecd21, 0xe23c, 0x44c3, { 0x8c, 0xf3, 0xd2, 0x02, 0x76, 0x56, 0x1d, 0x59 } };
#else
EXTERN_C const GUID GUID_DEVINTERFACE_VDD_CONTROL;
EXTERN_C const GUID GUID_VDD_CONTROL_API_NAMESPACE;
#endif

#define VDD_CONTROL_PROTOCOL_VERSION_MAJOR 1
#define VDD_CONTROL_PROTOCOL_VERSION_MINOR 3
#define VDD_CONTROL_PROTOCOL_VERSION_PATCH 0

#define VDD_TEMPORARY_DISPLAY_NAME_CHARS 32u

#define VDD_TEMPORARY_DISPLAY_DEFAULT_TIMEOUT_MS 10000u
#define VDD_TEMPORARY_DISPLAY_MIN_TIMEOUT_MS      3000u
#define VDD_TEMPORARY_DISPLAY_MAX_TIMEOUT_MS    300000u

#define IOCTL_VDD_GET_PROTOCOL_VERSION      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VDD_CREATE_TEMPORARY_DISPLAY  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_VDD_REMOVE_TEMPORARY_DISPLAY  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_VDD_FEED_LEASE                CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_VDD_RELEASE_LEASE             CTL_CODE(FILE_DEVICE_UNKNOWN, 0x904, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_VDD_QUERY_LEASE               CTL_CODE(FILE_DEVICE_UNKNOWN, 0x905, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_VDD_SET_STATIC_MONITOR_COUNT  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x906, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_VDD_QUERY_STATIC_MONITOR_COUNT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x907, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

typedef struct VDD_PROTOCOL_VERSION
{
    GUID ApiNamespace;
    uint16_t Major;
    uint16_t Minor;
    uint16_t Patch;
    uint16_t Reserved;
} VDD_PROTOCOL_VERSION;

typedef struct VDD_CREATE_TEMPORARY_DISPLAY
{
    GUID ApiNamespace;
    uint64_t LeaseId;
    // Durable caller-owned monitor identity. Do not recycle a DisplayId for a
    // different logical monitor; the driver persists EDID/container/connector
    // identity by this value across removals, reboots, and normal updates.
    uint64_t DisplayId;
    uint32_t Width;
    uint32_t Height;
    uint32_t RefreshRateMilliHz;
    uint32_t RequestedTimeoutMs;
    char DisplayName[VDD_TEMPORARY_DISPLAY_NAME_CHARS];
    uint32_t Flags;
    uint32_t Reserved;
} VDD_CREATE_TEMPORARY_DISPLAY;

typedef struct VDD_CREATE_TEMPORARY_DISPLAY_RESULT
{
    GUID ApiNamespace;
    uint64_t LeaseId;
    uint64_t DisplayId;
    LUID OsAdapterLuid;
    uint32_t TargetId;
    uint32_t ConnectorIndex;
    uint32_t EffectiveTimeoutMs;
    uint32_t Reserved;
} VDD_CREATE_TEMPORARY_DISPLAY_RESULT;

typedef struct VDD_LEASE_DISPLAY_REQUEST
{
    GUID ApiNamespace;
    uint64_t LeaseId;
    uint64_t DisplayId;
} VDD_LEASE_DISPLAY_REQUEST;

typedef struct VDD_LEASE_REQUEST
{
    GUID ApiNamespace;
    uint64_t LeaseId;
    uint32_t RequestedTimeoutMs;
    uint32_t Reserved;
} VDD_LEASE_REQUEST;

typedef struct VDD_QUERY_LEASE_RESULT
{
    GUID ApiNamespace;
    uint64_t LeaseId;
    uint32_t TemporaryDisplayCount;
    uint32_t EffectiveTimeoutMs;
    uint32_t RemainingMs;
    uint32_t LeaseExists;
} VDD_QUERY_LEASE_RESULT;

typedef struct VDD_STATIC_MONITOR_COUNT_REQUEST
{
    GUID ApiNamespace;
    uint32_t MonitorCount;
    uint32_t Flags;
    uint32_t Reserved[2];
} VDD_STATIC_MONITOR_COUNT_REQUEST;

typedef struct VDD_STATIC_MONITOR_COUNT_RESULT
{
    GUID ApiNamespace;
    uint32_t CurrentMonitorCount;
    uint32_t MaxMonitorCount;
    uint32_t TemporaryDisplayCount;
    uint32_t Reserved;
} VDD_STATIC_MONITOR_COUNT_RESULT;
