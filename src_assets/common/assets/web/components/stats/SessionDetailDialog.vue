<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiDelete, apiGet } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  InlineAlert,
  LoadingSkeleton,
  StatusBadge,
} from '@/components/ui';
import type { SessionDetail, SessionSummary } from '@/types/sessions';
import { formatBitrate, formatDuration } from '@/utils/format';

import { mergeSessionDetails, samplesToPerformancePoints } from './historyUtils';
import SessionPerformanceCharts from './SessionPerformanceCharts.vue';
import type { PerformancePoint } from './types';

const props = defineProps<{
  open: boolean;
  summary: SessionSummary | null;
  members?: SessionSummary[];
}>();

const emit = defineEmits<{
  'update:open': [value: boolean];
  deleted: [uuid: string];
}>();

const { locale, t } = useI18n();
const dialog = ref<HTMLDialogElement | null>(null);
const panel = ref<HTMLElement | null>(null);
const detail = ref<SessionDetail | null>(null);
const loading = ref(false);
const error = ref('');
const exportBusy = ref(false);
const deleteBusy = ref(false);
const deleteConfirmOpen = ref(false);
let requestGeneration = 0;
let restoreFocusTo: HTMLElement | null = null;

const selectedMembers = computed(() =>
  (props.members?.length ?? 0) > 1 ? (props.members ?? []) : props.summary ? [props.summary] : [],
);
const isGroup = computed(() => selectedMembers.value.length > 1);

const performancePoints = computed<PerformancePoint[]>(() => {
  return detail.value
    ? samplesToPerformancePoints(detail.value.samples ?? [], detail.value.protocol)
    : [];
});

function close(): void {
  emit('update:open', false);
}

function onCancel(event: Event): void {
  event.preventDefault();
  close();
}

function onBackdrop(event: MouseEvent): void {
  if (event.target === dialog.value) close();
}

async function fetchDetails(uuids: string[]): Promise<SessionDetail[]> {
  const details: SessionDetail[] = [];
  let next = 0;
  const worker = async (): Promise<void> => {
    while (next < uuids.length) {
      const index = next++;
      const uuid = uuids[index];
      if (!uuid) continue;
      const result = await apiGet<SessionDetail>(
        `/api/history/sessions/${encodeURIComponent(uuid)}?full=1`,
      );
      details.push({ ...result, samples: result.samples ?? [], events: result.events ?? [] });
    }
  };
  await Promise.all(Array.from({ length: Math.min(3, uuids.length) }, () => worker()));
  return details;
}

async function load(uuid: string): Promise<void> {
  const generation = ++requestGeneration;
  detail.value = null;
  error.value = '';
  loading.value = true;
  try {
    const members = selectedMembers.value;
    const results =
      members.length > 1
        ? await fetchDetails(members.map((member) => member.uuid))
        : await fetchDetails([uuid]);
    if (generation === requestGeneration && results.length) {
      detail.value = results.length > 1 ? mergeSessionDetails(results) : (results[0] ?? null);
    }
  } catch {
    if (generation === requestGeneration)
      error.value = t('ui.sessions.error.source_load', {
        source: t('sessions.history_detail_title'),
      });
  } finally {
    if (generation === requestGeneration) loading.value = false;
  }
}

function buildExportFilename(source: SessionDetail): string {
  const safeName = (source.app_name || source.client_name || 'session')
    .replace(/[^a-z0-9_-]+/gi, '_')
    .slice(0, 40);
  const timestamp = new Date((source.start_time_unix || Date.now() / 1000) * 1000)
    .toISOString()
    .replace(/[:.]/g, '-')
    .slice(0, 19);
  return `vibepollo-session-${safeName}-${timestamp}.json`;
}

async function exportJson(): Promise<void> {
  if (!detail.value || exportBusy.value) return;
  exportBusy.value = true;
  error.value = '';
  let url = '';
  try {
    const members = selectedMembers.value;
    const results =
      members.length > 1
        ? await fetchDetails(members.map((member) => member.uuid))
        : await fetchDetails([detail.value.uuid]);
    if (!results.length) throw new Error('No session data was returned');
    const exportDetail = results.length > 1 ? mergeSessionDetails(results) : results[0]!;
    const payload =
      members.length > 1
        ? { ...exportDetail, session_members: members.map((member) => member.uuid) }
        : exportDetail;
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
    url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = buildExportFilename(exportDetail);
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
  } catch {
    error.value = t('ui.sessions.error.source_load', {
      source: t('sessions.history_export_json'),
    });
  } finally {
    if (url) URL.revokeObjectURL(url);
    exportBusy.value = false;
  }
}

async function confirmDelete(): Promise<void> {
  const uuid = props.summary?.uuid;
  if (!uuid || isGroup.value || deleteBusy.value) return;
  deleteBusy.value = true;
  error.value = '';
  try {
    await apiDelete(`/api/history/sessions/${encodeURIComponent(uuid)}`);
    deleteConfirmOpen.value = false;
    emit('deleted', uuid);
    close();
  } catch {
    error.value = t('ui.sessions.error.action');
  } finally {
    deleteBusy.value = false;
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
                :label="
                  isGroup
                    ? t('sessions.history_group_title', { count: selectedMembers.length })
                    : (summary.protocol || t('_common.unknown')).toUpperCase()
                "
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
            <template v-if="detail">
              <AppButton
                :label="t('sessions.history_export_json')"
                icon="download"
                variant="secondary"
                :busy="exportBusy"
                :busy-label="t('_common.loading')"
                @click="exportJson"
              />
              <AppButton
                v-if="!isGroup"
                :label="t('sessions.history_delete')"
                icon="trash"
                variant="danger"
                :disabled="deleteBusy"
                @click="deleteConfirmOpen = true"
              />
            </template>
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
                  <h3>{{ t('sessions.history_performance_title', 'Session performance') }}</h3>
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
                :host-samples="detail.samples"
                :events="detail.events"
                mode="history"
              />
              <p v-else class="detail-empty">{{ t('sessions.history_no_samples') }}</p>
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
  <ConfirmDialog
    v-model:open="deleteConfirmOpen"
    :title="t('sessions.history_delete')"
    :description="t('sessions.history_delete_confirm')"
    :confirm-label="t('sessions.history_delete_confirm_yes')"
    :cancel-label="t('sessions.history_delete_confirm_no')"
    tone="danger"
    :busy="deleteBusy"
    :close-on-confirm="false"
    @confirm="confirmDelete"
  />
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

.stats-detail-dialog__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
  margin-left: auto;
}

.stats-detail-dialog__eyebrow {
  display: flex;
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

  .detail-summary {
    grid-template-columns: minmax(0, 1fr);
  }

  .detail-summary > div + div {
    border-top: 1px solid var(--vs-color-border-subtle);
    border-left: 0;
  }
}
</style>
