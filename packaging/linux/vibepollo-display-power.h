#ifndef VIBEPOLLO_DISPLAY_POWER_H
#define VIBEPOLLO_DISPLAY_POWER_H

// Included by the capability-free power helper, exec'd only after permanently
// dropping to the bound session UID and clearing every capability. No caller-supplied
// command, bus destination, cookie, or environment is accepted.
#include <gio/gio.h>
#include <glib-unix.h>
#include "vibepollo-display-borrow.h"

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
  bool retain_only;
  bool stopping;
  bool inhibit_only;
  bool release_unknown;
  bool published;
  bool release_authorized;
  char reason[80];
};

static const char power_service[] = "org.kde.Solid.PowerManagement.PolicyAgent";
static const char power_path[] = "/org/kde/Solid/PowerManagement/PolicyAgent";
static const char power_interface[] = "org.kde.Solid.PowerManagement.PolicyAgent";
// PowerDevil's freedesktop Inhibit API only blocks session interruption.
// Capture also needs ChangeScreenSettings (4) to prevent idle DPMS shutdown.
static const guint power_inhibition_policies = 1u | 4u;
// A retained private topology blocks only idle dimming/screen-off. It never
// changes power state itself and must not keep an idle host from suspending.
static const guint retained_inhibition_policies = 4u;

static bool display_power_wake(void) {
  const int lock=display_borrow_inherited_lock();
  if(lock==-2 || (lock>=0 && !display_borrow_acquire_lock(lock,false,true)))return false;
  char *const arguments[] = {
    "kscreen-doctor", "--dpms", "on", NULL
  };
  // The bounded runner owns/reaps its child. Keep stdout exclusively for the
  // readiness byte consumed by the host.
  bool result=run_command_bounded("/usr/bin/kscreen-doctor", arguments, 3000) == 0;
  if(lock>=0) {
    if(result && !display_borrow_clear_uncertainty(lock))result=false;
    if(!result)display_borrow_note_mutation_unknown(lock);
    (void)flock(lock,LOCK_UN);
  }
  return result;
}

