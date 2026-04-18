<template>
  <n-drawer v-model:show="visibleModel" :width="680" placement="right">
    <n-drawer-content
      :title="t('sessions.history_detail_title')"
      closable
      :native-scrollbar="false"
    >
      <template #footer>
        <div v-if="detail" class="flex items-center justify-end gap-2 w-full">
          <n-button size="small" tertiary @click="exportJson">
            <template #icon><i class="fas fa-file-export" /></template>
            {{ t('sessions.history_export_json') }}
          </n-button>
          <n-popconfirm
            :positive-text="t('sessions.history_delete_confirm_yes')"
            :negative-text="t('sessions.history_delete_confirm_no')"
            @positive-click="confirmDelete"
          >
            <template #trigger>
              <n-button size="small" type="error" :loading="deleting">
                <template #icon><i class="fas fa-trash" /></template>
                {{ t('sessions.history_delete') }}
              </n-button>
            </template>
            {{ t('sessions.history_delete_confirm') }}
          </n-popconfirm>
        </div>
      </template>
      <div v-if="loading && !detail" class="flex items-center justify-center py-10">
        <n-spin size="medium" />
      </div>

      <div v-else-if="detail">
        <!-- Session metadata header -->
        <div class="space-y-4 mb-6">
          <div class="flex flex-wrap items-center gap-2">
            <n-tag
              size="small"
              :bordered="false"
              :type="detail.protocol === 'rtsp' ? 'info' : 'warning'"
            >
              {{ (detail.protocol || '').toUpperCase() || '—' }}
            </n-tag>
            <n-tag v-if="detail.hdr" size="small" :bordered="false" type="warning">HDR</n-tag>
            <n-tag
              v-if="detail.verdict"
              size="small"
              :bordered="false"
              :type="verdictType(detail.verdict)"
            >
              {{ verdictLabel(detail.verdict) }}
            </n-tag>
          </div>

          <div class="grid grid-cols-2 sm:grid-cols-3 gap-3">
            <StatCell
              :label="t('sessions.history_client')"
              :value="detail.client_name || detail.device_name || '—'"
              :tip="t('sessions.tip_history_client')"
            />
            <StatCell
              v-if="detail.device_name && detail.client_name"
              :label="t('sessions.history_device')"
              :value="detail.device_name"
              :tip="t('sessions.tip_history_device')"
            />
            <StatCell
              :label="t('sessions.history_resolution')"
              :value="`${detail.width}×${detail.height}@${detail.target_fps}`"
              :tip="t('sessions.tip_resolution')"
            />
            <StatCell
              :label="t('sessions.codec')"
              :value="detail.codec || '—'"
              :tip="t('sessions.tip_codec')"
            />
            <StatCell
              :label="t('sessions.bitrate')"
              :value="formatBitrate(detail.target_bitrate_kbps)"
              :tip="t('sessions.tip_history_bitrate')"
            />
            <StatCell
              :label="t('sessions.history_duration')"
              :value="formatDuration(detail.duration_seconds)"
              :tip="t('sessions.tip_history_duration')"
            />
            <StatCell
              v-if="detail.app_name"
              :label="t('sessions.history_app')"
              :value="detail.app_name"
              :tip="t('sessions.tip_history_app')"
            />
            <StatCell
              :label="t('sessions.audio_channels')"
              :value="`${detail.audio_channels}ch`"
              :tip="t('sessions.tip_audio_channels')"
            />
          </div>
        </div>

        <!-- Performance Charts -->
        <SessionCharts
          v-if="(detail.samples?.length ?? 0) > 0"
          mode="history"
          :history-data="detail.samples"
          :events="detail.events"
          :protocol="detail.protocol === 'webrtc' ? 'webrtc' : 'rtsp'"
        />
        <n-empty v-else :description="t('sessions.history_no_samples')" size="small" class="my-4" />

        <!-- Event Timeline -->
        <div class="mt-6">
          <h3 class="text-sm font-semibold mb-3 flex items-center gap-2">
            <i class="fas fa-stream" /> {{ t('sessions.history_events') }}
          </h3>
          <n-empty
            v-if="!detail.events || detail.events.length === 0"
            :description="t('sessions.history_no_events')"
            size="small"
          />
          <n-timeline v-else>
            <n-timeline-item
              v-for="(event, idx) in detail.events"
              :key="idx"
              :type="eventTimelineType(event.event_type)"
              :title="event.event_type"
              v-bind="event.payload ? { content: event.payload } : {}"
              :time="formatEventTime(event.timestamp_unix)"
            />
          </n-timeline>
        </div>
      </div>

      <n-empty v-else :description="t('sessions.history_empty')" />
    </n-drawer-content>
  </n-drawer>
</template>

