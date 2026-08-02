<template>
  <n-card class="mb-8" :segmented="{ content: true, footer: false }">
    <template #header>
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div class="flex items-center gap-3">
          <h2 class="text-lg font-medium flex items-center gap-2">
            <i class="fas fa-broadcast-tower" /> {{ t('sessions.title') }}
          </h2>
          <span
            :class="[
              'h-2.5 w-2.5 rounded-full',
              hasActiveSessions
                ? 'bg-success animate-pulse ring-2 ring-success/30'
                : 'bg-dark/30 dark:bg-light/30',
            ]"
          />
          <span class="text-xs font-medium opacity-75">
            {{ hasActiveSessions ? t('sessions.live') : t('sessions.idle') }}
          </span>
        </div>
        <n-button size="small" :loading="loading" @click="refresh">
          <i class="fas fa-rotate" />
          <span class="ml-2">{{ t('sessions.refresh') }}</span>
        </n-button>
      </div>
    </template>

    <div v-if="!hasActiveSessions && !loading" class="text-sm opacity-60 py-2">
      {{ t('sessions.no_active') }}
    </div>

    <ActiveRtspSessionsSection
      :sessions="rtspSessions"
      :rtsp-count="rtspCount"
      :app-running="appRunning"
      :app-name="appName"
      :show-charts="showCharts"
      @update:show-charts="showCharts = $event"
    />

    <ActiveWebRtcSessionsSection
      :sessions="webrtcSessions"
      :show-charts="showWebrtcCharts"
      @update:show-charts="showWebrtcCharts = $event"
    />
  </n-card>
</template>

<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { NButton, NCard } from 'naive-ui';
import { useAuthStore } from '@/stores/auth';
import { useSessionsStore } from '@/stores/sessions';
import { storeToRefs } from 'pinia';
import ActiveRtspSessionsSection from './session/ActiveRtspSessionsSection.vue';
import ActiveWebRtcSessionsSection from './session/ActiveWebRtcSessionsSection.vue';

const { t } = useI18n();
const auth = useAuthStore();
const sessionsStore = useSessionsStore();
const { rtspSessions, webrtcSessions, rtspCount, appRunning, appName, loading, hasActiveSessions } =
  storeToRefs(sessionsStore);

const showCharts = ref(false);
const showWebrtcCharts = ref(false);
let unmounted = false;

async function refresh(): Promise<void> {
  await sessionsStore.refresh();
}

onMounted(async () => {
  await auth.waitForAuthentication();
  if (unmounted) {
    return;
  }
  sessionsStore.startPolling();
});

onBeforeUnmount(() => {
  unmounted = true;
  sessionsStore.stopPolling();
});
</script>
