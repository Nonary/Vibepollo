/* SPDX-License-Identifier: GPL-3.0-only
 * Exercise the privileged parser/transport without granting capabilities.
 */
#define main capture_helper_main
#include "packaging/linux/steamos/local/vibeshine-kms-capture.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static struct vibeshine_kms_capture_request request_for(unsigned operation) {
  struct vibeshine_kms_capture_request request = {
    .magic = VIBESHINE_KMS_CAPTURE_MAGIC,
    .version = VIBESHINE_KMS_CAPTURE_VERSION,
    .operation = operation,
  };
  return request;
}

static void request_validation(void) {
  struct vibeshine_kms_capture_request request = request_for(VIBESHINE_KMS_CAPTURE_OPEN);
  request.card_major = 226;
  assert(vibeshine_kms_capture_request_valid(&request, false));
  assert(!vibeshine_kms_capture_request_valid(&request, true));
  request.crtc_id = 1;
  assert(!vibeshine_kms_capture_request_valid(&request, false));
  request = request_for(VIBESHINE_KMS_CAPTURE_WAIT);
  request.crtc_id = 4; request.timeout_ms = 1000;
  assert(vibeshine_kms_capture_request_valid(&request, true));
  assert(!vibeshine_kms_capture_request_valid(&request, false));
  ++request.timeout_ms;
  assert(!vibeshine_kms_capture_request_valid(&request, true));
  request.timeout_ms = 0; request.reserved[1] = 1;
  assert(!vibeshine_kms_capture_request_valid(&request, true));
  request = request_for(VIBESHINE_KMS_CAPTURE_FRAME);
  request.crtc_id = 4;
  assert(vibeshine_kms_capture_request_valid(&request, true));
  request.plane_id = 1;
  assert(!vibeshine_kms_capture_request_valid(&request, true));
  request = request_for(VIBESHINE_KMS_CAPTURE_FB);
  request.crtc_id = 4; request.plane_id = 5; request.fb_id = 6;
  assert(vibeshine_kms_capture_request_valid(&request, true));
  request.sequence = 1;
  assert(!vibeshine_kms_capture_request_valid(&request, true));
  assert(!valid_card_name("card0/../../renderD128"));
  assert(!valid_card_name("renderD128"));
  assert(open_managed_device(1, 3) == -1 && errno == EACCES);
  /* Reject every physical card present, without opening any of them. */
  DIR *cards = opendir("/dev/dri");
  if (cards) {
    struct dirent *entry;
    while ((entry = readdir(cards))) {
      struct stat card;
      if (!valid_card_name(entry->d_name) || managed_card_path(entry->d_name) ||
          fstatat(dirfd(cards), entry->d_name, &card, AT_SYMLINK_NOFOLLOW) ||
          !S_ISCHR(card.st_mode)) continue;
      assert(open_managed_device(major(card.st_rdev), minor(card.st_rdev)) == -1 && errno == EACCES);
    }
    closedir(cards);
  }
}

static void response_descriptor_transport(void) {
  int pair[2]; assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
  const pid_t child = fork(); assert(child >= 0);
  if (!child) {
    close(pair[0]);
    assert(dup2(pair[1], VIBESHINE_KMS_CAPTURE_FD) == VIBESHINE_KMS_CAPTURE_FD);
    if (pair[1] != VIBESHINE_KMS_CAPTURE_FD) close(pair[1]);
    struct vibeshine_kms_capture_response response = {
      .magic = VIBESHINE_KMS_CAPTURE_MAGIC, .version = VIBESHINE_KMS_CAPTURE_VERSION,
      .operation = VIBESHINE_KMS_CAPTURE_FRAME,
    };
    empty_frame(&response.frame);
    response.frame.plane_count = 1;
    response.frame.dma_buf_fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    response.frame.sync_file_fds[0] = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    assert(response.frame.dma_buf_fds[0] >= 0 && response.frame.sync_file_fds[0] >= 0);
    assert(send_response(&response));
    close_frame_fds(&response.frame);
    _exit(0);
  }
  close(pair[1]);
  struct vibeshine_kms_capture_response response;
  unsigned char controls[CMSG_SPACE(sizeof(int) * 8)];
  struct iovec io = {.iov_base = &response, .iov_len = sizeof(response)};
  struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = controls, .msg_controllen = sizeof(controls)};
  assert(recvmsg(pair[0], &message, MSG_CMSG_CLOEXEC) == sizeof(response));
  assert(response.fd_count == 2 && response.frame.dma_buf_fds[0] == 0 && response.frame.sync_file_fds[0] == 1);
  struct cmsghdr *c = CMSG_FIRSTHDR(&message);
  assert(c && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(2 * sizeof(int)));
  int *fds = (int *) CMSG_DATA(c);
  char byte = 9;
  assert(read(fds[0], &byte, 1) == 0);
  assert(read(fds[1], &byte, 1) == 1 && byte == 0);
  assert(fcntl(fds[0], F_GETFD) & FD_CLOEXEC);
  close(fds[0]); close(fds[1]); close(pair[0]);
  int status; assert(waitpid(child, &status, 0) == child && status == 0);
}

int main(void) {
  request_validation();
  response_descriptor_transport();
  assert(!sanitize_startup());
  puts("Capture helper request validation, physical-device refusal, fd transport, and capability refusal passed.");
  return 0;
}
