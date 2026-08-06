<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiGet } from '@/api/client';
import {
  AppButton,
  EmptyState,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
  UiIcon,
  type StatusTone,
} from '@/components/ui';
import type { HostInfo, HostStatsSnapshot } from '@/types/host';
import type { SessionStatus } from '@/types/sessions';
import { useSystemStore } from '@/stores/system';
import { formatBytes } from '@/utils/format';

interface OverviewWarning {
  key: string;
  title: string;
  detail: string;
  to: string;
  action: string;
}

const { locale, t } = useI18n();
const system = useSystemStore();
const session = ref<SessionStatus | null>(null);
const hostStats = ref<HostStatsSnapshot | null>(null);
const hostInfo = ref<HostInfo | null>(null);
const loading = ref(true);
const refreshing = ref(false);
const fetchErrors = ref<string[]>([]);
const lastUpdatedAt = ref<number | null>(null);
let pollTimer: number | undefined;

function errorMessage(cause: unknown, fallback: string): string {
  return cause instanceof ApiError ? fallback : cause instanceof Error ? cause.message : fallback;
}

async function refresh(silent = false): Promise<void> {
  if (refreshing.value) return;
  refreshing.value = true;
  if (!silent) loading.value = true;

  const results = await Promise.allSettled([
    apiGet<SessionStatus>('/api/session/status'),
    apiGet<HostStatsSnapshot>('/api/host/stats'),
    apiGet<HostInfo>('/api/host/info'),
  ]);

  const nextErrors: string[] = [];
  const [sessionResult, statsResult, infoResult] = results;

  if (sessionResult.status === 'fulfilled') {
    session.value = sessionResult.value;
  } else {
    nextErrors.push(errorMessage(sessionResult.reason, t('ui.overview.errors.streamStatus')));
  }

  if (statsResult.status === 'fulfilled') {
    hostStats.value = statsResult.value;
  } else {
    nextErrors.push(errorMessage(statsResult.reason, t('ui.overview.errors.hostMetrics')));
  }

  if (infoResult.status === 'fulfilled') {
    hostInfo.value = infoResult.value;
  } else {
    nextErrors.push(errorMessage(infoResult.reason, t('ui.overview.errors.hostInfo')));
  }

  fetchErrors.value = [...new Set(nextErrors)];
  lastUpdatedAt.value = Date.now();
  refreshing.value = false;
  loading.value = false;
}

const isStreaming = computed(() =>
  Boolean(session.value?.appRunning || (session.value?.activeSessions ?? 0) > 0),
);

const warnings = computed<OverviewWarning[]>(() => {
  const result: OverviewWarning[] = [];
  if (fetchErrors.value.length) {
    result.push({
      key: 'partial-data',
      title: t('ui.overview.warnings.partialData.title'),
      detail: fetchErrors.value[0],
      to: '/logs',
      action: t('ui.overview.actions.openLogs'),
    });
  }
  if (session.value?.paused) {
    result.push({
      key: 'paused-app',
      title: t('ui.overview.warnings.paused.title'),
      detail: t('ui.overview.warnings.paused.detail', {
        app: session.value.appName || t('ui.overview.currentApplication'),
      }),
      to: '/stats',
      action: t('ui.overview.actions.openStats'),
    });
  }
  if (session.value?.lastEncoderProbeFailed) {
    result.push({
      key: 'encoder-probe-failed',
      title: t('ui.overview.warnings.encoderProbe.title'),
      detail: t('ui.overview.warnings.encoderProbe.detail'),
      to: '/logs',
      action: t('ui.overview.actions.openLogs'),
    });
  }
  const hottest = Math.max(hostStats.value?.cpu_percent ?? 0, hostStats.value?.gpu_percent ?? 0);
  if (hottest >= 95) {
    result.push({
      key: 'host-load',
      title: t('ui.overview.warnings.hostLoad.title'),
      detail: t('ui.overview.warnings.hostLoad.detail', { threshold: 95 }),
      to: '/stats',
      action: t('ui.overview.actions.openStats'),
    });
  }
  return result;
});

const readiness = computed<{ label: string; detail: string; tone: StatusTone }>(() => {
  if (isStreaming.value) {
    return {
      label: t('ui.overview.readiness.streaming'),
      detail: session.value?.appName || t('ui.overview.readiness.remoteSessionActive'),
      tone: 'info',
    };
  }
  if (warnings.value.length) {
    return {
      label: t('ui.overview.readiness.attention'),
      detail: warnings.value[0].title,
      tone: 'warning',
    };
  }
  return {
    label: t('ui.overview.readiness.ready'),
    detail: t('ui.overview.readiness.readyDetail'),
    tone: 'success',
  };
});

