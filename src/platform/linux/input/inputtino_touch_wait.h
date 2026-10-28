#pragma once

#include <gio/gio.h>

namespace platf::touch {
  // The caller may force-exit its owned child during shutdown. Its connection
  // to the session broker then closes, cancelling the generation-bound helper.
  inline int wait_for_mapping(GSubprocess *process, unsigned timeout_ms = 4000) {
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GMainLoop *loop = g_main_loop_new(context, FALSE);
    g_subprocess_wait_async(process, nullptr, [](GObject *source, GAsyncResult *result, gpointer data) {
      g_subprocess_wait_finish(G_SUBPROCESS(source), result, nullptr);
      g_main_loop_quit(static_cast<GMainLoop *>(data));
    },
                            loop);
    GSource *deadline = g_timeout_source_new(timeout_ms);
    g_source_set_callback(deadline, [](gpointer data) -> gboolean {
      g_subprocess_force_exit(G_SUBPROCESS(data));
      return G_SOURCE_REMOVE;
    },
                          process,
                          nullptr);
    g_source_attach(deadline, context);
    g_main_loop_run(loop);
    g_source_destroy(deadline);
    g_source_unref(deadline);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    return g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process) : 1;
  }
}  // namespace platf::touch
