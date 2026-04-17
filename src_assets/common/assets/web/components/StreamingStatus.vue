<template>
  <div class="flex items-center gap-2 text-xs">
    <span
      :class="[
        'h-2.5 w-2.5 rounded-full',
        streaming ? 'bg-success animate-pulse ring-2 ring-success/30' : 'bg-dark/30 dark:bg-light/30',
      ]"
    />
    <span class="font-medium" v-text="streaming ? 'Live' : 'Idle'" />
  </div>
</template>
<script setup lang="ts">
import { ref, onMounted, onBeforeUnmount } from 'vue';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';

const streaming = ref(false);
let iv: ReturnType<typeof setInterval> | undefined;

async function poll(): Promise<void> {
  const auth = useAuthStore();
  if (!auth.isAuthenticated) return;
  try {
    const [rtsp, webrtc] = await Promise.all([
      http.get('./api/session/status', { validateStatus: () => true }),
      http.get('./api/webrtc/sessions', { validateStatus: () => true }),
    ]);
    const rtspActive = rtsp.status === 200 && (rtsp.data?.activeSessions ?? 0) > 0;
    const webrtcActive =
      webrtc.status === 200 && Array.isArray(webrtc.data?.sessions) && webrtc.data.sessions.length > 0;
    streaming.value = rtspActive || webrtcActive;
  } catch {
    // ignore — will retry next interval
  }
}

onMounted(async () => {
  const auth = useAuthStore();
  await auth.waitForAuthentication();
  await poll();
  iv = setInterval(poll, 5000);
});
onBeforeUnmount(() => {
  if (iv) clearInterval(iv);
});
</script>
<style scoped></style>
