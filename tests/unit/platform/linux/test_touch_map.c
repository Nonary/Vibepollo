#include "../../../../packaging/linux/vibepollo-touch-map.h"

struct provider {
  const char *address;
  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  GMutex mutex;
  GCond condition;
  bool ready;
  int wrong_vendor, wrong_product, wrong_name, wrong_sysname, not_touch, ignore_set;
  int reads, writes, unpinned_calls;
  char output[128];
};

static GDBusMessage *check_destination(GDBusConnection *bus, GDBusMessage *message,
                                      gboolean incoming, gpointer data) {
  struct provider *p = data;
  if (incoming && g_dbus_message_get_message_type(message) == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
      !g_strcmp0(g_dbus_message_get_interface(message), "org.freedesktop.DBus.Properties") &&
      g_strcmp0(g_dbus_message_get_destination(message), g_dbus_connection_get_unique_name(bus))) {
    g_atomic_int_inc(&p->unpinned_calls);
  }
  return message;
}

static GVariant *get_property(GDBusConnection *bus, const char *sender, const char *path,
                             const char *interface, const char *property,
                             GError **error, gpointer data) {
  (void) bus; (void) sender; (void) error;
  g_assert_cmpstr(interface, ==, "org.kde.KWin.InputDevice");
  struct provider *p = data;
  if (!strcmp(property, "vendor")) return g_variant_new_uint32(g_atomic_int_get(&p->wrong_vendor) ? 1 : 0xBEEF);
  if (!strcmp(property, "product")) return g_variant_new_uint32(g_atomic_int_get(&p->wrong_product) ? 1 : 0xDEAD);
  if (!strcmp(property, "touch")) return g_variant_new_boolean(!g_atomic_int_get(&p->not_touch));
  if (!strcmp(property, "name")) return g_variant_new_string(g_atomic_int_get(&p->wrong_name) ? "Physical touchscreen" : "Touch passthrough");
  if (!strcmp(property, "sysName")) return g_variant_new_string(g_atomic_int_get(&p->wrong_sysname) ? "event999" : (!strcmp(path, "/org/kde/KWin/InputDevice/event27") ? "event27" : "event28"));
  g_assert_cmpstr(property, ==, "outputName");
  g_atomic_int_inc(&p->reads);
  if (!strcmp(path, "/org/kde/KWin/InputDevice/event28")) return g_variant_new_string("DP-3");
  g_mutex_lock(&p->mutex);
  GVariant *result = g_variant_new_string(p->output);
  g_mutex_unlock(&p->mutex);
  return result;
}

static gboolean set_property(GDBusConnection *bus, const char *sender, const char *path,
                             const char *interface, const char *property, GVariant *value,
                             GError **error, gpointer data) {
  (void) bus; (void) sender; (void) error;
  g_assert_cmpstr(path, ==, "/org/kde/KWin/InputDevice/event27");
  g_assert_cmpstr(interface, ==, "org.kde.KWin.InputDevice");
  g_assert_cmpstr(property, ==, "outputName");
  struct provider *p = data;
  g_atomic_int_inc(&p->writes);
  g_mutex_lock(&p->mutex);
  if (!g_atomic_int_get(&p->ignore_set)) g_strlcpy(p->output, g_variant_get_string(value, NULL), sizeof(p->output));
  g_mutex_unlock(&p->mutex);
  return TRUE;
}

