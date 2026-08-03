<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet } from '@/api/client';
import { AppButton, InlineAlert, LoadingSkeleton, StatusBadge, UiIcon } from '@/components/ui';
import type { SessionDetail, SessionSample, SessionSummary } from '@/types/sessions';
import { formatBitrate, formatDuration } from '@/utils/format';

import MetricChart from './MetricChart.vue';
import SessionPerformanceCharts from './SessionPerformanceCharts.vue';
import type { PerformancePoint } from './types';

const props = defineProps<{
  open: boolean;
  summary: SessionSummary | null;
}>();

const emit = defineEmits<{
  'update:open': [value: boolean];
  delete: [summary: SessionSummary];
}>();

const { locale, t } = useI18n();
const dialog = ref<HTMLDialogElement | null>(null);
const panel = ref<HTMLElement | null>(null);
const detail = ref<SessionDetail | null>(null);
const loading = ref(false);
const error = ref('');
let requestGeneration = 0;
let restoreFocusTo: HTMLElement | null = null;

const performancePoints = computed<PerformancePoint[]>(() => {
  const samples = detail.value?.samples ?? [];
  const isWebRtc = detail.value?.protocol?.toLocaleLowerCase() === 'webrtc';
  return samples.map((sample, index) => {
    const previous = samples[index - 1];
    const qualityEvents = previous
      ? isWebRtc
        ? Math.max(0, sample.video_dropped - previous.video_dropped) +
          Math.max(0, sample.audio_dropped - previous.audio_dropped)
        : Math.max(0, sample.client_reported_losses - previous.client_reported_losses) +
          Math.max(0, sample.idr_requests - previous.idr_requests) +
          Math.max(0, sample.ref_invalidations - previous.ref_invalidations)
      : 0;
    return {
      timestamp: sample.timestamp_unix * 1000,
      latencyMs: Number.isFinite(sample.encode_latency_ms) ? sample.encode_latency_ms : null,
      throughputMbps: Math.max(0, sample.actual_bitrate_kbps / 1000),
      qualityEvents,
      fps: Math.max(0, sample.actual_fps),
    };
  });
});

function sampleValues(field: keyof SessionSample): number[] {
  return (detail.value?.samples ?? []).flatMap((sample) => {
    const value = sample[field];
    return typeof value === 'number' && Number.isFinite(value) ? [value] : [];
  });
}

function latestValue(field: keyof SessionSample, suffix = '%'): string {
  const values = sampleValues(field);
  const value = values.at(-1);
  return value == null
    ? '—'
    : `${value.toLocaleString(undefined, { maximumFractionDigits: 1 })}${suffix}`;
}

function close(): void {
  emit('update:open', false);
}

function requestDelete(): void {
  if (props.summary) emit('delete', props.summary);
}

function onCancel(event: Event): void {
  event.preventDefault();
  close();
}

function onBackdrop(event: MouseEvent): void {
  if (event.target === dialog.value) close();
}

async function load(uuid: string): Promise<void> {
  const generation = ++requestGeneration;
  detail.value = null;
  error.value = '';
  loading.value = true;
  try {
    const result = await apiGet<SessionDetail>(
      `/api/history/sessions/${encodeURIComponent(uuid)}?full=1`,
    );
    if (generation === requestGeneration) detail.value = result;
  } catch {
    if (generation === requestGeneration)
      error.value = t('ui.sessions.error.source_load', {
        source: t('sessions.history_detail_title'),
      });
  } finally {
    if (generation === requestGeneration) loading.value = false;
  }
}

watch(
  () => props.open,
  async (open) => {
    const element = dialog.value;
    if (open && element) {
      restoreFocusTo =
        document.activeElement instanceof HTMLElement ? document.activeElement : null;
      if (!element.open) element.showModal();
      await nextTick();
      panel.value?.focus();
      if (props.summary?.uuid) void load(props.summary.uuid);
    } else if (!open && element?.open) {
      element.close();
      const focusTarget = restoreFocusTo;
      restoreFocusTo = null;
      if (focusTarget?.isConnected) nextTick(() => focusTarget.focus());
    }
  },
  { flush: 'post' },
);

watch(
  () => props.summary?.uuid,
  (uuid) => {
    if (props.open && uuid) void load(uuid);
  },
);

onBeforeUnmount(() => {
  requestGeneration += 1;
  if (dialog.value?.open) dialog.value.close();
});
</script>

