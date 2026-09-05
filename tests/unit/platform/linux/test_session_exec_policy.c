#define _GNU_SOURCE

#include <stdio.h>

int vibepollo_session_broker_entrypoint(int argc, char **argv);
#define main vibepollo_session_broker_entrypoint
#include "../../../../packaging/linux/vibepollo-session-broker.c"
#undef main

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    return 1; \
  } \
} while (0)

int main(void) {
  CHECK(artwork_request_is_safe("provider-steam-artwork:42", "provider-steam-artwork:", UINT32_MAX));
  CHECK(!artwork_request_is_safe("provider-steam-artwork:0", "provider-steam-artwork:", UINT32_MAX));
  CHECK(!artwork_request_is_safe("provider-steam-artwork:4294967296", "provider-steam-artwork:", UINT32_MAX));
  CHECK(!artwork_request_is_safe("provider-steam-artwork:42/../../etc/passwd", "provider-steam-artwork:", UINT32_MAX));
  CHECK(!artwork_request_is_safe("provider-lutris-artwork:42", "provider-steam-artwork:", UINT32_MAX));
  unsigned long number = 0;
  CHECK(!parse_number(NULL, 1, 10, &number));
  CHECK(!parse_number("", 1, 10, &number));
  CHECK(!parse_number("0", 1, 10, &number));
  CHECK(!parse_number("11", 1, 10, &number));
  CHECK(!parse_number("1x", 1, 10, &number));
  CHECK(!parse_number("+1", 0, 10, &number));
  CHECK(!parse_number("-0", 0, 10, &number));
  CHECK(!parse_number(" 1", 0, 10, &number));
  CHECK(!parse_number("1 ", 0, 10, &number));
  CHECK(!parse_number("184467440737095516160", 0, ULONG_MAX, &number));
  CHECK(parse_number("0", 0, 10, &number) && number == 0);
  CHECK(parse_number("10", 1, 10, &number) && number == 10);

  char *valid_steam_direct[] = {
    "vibeshine-session-broker", "steam-direct", "1182900",
    "mangohud-proton", "116000", "3", "1", "late", "0", "0", NULL
  };
  CHECK(steam_direct_arguments_are_safe(10, valid_steam_direct));
  valid_steam_direct[3] = "proton";
  valid_steam_direct[5] = "custom";
  valid_steam_direct[6] = "0";
  CHECK(steam_direct_arguments_are_safe(10, valid_steam_direct));
  valid_steam_direct[3] = "disabled";
  valid_steam_direct[4] = "0";
  valid_steam_direct[8] = "1";
  valid_steam_direct[9] = "1";
  CHECK(steam_direct_arguments_are_safe(10, valid_steam_direct));
  valid_steam_direct[9] = "2";
  CHECK(!steam_direct_arguments_are_safe(10, valid_steam_direct));
  valid_steam_direct[9] = "0";
  valid_steam_direct[8] = "0";
  CHECK(!steam_direct_arguments_are_safe(10, valid_steam_direct));
  valid_steam_direct[3] = "proton";
  valid_steam_direct[4] = "116000";
  valid_steam_direct[6] = "1";
  CHECK(!steam_direct_arguments_are_safe(10, valid_steam_direct));
  valid_steam_direct[6] = "0";
  valid_steam_direct[4] = "116.0";
  CHECK(!steam_direct_arguments_are_safe(10, valid_steam_direct));
  valid_steam_direct[4] = "116000";
  valid_steam_direct[2] = "0";
  CHECK(!steam_direct_arguments_are_safe(10, valid_steam_direct));

  CHECK(xauthority_mode_is_safe(0600));
  CHECK(xauthority_mode_is_safe(0400));
  CHECK(!xauthority_mode_is_safe(0620));
  CHECK(!xauthority_mode_is_safe(0602));
  CHECK(!xauthority_mode_is_safe(0644));

  struct session_identity xauthority_identity = {0};
  CHECK(snprintf(xauthority_identity.runtime, sizeof(xauthority_identity.runtime),
                 "/run/user/1000") > 0);
  CHECK(snprintf(xauthority_identity.xauthority, sizeof(xauthority_identity.xauthority),
                 "/run/user/1000/Xauthority") > 0);
  CHECK(xauthority_path_is_confined(&xauthority_identity));
  CHECK(snprintf(xauthority_identity.xauthority, sizeof(xauthority_identity.xauthority),
                 "/run/user/1000/../1001/Xauthority") > 0);
  CHECK(!xauthority_path_is_confined(&xauthority_identity));
  CHECK(!validate_xauthority(&xauthority_identity));
  CHECK(snprintf(xauthority_identity.xauthority, sizeof(xauthority_identity.xauthority),
                 "/run/user/1000/cache/..") > 0);
  CHECK(!xauthority_path_is_confined(&xauthority_identity));
  CHECK(!validate_xauthority(&xauthority_identity));

  CHECK(runtime_mode_is_safe(0700));
  CHECK(!runtime_mode_is_safe(0710));
  CHECK(!runtime_mode_is_safe(0770));
  CHECK(!runtime_mode_is_safe(0701));
  CHECK(!runtime_mode_is_safe(0600));

  char shell_word[PATH_MAX] = {0};
  char working_directory[PATH_MAX] = {0};
  CHECK(parse_first_shell_word("  /usr/bin/game --flag", shell_word, sizeof(shell_word)));
  CHECK(!strcmp(shell_word, "/usr/bin/game"));
  CHECK(parse_first_shell_word("'/opt/Game Folder'/bin/game --flag", shell_word, sizeof(shell_word)));
  CHECK(!strcmp(shell_word, "/opt/Game Folder/bin/game"));
  CHECK(parse_first_shell_word("/opt/Game\\ Folder/bin/game --flag", shell_word, sizeof(shell_word)));
  CHECK(!strcmp(shell_word, "/opt/Game Folder/bin/game"));
  CHECK(parse_first_shell_word("\"/opt/a\\q/bin/game\" --flag", shell_word, sizeof(shell_word)));
  CHECK(!strcmp(shell_word, "/opt/a\\q/bin/game"));
  CHECK(parse_first_shell_word("\"$HOME/bin/game\" --flag", shell_word, sizeof(shell_word)));
  CHECK(!strcmp(shell_word, "$HOME/bin/game"));
  CHECK(!parse_first_shell_word("'/opt/game", shell_word, sizeof(shell_word)));
  CHECK(!parse_first_shell_word("/opt/game\\", shell_word, sizeof(shell_word)));
  CHECK(!parse_first_shell_word("\"\" --flag", shell_word, sizeof(shell_word)));
  CHECK(!parse_first_shell_word("abcd", shell_word, 4));

  CHECK(executable_parent_directory("\"/opt/Game Folder/bin/game\" --flag",
                                    working_directory, sizeof(working_directory)));
  CHECK(!strcmp(working_directory, "/opt/Game Folder/bin"));
  CHECK(executable_parent_directory("systemd-run --version",
                                    working_directory, sizeof(working_directory)));
  CHECK(!strcmp(working_directory, "/usr/local/bin") ||
        !strcmp(working_directory, "/usr/bin") || !strcmp(working_directory, "/bin"));
  CHECK(!executable_parent_directory("https://example.invalid/game", working_directory,
                                     sizeof(working_directory)) && !working_directory[0]);
  CHECK(!executable_parent_directory("definitely-not-a-vibepollo-executable", working_directory,
                                     sizeof(working_directory)) && !working_directory[0]);
  CHECK(!executable_parent_directory("./relative-game", working_directory,
                                     sizeof(working_directory)) && !working_directory[0]);

  CHECK(wait_status_exit_code(0) == 0);
  CHECK(wait_status_exit_code(7 << 8) == 7);
  CHECK(wait_status_exit_code(SIGTERM) == 128 + SIGTERM);

  char command_output[32] = {0};
  char *const print_arguments[] = {"printf", "inactive\n", NULL};
  CHECK(run_command_bounded("/usr/bin/printf", print_arguments, 500,
                            command_output, sizeof(command_output)) == 0);
  CHECK(application_unit_name_is_safe("vibepollo-app-7-1.service"));
  CHECK(steam_launch_retry_is_safe(126, true, 0, 2));
  CHECK(!steam_launch_retry_is_safe(126, false, 0, 2));
  CHECK(!steam_launch_retry_is_safe(126, true, 1, 2));
  CHECK(!steam_launch_retry_is_safe(126, true, 0, 1));
  CHECK(!steam_launch_retry_is_safe(0, true, 0, 2));
  termination_signal = SIGTERM;
  CHECK(!steam_launch_retry_is_safe(126, true, 0, 2));
  // Model cancellation after retry admission, during its delay: the next
  // supervision must not clear the signal or start a new systemd-run worker.
  bool cancelled_cleanup = true;
  char *const cancelled_arguments[] = {NULL};
  CHECK(supervise_user_service("cancelled-retry", cancelled_arguments,
                               &cancelled_cleanup) == 128 + SIGTERM);
  CHECK(!cancelled_cleanup);
  termination_signal = 0;
  CHECK(application_unit_name_is_safe("vibepollo-app-7-1-2.service"));
  CHECK(!application_unit_name_is_safe("vibepollo-app-7.service"));
  CHECK(!application_unit_name_is_safe("vibepollo-app-7-1-x.service"));
  CHECK(!application_unit_name_is_safe("vibepollo-app-7-1.service.extra"));
  CHECK(!application_unit_name_is_safe("--all"));
  CHECK(application_cgroup_path_is_safe(
          "/user.slice/user-1000.slice/user@1000.service/app.slice/vibepollo-app-7-1.service"));
  CHECK(!application_cgroup_path_is_safe("/user.slice/../system.slice"));
  CHECK(!application_cgroup_path_is_safe("/user.slice//app.slice"));
  CHECK(unit_state_is_quiescent(
          "LoadState=not-found\nMainPID=0\nControlGroup=\n", "/sys/fs/cgroup"));
  CHECK(!unit_state_is_quiescent(
          "LoadState=not-found\nMainPID=1\nControlGroup=\n", "/sys/fs/cgroup"));
  CHECK(!unit_state_is_quiescent(
          "LoadState=loaded\nMainPID=0\nControlGroup=\n", "/sys/fs/cgroup"));
  CHECK(!stop_user_service_using("/usr/bin/true", "--all", 100));

  char cgroup_root[] = "/tmp/vibepollo-broker-cgroup.XXXXXX";
  CHECK(mkdtemp(cgroup_root));
  char test_cgroup[PATH_MAX] = {0}, test_events[PATH_MAX] = {0};
  CHECK(snprintf(test_cgroup, sizeof(test_cgroup), "%s/test.service", cgroup_root) > 0);
  CHECK(!mkdir(test_cgroup, 0700));
  CHECK(snprintf(test_events, sizeof(test_events), "%s/cgroup.events", test_cgroup) > 0);
  int events_fd = open(test_events, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CHECK(events_fd >= 0);
  static const char unpopulated_events[] = "populated 0\nfrozen 0\n";
  CHECK(write(events_fd, unpopulated_events, sizeof(unpopulated_events) - 1) ==
        (ssize_t) sizeof(unpopulated_events) - 1);
  close(events_fd);
  CHECK(unit_state_is_quiescent(
          "LoadState=loaded\nMainPID=0\nControlGroup=/test.service\n", cgroup_root));
  events_fd = open(test_events, O_WRONLY | O_TRUNC | O_CLOEXEC);
  CHECK(events_fd >= 0);
  static const char populated_events[] = "populated 1\nfrozen 0\n";
  CHECK(write(events_fd, populated_events, sizeof(populated_events) - 1) ==
        (ssize_t) sizeof(populated_events) - 1);
  close(events_fd);
  CHECK(!unit_state_is_quiescent(
          "LoadState=loaded\nMainPID=0\nControlGroup=/test.service\n", cgroup_root));
  CHECK(!unlink(test_events));
  CHECK(!rmdir(test_cgroup));
  CHECK(!rmdir(cgroup_root));

  int watchdog_pipe[2] = {-1, -1};
  CHECK(make_watchdog_pipe(watchdog_pipe));
  CHECK((fcntl(watchdog_pipe[0], F_GETFD) & FD_CLOEXEC) != 0);
  CHECK((fcntl(watchdog_pipe[1], F_GETFD) & FD_CLOEXEC) != 0);
  CHECK((fcntl(watchdog_pipe[0], F_GETFL) & O_ACCMODE) == O_RDONLY);
  CHECK((fcntl(watchdog_pipe[1], F_GETFL) & O_ACCMODE) == O_WRONLY);
  close(watchdog_pipe[0]);
  close(watchdog_pipe[1]);

  char *const sleep_arguments[] = {"sleep", "5", NULL};
  uint64_t bounded_started = monotonic_milliseconds();
  CHECK(bounded_started);
  CHECK(run_command_bounded("/usr/bin/sleep", sleep_arguments, 100, NULL, 0) ==
        BOUNDED_COMMAND_TIMEOUT);
  const uint64_t bounded_finished = monotonic_milliseconds();
  CHECK(bounded_finished >= bounded_started && bounded_finished - bounded_started < 1000);
  int reaped_status = 0;
  errno = 0;
  CHECK(waitpid(-1, &reaped_status, WNOHANG) < 0 && errno == ECHILD);

  int child_ready[2] = {-1, -1};
  CHECK(!pipe2(child_ready, O_CLOEXEC));
  const pid_t stubborn_child = fork();
  CHECK(stubborn_child >= 0);
  if (!stubborn_child) {
    close(child_ready[0]);
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    if (sigemptyset(&ignore.sa_mask) || sigaction(SIGTERM, &ignore, NULL) ||
        write(child_ready[1], "x", 1) != 1) _exit(1);
    for (;;) pause();
  }
  close(child_ready[1]);
  char ready_byte = 0;
  CHECK(read(child_ready[0], &ready_byte, 1) == 1 && ready_byte == 'x');
  close(child_ready[0]);
  int stubborn_status = 0;
  bounded_started = monotonic_milliseconds();
  CHECK(terminate_and_reap_child(stubborn_child, SIGTERM, 50, &stubborn_status));
  CHECK(WIFSIGNALED(stubborn_status) && WTERMSIG(stubborn_status) == SIGKILL);
  CHECK(monotonic_milliseconds() - bounded_started < 1000);
  errno = 0;
  CHECK(waitpid(stubborn_child, &stubborn_status, WNOHANG) < 0 && errno == ECHILD);

  CHECK(valid_xdisplay(":0"));
  CHECK(valid_xdisplay(":12.3"));
  CHECK(!valid_xdisplay("0"));
  CHECK(!valid_xdisplay(":x"));
  CHECK(!valid_xdisplay(":0.bad"));

  const char *valid_display_arguments[] = {
    "output.Virtual-1.enable",
    "output.HDMI-A-1.disable",
    "output.Virtual-2.mode.123",
    "output.Virtual-2.vrrpolicy.always",
    "output.Virtual-2.hdr.enable",
    "output.Virtual-2.hdr.disable",
    "output.Virtual-2.scale.1.250000",
    "output.Virtual-2.position.-3840,0",
    "output.Virtual-2.priority.2",
    "output.Virtual-2.addCustomMode.3840.2160.120000.reduced",
  };
  for (size_t index = 0; index < sizeof(valid_display_arguments) / sizeof(valid_display_arguments[0]); ++index) {
    CHECK(display_argument_is_safe(valid_display_arguments[index]));
  }

  const char *invalid_display_arguments[] = {
    "output.Virtual-1.enable;touch /tmp/x",
    "output.Virtual-1.scale.-1",
    "output.Virtual-1.mode../../bin/sh",
    "output.Virtual-1.addCustomMode.0.2160.120000.reduced",
    "output.Virtual-1.addCustomMode.3840.2160.0.reduced",
    "output.Virtual-1.position.0,0\noutput.Virtual-2.enable",
    "--help",
  };
  for (size_t index = 0; index < sizeof(invalid_display_arguments) / sizeof(invalid_display_arguments[0]); ++index) {
    CHECK(!display_argument_is_safe(invalid_display_arguments[index]));
  }

  CHECK(sink_name_is_safe("alsa_output.pci-0000_01_00.1.hdmi-stereo@DEFAULT@"));
  CHECK(!sink_name_is_safe("sink name"));
  CHECK(!sink_name_is_safe("sink;command"));
  CHECK(!sink_name_is_safe(""));

  unsigned char mapping[8] = {0};
  char formatted_mapping[192] = {0};
  CHECK(parse_channel_mapping("0,1", 2, mapping));
  CHECK(mapping[0] == 0 && mapping[1] == 1);
  CHECK(format_channel_mapping(mapping, 2, "channel_map=", formatted_mapping, sizeof(formatted_mapping)));
  CHECK(!strcmp(formatted_mapping, "channel_map=front-left,front-right"));

  CHECK(parse_channel_mapping("1,0,2,3,5,4", 6, mapping));
  CHECK(format_channel_mapping(mapping, 6, "--channel-map=", formatted_mapping, sizeof(formatted_mapping)));
  CHECK(!strcmp(formatted_mapping,
                "--channel-map=front-right,front-left,front-center,lfe,rear-right,rear-left"));
  CHECK(parse_channel_mapping("0,0", 2, mapping));
  CHECK(format_channel_mapping(mapping, 2, "channel_map=", formatted_mapping, sizeof(formatted_mapping)));
  CHECK(!strcmp(formatted_mapping, "channel_map=front-left,front-left"));
  CHECK(!parse_channel_mapping(NULL, 2, mapping));
  CHECK(!parse_channel_mapping("0", 0, mapping));
  CHECK(!parse_channel_mapping("0,1,2,3,4,5,6,7,0", 9, mapping));
  CHECK(!parse_channel_mapping("", 2, mapping));
  CHECK(!parse_channel_mapping("0", 2, mapping));
  CHECK(!parse_channel_mapping("0,1,0", 2, mapping));
  CHECK(!parse_channel_mapping("0,,1", 2, mapping));
  CHECK(!parse_channel_mapping("0,", 2, mapping));
  CHECK(!parse_channel_mapping("-1,0", 2, mapping));
  CHECK(!parse_channel_mapping("0,2", 2, mapping));
  CHECK(!parse_channel_mapping("0,1,2,3,4,6", 6, mapping));
  CHECK(!parse_channel_mapping("0,1,2,3,4,5,6,8", 8, mapping));
  CHECK(!parse_channel_mapping("0,x", 2, mapping));
  CHECK(layout_channel_count("stereo") == 2);
  CHECK(layout_channel_count("surround51") == 6);
  CHECK(layout_channel_count("surround71") == 8);
  CHECK(layout_channel_count("unknown") == 0);
  CHECK(layout_channel_count(NULL) == 0);

  unsigned char request_packet[256] = {0};
  const char request_payload[] = "display-query";
  struct vibepollo_session_message request_header = {
    .magic = VIBEPOLLO_SESSION_PROTOCOL_MAGIC,
    .version = VIBEPOLLO_SESSION_PROTOCOL_VERSION,
    .type = VIBEPOLLO_SESSION_REQUEST,
    .payload_length = sizeof(request_payload),
    .argument_count = 1,
    .generation = 7,
  };
  memcpy(request_packet, &request_header, sizeof(request_header));
  memcpy(request_packet + sizeof(request_header), request_payload, sizeof(request_payload));
  struct decoded_request decoded = {0};
  const size_t request_length = sizeof(request_header) + sizeof(request_payload);
  CHECK(decode_request(request_packet, request_length, &decoded));
  CHECK(decoded.argc == 2 && !strcmp(decoded.argv[1], "display-query") && !decoded.argv[2]);
  CHECK(decoded.header.generation == 7);

  struct vibepollo_session_message *mutable_header =
    (struct vibepollo_session_message *) request_packet;
#define REJECT_HEADER(field, value) do { \
  const __typeof__(mutable_header->field) saved = mutable_header->field; \
  mutable_header->field = (value); \
  CHECK(!decode_request(request_packet, request_length, &decoded)); \
  mutable_header->field = saved; \
} while (0)
  REJECT_HEADER(magic, 0);
  REJECT_HEADER(version, VIBEPOLLO_SESSION_PROTOCOL_VERSION + 1);
  REJECT_HEADER(type, VIBEPOLLO_SESSION_STDOUT);
  REJECT_HEADER(status, 1);
  REJECT_HEADER(reserved, 1);
  REJECT_HEADER(generation, 0);
  REJECT_HEADER(argument_count, 0);
  REJECT_HEADER(argument_count, VIBEPOLLO_SESSION_PROTOCOL_MAX_ARGUMENTS + 1);
  REJECT_HEADER(payload_length, sizeof(request_payload) - 1);
#undef REJECT_HEADER
  request_packet[request_length - 1] = 'x';
  CHECK(!decode_request(request_packet, request_length, &decoded));
  request_packet[request_length - 1] = 0;
  request_packet[sizeof(request_header) + 1] = '\n';
  CHECK(!decode_request(request_packet, request_length, &decoded));
  request_packet[sizeof(request_header) + 1] = 'i';
  CHECK(!decode_request(request_packet, request_length + 1, &decoded));

  int peers[2] = {-1, -1};
  CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, peers));
  CHECK(peer_is_authorized(peers[0], getuid(), getgid()));
  CHECK(!peer_is_authorized(peers[0], getuid() ? 0 : 1, getgid()));
  CHECK(!peer_is_authorized(peers[0], getuid(), getgid() ? 0 : 1));
  CHECK(send(peers[1], request_packet, request_length, MSG_NOSIGNAL) ==
        (ssize_t) request_length);
  unsigned char received_request[256] = {0};
  CHECK(receive_request(peers[0], received_request, sizeof(received_request)) ==
        (ssize_t) request_length);
  CHECK(decode_request(received_request, request_length, &decoded));
  CHECK(decoded.header.generation == 7 && !strcmp(decoded.argv[1], "display-query"));
  const char output_payload[] = "bounded output";
  CHECK(send_frame(peers[0], VIBEPOLLO_SESSION_STDOUT, 7, 0,
                   output_payload, sizeof(output_payload) - 1));
  unsigned char response_packet[256] = {0};
  const ssize_t response_length = recv(peers[1], response_packet, sizeof(response_packet), 0);
  CHECK(response_length == (ssize_t) (sizeof(struct vibepollo_session_message) +
                                      sizeof(output_payload) - 1));
  struct vibepollo_session_message response_header = {0};
  memcpy(&response_header, response_packet, sizeof(response_header));
  CHECK(response_header.magic == VIBEPOLLO_SESSION_PROTOCOL_MAGIC);
  CHECK(response_header.version == VIBEPOLLO_SESSION_PROTOCOL_VERSION);
  CHECK(response_header.type == VIBEPOLLO_SESSION_STDOUT);
  CHECK(response_header.payload_length == sizeof(output_payload) - 1);
  CHECK(response_header.generation == 7 && response_header.status == 0);
  CHECK(!memcmp(response_packet + sizeof(response_header), output_payload,
                sizeof(output_payload) - 1));
  CHECK(!send_frame(peers[0], VIBEPOLLO_SESSION_STDOUT, 7, 0, output_payload,
                    VIBEPOLLO_SESSION_PROTOCOL_OUTPUT_CHUNK + 1));
  close(peers[0]);
  close(peers[1]);

  puts("PASS: session broker protocol, peer credentials, and semantic policy");
  return 0;
}
