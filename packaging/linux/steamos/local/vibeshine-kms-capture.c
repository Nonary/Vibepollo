/* SPDX-License-Identifier: GPL-3.0-only
 * Restricted capture bridge. Only a root-owned vibeshine_drm device can be
 * opened; no request supplies a pathname, ioctl number, or caller-owned fd.
 */
#define _GNU_SOURCE
#include "src/platform/linux/kms_capture_protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define DRM_IOCTL_VIBESHINE_WAIT_PRESENT \
  DRM_IOWR(DRM_COMMAND_BASE + DRM_VIBESHINE_WAIT_PRESENT, struct vibeshine_drm_wait_present)
#define DRM_IOCTL_VIBESHINE_GET_FRAME \
  DRM_IOWR(DRM_COMMAND_BASE + DRM_VIBESHINE_GET_FRAME, struct vibeshine_drm_frame)

static bool set_effective(bool enabled) {
  cap_t desired = cap_init();
  cap_t actual = NULL;
  cap_value_t capability = CAP_SYS_ADMIN;
  bool ok = desired && !cap_set_flag(desired, CAP_PERMITTED, 1, &capability, CAP_SET) &&
            (!enabled || !cap_set_flag(desired, CAP_EFFECTIVE, 1, &capability, CAP_SET)) &&
            !cap_set_proc(desired);
  if (ok) actual = cap_get_proc();
  ok = ok && actual && cap_compare(desired, actual) == 0;
  if (actual) cap_free(actual);
  if (desired) cap_free(desired);
  if (!ok) errno = EPERM;
  return ok;
}

static bool sanitize_startup(void) {
  if (!getuid() || getuid() != geteuid() || getgid() != getegid()) return false;
  cap_t actual = cap_get_proc();
  cap_t expected = cap_init();
  cap_value_t capability = CAP_SYS_ADMIN;
  const bool valid = actual && expected &&
    !cap_set_flag(expected, CAP_PERMITTED, 1, &capability, CAP_SET) &&
    cap_compare(actual, expected) == 0;
  if (actual) cap_free(actual);
  if (expected) cap_free(expected);
  if (!valid || prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) ||
      prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
      prctl(PR_SET_DUMPABLE, 0, 0, 0, 0)) return false;
  return set_effective(false);
}

static bool valid_card_name(const char *name) {
  if (strncmp(name, "card", 4) || !name[4]) return false;
  for (size_t i = 4; name[i]; ++i) if (name[i] < '0' || name[i] > '9') return false;
  return strlen(name) < 32;
}

static bool managed_card_path(const char *card_name) {
  struct stat sysfs;
  char path[PATH_MAX], resolved[PATH_MAX];
  return snprintf(path, sizeof(path), "/sys/class/drm/%s", card_name) < (int) sizeof(path) &&
      realpath(path, resolved) &&
      !strncmp(resolved, "/sys/devices/faux/vibeshine/", strlen("/sys/devices/faux/vibeshine/")) &&
      !stat(resolved, &sysfs) && S_ISDIR(sysfs.st_mode) && !sysfs.st_uid;
}

static bool managed_device_identity(int fd, const char *card_name, dev_t device) {
  struct stat opened;
  if (fstat(fd, &opened) || !S_ISCHR(opened.st_mode) || opened.st_uid ||
      opened.st_rdev != device || !managed_card_path(card_name)) return false;
  drmVersionPtr version = drmGetVersion(fd);
  const bool valid = version && version->name && version->name_len == strlen("vibeshine_drm") &&
                     !memcmp(version->name, "vibeshine_drm", version->name_len);
  if (version) drmFreeVersion(version);
  return valid;
}

static int open_managed_device(uint32_t card_major, uint32_t card_minor) {
  const dev_t wanted = makedev(card_major, card_minor);
  if (major(wanted) != card_major || minor(wanted) != card_minor || card_major != 226) {
    errno = EACCES; return -1;
  }
  DIR *directory = opendir("/dev/dri");
  if (!directory) return -1;
  int result = -1;
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (!valid_card_name(entry->d_name) || !managed_card_path(entry->d_name)) continue;
    struct stat attributes;
    if (fstatat(dirfd(directory), entry->d_name, &attributes, AT_SYMLINK_NOFOLLOW) ||
        !S_ISCHR(attributes.st_mode) || attributes.st_uid || attributes.st_rdev != wanted) continue;
    int fd = openat(dirfd(directory), entry->d_name, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (fd < 0) continue;
    if (!managed_device_identity(fd, entry->d_name, wanted)) { close(fd); continue; }
    if (drmIsMaster(fd)) {
      if (!set_effective(true)) _exit(126);
      const int dropped = drmDropMaster(fd);
      if (!set_effective(false)) _exit(126);
      if (dropped) { close(fd); continue; }
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) { close(fd); continue; }
    result = fd;
    break;
  }
  closedir(directory);
  if (result < 0) errno = EACCES;
  return result;
}