static bool display_power_release(struct display_power_state *state) {
  bool confirmed=true;
  if (state->inhibited) {
    GVariant *reply = g_dbus_connection_call_sync(
      state->bus, state->owner, power_path, power_interface, "ReleaseInhibition",
      g_variant_new("(u)", state->cookie), NULL,
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
    confirmed=reply!=NULL;
    if (reply) g_variant_unref(reply);
  }
  g_clear_pointer(&state->owner, g_free);
  state->inhibited = false;
  if(state->inhibit_only && !confirmed)state->release_unknown=true;
  return confirmed;
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
  if(state->inhibit_only && state->published && state->owner && g_strcmp0(owner,state->owner)) {
    g_variant_unref(reply);return false; // Published ownership cannot be renewed across a gap.
  }
  char *new_owner = g_strdup(owner);
  g_variant_unref(reply);
  // Address the unique owner, so a replacement service cannot receive an old
  // cookie or impersonate a successful acquisition in the same transaction.
  reply = g_dbus_connection_call_sync(
    state->bus, new_owner, power_path, power_interface, "AddInhibition",
    g_variant_new("(uss)", state->retain_only ? retained_inhibition_policies : power_inhibition_policies, "Vibepollo", state->retain_only ? "Retained remote display" : (state->inhibit_only ? state->reason : "Active remote display")),
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
  state->wake_pending = !state->inhibit_only && !state->retain_only;
  return true;
}

static bool display_power_same_owner(struct display_power_state *state) {
  GVariant *reply=g_dbus_connection_call_sync(state->bus,"org.freedesktop.DBus","/org/freedesktop/DBus",
    "org.freedesktop.DBus","GetNameOwner",g_variant_new("(s)",power_service),G_VARIANT_TYPE("(s)"),
    G_DBUS_CALL_FLAGS_NO_AUTO_START,1000,NULL,NULL);
  if(!reply)return false;const char *owner=NULL;g_variant_get(reply,"(&s)",&owner);
  const bool same=state->owner && !g_strcmp0(owner,state->owner);g_variant_unref(reply);return same;
}

static bool display_power_active(struct display_power_state *state) {
  if(!state->inhibit_only)return true;
  if(!display_power_same_owner(state))return false;
  // AddInhibition's cookie can precede enforcement and user policy rejection.
  // Require the unique context's entry in the active list from the same owner.
  GVariant *reply=g_dbus_connection_call_sync(state->bus,state->owner,power_path,power_interface,
    "ListInhibitions",NULL,G_VARIANT_TYPE("(aas)"),G_DBUS_CALL_FLAGS_NO_AUTO_START,1000,NULL,NULL);
  if(!reply)return false;
  bool active=false;GVariant *rows=g_variant_get_child_value(reply,0);
  for(gsize i=0;i<g_variant_n_children(rows);++i) {
    GVariant *row=g_variant_get_child_value(rows,i);
    if(g_variant_n_children(row)==2) {
      GVariant *who=g_variant_get_child_value(row,0),*why=g_variant_get_child_value(row,1);
      if(!strcmp(g_variant_get_string(who,NULL),"Vibepollo") && !strcmp(g_variant_get_string(why,NULL),state->reason))active=true;
      g_variant_unref(who);g_variant_unref(why);
    }
    g_variant_unref(row);
  }
  g_variant_unref(rows);g_variant_unref(reply);return active && display_power_same_owner(state);
}

static bool display_power_prepare(struct display_power_state *state) {
  if (!display_power_inhibit(state) || !display_power_active(state) ||
      (state->wake_pending && !state->inhibit_only && !display_power_wake())) {
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
  if(state->inhibit_only && !ready) {
    state->failed=true;
    if(state->published)(void)write(STDOUT_FILENO,"X",1);
    else g_main_loop_quit(state->loop);
  }
  return G_SOURCE_CONTINUE;
}

static void display_power_resume(GDBusConnection *connection, const gchar *sender,
                                 const gchar *path, const gchar *interface,
                                 const gchar *signal, GVariant *parameters,
                                 gpointer data) {
  (void) connection; (void) sender; (void) path; (void) interface; (void) signal;
  struct display_power_state *state = data;
  gboolean sleeping = FALSE;
  g_variant_get(parameters, "(b)", &sleeping);
  if (!sleeping && !state->inhibit_only && !state->retain_only) state->wake_pending = true;
}

static gboolean display_power_stop(gpointer data) {
  struct display_power_state *state = data;
  // A generic signal is not proof that retained host captures have drained.
  if(state->inhibit_only && state->published && !state->release_authorized) {
    state->failed=true;return G_SOURCE_CONTINUE;
  }
  state->stopping = true;
  g_main_loop_quit(state->loop);
  return G_SOURCE_CONTINUE;
}

static gboolean display_power_control(gint fd,GIOCondition condition,gpointer data) {
  struct display_power_state *state=data;char command=0;
  if((condition&G_IO_IN) && recv(fd,&command,1,MSG_DONTWAIT|MSG_TRUNC)==1 && command=='R') {
    state->release_authorized=true;return display_power_stop(state);
  }
  // HUP/unknown control leaves the native connection owned by the host's
  // duplicate descriptor. Never explicitly release its inhibition here.
  state->failed=true;return G_SOURCE_REMOVE;
}
static int display_power_bus_fd(GDBusConnection *bus) {
  GIOStream *stream=g_dbus_connection_get_stream(bus);
  if(!stream || !G_IS_SOCKET_CONNECTION(stream))return -1;
  return g_socket_get_fd(g_socket_connection_get_socket(G_SOCKET_CONNECTION(stream)));
}
static bool display_power_publish(const char *context,int native_fd) {
  char ready[37];const int length=snprintf(ready,sizeof(ready),"B2:%s\n",context);
  union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(int))];} control={0};
  struct iovec vector={.iov_base=ready,.iov_len=(size_t)length};
  struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
  struct cmsghdr *cmsg=CMSG_FIRSTHDR(&message);cmsg->cmsg_level=SOL_SOCKET;cmsg->cmsg_type=SCM_RIGHTS;cmsg->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(cmsg),&native_fd,sizeof(native_fd));
  return length==36 && sendmsg(STDOUT_FILENO,&message,MSG_NOSIGNAL)==length;
}

static void display_power_native_changed(GDBusConnection *connection,const gchar *sender,
  const gchar *path,const gchar *interface,const gchar *signal,GVariant *parameters,gpointer data) {
  (void)connection;(void)sender;(void)path;(void)interface;
  struct display_power_state *state=data;bool owner_lost=false;
  if(!strcmp(signal,"NameOwnerChanged")) {
    const char *name=NULL,*old_owner=NULL,*new_owner=NULL;
    g_variant_get(parameters,"(&s&s&s)",&name,&old_owner,&new_owner);(void)old_owner;
    owner_lost=!g_strcmp0(name,power_service) && g_strcmp0(new_owner,state->owner);
  }
  if(state->published && (owner_lost || !display_power_active(state))) {state->failed=true;(void)write(STDOUT_FILENO,"X",1);}
}

