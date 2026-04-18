<template>
  <n-drawer v-model:show="visibleModel" :width="680" placement="right" :native-scrollbar="false">
    <n-drawer-content :title="t('sessions.history_detail_title')" closable>
      <n-spin :show="loading">
        <template v-if="detail">
          <!-- Session metadata header -->
          <div class="space-y-4 mb-6">
            <div class="flex flex-wrap items-center gap-2">
              <n-tag
                size="small"
                :bordered="false"
                :type="detail.protocol === 'rtsp' ? 'info' : 'warning'"
              >
                {{ detail.protocol.toUpperCase() }}
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
              <div class="stat-cell">
                <div class="stat-label">{{ t('sessions.history_client') }}</div>
                <div class="stat-value">{{ detail.client_name || detail.device_name || '—' }}</div>
              </div>
              <div v-if="detail.device_name && detail.client_name" class="stat-cell">
                <div class="stat-label">{{ t('sessions.resolution') }}</div>
                <div class="stat-value">{{ detail.device_name }}</div>
              </div>
              <div class="stat-cell">
                <div class="stat-label">{{ t('sessions.history_resolution') }}</div>
                <div class="stat-value font-mono">
                  {{ detail.width }}×{{ detail.height }}@{{ detail.target_fps }}
                </div>
              </div>
              <div class="stat-cell">
                <div class="stat-label">{{ t('sessions.codec') }}</div>
                <div class="stat-value">{{ detail.codec }}</div>
              </div>
              <div class="stat-cell">
                <div class="stat-label">{{ t('sessions.bitrate') }}</div>
                <div class="stat-value">{{ formatBitrate(detail.target_bitrate_kbps) }}</div>
              </div>
              <div class="stat-cell">
                <div class="stat-label">{{ t('sessions.history_duration') }}</div>
                <div class="stat-value font-mono">
                  {{ formatDuration(detail.duration_seconds) }}
                </div>
              </div>
              <div v-if="detail.app_name" class="stat-cell">
                <div class="stat-label">{{ t('sessions.history_app') }}</div>
                <div class="stat-value">{{ detail.app_name }}</div>
              </div>
              <div class="stat-cell">
                <div class="stat-label">{{ t('sessions.audio_channels') }}</div>
                <div class="stat-value">{{ detail.audio_channels }}ch</div>
              </div>
            </div>
          </div>

          <!-- Performance Charts -->
          <SessionCharts
            v-if="detail.samples.length > 0"
            mode="history"
            :history-data="detail.samples"
            :events="detail.events"
            :protocol="detail.protocol === 'webrtc' ? 'webrtc' : 'rtsp'"
          />

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
        </template>

        <n-empty v-else-if="!loading" :description="t('sessions.history_empty')" />
      </n-spin>
    </n-drawer-content>
  </n-drawer>
</template>

<script setup lang="ts">
import { ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { NDrawer, NDrawerContent, NEmpty, NSpin, NTag, NTimeline, NTimelineItem } from 'naive-ui';
import { fetchSessionDetail } from '@/services/sessionsApi';
import type { SessionDetail } from '@/types/sessions';
import SessionCharts from './SessionCharts.vue';

const { t } = useI18n();

const props = defineProps<{
  uuid: string;
  visible: boolean;
}>();

const emit = defineEmits<{
  (e: 'update:visible', value: boolean): void;
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

watch(
  () => props.uuid,
  async (uuid) => {
    if (!uuid) return;
    loading.value = true;
    detail.value = undefined;
    try {
      detail.value = await fetchSessionDetail(uuid);
    } catch {
      // Silently ignore
    } finally {
      loading.value = false;
    }
  },
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