const lastUpdatedLabel = computed(() =>
  lastUpdatedAt.value
    ? new Intl.DateTimeFormat(locale.value || undefined, {
        hour: 'numeric',
        minute: '2-digit',
      }).format(lastUpdatedAt.value)
    : t('ui.overview.notUpdated'),
);

function percent(value: number | undefined): string {
  return Number.isFinite(value)
    ? new Intl.NumberFormat(locale.value || undefined, {
        maximumFractionDigits: 0,
        style: 'percent',
      }).format((value ?? 0) / 100)
    : t('ui.overview.unavailable');
}

function onVisibilityChange(): void {
  if (document.visibilityState === 'visible') void refresh(true);
}

onMounted(() => {
  void refresh();
  pollTimer = window.setInterval(() => {
    if (document.visibilityState === 'visible') void refresh(true);
  }, 10_000);
  document.addEventListener('visibilitychange', onVisibilityChange);
});

onBeforeUnmount(() => {
  if (pollTimer) window.clearInterval(pollTimer);
  document.removeEventListener('visibilitychange', onVisibilityChange);
});
</script>

<template>
  <div class="page page--wide overview-page">
    <PageHeader :title="t('ui.overview.title')" :description="t('ui.overview.description')">
      <template #meta>
        <span class="overview-updated">{{
          t('ui.overview.updated', { time: lastUpdatedLabel })
        }}</span>
      </template>
      <template #actions>
        <a class="button button--secondary" href="/">
          {{ t('ui.overview.actions.useLegacyWebUi') }}
        </a>
        <AppButton
          icon="refresh"
          :label="t('_common.refresh')"
          variant="secondary"
          :busy="refreshing"
          :busy-label="t('ui.overview.refreshing')"
          @click="refresh()"
        />
      </template>
    </PageHeader>

    <div class="visually-hidden" aria-live="polite" aria-atomic="true">
      {{ readiness.label }}
    </div>

    <template v-if="loading">
      <LoadingSkeleton variant="block" height="168px" :label="t('ui.overview.loadingReadiness')" />
      <div class="overview-summary-grid" aria-hidden="true">
        <LoadingSkeleton variant="block" height="136px" />
        <LoadingSkeleton variant="block" height="136px" />
      </div>
    </template>

    <template v-else>
      <section
        class="readiness-panel"
        :data-tone="readiness.tone"
        aria-labelledby="readiness-title"
      >
        <div class="readiness-panel__state">
          <span class="readiness-panel__icon" aria-hidden="true">
            <UiIcon
              :name="isStreaming ? 'activity' : warnings.length ? 'alert-triangle' : 'check-circle'"
              :size="24"
            />
          </span>
          <div>
            <StatusBadge :label="readiness.label" :tone="readiness.tone" />
            <h2 id="readiness-title">{{ readiness.detail }}</h2>
            <p>
              {{ hostInfo?.cpu_model || t('ui.overview.hardwareUnavailable') }}
              <span v-if="hostInfo?.gpu_model"> &middot; {{ hostInfo.gpu_model }}</span>
            </p>
          </div>
        </div>
        <div class="readiness-panel__actions">
          <RouterLink class="button button--secondary" to="/stats">
            {{ t('ui.overview.actions.openStats') }}
            <UiIcon name="chevron-right" aria-hidden="true" />
          </RouterLink>
          <RouterLink v-if="!isStreaming" class="button button--primary" to="/stream">
            <UiIcon name="play" aria-hidden="true" />
            {{ t('ui.overview.actions.startBrowserStream') }}
          </RouterLink>
        </div>
      </section>

      <div v-if="isStreaming || warnings.length" class="overview-summary-grid">
        <section
          v-if="isStreaming"
          class="overview-summary-card"
          aria-labelledby="active-stream-title"
        >
          <div class="overview-summary-card__heading">
            <span class="summary-icon summary-icon--info" aria-hidden="true"
              ><UiIcon name="activity"
            /></span>
            <StatusBadge :label="t('_common.active')" tone="info" compact />
          </div>
          <h2 id="active-stream-title">{{ session?.appName || t('ui.overview.remoteStream') }}</h2>
          <p>
            {{
              t(
                (session?.activeSessions ?? 0) === 1
                  ? 'ui.overview.activeSessions.one'
                  : 'ui.overview.activeSessions.other',
                { count: session?.activeSessions ?? 0 },
              )
            }}
          </p>
          <RouterLink to="/stats">{{ t('ui.overview.actions.openStats') }}</RouterLink>
        </section>

        <section
          v-for="warning in warnings.slice(0, isStreaming ? 1 : 2)"
          :key="warning.key"
          class="overview-summary-card"
          :aria-labelledby="`warning-${warning.key}`"
        >
          <div class="overview-summary-card__heading">
            <span class="summary-icon summary-icon--warning" aria-hidden="true"
              ><UiIcon name="alert-triangle"
            /></span>
            <StatusBadge :label="t('ui.overview.attention')" tone="warning" compact />
          </div>
          <h2 :id="`warning-${warning.key}`">{{ warning.title }}</h2>
          <p>{{ warning.detail }}</p>
          <RouterLink :to="warning.to">{{ warning.action }}</RouterLink>
        </section>
      </div>

      <InlineAlert
        v-if="fetchErrors.length > 1"
        tone="warning"
        :title="t('ui.overview.additionalDataUnavailable')"
      >
        {{ fetchErrors.slice(1).join(' ') }}
      </InlineAlert>

      <div class="overview-detail-grid">
        <section class="overview-panel" aria-labelledby="host-metrics-title">
          <div class="overview-panel__heading">
            <div>
              <h2 id="host-metrics-title">{{ t('ui.overview.hostLoad.title') }}</h2>
              <p>{{ t('ui.overview.hostLoad.description') }}</p>
            </div>
            <RouterLink to="/stats">{{ t('ui.overview.actions.openStats') }}</RouterLink>
          </div>
          <dl v-if="hostStats" class="metric-grid">
            <div>
              <dt>{{ t('host.cpu') }}</dt>
              <dd>{{ percent(hostStats.cpu_percent) }}</dd>
            </div>
            <div>
              <dt>{{ t('host.gpu') }}</dt>
              <dd>{{ percent(hostStats.gpu_percent) }}</dd>
            </div>
            <div>
              <dt>{{ t('ui.overview.memory') }}</dt>
              <dd>{{ percent(hostStats.ram_percent) }}</dd>
            </div>
            <div>
              <dt>{{ t('host.vram') }}</dt>
              <dd>{{ percent(hostStats.vram_percent) }}</dd>
            </div>
          </dl>
          <EmptyState
            v-else
            compact
            icon="activity"
            :title="t('ui.overview.hostLoad.unavailableTitle')"
            :description="t('ui.overview.hostLoad.unavailableDescription')"
          />
          <p v-if="hostStats" class="metric-footnote">
            {{
              t('ui.overview.hostLoad.memoryInUse', {
                total: formatBytes(hostStats.ram_total_bytes, locale),
                used: formatBytes(hostStats.ram_used_bytes, locale),
              })
            }}
          </p>
        </section>

        <section class="overview-panel overview-action-panel" aria-labelledby="report-bug-title">
          <div class="overview-panel__heading">
            <div>
              <span class="overview-action-panel__icon" aria-hidden="true">
                <UiIcon name="help" :size="20" />
              </span>
              <h2 id="report-bug-title">{{ t('ui.overview.reportBug.title') }}</h2>
              <p>{{ t('ui.overview.reportBug.description') }}</p>
            </div>
          </div>
          <a
            class="button button--secondary button--compact"
            href="https://github.com/Nonary/Vibepollo/issues/new/choose"
            target="_blank"
            rel="noopener noreferrer"
          >
            {{ t('ui.overview.actions.reportBug') }}
            <UiIcon name="external-link" :size="16" aria-hidden="true" />
          </a>
        </section>

        <section class="overview-panel overview-action-panel" aria-labelledby="updates-title">
          <div class="overview-panel__heading">
            <div>
              <span class="overview-action-panel__icon" aria-hidden="true">
                <UiIcon name="download" :size="20" />
              </span>
              <h2 id="updates-title">{{ t('ui.overview.updates.title') }}</h2>
              <p>
                {{
                  t('ui.overview.updates.installed', {
                    version: system.metadata?.version || t('_common.unknown'),
                  })
                }}
              </p>
            </div>
          </div>
          <a
            class="button button--secondary button--compact"
            href="https://github.com/Nonary/Vibepollo/releases/latest"
            target="_blank"
            rel="noopener noreferrer"
          >
            {{ t('ui.overview.actions.checkUpdates') }}
            <UiIcon name="external-link" :size="16" aria-hidden="true" />
          </a>
        </section>
      </div>
    </template>
  </div>
