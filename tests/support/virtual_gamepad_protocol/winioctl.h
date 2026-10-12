#pragma once
// Windows' fixed control-code encoding used by libvirtualgamepad/protocol.h.
#define FILE_DEVICE_UNKNOWN 0x00000022
#define METHOD_BUFFERED 0
#define FILE_READ_DATA 0x0001
#define FILE_WRITE_DATA 0x0002
#define CTL_CODE(DeviceType, Function, Method, Access) (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