static gpointer serve(gpointer data) {
  struct provider *p = data;
  p->context = g_main_context_new();
  g_main_context_push_thread_default(p->context);
  p->loop = g_main_loop_new(p->context, FALSE);
  GDBusConnection *bus = g_dbus_connection_new_for_address_sync(
    p->address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
  g_assert_nonnull(bus);
  guint filter = g_dbus_connection_add_filter(bus, check_destination, p, NULL);
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(
    "<node><interface name='org.kde.KWin.InputDevice'>"
    "<property name='vendor' type='u' access='read'/>"
    "<property name='product' type='u' access='read'/>"
    "<property name='name' type='s' access='read'/>"
    "<property name='sysName' type='s' access='read'/>"
    "<property name='touch' type='b' access='read'/>"
    "<property name='outputName' type='s' access='readwrite'/>"
    "</interface></node>", NULL);
  const GDBusInterfaceVTable vtable = {.get_property = get_property, .set_property = set_property};
  guint first = g_dbus_connection_register_object(bus, "/org/kde/KWin/InputDevice/event27", info->interfaces[0], &vtable, p, NULL, NULL);
  guint second = g_dbus_connection_register_object(bus, "/org/kde/KWin/InputDevice/event28", info->interfaces[0], &vtable, p, NULL, NULL);
  g_assert_cmpuint(first, >, 0);
  g_assert_cmpuint(second, >, 0);
  GVariant *reply = g_dbus_connection_call_sync(
    bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", "org.kde.KWin", 0u), G_VARIANT_TYPE("(u)"),
    G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  g_assert_nonnull(reply);
  g_variant_unref(reply);
  g_mutex_lock(&p->mutex);
  p->ready = true;
  g_cond_signal(&p->condition);
  g_mutex_unlock(&p->mutex);
  g_main_loop_run(p->loop);
  g_dbus_connection_unregister_object(bus, first);
  g_dbus_connection_unregister_object(bus, second);
  g_dbus_connection_remove_filter(bus, filter);
  g_dbus_node_info_unref(info);
  g_dbus_connection_close_sync(bus, NULL, NULL);
  g_object_unref(bus);
  g_main_loop_unref(p->loop);
  g_main_context_pop_thread_default(p->context);
  g_main_context_unref(p->context);
  return NULL;
}

static gboolean stop(gpointer data) {
  g_main_loop_quit(((struct provider *) data)->loop);
  return G_SOURCE_REMOVE;
}

int main(void) {
  g_assert_false(touch_map_arguments_are_safe("event", "Virtual-1"));
  g_assert_false(touch_map_arguments_are_safe("../event27", "Virtual-1"));
  g_assert_false(touch_map_arguments_are_safe("event27", "Virtual-1\nDP-3"));
  GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(test_bus);
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_nonnull(bus);
  // A non-KWin compositor is explicitly distinguished from a mapping failure.
  g_assert_cmpint(touch_map_run("event27", "Virtual-1"), ==, 3);
  struct provider p = {.address = g_test_dbus_get_bus_address(test_bus)};
  p.thread = g_thread_new("fake-kwin", serve, &p);
  g_mutex_lock(&p.mutex);
  while (!p.ready) g_cond_wait(&p.condition, &p.mutex);
  g_mutex_unlock(&p.mutex);

  g_assert_cmpint(touch_map_run("event27", "Virtual-1"), ==, 0);
  g_assert_cmpint(g_atomic_int_get(&p.writes), ==, 1);
  g_assert_cmpint(g_atomic_int_get(&p.reads), >=, 2); // Read back the assignment.
  // Repeated input does not rewrite KWin's persistent configuration.
  g_assert_cmpint(touch_map_once(bus, "event27", "Virtual-1"), ==, 0);
  g_assert_cmpint(g_atomic_int_get(&p.writes), ==, 1);
  // A new capture output replaces the previous stream's association.
  g_assert_cmpint(touch_map_once(bus, "event27", "DP-3"), ==, 0);
  g_assert_cmpint(g_atomic_int_get(&p.writes), ==, 2);
  // A second client's identically named device must retain its own output.
  g_assert_cmpint(touch_map_once(bus, "event28", "DP-3"), ==, 0);
  g_assert_cmpint(g_atomic_int_get(&p.writes), ==, 2);

  int *identity_faults[] = {&p.wrong_vendor, &p.wrong_product, &p.wrong_name, &p.wrong_sysname, &p.not_touch};
  for (unsigned i = 0; i < G_N_ELEMENTS(identity_faults); ++i) {
    g_atomic_int_set(identity_faults[i], 1);
    g_assert_cmpint(touch_map_once(bus, "event27", "Virtual-1"), ==, 126);
    g_assert_cmpint(g_atomic_int_get(&p.writes), ==, 2);
    g_atomic_int_set(identity_faults[i], 0);
  }
  // An ignored write must not claim successful mapping just because Set returned.
  g_atomic_int_set(&p.ignore_set, 1);
  g_assert_cmpint(touch_map_once(bus, "event27", "Missing-1"), ==, 1);
  g_assert_cmpint(g_atomic_int_get(&p.writes), ==, 3);
  g_assert_cmpint(touch_map_once(bus, "event99", "Virtual-1"), ==, 1);
  // An output absent during setup can appear later; retry the same device and
  // connector instead of treating the earlier failure as permanent.
  g_atomic_int_set(&p.ignore_set, 0);
  g_assert_cmpint(touch_map_run("event27", "Missing-1"), ==, 0);
  g_assert_cmpint(g_atomic_int_get(&p.writes), ==, 4);
  // KWin/output recreation can lose an assignment without changing its name.
  g_mutex_lock(&p.mutex);
  p.output[0] = 0;
  g_mutex_unlock(&p.mutex);
  g_assert_cmpint(touch_map_run("event27", "Missing-1"), ==, 0);
  g_assert_cmpint(g_atomic_int_get(&p.writes), ==, 5);
  g_assert_cmpint(g_atomic_int_get(&p.unpinned_calls), ==, 0);

  g_main_context_invoke(p.context, stop, &p);
  g_thread_join(p.thread);
  g_mutex_clear(&p.mutex);
  g_cond_clear(&p.condition);
  g_dbus_connection_close_sync(bus, NULL, NULL);
  g_object_unref(bus);
  g_test_dbus_down(test_bus);
  g_object_unref(test_bus);
  return 0;
}
