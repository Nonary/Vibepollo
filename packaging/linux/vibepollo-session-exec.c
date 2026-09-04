#define _GNU_SOURCE

#include "vibepollo-session-protocol.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <unistd.h>

static bool drop_client_capabilities(void) {
  if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0)) return false;
  cap_t empty = cap_init();
  if (!empty) return false;
  const int result = cap_set_proc(empty);
  cap_free(empty);
  return !result && !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
}

static bool restore_termination_signals(void) {
  // The host blocks SIGINT/SIGTERM for its sigwait() thread. The mask and
  // ignored shell dispositions survive exec, but this client has no signal
  // monitor. Restore ordinary termination before any blocking broker I/O so
  // stopping the host closes this connection and lets the broker drain its app.
  struct sigaction action = {.sa_handler = SIG_DFL};
  if (sigemptyset(&action.sa_mask) || sigaction(SIGTERM, &action, NULL) ||
      sigaction(SIGINT, &action, NULL) || sigaction(SIGHUP, &action, NULL)) return false;

  sigset_t signals;
  if (sigemptyset(&signals) || sigaddset(&signals, SIGTERM) ||
      sigaddset(&signals, SIGINT) || sigaddset(&signals, SIGHUP)) return false;
  return !sigprocmask(SIG_UNBLOCK, &signals, NULL);
}

static bool parse_generation(const char *value, uint64_t *generation) {
  if (!value || !value[0] || !generation) return false;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
  }
  char *end = NULL;
  errno = 0;
  const unsigned long long parsed = strtoull(value, &end, 10);
  if (errno || !end || *end || !parsed) return false;
  *generation = (uint64_t) parsed;
  return true;
}

static int fail(const char *message) {
  if (errno) fprintf(stderr, "vibepollo-session-exec: %s: %s\n", message, strerror(errno));
  else fprintf(stderr, "vibepollo-session-exec: %s\n", message);
  return 126;
}