<script setup lang="ts">
import { ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import {
  NButton,
  NDrawer,
  NDrawerContent,
  NEmpty,
  NPopconfirm,
  NSpin,
  NTag,
  NTimeline,
  NTimelineItem,
} from 'naive-ui';
import { fetchSessionDetail, deleteSessionHistory } from '@/services/sessionsApi';
import type { SessionDetail } from '@/types/sessions';
import SessionCharts from './SessionCharts.vue';
import StatCell from './StatCell.vue';

const { t } = useI18n();

const props = defineProps<{
  uuid: string;
  visible: boolean;
}>();

const emit = defineEmits<{
  (e: 'update:visible', value: boolean): void;
  (e: 'deleted', uuid: string): void;
}>();

const visibleModel = ref(props.visible);
watch(
  () => props.visible,
  (v) => {
    visibleModel.value = v;
  },
);
watch(visibleModel, (v) => {
  emit('update:visible', v);
});

const detail = ref<SessionDetail>();
const loading = ref(false);
const deleting = ref(false);
let lastLoadedUuid = '';

function exportJson(): void {
  if (!detail.value) return;
  const blob = new Blob([JSON.stringify(detail.value, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  const safeName = (detail.value.app_name || detail.value.client_name || 'session')
    .replace(/[^a-z0-9_-]+/gi, '_')
    .slice(0, 40);
  const ts = new Date((detail.value.start_time_unix ?? Date.now() / 1000) * 1000)
    .toISOString()
    .replace(/[:.]/g, '-')
    .slice(0, 19);
  a.href = url;
  a.download = `sunshine-session-${safeName}-${ts}.json`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

async function confirmDelete(): Promise<void> {
  if (!detail.value || !props.uuid) return;
  deleting.value = true;
  try {
    await deleteSessionHistory(props.uuid);
    emit('deleted', props.uuid);
    visibleModel.value = false;
    detail.value = undefined;
    lastLoadedUuid = '';
  } catch (e) {
    console.error('Failed to delete session', e);
  } finally {
    deleting.value = false;
  }
}

async function loadDetail(uuid: string): Promise<void> {
  if (!uuid) {
    detail.value = undefined;
    lastLoadedUuid = '';
    return;
  }
  loading.value = true;
  try {
    const result = await fetchSessionDetail(uuid);
    // Defensive: ensure samples/events exist so the template never crashes.
    if (result) {
      result.samples = result.samples ?? [];
      result.events = result.events ?? [];
    }
    detail.value = result;
    lastLoadedUuid = uuid;
  } catch {
    detail.value = undefined;
    lastLoadedUuid = '';
  } finally {
    loading.value = false;
  }
}

// Re-fetch whenever uuid changes OR drawer is (re)opened for the same uuid.
watch(
  [() => props.uuid, () => props.visible],
  ([uuid, visible], [, prevVisible]) => {
    if (!visible) return;
    const opening = !prevVisible && visible;
    if (uuid && (uuid !== lastLoadedUuid || opening)) {
      void loadDetail(uuid);
    }
  },
  { immediate: true },
);

function verdictType(verdict?: string): 'success' | 'warning' | 'error' | 'default' {
  switch (verdict) {
    case 'healthy':
      return 'success';
    case 'degraded':
      return 'warning';
    case 'failed':
      return 'error';
    default:
      return 'default';
  }
}

function verdictLabel(verdict?: string): string {
  switch (verdict) {
    case 'healthy':
      return t('sessions.history_verdict_healthy');
    case 'degraded':
      return t('sessions.history_verdict_degraded');
    case 'failed':
      return t('sessions.history_verdict_failed');
    default:
      return t('sessions.history_verdict_unknown');
  }
}

function formatDuration(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function formatBitrate(kbps: number): string {
  if (kbps >= 1000) return `${(kbps / 1000).toFixed(1)} Mbps`;
  return `${kbps} Kbps`;
}

function formatEventTime(unixTime: number): string {
  return new Date(unixTime * 1000).toLocaleTimeString([], {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

function eventTimelineType(
  eventType: string,
): 'default' | 'success' | 'warning' | 'error' | 'info' {
  if (/error|fail|crash/i.test(eventType)) return 'error';
  if (/warn|degrad/i.test(eventType)) return 'warning';
  if (/start|connect|begin/i.test(eventType)) return 'success';
  if (/end|stop|disconnect/i.test(eventType)) return 'info';
  return 'default';
}
</script>

<style scoped>
.stat-cell {
  @apply rounded-lg bg-dark/[0.04] dark:bg-light/[0.06] px-3 py-2;
}
.stat-label {
  @apply text-[10px] uppercase tracking-wider opacity-60 font-semibold mb-0.5;
}
.stat-value {
  @apply text-sm font-semibold;
}
</style>

<!--
  Naive UI's drawer panel relies on a flex chain (.n-drawer-content { display: flex;
  flex-direction: column; height: 100% }) so that .n-drawer-body can grow with flex:1.
  In some environments that chain breaks and the body collapses to height:0, which
  hides our content even though it's in the DOM. Force the chain explicitly here.
-->
<style>
.n-drawer-content {
  display: flex !important;
  flex-direction: column !important;
  height: 100% !important;
}
.n-drawer-content > .n-drawer-body {
  flex: 1 1 auto !important;
  min-height: 0 !important;
  height: auto !important;
}
.n-drawer-body > .n-drawer-body-content-wrapper {
  height: 100%;
}
</style>