<template>
  <Teleport to="body">
    <dialog ref="dialog" class="stats-detail-dialog" @cancel="onCancel" @click="onBackdrop">
      <section ref="panel" class="stats-detail-dialog__panel" tabindex="-1">
        <header class="stats-detail-dialog__header">
          <div>
            <div class="stats-detail-dialog__eyebrow">
              <StatusBadge
                v-if="summary"
                :label="(summary.protocol || t('_common.unknown')).toUpperCase()"
                tone="info"
                compact
              />
              <StatusBadge
                v-if="summary?.verdict"
                :label="t(`sessions.history_verdict_${summary.verdict}`)"
                :tone="
                  summary.verdict === 'healthy'
                    ? 'success'
                    : summary.verdict === 'failed'
                      ? 'danger'
                      : 'warning'
                "
                compact
              />
            </div>
            <h2>{{ summary?.app_name || t('sessions.history_detail_title') }}</h2>
            <p>
              {{
                summary?.client_name ||
                summary?.device_name ||
                t('ui.sessions.value.unknown_client')
              }}
            </p>
          </div>
          <div class="stats-detail-dialog__actions">
            <AppButton
              :label="t('ui.sessions.action.delete_record')"
              icon="trash"
              variant="tertiary"
              size="compact"
              :disabled="!summary"
              @click="requestDelete"
            />
            <AppButton
              :label="t('_common.close')"
              :aria-label="t('ui.stats.close_detail')"
              icon="x"
              icon-only
              variant="tertiary"
              @click="close"
            />
          </div>
        </header>

        <div class="stats-detail-dialog__body">
          <InlineAlert
            v-if="error"
            tone="danger"
            :title="t('ui.sessions.alert.partial_failure_title')"
          >
            {{ error }}
          </InlineAlert>

          <div v-if="loading" class="stats-detail-dialog__loading">
            <LoadingSkeleton variant="block" height="8rem" />
            <LoadingSkeleton variant="block" height="18rem" />
          </div>

          <template v-else-if="detail">
            <dl class="detail-summary">
              <div>
                <dt>{{ t('sessions.history_resolution') }}</dt>
                <dd>{{ detail.width }} × {{ detail.height }} @ {{ detail.target_fps }}</dd>
              </div>
              <div>
                <dt>{{ t('sessions.codec') }}</dt>
                <dd>{{ detail.codec || t('_common.unknown') }}</dd>
              </div>
              <div>
                <dt>{{ t('sessions.bitrate') }}</dt>
                <dd>{{ formatBitrate(detail.encoder_bitrate_kbps, locale) }}</dd>
              </div>
              <div>
                <dt>{{ t('sessions.history_duration') }}</dt>
                <dd>{{ formatDuration(detail.duration_seconds, locale) }}</dd>
              </div>
            </dl>

            <InlineAlert
              v-if="detail.samples_truncated || detail.events_truncated"
              tone="warning"
              :title="t('sessions.history_detail_title')"
            >
              <span v-if="detail.samples_truncated">
                {{
                  t('sessions.history_samples_truncated', {
                    shown: detail.samples.length,
                    total: detail.total_samples ?? detail.samples.length,
                  })
                }}
              </span>
              <span v-if="detail.events_truncated">
                {{
                  t('sessions.history_events_truncated', {
                    shown: detail.events.length,
                    total: detail.total_events ?? detail.events.length,
                  })
                }}
              </span>
            </InlineAlert>

            <section class="detail-section">
              <div class="detail-section__heading">
                <div>
                  <h3>{{ t('sessions.active_unified') }}</h3>
                  <p>{{ t('stats.subtitle') }}</p>
                </div>
                <span>{{
                  t(
                    'ui.stats.sample_count',
                    { count: detail.samples.length },
                    detail.samples.length,
                  )
                }}</span>
              </div>
              <SessionPerformanceCharts
                v-if="performancePoints.length"
                :points="performancePoints"
                :protocol="detail.protocol"
                :target-fps="detail.target_fps"
              />
              <p v-else class="detail-empty">{{ t('sessions.history_no_samples') }}</p>
            </section>

            <section v-if="detail.samples.length" class="detail-section">
              <div class="detail-section__heading">
                <div>
                  <h3>{{ t('sessions.chart_host_compute') }}</h3>
                  <p>{{ t('sessions.tip_chart_host_compute') }}</p>
                </div>
              </div>
              <div class="host-detail-charts">
                <MetricChart
                  :title="t('sessions.chart_host_cpu')"
                  :value="latestValue('host_cpu_percent')"
                  :values="sampleValues('host_cpu_percent')"
                  unit="%"
                  :ceiling="100"
                  color="var(--vs-color-status-info)"
                />
                <MetricChart
                  :title="t('sessions.chart_host_gpu')"
                  :value="latestValue('host_gpu_percent')"
                  :values="sampleValues('host_gpu_percent')"
                  unit="%"
                  :ceiling="100"
                  color="var(--vs-color-status-success)"
                />
                <MetricChart
                  :title="t('sessions.chart_host_ram')"
                  :value="latestValue('host_ram_percent')"
                  :values="sampleValues('host_ram_percent')"
                  unit="%"
                  :ceiling="100"
                  color="var(--vs-color-status-warning)"
                />
                <MetricChart
                  :title="t('sessions.chart_host_vram')"
                  :value="latestValue('host_vram_percent')"
                  :values="sampleValues('host_vram_percent')"
                  unit="%"
                  :ceiling="100"
                  color="var(--vs-color-data-accent)"
                />
              </div>
            </section>

            <section class="detail-section">
              <div class="detail-section__heading">
                <div>
                  <h3>{{ t('sessions.history_events') }}</h3>
                </div>
              </div>
              <ol v-if="detail.events.length" class="event-list">
                <li
                  v-for="event in detail.events"
                  :key="`${event.timestamp_unix}:${event.event_type}`"
                >
                  <span class="event-list__marker" />
                  <div>
                    <strong>{{ event.event_type.replaceAll('_', ' ') }}</strong>
                    <time>{{ new Date(event.timestamp_unix * 1000).toLocaleString(locale) }}</time>
                    <p v-if="event.payload">{{ event.payload }}</p>
                  </div>
                </li>
              </ol>
              <p v-else class="detail-empty">{{ t('sessions.history_no_events') }}</p>
            </section>
          </template>
        </div>
      </section>
    </dialog>
  </Teleport>
