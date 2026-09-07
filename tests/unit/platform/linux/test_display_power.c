#define _GNU_SOURCE

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int wake_count;
static int wake_result;
static int active_inhibitors;
static bool check_wake_order = true;

// Replace only the external command runner. Exercise the real power protocol
// against a private bus, without changing the developer's desktop or DPMS.
static int run_command_bounded(const char *path, char *const arguments[],
                               unsigned timeout) {
  g_assert_cmpstr(path, ==, "/usr/bin/kscreen-doctor");
  g_assert_cmpstr(arguments[0], ==, "kscreen-doctor");
  g_assert_cmpstr(arguments[1], ==, "--dpms");
  g_assert_cmpstr(arguments[2], ==, "on");
  g_assert_null(arguments[3]);
  g_assert_cmpuint(timeout, ==, 3000);
  if (check_wake_order) g_assert_cmpint(g_atomic_int_get(&active_inhibitors), >, 0);
  ++wake_count;
  return wake_result;
}

#include "../../../../packaging/linux/vibepollo-display-power.h"

struct provider {
  const char *address;
  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  GMutex mutex;
  GCond condition;
  bool ready;
  guint flags;
  int acquired;
  int released;
  int reject;
};

static void power_call(GDBusConnection *bus, const char *sender, const char *path,
                       const char *interface, const char *method,
                       GVariant *parameters, GDBusMethodInvocation *invocation,
                       gpointer data) {
  (void) bus; (void) sender; (void) path; (void) interface;
  struct provider *provider = data;
  if (!strcmp(method, "Inhibit")) {
    if (g_atomic_int_get(&provider->reject)) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.test.Unavailable", "restarting");
      return;
    }
    const char *app, *reason;
    g_variant_get(parameters, "(&s&s)", &app, &reason);
    g_assert_cmpstr(app, ==, "Vibepollo");
    g_assert_cmpstr(reason, ==, "Active remote display");
    const guint cookie = g_atomic_int_add(&provider->acquired, 1) + 1;
    g_atomic_int_inc(&active_inhibitors);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", cookie));
  } else {
    g_assert_cmpstr(method, ==, "UnInhibit");
    guint cookie;
    g_variant_get(parameters, "(u)", &cookie);
    g_assert_cmpuint(cookie, >, 0);
    g_assert_cmpuint(cookie, <=, g_atomic_int_get(&provider->acquired));
    g_atomic_int_inc(&provider->released);
    g_atomic_int_add(&active_inhibitors, -1);
    g_dbus_method_invocation_return_value(invocation, NULL);
  }
}

static gpointer serve_power(gpointer data) {
  struct provider *provider = data;
  provider->context = g_main_context_new();
  g_main_context_push_thread_default(provider->context);
  provider->loop = g_main_loop_new(provider->context, FALSE);
  GDBusConnection *bus = g_dbus_connection_new_for_address_sync(
    provider->address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
  g_assert_nonnull(bus);
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(
    "<node><interface name='org.freedesktop.PowerManagement.Inhibit'>"
    "<method name='Inhibit'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='u' direction='out'/></method>"
    "<method name='UnInhibit'><arg type='u' direction='in'/></method>"
    "</interface></node>", NULL);
  const GDBusInterfaceVTable vtable = {.method_call = power_call};
  const guint object = g_dbus_connection_register_object(
    bus, power_path, info->interfaces[0], &vtable, provider, NULL, NULL);
  g_assert_cmpuint(object, >, 0);
  GVariant *reply = g_dbus_connection_call_sync(
    bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
    "RequestName", g_variant_new("(su)", power_service, provider->flags),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  g_assert_nonnull(reply);
  g_variant_unref(reply);
  g_mutex_lock(&provider->mutex);
  provider->ready = true;
  g_cond_signal(&provider->condition);
  g_mutex_unlock(&provider->mutex);
  g_main_loop_run(provider->loop);
  g_dbus_connection_unregister_object(bus, object);
  g_dbus_node_info_unref(info);
  g_dbus_connection_close_sync(bus, NULL, NULL);
  g_object_unref(bus);
  g_main_loop_unref(provider->loop);
  g_main_context_pop_thread_default(provider->context);
  g_main_context_unref(provider->context);
  return NULL;
}

static void start_provider(struct provider *provider, const char *address, guint flags) {
  provider->address = address;
  provider->flags = flags;
  provider->thread = g_thread_new("fake-power", serve_power, provider);
  g_mutex_lock(&provider->mutex);
  while (!provider->ready) g_cond_wait(&provider->condition, &provider->mutex);
  g_mutex_unlock(&provider->mutex);
}

static gboolean stop_provider(gpointer data) {
  g_main_loop_quit(((struct provider *) data)->loop);
  return G_SOURCE_REMOVE;
}

static void join_provider(struct provider *provider) {
  g_main_context_invoke(provider->context, stop_provider, provider);
  g_thread_join(provider->thread);
  g_mutex_clear(&provider->mutex);
  g_cond_clear(&provider->condition);
}

static GSubprocess *start_worker(const char *executable, const char *mode) {
  GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL,
                                         executable, mode, NULL);
  g_assert_nonnull(process);
  return process;
}

static bool read_ready(GSubprocess *process) {
  GInputStream *output = g_subprocess_get_stdout_pipe(process);
  struct pollfd fd = {.fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(output)), .events = POLLIN};
  g_assert_cmpint(poll(&fd, 1, 8000), >, 0);
  char token = 0;
  return read(fd.fd, &token, 1) == 1 && token == 'R';
}

