#ifndef VIBEPOLLO_DISPLAY_POWER_H
#define VIBEPOLLO_DISPLAY_POWER_H

// Included by the capability-free power helper, exec'd only after permanently
// dropping to the bound session UID and clearing every capability. No caller-supplied
// command, bus destination, cookie, or environment is accepted.
#include <gio/gio.h>
#include <glib-unix.h>

struct display_power_state {
  GDBusConnection *bus;
  GMainLoop *loop;
  char *owner;
  guint cookie;
  bool inhibited;
  bool failed;
  bool retrying;
  bool wake_pending;
  bool require_inhibit;
  bool stopping;
};

static const char power_service[] = "org.freedesktop.PowerManagement";
static const char power_path[] = "/org/freedesktop/PowerManagement/Inhibit";
static const char power_interface[] = "org.freedesktop.PowerManagement.Inhibit";

static bool display_power_wake(void) {
  char *const arguments[] = {
    "kscreen-doctor", "--dpms", "on", NULL
  };
  // The bounded runner owns/reaps its child. Keep stdout exclusively for the
  // readiness byte consumed by the host.
  return run_command_bounded("/usr/bin/kscreen-doctor", arguments, 3000) == 0;
}

static void display_power_release(struct display_power_state *state) {
  if (state->inhibited) {
    GVariant *reply = g_dbus_connection_call_sync(
      state->bus, state->owner, power_path, power_interface, "UnInhibit",
      g_variant_new("(u)", state->cookie), NULL,
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
    if (reply) g_variant_unref(reply);
  }
  g_clear_pointer(&state->owner, g_free);
  state->inhibited = false;
}

static bool display_power_inhibit(struct display_power_state *state) {
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
    state->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
    "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", power_service),
    G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
  if (!reply) {
    g_clear_error(&error);
    // The Wayland greeter may not run PowerDevil. It still needs a DPMS wake,
    // but must remain reachable without a desktop power-management daemon.
    return !state->require_inhibit;
  }
  const char *owner = NULL;
  g_variant_get(reply, "(&s)", &owner);
  if (state->inhibited && !g_strcmp0(owner, state->owner)) {
    g_variant_unref(reply);
    return true;
  }
  char *new_owner = g_strdup(owner);
  g_variant_unref(reply);
  // Address the unique owner, so a replacement service cannot receive an old
  // cookie or impersonate a successful acquisition in the same transaction.
  reply = g_dbus_connection_call_sync(
    state->bus, new_owner, power_path, power_interface, "Inhibit",
    g_variant_new("(ss)", "Vibepollo", "Active remote display"),
    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
  if (!reply) {
    g_clear_error(&error);
    g_free(new_owner);
    return !state->require_inhibit;
  }
  // Acquire the replacement before releasing the previous owner. Never send
  // the previous service's cookie to the new PowerDevil instance.
  display_power_release(state);
  state->owner = new_owner;
  g_variant_get(reply, "(u)", &state->cookie);
  g_variant_unref(reply);
  state->inhibited = true;
  state->wake_pending = true;
  return true;
}

static bool display_power_prepare(struct display_power_state *state) {
  if (!display_power_inhibit(state) ||
      (state->wake_pending && !display_power_wake())) {
    return false;
  }
  state->wake_pending = false;
  return true;
}

static gboolean display_power_refresh(gpointer data) {
  struct display_power_state *state = data;
  const bool ready = display_power_prepare(state);
  // Resume and PowerDevil restarts can temporarily make the services
  // unavailable. Keep the lease, and retry while this stream still owns it.
  if (!ready && !state->retrying) {
    fprintf(stderr, "vibepollo-session-broker: retrying display power recovery\n");
  } else if (ready && state->retrying) {
    fprintf(stderr, "vibepollo-session-broker: display power recovered\n");
  }
  state->retrying = !ready;
  return G_SOURCE_CONTINUE;
}

static void display_power_resume(GDBusConnection *connection, const gchar *sender,
                                 const gchar *path, const gchar *interface,
                                 const gchar *signal, GVariant *parameters,
                                 gpointer data) {
  (void) connection; (void) sender; (void) path; (void) interface; (void) signal;
  gboolean sleeping = FALSE;
  g_variant_get(parameters, "(b)", &sleeping);
  if (!sleeping) ((struct display_power_state *) data)->wake_pending = true;
}

static gboolean display_power_stop(gpointer data) {
  struct display_power_state *state = data;
  state->stopping = true;
  g_main_loop_quit(state->loop);
  return G_SOURCE_CONTINUE;
}

static int run_display_power(bool require_inhibit) {
  // GIO connection establishment has no timeout parameter. Bound the entire
  // readiness transaction, including bus authentication, with a process-level
  // deadline. This helper has no privileges; exit also releases its bus holds.
  struct sigaction alarm_action = {.sa_handler = SIG_DFL};
  sigemptyset(&alarm_action.sa_mask);
  sigset_t alarm_mask;
  sigemptyset(&alarm_mask);
  sigaddset(&alarm_mask, SIGALRM);
  if (sigaction(SIGALRM, &alarm_action, NULL) ||
      sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL)) return 126;
  alarm(7);
  const gint64 ready_deadline = g_get_monotonic_time() + 6 * G_TIME_SPAN_SECOND;
  // The controller binds Wayland sessions only. Do not let Qt choose Xwayland
  // (or an offscreen backend) for the DPMS protocol after a desktop resume.
  if (setenv("QT_QPA_PLATFORM", "wayland", 1)) return 126;
  struct display_power_state state = {.wake_pending = true, .require_inhibit = require_inhibit};
  GError *error = NULL;
  state.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!state.bus) {
    fprintf(stderr, "vibepollo-session-broker: display power bus: %s\n", error->message);
    g_clear_error(&error);
    return 126;
  }
  state.loop = g_main_loop_new(NULL, FALSE);
  // Register before starting the inhibitor/wake transaction so cancellation
  // never leaves a live D-Bus owner after the broker drops admission.
  const guint term = g_unix_signal_add(SIGTERM, display_power_stop, &state);
  const guint interrupt = g_unix_signal_add(SIGINT, display_power_stop, &state);
  const guint hangup = g_unix_signal_add(SIGHUP, display_power_stop, &state);
  GDBusConnection *system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
  const guint resume = system_bus ? g_dbus_connection_signal_subscribe(
    system_bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
    "PrepareForSleep", "/org/freedesktop/login1", NULL,
    G_DBUS_SIGNAL_FLAGS_NONE, display_power_resume, &state, NULL) : 0;
  guint refresh = 0;
  bool prepared = false;
  if (term && interrupt && hangup && resume) {
    do {
      while (g_main_context_iteration(NULL, FALSE)) {}
      if (state.stopping) break;
      prepared = display_power_prepare(&state);
      if (prepared) break;
      g_usleep(100000);
    } while (g_get_monotonic_time() < ready_deadline);
  }
  if (prepared && !state.stopping) {
    if (write(STDOUT_FILENO, "R", 1) == 1) {
      alarm(0);
      refresh = g_timeout_add_seconds(1, display_power_refresh, &state);
      g_main_loop_run(state.loop);
    } else {
      state.failed = true;
    }
  } else {
    state.failed = true;
  }
  if (refresh) g_source_remove(refresh);
  if (resume) g_dbus_connection_signal_unsubscribe(system_bus, resume);
  if (system_bus) g_object_unref(system_bus);
  if (term) g_source_remove(term);
  if (interrupt) g_source_remove(interrupt);
  if (hangup) g_source_remove(hangup);
  display_power_release(&state);
  g_object_unref(state.bus);
  g_main_loop_unref(state.loop);
  alarm(0);
  if (state.failed) fprintf(stderr, "vibepollo-session-broker: display wake/inhibition failed\n");
  return state.failed ? 126 : 0;
}

#endif