static bool known_crtc(int fd, uint32_t crtc_id) {
  drmModeResPtr resources = drmModeGetResources(fd);
  if (!resources) return false;
  bool found = false;
  for (int i = 0; i < resources->count_crtcs; ++i)
    if (resources->crtcs[i] == crtc_id) found = true;
  drmModeFreeResources(resources);
  return found;
}

static void close_frame_fds(struct vibeshine_drm_frame *frame) {
  for (unsigned i = 0; i < VIBESHINE_DRM_FRAME_MAX_PLANES; ++i) {
    if (frame->dma_buf_fds[i] >= 0) close(frame->dma_buf_fds[i]);
    if (frame->sync_file_fds[i] >= 0) close(frame->sync_file_fds[i]);
    frame->dma_buf_fds[i] = frame->sync_file_fds[i] = -1;
  }
}

static void empty_frame(struct vibeshine_drm_frame *frame) {
  memset(frame, 0, sizeof(*frame));
  for (unsigned i = 0; i < VIBESHINE_DRM_FRAME_MAX_PLANES; ++i)
    frame->dma_buf_fds[i] = frame->sync_file_fds[i] = -1;
}

static void close_fb_handles(int fd, drmModeFB2Ptr fb) {
  for (unsigned i = 0; i < 4; ++i) {
    if (!fb->handles[i]) continue;
    bool earlier = false;
    for (unsigned j = 0; j < i; ++j) if (fb->handles[j] == fb->handles[i]) earlier = true;
    if (!earlier) {
      struct drm_gem_close close_request = {.handle = fb->handles[i]};
      (void) drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_request);
    }
  }
}

static int export_framebuffer(int fd, const struct vibeshine_kms_capture_request *request,
                              struct vibeshine_drm_frame *frame) {
  drmModePlanePtr plane = drmModeGetPlane(fd, request->plane_id);
  const bool matches = plane && plane->crtc_id == request->crtc_id &&
                       plane->fb_id == request->fb_id;
  if (plane) drmModeFreePlane(plane);
  if (!matches) return ESTALE;
  if (!set_effective(true)) _exit(126);
  drmModeFB2Ptr fb = drmModeGetFB2(fd, request->fb_id);
  int error = fb ? 0 : (errno ? errno : EIO);
  if (!set_effective(false)) _exit(126);
  if (!fb) return error;
  empty_frame(frame);
  frame->abi_version = VIBESHINE_DRM_FRAME_ABI_VERSION;
  frame->crtc_id = request->crtc_id;
  frame->flags = VIBESHINE_DRM_FRAME_READY;
  frame->width = fb->width;
  frame->height = fb->height;
  frame->fourcc = fb->pixel_format;
  frame->modifier = (fb->flags & DRM_MODE_FB_MODIFIERS) ? fb->modifier : DRM_FORMAT_MOD_INVALID;
  for (unsigned i = 0; i < 4; ++i) {
    if (!fb->handles[i]) break;
    if (drmPrimeHandleToFD(fd, fb->handles[i], DRM_CLOEXEC, &frame->dma_buf_fds[i])) {
      error = errno ? errno : EIO; break;
    }
    frame->pitches[i] = fb->pitches[i];
    frame->offsets[i] = fb->offsets[i];
    ++frame->plane_count;
  }
  if (!frame->plane_count) error = error ? error : EACCES;
  close_fb_handles(fd, fb);
  drmModeFreeFB2(fb);
  /* Reject a topology race even though the framebuffer stayed on this card. */
  plane = drmModeGetPlane(fd, request->plane_id);
  if (!plane || plane->crtc_id != request->crtc_id || plane->fb_id != request->fb_id) error = ESTALE;
  if (plane) drmModeFreePlane(plane);
  return error;
}