</template>

<style scoped>
.overview-page {
  display: grid;
  gap: var(--vs-space-24);
}

.overview-updated {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}

.readiness-panel,
.overview-summary-card,
.overview-panel {
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.readiness-panel {
  --readiness-color: var(--vs-color-status-success);
  display: flex;
  min-height: 10.5rem;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-24);
  padding: var(--vs-space-24);
  border-inline-start: var(--vs-border-emphasis-width) solid var(--readiness-color);
}

.readiness-panel[data-tone='info'] {
  --readiness-color: var(--vs-color-status-info);
}

.readiness-panel[data-tone='warning'] {
  --readiness-color: var(--vs-color-status-warning);
}

.readiness-panel__state {
  display: flex;
  min-width: 0;
  align-items: flex-start;
  gap: var(--vs-space-16);
}

.readiness-panel__actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

.readiness-panel__icon,
.summary-icon {
  display: grid;
  flex: none;
  place-items: center;
  border-radius: var(--vs-radius-control);
}

.readiness-panel__icon {
  width: 3rem;
  height: 3rem;
  background: color-mix(in srgb, var(--readiness-color) 12%, transparent);
  color: var(--readiness-color);
}

.readiness-panel h2 {
  margin-top: var(--vs-space-8);
  font-size: var(--vs-type-size-panel);
  line-height: var(--vs-type-line-height-panel);
}