</template>

<style scoped>
.stats-detail-dialog {
  width: min(94vw, 86rem);
  max-width: none;
  height: min(92vh, 68rem);
  max-height: none;
  padding: 0;
  overflow: hidden;
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-canvas);
  color: var(--vs-color-text-primary);
  box-shadow: var(--vs-shadow-overlay);
}

.stats-detail-dialog::backdrop {
  background: rgb(0 0 0 / 0.68);
  backdrop-filter: blur(3px);
}

.stats-detail-dialog__panel {
  display: grid;
  height: 100%;
  grid-template-rows: auto minmax(0, 1fr);
  outline: none;
}

.stats-detail-dialog__header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-20);
  padding: var(--vs-space-20) var(--vs-space-24);
  border-bottom: 1px solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-surface);
}

.stats-detail-dialog__header h2 {
  margin-top: var(--vs-space-8);
  font-size: var(--vs-type-size-page);
  line-height: var(--vs-type-line-height-page);
}

.stats-detail-dialog__header p {
  margin-top: var(--vs-space-2);
  color: var(--vs-color-text-secondary);
}

.stats-detail-dialog__eyebrow {
  display: flex;
  gap: var(--vs-space-8);
}

.stats-detail-dialog__actions {
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
}

.stats-detail-dialog__body {
  display: grid;
  align-content: start;
  gap: var(--vs-space-24);
  padding: var(--vs-space-24);
  overflow: auto;
}

.stats-detail-dialog__loading {
  display: grid;
  gap: var(--vs-space-16);
}

.detail-summary {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.detail-summary > div {
  padding: var(--vs-space-16);
}

.detail-summary > div + div {
  border-left: 1px solid var(--vs-color-border-subtle);
}

.detail-summary dt {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-semibold);
  text-transform: uppercase;
}

.detail-summary dd {
  margin-top: var(--vs-space-4);
  font-variant-numeric: tabular-nums;
}

.detail-section {
  display: grid;
  gap: var(--vs-space-12);
}

.detail-section__heading {
  display: flex;
  align-items: end;
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.detail-section__heading h3 {
  font-size: var(--vs-type-size-section);
}

.detail-section__heading p,
.detail-section__heading > span,
.detail-empty {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}

.host-detail-charts {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.event-list {
  display: grid;
  gap: 0;
  padding: 0;
  list-style: none;
}

.event-list li {
  display: grid;
  position: relative;
  grid-template-columns: 1rem minmax(0, 1fr);
  gap: var(--vs-space-12);
  padding-bottom: var(--vs-space-16);
}

.event-list li:not(:last-child)::before {
  position: absolute;
  top: 0.8rem;
  bottom: 0;
  left: 0.34rem;
  width: 1px;
  background: var(--vs-color-border-strong);
  content: '';
}

.event-list__marker {
  width: 0.7rem;
  height: 0.7rem;
  margin-top: 0.3rem;
  border: 2px solid var(--vs-color-accent-default);
  border-radius: 50%;
  background: var(--vs-color-bg-canvas);
  z-index: 1;
}

.event-list strong {
  text-transform: capitalize;
}

.event-list time {
  display: block;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.event-list p {
  margin-top: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-family: var(--vs-type-family-mono);
  font-size: var(--vs-type-size-helper);
  overflow-wrap: anywhere;
}

@media (max-width: 1023px) {
  .host-detail-charts,
  .detail-summary {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .detail-summary > div:nth-child(3) {
    border-top: 1px solid var(--vs-color-border-subtle);
    border-left: 0;
  }

  .detail-summary > div:nth-child(4) {
    border-top: 1px solid var(--vs-color-border-subtle);
  }
}

@media (max-width: 639px) {
  .stats-detail-dialog {
    width: 100vw;
    height: 100dvh;
    border: 0;
    border-radius: 0;
  }

  .stats-detail-dialog__header,
  .stats-detail-dialog__body {
    padding: var(--vs-space-16);
  }

  .host-detail-charts,
  .detail-summary {
    grid-template-columns: minmax(0, 1fr);
  }

  .detail-summary > div + div {
    border-top: 1px solid var(--vs-color-border-subtle);
    border-left: 0;
  }
}
</style>
