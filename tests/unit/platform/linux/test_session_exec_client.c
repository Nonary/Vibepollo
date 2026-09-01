#define _GNU_SOURCE

#include <stdio.h>
#include <sys/wait.h>

int vibepollo_session_exec_entrypoint(int argc, char **argv);
#define main vibepollo_session_exec_entrypoint
#include "../../../../packaging/linux/vibepollo-session-exec.c"
#undef main

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    return 1; \
  } \
} while (0)

int main(void) {
  uint64_t generation = 0;
  CHECK(!parse_generation(NULL, &generation));
  CHECK(!parse_generation("", &generation));
  CHECK(!parse_generation("0", &generation));
  CHECK(!parse_generation("+1", &generation));
  CHECK(!parse_generation(" 1", &generation));
  CHECK(!parse_generation("1x", &generation));
  CHECK(!parse_generation("184467440737095516160", &generation));
  CHECK(parse_generation("18446744073709551615", &generation));
  CHECK(generation == UINT64_MAX);

  int peers[2] = {-1, -1};
  CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, peers));
  CHECK(peer_is_root_broker(peers[0]) == (getuid() == 0 && getgid() == 0));
  char *arguments[] = {
    "vibepollo-session-exec", "audio-set-default", "safe_sink", NULL
  };
  CHECK(send_request(peers[0], 3, arguments, 42));
  unsigned char packet[512] = {0};
  const ssize_t received = recv(peers[1], packet, sizeof(packet), 0);
  CHECK(received > (ssize_t) sizeof(struct vibepollo_session_message));
  struct vibepollo_session_message header = {0};
  memcpy(&header, packet, sizeof(header));
  CHECK(header.magic == VIBEPOLLO_SESSION_PROTOCOL_MAGIC);
  CHECK(header.version == VIBEPOLLO_SESSION_PROTOCOL_VERSION);
  CHECK(header.type == VIBEPOLLO_SESSION_REQUEST);
  CHECK(header.argument_count == 2 && header.generation == 42);
  CHECK(header.payload_length == strlen("audio-set-default") + 1 + strlen("safe_sink") + 1);
  const char *first = (const char *) packet + sizeof(header);
  const char *second = first + strlen(first) + 1;
  CHECK(!strcmp(first, "audio-set-default") && !strcmp(second, "safe_sink"));

  const struct vibepollo_session_message exit_header = {
    .magic = VIBEPOLLO_SESSION_PROTOCOL_MAGIC,
    .version = VIBEPOLLO_SESSION_PROTOCOL_VERSION,
    .type = VIBEPOLLO_SESSION_EXIT,
    .generation = 42,
    .status = 7,
  };
  CHECK(send(peers[1], &exit_header, sizeof(exit_header), MSG_NOSIGNAL) ==
        (ssize_t) sizeof(exit_header));
  CHECK(relay_responses(peers[0], 42) == 7);
  close(peers[0]);
  close(peers[1]);

  const pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    if (!drop_client_capabilities() || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) _exit(1);
    cap_t current = cap_get_proc();
    cap_t empty = cap_init();
    const bool clear = current && empty && cap_compare(current, empty) == 0;
    if (current) cap_free(current);
    if (empty) cap_free(empty);
    _exit(clear ? 0 : 1);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  puts("PASS: unprivileged session client framing and capability discard");
  return 0;
}
