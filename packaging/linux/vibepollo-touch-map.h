#pragma once

#include "vibepollo-touch-map-policy.h"
#include <gio/gio.h>

// All D-Bus work runs as the selected unprivileged session user. In particular,
// never link GIO or connect to a session bus in the capability-bearing broker.
static int touch_map_device(GDBusConnection *bus, const char *owner, const char *node, const char *output) {
  char path[96];
  g_snprintf(path, sizeof(path), "/org/kde/KWin/InputDevice/%s", node);
  GVariant *reply = g_dbus_connection_call_sync(
    bus, owner, path, "org.freedesktop.DBus.Properties", "GetAll",
    g_variant_new("(s)", "org.kde.KWin.InputDevice"), G_VARIANT_TYPE("(a{sv})"),
    G_DBUS_CALL_FLAGS_NO_AUTO_START, 200, NULL, NULL);
  if (!reply) return 1; // The new uinput device may not be registered yet.
  GVariant *properties;
  g_variant_get(reply, "(@a{sv})", &properties);
  guint vendor = 0, product = 0;
  gboolean touch = FALSE;
  const char *name = NULL, *sysname = NULL, *assigned = NULL;
  const bool ours = g_variant_lookup(properties, "vendor", "u", &vendor) && vendor == 0xBEEF &&
    g_variant_lookup(properties, "product", "u", &product) && product == 0xDEAD &&
    g_variant_lookup(properties, "touch", "b", &touch) && touch &&
    g_variant_lookup(properties, "sysName", "&s", &sysname) && !strcmp(sysname, node) &&
    g_variant_lookup(properties, "name", "&s", &name) &&
    (!strcmp(name, "Touch passthrough") || g_str_has_prefix(name, "Touch passthrough (seat"));
  const bool already_mapped = ours &&
    g_variant_lookup(properties, "outputName", "&s", &assigned) && !strcmp(assigned, output);
  g_variant_unref(properties);
  g_variant_unref(reply);
  if (!ours) return 126; // Never change a physical input device.
  if (already_mapped) return 0;
  reply = g_dbus_connection_call_sync(
    bus, owner, path, "org.freedesktop.DBus.Properties", "Set",
    g_variant_new("(ssv)", "org.kde.KWin.InputDevice", "outputName", g_variant_new_string(output)),
    G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 200, NULL, NULL);
  if (!reply) return 1;
  g_variant_unref(reply);
  reply = g_dbus_connection_call_sync(
    bus, owner, path, "org.freedesktop.DBus.Properties", "Get",
    g_variant_new("(ss)", "org.kde.KWin.InputDevice", "outputName"), G_VARIANT_TYPE("(v)"),
    G_DBUS_CALL_FLAGS_NO_AUTO_START, 200, NULL, NULL);
  if (!reply) return 1;
  GVariant *assigned_value;
  g_variant_get(reply, "(v)", &assigned_value);
  const bool mapped = g_variant_is_of_type(assigned_value, G_VARIANT_TYPE_STRING) &&
    !strcmp(g_variant_get_string(assigned_value, NULL), output);
  g_variant_unref(assigned_value);
  g_variant_unref(reply);
  return mapped ? 0 : 1;
}

static int touch_map_once(GDBusConnection *bus, const char *node, const char *output) {
  // Pin validation, mutation and readback to one compositor. A replacement
  // must never receive a Set authorized against the preceding owner's device.
  GVariant *reply = g_dbus_connection_call_sync(
    bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
    g_variant_new("(s)", "org.kde.KWin"), G_VARIANT_TYPE("(s)"),
    G_DBUS_CALL_FLAGS_NONE, 200, NULL, NULL);
  if (!reply) return 1;
  const char *owner = NULL;
  g_variant_get(reply, "(&s)", &owner);
  const int result = touch_map_device(bus, owner, node, output);
  g_variant_unref(reply);
  return result;
}

static int touch_map_run(const char *node, const char *output) {
  if (!touch_map_arguments_are_safe(node, output)) return 126;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus) return 1;
  GVariant *owner = g_dbus_connection_call_sync(
    bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
    g_variant_new("(s)", "org.kde.KWin"), G_VARIANT_TYPE("(b)"),
    G_DBUS_CALL_FLAGS_NONE, 200, NULL, NULL);
  const bool owner_queried = owner != NULL;
  gboolean available = FALSE;
  if (owner) {
    g_variant_get(owner, "(b)", &available);
    g_variant_unref(owner);
  }
  if (!available) {
    g_object_unref(bus);
    return owner_queried ? 3 : 1; // Other compositors retain their usual touch handling.
  }
  const gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
  int result;
  do {
    result = touch_map_once(bus, node, output);
    if (result != 1) break;
    g_usleep(50000);
  } while (g_get_monotonic_time() < deadline);
  g_object_unref(bus);
  return result;
}