static bool valid_frame_response(const struct vibeshine_drm_frame *frame, uint32_t crtc) {
  if (frame->abi_version != VIBESHINE_DRM_FRAME_ABI_VERSION || frame->crtc_id != crtc ||
      frame->reserved_u32) return false;
  for (unsigned i = 0; i < 4; ++i) if (frame->reserved[i]) return false;
  if (frame->flags == VIBESHINE_DRM_FRAME_EMPTY) {
    if (frame->width || frame->height || frame->fourcc || frame->modifier || frame->plane_count) return false;
  } else if (frame->flags != VIBESHINE_DRM_FRAME_READY || !frame->width || !frame->height ||
             frame->width > 32768 || frame->height > 32768 || !frame->fourcc ||
             !frame->plane_count || frame->plane_count > 4) return false;
  for (unsigned i = 0; i < 4; ++i) {
    if (i < frame->plane_count) {
      if (frame->dma_buf_fds[i] < 0 || frame->sync_file_fds[i] < -1 || !frame->pitches[i]) return false;
    } else if (frame->dma_buf_fds[i] != -1 || frame->sync_file_fds[i] != -1 ||
               frame->pitches[i] || frame->offsets[i]) return false;
  }
  return true;
}

static int perform(int *card, const struct vibeshine_kms_capture_request *request,
                   struct vibeshine_kms_capture_response *response) {
  if (!vibeshine_kms_capture_request_valid(request, *card >= 0)) return EINVAL;
  if (request->operation == VIBESHINE_KMS_CAPTURE_OPEN) {
    *card = open_managed_device(request->card_major, request->card_minor);
    return *card < 0 ? errno : 0;
  }
  if (!known_crtc(*card, request->crtc_id)) return ENOENT;
  if (request->operation == VIBESHINE_KMS_CAPTURE_FB) {
    int error = export_framebuffer(*card, request, &response->frame);
    if (!error && !valid_frame_response(&response->frame, request->crtc_id)) error = EPROTO;
    return error;
  }
  int result, error;
  if (request->operation == VIBESHINE_KMS_CAPTURE_WAIT) {
    struct vibeshine_drm_wait_present wait = {
      .abi_version = VIBESHINE_DRM_PRESENT_ABI_VERSION,
      .crtc_id = request->crtc_id, .sequence = request->sequence, .timeout_ms = request->timeout_ms,
    };
    if (!set_effective(true)) _exit(126);
    result = ioctl(*card, DRM_IOCTL_VIBESHINE_WAIT_PRESENT, &wait);
    error = result < 0 ? errno : 0;
    if (!set_effective(false)) _exit(126);
    if (!error && (wait.abi_version != VIBESHINE_DRM_PRESENT_ABI_VERSION ||
        wait.crtc_id != request->crtc_id || wait.timeout_ms != request->timeout_ms ||
        wait.reserved[0] || wait.reserved[1] ||
        (wait.flags & ~(VIBESHINE_DRM_PRESENT_CHANGED | VIBESHINE_DRM_PRESENT_TIMEOUT | VIBESHINE_DRM_PRESENT_PENDING)))) error = EPROTO;
    if (!error) response->presentation = wait;
    return error;
  }
  struct vibeshine_drm_frame frame = {.abi_version = VIBESHINE_DRM_FRAME_ABI_VERSION, .crtc_id = request->crtc_id};
  if (!set_effective(true)) _exit(126);
  result = ioctl(*card, DRM_IOCTL_VIBESHINE_GET_FRAME, &frame);
  error = result < 0 ? errno : 0;
  if (!set_effective(false)) _exit(126);
  /* A failing ioctl owns no returned fds; the driver closes partial exports. */
  if (!error) {
    response->frame = frame;
    if (!valid_frame_response(&frame, request->crtc_id)) error = EPROTO;
  }
  return error;
}

static ssize_t receive_request(struct vibeshine_kms_capture_request *request) {
  unsigned char controls[CMSG_SPACE(sizeof(int) * VIBESHINE_KMS_CAPTURE_MAX_FDS)] = {0};
  struct iovec io = {.iov_base = request, .iov_len = sizeof(*request)};
  struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = controls, .msg_controllen = sizeof(controls)};
  ssize_t received;
  do { received = recvmsg(VIBESHINE_KMS_CAPTURE_FD, &message, MSG_CMSG_CLOEXEC); }
  while (received < 0 && errno == EINTR);
  bool ancillary = false;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
    ancillary = true;
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(0)) {
      const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      int *fds = (int *) CMSG_DATA(c);
      for (size_t i = 0; i < count; ++i) close(fds[i]);
    }
  }
  if (received == 0) return 0;
  if (received != sizeof(*request) || ancillary || message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return -1;
  return received;
}