static int run_display_power_mode(bool require_inhibit,bool inhibit_only,const char *context,bool retain_only) {
  if(inhibit_only && (!display_borrow_context_valid(context) || display_borrow_inherited_lock()<0))return 126;
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
  alarm(inhibit_only?14:7);
  const gint64 ready_deadline = g_get_monotonic_time() + (inhibit_only?13:6) * G_TIME_SPAN_SECOND;
  // The controller binds Wayland sessions only. Do not let Qt choose Xwayland
  // (or an offscreen backend) for the DPMS protocol after a desktop resume.
  if (!inhibit_only && setenv("QT_QPA_PLATFORM", "wayland", 1)) return 126;
  struct display_power_state state = {.wake_pending = !inhibit_only && !retain_only, .require_inhibit = require_inhibit, .inhibit_only=inhibit_only, .retain_only=retain_only};
  if(inhibit_only)snprintf(state.reason,sizeof(state.reason),"Borrow existing target %s",context);
  GError *error = NULL;
  state.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!state.bus) {
    fprintf(stderr, "vibepollo-session-broker: display power bus: %s\n", error->message);
    g_clear_error(&error);
    return 126;
  }
  const int native_fd=inhibit_only?display_power_bus_fd(state.bus):-1;
  if(inhibit_only && native_fd<0){g_object_unref(state.bus);return 126;}
  state.loop = g_main_loop_new(NULL, FALSE);
  // Register before starting the inhibitor/wake transaction so cancellation
  // never leaves a live D-Bus owner after the broker drops admission.
  const guint term = g_unix_signal_add(SIGTERM, display_power_stop, &state);
  const guint interrupt = g_unix_signal_add(SIGINT, display_power_stop, &state);
  const guint hangup = g_unix_signal_add(SIGHUP, display_power_stop, &state);
  GDBusConnection *system_bus = inhibit_only?NULL:g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
  const guint resume = system_bus ? g_dbus_connection_signal_subscribe(
    system_bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
    "PrepareForSleep", "/org/freedesktop/login1", NULL,
    G_DBUS_SIGNAL_FLAGS_NONE, display_power_resume, &state, NULL) : 0;
  const guint owner_watch=inhibit_only?g_dbus_connection_signal_subscribe(state.bus,"org.freedesktop.DBus",
    "org.freedesktop.DBus","NameOwnerChanged","/org/freedesktop/DBus",power_service,
    G_DBUS_SIGNAL_FLAGS_NONE,display_power_native_changed,&state,NULL):0;
  const guint policy_watch=inhibit_only?g_dbus_connection_signal_subscribe(state.bus,power_service,
    "org.freedesktop.DBus.Properties","PropertiesChanged",power_path,power_interface,
    G_DBUS_SIGNAL_FLAGS_NONE,display_power_native_changed,&state,NULL):0;
  guint refresh = 0;
  const guint control=inhibit_only?g_unix_fd_add(STDOUT_FILENO,G_IO_IN|G_IO_HUP|G_IO_ERR,display_power_control,&state):0;
  bool prepared = false;
  if (term && interrupt && hangup && (inhibit_only?(control!=0&&owner_watch!=0&&policy_watch!=0):resume!=0)) {
    do {
      while (g_main_context_iteration(NULL, FALSE)) {}
      if (state.stopping) break;
      prepared = display_power_prepare(&state);
      if (prepared) break;
      g_usleep(100000);
    } while (g_get_monotonic_time() < ready_deadline);
  }
  if (prepared && !state.stopping) {
    const bool published=inhibit_only?display_power_publish(context,native_fd):write(STDOUT_FILENO,"R",1)==1;
    state.published=published;
    if (published) {
      alarm(0);
      refresh = g_timeout_add_seconds(1, display_power_refresh, &state);
      g_main_loop_run(state.loop);
    } else {
      state.failed = true;
    }
  } else {
    state.failed = true;
  }
  if (control) g_source_remove(control);
  if (refresh) g_source_remove(refresh);
  if (resume) g_dbus_connection_signal_unsubscribe(system_bus, resume);
  if (system_bus) g_object_unref(system_bus);
  if (term) g_source_remove(term);
  if (interrupt) g_source_remove(interrupt);
  if (hangup) g_source_remove(hangup);
  if(inhibit_only && state.published && !state.release_authorized) {
    // Deliberately leave GDBus alive: unref may close the shared transport. The
    // OS closes only this process's descriptor; the host retains the resource.
    alarm(0);return VIBEPOLLO_SESSION_COMPLETION_UNKNOWN;
  }
  if(owner_watch)g_dbus_connection_signal_unsubscribe(state.bus,owner_watch);
  if(policy_watch)g_dbus_connection_signal_unsubscribe(state.bus,policy_watch);
  const bool release_confirmed=display_power_release(&state);
  if(inhibit_only && (!release_confirmed || state.release_unknown))state.failed=true;
  g_object_unref(state.bus);
  g_main_loop_unref(state.loop);
  alarm(0);
  if (state.failed) fprintf(stderr, "vibepollo-session-broker: display wake/inhibition failed\n");
  return state.failed ? (inhibit_only?VIBEPOLLO_SESSION_COMPLETION_UNKNOWN:126) : 0;
}

static int run_display_power(bool require_inhibit, bool retain_only) { return run_display_power_mode(require_inhibit,false,NULL,retain_only); }

#endif