.readiness-panel p,
.overview-summary-card p,
.overview-panel__heading p,
.metric-footnote {
  color: var(--vs-color-text-secondary);
}

.readiness-panel p {
  margin-top: var(--vs-space-4);
}

.overview-summary-grid,
.overview-detail-grid {
  display: grid;
  gap: var(--vs-space-16);
}

.overview-summary-grid {
  grid-template-columns: repeat(2, minmax(0, 1fr));
}

.overview-summary-card {
  display: grid;
  min-height: 8.5rem;
  align-content: start;
  gap: var(--vs-space-8);
  padding: var(--vs-space-16);
}

.overview-summary-card__heading,
.overview-panel__heading {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.summary-icon {
  width: 2rem;
  height: 2rem;
  background: var(--vs-color-bg-subtle);
}

.summary-icon--info {
  color: var(--vs-color-status-info);
}

.summary-icon--warning {
  color: var(--vs-color-status-warning);
}

.overview-summary-card h2,
.overview-panel h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.overview-summary-card > a,
.overview-panel__heading > a {
  width: fit-content;
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
}

.overview-detail-grid {
  grid-template-columns: minmax(0, 1.35fr) repeat(2, minmax(14rem, 0.65fr));
}

.overview-panel {
  min-width: 0;
  padding: var(--vs-card-padding);
}

.overview-panel__heading p {
  margin-top: var(--vs-space-2);
  font-size: var(--vs-type-size-metadata);
}

.metric-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--vs-space-8);
  margin-top: var(--vs-space-20);
}

.metric-grid > div {
  min-width: 0;
  padding: var(--vs-space-12);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.metric-grid dt {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.metric-grid dd {
  margin: var(--vs-space-4) 0 0;
  font-size: var(--vs-type-size-section);
  font-weight: var(--vs-type-weight-semibold);
  font-variant-numeric: tabular-nums;
}

.metric-footnote {
  margin-top: var(--vs-space-12);
  font-size: var(--vs-type-size-metadata);
}

.overview-action-panel {
  display: flex;
  min-height: 13rem;
  flex-direction: column;
  justify-content: space-between;
  gap: var(--vs-space-20);
}

.overview-action-panel__icon {
  display: grid;
  width: 2.5rem;
  height: 2.5rem;
  place-items: center;
  margin-bottom: var(--vs-space-12);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-accent-default);
}

.overview-action-panel > .button {
  width: fit-content;
}

@media (max-width: 63.999rem) {
  .overview-detail-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .overview-detail-grid > :first-child {
    grid-column: 1 / -1;
  }
}

@media (max-width: 47.999rem) {
  .readiness-panel {
    align-items: stretch;
    flex-direction: column;
  }

  .readiness-panel__actions {
    width: 100%;
    flex-direction: column;
  }

  .readiness-panel__actions > .button {
    width: 100%;
  }

  .overview-summary-grid,
  .overview-detail-grid {
    grid-template-columns: minmax(0, 1fr);
  }

  .metric-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (max-width: 29.999rem) {
  .readiness-panel__state {
    flex-direction: column;
  }
}

@media (forced-colors: active) {
  .readiness-panel,
  .overview-summary-card,
  .overview-panel,
  .metric-grid > div {
    border: var(--vs-border-width) solid CanvasText;
  }
}
</style>