static bool send_response(struct vibeshine_kms_capture_response *response) {
  int descriptors[VIBESHINE_KMS_CAPTURE_MAX_FDS];
  unsigned count = 0;
  struct vibeshine_kms_capture_response wire = *response;
  if (!response->error && (response->operation == VIBESHINE_KMS_CAPTURE_FRAME || response->operation == VIBESHINE_KMS_CAPTURE_FB)) {
    for (unsigned i = 0; i < 4; ++i) {
      if (response->frame.dma_buf_fds[i] >= 0) {
        wire.frame.dma_buf_fds[i] = (int) count;
        descriptors[count++] = response->frame.dma_buf_fds[i];
      }
      if (response->frame.sync_file_fds[i] >= 0) {
        wire.frame.sync_file_fds[i] = (int) count;
        descriptors[count++] = response->frame.sync_file_fds[i];
      }
    }
  } else {
    memset(&wire.frame, 0, sizeof(wire.frame));
    if (response->error) memset(&wire.presentation, 0, sizeof(wire.presentation));
  }
  wire.fd_count = count;
  unsigned char controls[CMSG_SPACE(sizeof(descriptors))] = {0};
  struct iovec io = {.iov_base = &wire, .iov_len = sizeof(wire)};
  struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1};
  if (count) {
    message.msg_control = controls;
    message.msg_controllen = CMSG_SPACE(sizeof(int) * count);
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * count);
    memcpy(CMSG_DATA(c), descriptors, sizeof(int) * count);
  }
  ssize_t sent;
  do { sent = sendmsg(VIBESHINE_KMS_CAPTURE_FD, &message, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
  return sent == sizeof(wire);
}

int main(void) {
  if (!sanitize_startup()) { fputs("vibeshine-kms-capture: unsafe capability context\n", stderr); return 126; }
  int type = 0, domain = 0;
  socklen_t length = sizeof(type);
  socklen_t domain_length = sizeof(domain);
  struct ucred peer = {0};
  socklen_t peer_length = sizeof(peer);
  if (getsockopt(VIBESHINE_KMS_CAPTURE_FD, SOL_SOCKET, SO_TYPE, &type, &length) ||
      type != SOCK_SEQPACKET ||
      getsockopt(VIBESHINE_KMS_CAPTURE_FD, SOL_SOCKET, SO_DOMAIN, &domain, &domain_length) ||
      domain != AF_UNIX ||
      getsockopt(VIBESHINE_KMS_CAPTURE_FD, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) ||
      peer_length != sizeof(peer) || peer.uid != getuid() || peer.gid != getgid() || peer.pid <= 0 ||
      close_range(VIBESHINE_KMS_CAPTURE_FD + 1, UINT_MAX, 0)) return 126;
  /* An abandoned launcher must not leave an unbound helper waiting forever. */
  struct pollfd initial_request = {.fd = VIBESHINE_KMS_CAPTURE_FD, .events = POLLIN};
  int ready;
  do { ready = poll(&initial_request, 1, 5000); } while (ready < 0 && errno == EINTR);
  if (ready <= 0 || !(initial_request.revents & POLLIN)) return 126;
  int card = -1;
  for (;;) {
    struct vibeshine_kms_capture_request request = {0};
    ssize_t received = receive_request(&request);
    if (!received) break;
    if (received < 0) { if (card >= 0) close(card); return 126; }
    struct vibeshine_kms_capture_response response = {
      .magic = VIBESHINE_KMS_CAPTURE_MAGIC, .version = VIBESHINE_KMS_CAPTURE_VERSION,
      .operation = request.operation,
    };
    empty_frame(&response.frame);
    response.error = perform(&card, &request, &response);
    const bool sent = send_response(&response);
    close_frame_fds(&response.frame);
    if (!sent || response.error == EINVAL || (request.operation == VIBESHINE_KMS_CAPTURE_OPEN && response.error)) break;
  }
  if (card >= 0) close(card);
  return 0;
}
