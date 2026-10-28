/** @file Deadline-bounded replies from the generation-bound Linux session helper. */
#pragma once

#include <algorithm>
#include <chrono>
#include <gio/gio.h>
#include <string>
#include <utility>

namespace platf::linux_private_display::helper_process {
  struct result_t {
    bool success {false};
    bool timed_out {false};
    bool completion_unknown {false};
    std::string stdout_text;
    std::string stderr_text;
  };

  inline result_t communicate_until(GSubprocess *process,
                                    std::chrono::steady_clock::time_point deadline,
                                    bool brokered = false) {
    struct reply_t {
      GSubprocess *process;
      GCancellable *cancel;
      bool brokered;
      bool done {false};
      result_t result;
    } reply {process, g_cancellable_new(), brokered};

    // A private context keeps helper timeouts independent of the GUI/default
    // GLib loop, including when multiple startup/query callers use this code.
    auto *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    auto *timer = g_timeout_source_new(static_cast<guint>(std::clamp<long long>(remaining.count(), 1, G_MAXUINT)));
    g_source_set_callback(timer, [](gpointer data) -> gboolean {
      auto &reply = *static_cast<reply_t *>(data);
      reply.result.timed_out = true;
      // The broker normally cancels/reaps KScreen before its shorter deadline
      // and acknowledges completion. This is a transport failsafe: closing
      // the client cannot prove the remote worker has stopped, so the caller
      // must fence subsequent mutations when completion is unknown.
      g_subprocess_force_exit(reply.process);
      g_cancellable_cancel(reply.cancel);
      return G_SOURCE_REMOVE;
    }, &reply, nullptr);
    g_source_attach(timer, context);
    g_subprocess_communicate_utf8_async(process, nullptr, reply.cancel,
      [](GObject *source, GAsyncResult *async_result, gpointer data) {
        auto &reply = *static_cast<reply_t *>(data);
        gchar *out = nullptr, *err = nullptr;
        GError *error = nullptr;
        const bool communicated = g_subprocess_communicate_utf8_finish(
          G_SUBPROCESS(source), async_result, &out, &err, &error);
        if (out) { reply.result.stdout_text = out; g_free(out); }
        if (err) { reply.result.stderr_text = err; g_free(err); }
        if (error) {
          if (!reply.result.stderr_text.empty()) reply.result.stderr_text += ": ";
          reply.result.stderr_text += error->message;
          g_error_free(error);
        }
        if (reply.result.timed_out) reply.result.stderr_text += " (display helper reply timed out)";
        // 125 is VIBEPOLLO_SESSION_COMPLETION_UNKNOWN in the local protocol.
        reply.result.completion_unknown = reply.result.timed_out || !communicated ||
          !g_subprocess_get_if_exited(reply.process) ||
          (reply.brokered && g_subprocess_get_exit_status(reply.process) == 125);
        reply.result.success = communicated && !reply.result.timed_out && g_subprocess_get_successful(reply.process);
        reply.done = true;
      }, &reply);
    while (!reply.done) g_main_context_iteration(context, TRUE);
    g_source_destroy(timer);
    g_source_unref(timer);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    g_object_unref(reply.cancel);
    return std::move(reply.result);
  }
}  // namespace platf::linux_private_display::helper_process