int main(int argc, char **argv) {
  if (argc == 2) {
    check_wake_order = false;
    wake_result = !strcmp(argv[1], "--wake-failure") ? 124 : 0;
    return run_display_power(strcmp(argv[1], "--greeter") != 0);
  }
  GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(test_bus);
  const char *address = g_test_dbus_get_bus_address(test_bus);
  g_setenv("DBUS_SYSTEM_BUS_ADDRESS", address, TRUE);
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_nonnull(bus);
  struct display_power_state state = {.bus = bus, .require_inhibit = true, .wake_pending = true};

  // No desktop inhibitor must not claim readiness or issue a wake.
  g_assert_false(display_power_prepare(&state));
  g_assert_cmpint(wake_count, ==, 0);
  // A greeter has no PowerDevil but still gets a bounded wake.
  state.require_inhibit = false;
  check_wake_order = false;
  g_assert_true(display_power_prepare(&state));
  g_assert_cmpint(wake_count, ==, 1);
  check_wake_order = true;
  state.require_inhibit = true;

  struct provider first = {0};
  start_provider(&first, address, 1); // Allow a replacement owner.
  g_assert_true(display_power_prepare(&state));
  g_assert_cmpint(wake_count, ==, 2);
  g_assert_cmpint(g_atomic_int_get(&first.acquired), ==, 1);
  g_assert_true(display_power_prepare(&state));
  g_assert_cmpint(wake_count, ==, 2); // Do not continually fight desktop DPMS.

  // Explicit resume wakes again, but does not leak another inhibitor.
  GVariant *resume = g_variant_ref_sink(g_variant_new("(b)", FALSE));
  display_power_resume(NULL, NULL, NULL, NULL, NULL, resume, &state);
  g_variant_unref(resume);
  wake_result = 124;
  g_assert_cmpint(display_power_refresh(&state), ==, G_SOURCE_CONTINUE);
  g_assert_true(state.retrying);
  g_assert_true(state.wake_pending);
  wake_result = 0;
  display_power_refresh(&state);
  g_assert_false(state.retrying);
  g_assert_false(state.wake_pending);
  g_assert_cmpint(g_atomic_int_get(&first.acquired), ==, 1);

  struct display_power_state second = {.bus = bus, .require_inhibit = true, .wake_pending = true};
  g_assert_true(display_power_prepare(&second));
  g_assert_cmpint(g_atomic_int_get(&active_inhibitors), ==, 2);
  display_power_release(&second);
  g_assert_cmpint(g_atomic_int_get(&active_inhibitors), ==, 1);

  // A service restart gets a fresh cookie; failure keeps the old hold until
  // the replacement can acquire one. UnInhibit targets the old unique owner.
  struct provider replacement = {.reject = 1};
  start_provider(&replacement, address, 6);
  g_assert_false(display_power_prepare(&state));
  g_assert_cmpint(g_atomic_int_get(&first.released), ==, 1);
  g_assert_cmpint(g_atomic_int_get(&active_inhibitors), ==, 1);
  struct display_power_state greeter = {.bus = bus, .wake_pending = true};
  g_assert_true(display_power_prepare(&greeter)); // Owner rejects inhibition.
  g_assert_false(greeter.inhibited);
  g_atomic_int_set(&replacement.reject, 0);
  g_assert_true(display_power_prepare(&state));
  g_assert_cmpint(g_atomic_int_get(&first.released), ==, 2);
  g_assert_cmpint(g_atomic_int_get(&replacement.acquired), ==, 1);
  display_power_release(&state);
  display_power_release(&state); // Idempotent cleanup.
  g_assert_cmpint(g_atomic_int_get(&active_inhibitors), ==, 0);

  // Exercise the actual readiness pipe and signal-driven lease lifetime.
  g_atomic_int_set(&replacement.reject, 1);
  GSubprocess *worker = start_worker(argv[0], "--worker");
  g_usleep(200000); // Simulate PowerDevil still settling on the first attempt.
  g_atomic_int_set(&replacement.reject, 0);
  g_assert_true(read_ready(worker));
  g_assert_cmpint(g_atomic_int_get(&active_inhibitors), ==, 1);
  g_subprocess_send_signal(worker, SIGTERM);
  g_assert_true(g_subprocess_wait_check(worker, NULL, NULL));
  g_object_unref(worker);
  g_assert_cmpint(g_atomic_int_get(&active_inhibitors), ==, 0);

  // A Unix socket that accepts connections but never authenticates must not
  // strand a power worker. The hard deadline must also unblock inherited ALRM.
  char *temporary = g_dir_make_tmp("vibepollo-power-test-XXXXXX", NULL);
  g_assert_nonnull(temporary);
  struct sockaddr_un stalled = {.sun_family = AF_UNIX};
  g_snprintf(stalled.sun_path, sizeof(stalled.sun_path), "%s/bus", temporary);
  int stalled_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint(stalled_fd, >=, 0);
  g_assert_cmpint(bind(stalled_fd, (struct sockaddr *) &stalled, sizeof(stalled)), ==, 0);
  g_assert_cmpint(listen(stalled_fd, 1), ==, 0);
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  char *stalled_address = g_strdup_printf("unix:path=%s", stalled.sun_path);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS", stalled_address, TRUE);
  g_free(stalled_address);
  sigset_t blocked, previous;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGALRM);
  g_assert_cmpint(sigprocmask(SIG_BLOCK, &blocked, &previous), ==, 0);
  worker = g_subprocess_launcher_spawn(launcher, NULL, argv[0], "--worker", NULL);
  g_assert_cmpint(sigprocmask(SIG_SETMASK, &previous, NULL), ==, 0);
  g_assert_nonnull(worker);
  g_assert_false(read_ready(worker));
  g_assert_true(g_subprocess_wait(worker, NULL, NULL));
  g_assert_true(g_subprocess_get_if_signaled(worker));
  g_assert_cmpint(g_subprocess_get_term_sig(worker), ==, SIGALRM);
  g_object_unref(worker);
  g_object_unref(launcher);
  close(stalled_fd);
  unlink(stalled.sun_path);
  rmdir(temporary);
  g_free(temporary);
  worker = start_worker(argv[0], "--wake-failure");
  g_assert_false(read_ready(worker));
  g_assert_true(g_subprocess_wait(worker, NULL, NULL));
  g_assert_cmpint(g_subprocess_get_exit_status(worker), ==, 126);
  g_object_unref(worker);
  g_assert_cmpint(g_atomic_int_get(&active_inhibitors), ==, 0);

  join_provider(&replacement);
  join_provider(&first);
  worker = start_worker(argv[0], "--greeter");
  g_assert_true(read_ready(worker));
  g_subprocess_send_signal(worker, SIGTERM);
  g_assert_true(g_subprocess_wait_check(worker, NULL, NULL));
  g_object_unref(worker);
  g_dbus_connection_close_sync(bus, NULL, NULL);
  g_object_unref(bus);
  g_test_dbus_down(test_bus);
  g_object_unref(test_bus);
  return 0;
}
