// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#ifdef AURORA_GPU_HEVC_EXPORTS
#define AGH_API __declspec(dllexport)
#else
#define AGH_API
#endif
#else
#define AGH_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
// Development ABI, not an advertised Apollo capability. Calls for a session and
// its D3D11 immediate context must be serialized. No internal frame queue.
#define AGH_ABI_VERSION 3u
typedef struct agh_session agh_session;
typedef struct agh_config {
    uint32_t size,abi_version,width,height,bit_depth,hdr;
    void* d3d11_device; // ID3D11Device*, retained until agh_close.
    const char* kernel_directory;
    uint32_t bitrate_kbps,framerate; // both zero: fixed-QP fixtures only
} agh_config;
typedef struct agh_packet {
    uint32_t size,is_idr;
    const uint8_t* data; // Owned by session; invalidated by next encode or close.
    size_t bytes;
    uint64_t frame_id;
    double submit_to_packet_ms;
    uint32_t qp,encode_passes;
} agh_packet;
AGH_API agh_session* agh_open(const agh_config*,char* error,size_t error_capacity);
// Input is a BGRA8/RGBA8 or scRGB float16 texture on the supplied D3D11 device.
// It must match the coded dimensions. Caller owns capture/mutex synchronization.
// The function copies the texture on the GPU, converts it, and encodes it.
// NULL input is deliberately rejected; no stale-picture or CPU fallback.
// First submission is IDR. force_idr handles an explicit recovery request.
// frame_id is returned unchanged; POC advances per encoded picture independently.
AGH_API int agh_encode(agh_session*,void* d3d11_texture,uint64_t frame_id,uint32_t force_idr,
                       agh_packet*,char* error,size_t error_capacity);
AGH_API void agh_close(agh_session*);
#ifdef __cplusplus
}
#endif
