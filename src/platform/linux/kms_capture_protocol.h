/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef VIBESHINE_KMS_CAPTURE_PROTOCOL_H
#define VIBESHINE_KMS_CAPTURE_PROTOCOL_H

#include <stdint.h>
#include <vibeshine_drm_uapi.h>

#define VIBESHINE_KMS_CAPTURE_MAGIC UINT32_C(0x564b4350)
#define VIBESHINE_KMS_CAPTURE_VERSION UINT16_C(1)
#define VIBESHINE_KMS_CAPTURE_MAX_FDS 8
#define VIBESHINE_KMS_CAPTURE_FD 3
#define VIBESHINE_KMS_CAPTURE_HELPER "/opt/vibeshine-private-display/current/bin/vibeshine-kms-capture"

enum vibeshine_kms_capture_operation {
  VIBESHINE_KMS_CAPTURE_OPEN = 1,
  VIBESHINE_KMS_CAPTURE_FB = 2,
  VIBESHINE_KMS_CAPTURE_WAIT = 3,
  VIBESHINE_KMS_CAPTURE_FRAME = 4,
};

/* Every unused field must be zero. OPEN binds one immutable DRM device. */
struct vibeshine_kms_capture_request {
  uint32_t magic;
  uint16_t version;
  uint16_t operation;
  uint32_t card_major;
  uint32_t card_minor;
  uint32_t crtc_id;
  uint32_t plane_id;
  uint32_t fb_id;
  uint32_t timeout_ms;
  uint64_t sequence;
  uint64_t reserved[2];
};

/* The descriptor's fd arrays contain SCM_RIGHTS indices, never sender fds.
 * FB uses frame for framebuffer geometry and DMA-BUF planes; sequence/time
 * are zero. WAIT uses presentation; OPEN and error responses have no payload.
 */
struct vibeshine_kms_capture_response {
  uint32_t magic;
  uint16_t version;
  uint16_t operation;
  int32_t error;
  uint32_t fd_count;
  struct vibeshine_drm_frame frame;
  struct vibeshine_drm_wait_present presentation;
};

#ifdef __cplusplus
static_assert(sizeof(vibeshine_kms_capture_request) == 56);
static_assert(sizeof(vibeshine_kms_capture_response) == 216);
#else
_Static_assert(sizeof(struct vibeshine_kms_capture_request) == 56, "fixed request ABI");
_Static_assert(sizeof(struct vibeshine_kms_capture_response) == 216, "fixed response ABI");
#endif

/* Shared parser also supports malformed-request tests without capabilities. */
static inline int vibeshine_kms_capture_request_valid(const struct vibeshine_kms_capture_request *r,
                                                     int device_open) {
  if (!r || r->magic != VIBESHINE_KMS_CAPTURE_MAGIC ||
      r->version != VIBESHINE_KMS_CAPTURE_VERSION || r->reserved[0] || r->reserved[1]) return 0;
  if (r->operation == VIBESHINE_KMS_CAPTURE_OPEN) {
    return !device_open && r->card_major && !r->crtc_id && !r->plane_id &&
           !r->fb_id && !r->timeout_ms && !r->sequence;
  }
  if (!device_open || r->card_major || r->card_minor || !r->crtc_id) return 0;
  if (r->operation == VIBESHINE_KMS_CAPTURE_FB)
    return r->plane_id && r->fb_id && !r->timeout_ms && !r->sequence;
  if (r->operation == VIBESHINE_KMS_CAPTURE_WAIT)
    return !r->plane_id && !r->fb_id && r->timeout_ms <= VIBESHINE_DRM_PRESENT_MAX_TIMEOUT_MS;
  if (r->operation == VIBESHINE_KMS_CAPTURE_FRAME)
    return !r->plane_id && !r->fb_id && !r->timeout_ms && !r->sequence;
  return 0;
}

#endif