static bool write_all(int fd, const void *buffer, size_t length) {
  const unsigned char *cursor = buffer;
  while (length) {
    const ssize_t written = write(fd, cursor, length);
    if (written > 0) {
      cursor += (size_t) written;
      length -= (size_t) written;
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

static bool peer_is_root_broker(int socket_fd) {
  struct ucred peer = {0};
  socklen_t peer_size = sizeof(peer);
  return !getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) &&
         peer_size == sizeof(peer) && peer.pid > 0 && peer.uid == 0 && peer.gid == 0;
}

static int connect_to_broker(void) {
  const int socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (socket_fd < 0) return -1;
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (strlen(VIBEPOLLO_SESSION_BROKER_SOCKET) >= sizeof(address.sun_path)) {
    close(socket_fd);
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(address.sun_path, VIBEPOLLO_SESSION_BROKER_SOCKET,
         sizeof(VIBEPOLLO_SESSION_BROKER_SOCKET));
  if (connect(socket_fd, (const struct sockaddr *) &address, sizeof(address))) {
    const int saved_errno = errno;
    close(socket_fd);
    errno = saved_errno;
    return -1;
  }
  if (!peer_is_root_broker(socket_fd)) {
    close(socket_fd);
    errno = EPERM;
    return -1;
  }
  return socket_fd;
}

static bool send_request(int socket_fd, int argc, char **argv, uint64_t generation) {
  if (argc < 2 || (uint32_t) (argc - 1) > VIBEPOLLO_SESSION_PROTOCOL_MAX_ARGUMENTS) {
    errno = E2BIG;
    return false;
  }
  size_t payload_length = 0;
  for (int index = 1; index < argc; ++index) {
    const size_t length = strlen(argv[index]) + 1;
    if (length > VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE ||
        payload_length > VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE - length) {
      errno = E2BIG;
      return false;
    }
    payload_length += length;
  }
  const size_t message_length = sizeof(struct vibepollo_session_message) + payload_length;
  if (message_length > VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE) {
    errno = E2BIG;
    return false;
  }
  unsigned char *message = calloc(1, message_length);
  if (!message) return false;
  struct vibepollo_session_message header = {
    .magic = VIBEPOLLO_SESSION_PROTOCOL_MAGIC,
    .version = VIBEPOLLO_SESSION_PROTOCOL_VERSION,
    .type = VIBEPOLLO_SESSION_REQUEST,
    .payload_length = (uint32_t) payload_length,
    .argument_count = (uint32_t) (argc - 1),
    .generation = generation,
  };
  memcpy(message, &header, sizeof(header));
  size_t offset = sizeof(header);
  for (int index = 1; index < argc; ++index) {
    const size_t length = strlen(argv[index]) + 1;
    memcpy(message + offset, argv[index], length);
    offset += length;
  }
  ssize_t sent;
  do {
    sent = send(socket_fd, message, message_length, MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  free(message);
  if (sent != (ssize_t) message_length) {
    if (sent >= 0) errno = EIO;
    return false;
  }
  return true;
}

static int relay_responses(int socket_fd, uint64_t generation) {
  unsigned char packet[sizeof(struct vibepollo_session_message) +
                       VIBEPOLLO_SESSION_PROTOCOL_OUTPUT_CHUNK];
  for (;;) {
    struct iovec vector = {.iov_base = packet, .iov_len = sizeof(packet)};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    ssize_t received;
    do {
      received = recvmsg(socket_fd, &message, 0);
    } while (received < 0 && errno == EINTR);
    if (!received) {
      errno = ECONNRESET;
      return fail("broker disconnected before reporting completion");
    }
    if (received < 0) return fail("could not receive broker response");
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        received < (ssize_t) sizeof(struct vibepollo_session_message)) {
      errno = EPROTO;
      return fail("invalid broker response");
    }
    struct vibepollo_session_message header;
    memcpy(&header, packet, sizeof(header));
    const size_t payload_length = (size_t) received - sizeof(header);
    if (header.magic != VIBEPOLLO_SESSION_PROTOCOL_MAGIC ||
        header.version != VIBEPOLLO_SESSION_PROTOCOL_VERSION ||
        header.generation != generation || header.reserved ||
        header.argument_count || header.payload_length != payload_length) {
      errno = EPROTO;
      return fail("invalid broker response");
    }
    if (header.type == VIBEPOLLO_SESSION_STDOUT ||
        header.type == VIBEPOLLO_SESSION_STDERR) {
      if (!payload_length || payload_length > VIBEPOLLO_SESSION_PROTOCOL_OUTPUT_CHUNK ||
          header.status) {
        errno = EPROTO;
        return fail("invalid broker output frame");
      }
      if (!write_all(header.type == VIBEPOLLO_SESSION_STDOUT ? STDOUT_FILENO : STDERR_FILENO,
                     packet + sizeof(header), payload_length)) {
        return fail("could not forward broker output");
      }
    } else if (header.type == VIBEPOLLO_SESSION_EXIT) {
      if (payload_length || header.status < 0 || header.status > 255) {
        errno = EPROTO;
        return fail("invalid broker exit frame");
      }
      return header.status;
    } else {
      errno = EPROTO;
      return fail("unknown broker response");
    }
  }
}

int main(int argc, char **argv) {
  uint64_t generation = 0;
  if (!drop_client_capabilities()) return fail("could not discard inherited capabilities");
  if (!restore_termination_signals()) return fail("could not restore termination signals");
  if (argc < 2) return 2;
  if (!parse_generation(getenv("VIBEPOLLO_SESSION_GENERATION"), &generation)) {
    errno = 0;
    return fail("missing or invalid VIBEPOLLO_SESSION_GENERATION");
  }
  const int socket_fd = connect_to_broker();
  if (socket_fd < 0) return fail("could not connect to the session broker");
  if (!send_request(socket_fd, argc, argv, generation)) {
    const int saved_errno = errno;
    close(socket_fd);
    errno = saved_errno;
    return fail("could not send request to the session broker");
  }
  const int status = relay_responses(socket_fd, generation);
  close(socket_fd);
  return status;
}
