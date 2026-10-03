#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>

int vibepollo_session_exec_entrypoint(int argc, char **argv);
#define main vibepollo_session_exec_entrypoint
#include "../../../../packaging/linux/vibepollo-session-exec.c"
#undef main

#include "test_session_display_request.h"

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    return 1; \
  } \
} while (0)

static int check_client_termination(int signal_number, bool pending) {
  int peers[2];
  CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, peers));
  const pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(peers[0]);
    // Bound regressions: an inherited blocked termination signal must not
    // leave this test (or a real broker client) waiting forever.
    struct sigaction action = {.sa_handler = SIG_DFL};
    sigset_t signals;
    if (sigemptyset(&action.sa_mask) || sigaction(SIGALRM, &action, NULL) ||
        sigemptyset(&signals) || sigaddset(&signals, SIGALRM) ||
        sigprocmask(SIG_UNBLOCK, &signals, NULL)) _exit(1);
    alarm(2);

    // The host blocks SIGINT/SIGTERM for sigwait(). Shell launchers can also
    // pass ignored dispositions through exec. Exercise both at client entry.
    action.sa_handler = pending ? SIG_DFL : SIG_IGN;
    if (sigaction(signal_number, &action, NULL) || sigemptyset(&signals) ||
        sigaddset(&signals, signal_number) ||
        sigprocmask(SIG_BLOCK, &signals, NULL)) _exit(1);
    if (pending && kill(getpid(), signal_number)) _exit(1);

    char *arguments[] = {"vibepollo-session-exec", NULL};
    if (vibepollo_session_exec_entrypoint(1, arguments) != 2) _exit(1);
    if (pending) _exit(1); // A pending stop must take effect at client entry.
    if (send(peers[1], "R", 1, MSG_NOSIGNAL) != 1) _exit(1);
    _exit(relay_responses(peers[1], 42));
  }
  close(peers[1]);
  char ready = 0;
  const ssize_t received = pending ? 0 : recv(peers[0], &ready, 1, 0);
  const int sent = !pending && received == 1 ? kill(child, signal_number) : 0;
  int status = 0;
  const pid_t reaped = waitpid(child, &status, 0);
  // Termination must close the broker connection as well as empty the host
  // cgroup, so the broker can cancel the generation-bound application.
  const ssize_t disconnected = recv(peers[0], &ready, 1, MSG_DONTWAIT);
  close(peers[0]);
  CHECK(pending || (received == 1 && !sent));
  CHECK(reaped == child && WIFSIGNALED(status) && WTERMSIG(status) == signal_number);
  CHECK(disconnected == 0);
  return 0;
}

static int check_display_request_limits(void) {
  struct display_request_fixture fixture;
  CHECK(compose_eight_client_display_request(&fixture));
  CHECK(fixture.argc == 67); // program + operation + 65 composed properties
  CHECK(fixture.message_length < VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE);
  int peers[2];
  CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, peers));
  unsigned char *packet = malloc(VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE + 1);
  unsigned char *expected = malloc(VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE + 1);
  CHECK(packet && expected);
  CHECK(send_request(peers[0], fixture.argc, fixture.argv, 42));
  CHECK(recv(peers[1], packet, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE + 1, 0) ==
        (ssize_t) fixture.message_length);
  CHECK(encode_display_request(&fixture, expected, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE) == fixture.message_length);
  CHECK(!memcmp(packet, expected, fixture.message_length));
  struct vibepollo_session_message header;
  memcpy(&header, packet, sizeof(header));
  CHECK(header.argument_count == 66 && header.generation == 42);
  CHECK(header.payload_length + sizeof(header) == fixture.message_length);

  // Exercise the public client's exact argument ceiling, then one extra
  // property. No partial frame may be sent when the complete request is too big.
  while (fixture.argc < TEST_DISPLAY_MAX_PROPERTIES + 2) {
    CHECK(append_display_property(&fixture, "output.Virtual-8.enable"));
  }
  CHECK(send_request(peers[0], fixture.argc, fixture.argv, 42));
  CHECK(recv(peers[1], packet, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE + 1, 0) ==
        (ssize_t) fixture.message_length);
  memcpy(&header, packet, sizeof(header));
  CHECK(header.argument_count == TEST_DISPLAY_MAX_PROPERTIES + 1);
  CHECK(append_display_property(&fixture, "output.Virtual-8.enable"));
  errno = 0;
  CHECK(!send_request(peers[0], fixture.argc, fixture.argv, 42) && errno == E2BIG);
  CHECK(recv(peers[1], packet, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE + 1, MSG_DONTWAIT) < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK));

  // Longest identifiers and bounded restore values for all 64 accepted
  // snapshot outputs still fit the unchanged byte limit in one frame.
  CHECK(compose_maximum_restore_request(&fixture));
  CHECK(fixture.message_length < VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE);
  CHECK(send_request(peers[0], fixture.argc, fixture.argv, 42));
  CHECK(recv(peers[1], packet, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE + 1, 0) ==
        (ssize_t) fixture.message_length);
  CHECK(encode_display_request(&fixture, expected, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE) == fixture.message_length);
  CHECK(!memcmp(packet, expected, fixture.message_length));

  // Independently lock down the aggregate byte ceiling with only two wire
  // arguments, so relaxing the argument count cannot bypass this check.
  const size_t half_payload = (VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE - sizeof(header)) / 2;
  char *large_argument = malloc(half_payload + 1);
  CHECK(large_argument);
  memset(large_argument, 'A', half_payload);
  large_argument[half_payload - 1] = 0;
  char *byte_arguments[] = {"vibepollo-session-exec", large_argument, large_argument, NULL};
  CHECK(send_request(peers[0], 3, byte_arguments, 42));
  CHECK(recv(peers[1], packet, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE + 1, 0) ==
        VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE);
  large_argument[half_payload - 1] = 'A';
  large_argument[half_payload] = 0;
  byte_arguments[1] = (char *) packet + sizeof(header);
  errno = 0;
  CHECK(!send_request(peers[0], 3, byte_arguments, 42) && errno == E2BIG);
  CHECK(recv(peers[1], packet, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE + 1, MSG_DONTWAIT) < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK));
  free(large_argument);
  free(expected);
  free(packet);
  close(peers[0]);
  close(peers[1]);
  return 0;
}

int main(void) {
  CHECK(!check_display_request_limits());
  CHECK(!check_client_termination(SIGTERM, true));
  CHECK(!check_client_termination(SIGTERM, false));
  CHECK(!check_client_termination(SIGINT, false));
  CHECK(!check_client_termination(SIGHUP, false));

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

  CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, peers));
  close(peers[1]);
  CHECK(relay_responses(peers[0], 42) == VIBEPOLLO_SESSION_COMPLETION_UNKNOWN);
  close(peers[0]);

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

  puts("PASS: unprivileged session client termination, framing and capability discard");
  return 0;
}
