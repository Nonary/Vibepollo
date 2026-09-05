<script setup lang="ts">
import { supportsManagedLinuxDisplay } from '@/utils/providerCapabilities';
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import LinuxCaptureStatus from '@/components/settings/LinuxCaptureStatus.vue';
import { ApiError, apiGet } from '@/api/client';
import {
  AppButton,
  EmptyState,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
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
const statsStale = ref(false);
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

  void system.refreshHost();
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
    statsStale.value = false;
  } else {
    statsStale.value = true;
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
  if (
    session.value?.lastEncoderProbeFailed ||
    system.metadata?.encoder_status?.state === 'failed'
  ) {
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
  if (system.health === 'unknown')
    return {
      label: t('ui.settings.linux.states.unknown'),
      detail: t('ui.settings.linux.unverified'),
      tone: 'neutral',
    };
  if (system.health === 'warning' && !warnings.value.length)
    return {
      label: t('ui.status.needs_attention'),
      detail: t('ui.settings.linux.check_setup'),
      tone: 'warning',
    };
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

const readinessIcon = computed(() => {
  if (readiness.value.tone === 'warning') return 'alert-triangle';
  if (readiness.value.tone === 'neutral') return 'info';
  return isStreaming.value ? 'activity' : 'check-circle';
});
const primaryAction = computed(() => {
  if (isStreaming.value)
    return { to: '/stats', label: t('ui.overview.actions.openStats'), icon: 'activity' } as const;
  if (warnings.value.length)
    return {
      to: warnings.value[0].to,
      label: warnings.value[0].action,
      icon: 'chevron-right',
    } as const;
  if (system.health === 'unknown' || system.health === 'warning')
    return {
      to: '/settings?category=display',
      label: t('ui.overview.actions.reviewSetup'),
      icon: 'settings',
    } as const;
  return { to: '/pair', label: t('ui.overview.actions.pairDevice'), icon: 'plus' } as const;
});
const quickActions = [
  { key: 'library', to: '/library', icon: 'library' },
  { key: 'devices', to: '/devices', icon: 'devices' },
  { key: 'settings', to: '/settings', icon: 'settings' },
] as const;
const metrics = computed(() => [
  { label: t('host.cpu'), value: hostStats.value?.cpu_percent },
  { label: t('host.gpu'), value: hostStats.value?.gpu_percent },
  { label: t('ui.overview.memory'), value: hostStats.value?.ram_percent },
  { label: t('host.vram'), value: hostStats.value?.vram_percent },
]);
function validPercent(value: number | undefined): value is number {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 100;
}
function percent(value: number | undefined): string {
  return validPercent(value)
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
      <template #actions>
        <div class="overview-refresh">
          <span class="overview-updated">{{
            t('ui.overview.updated', { time: lastUpdatedLabel })
          }}</span>
          <AppButton
            icon="refresh"
            :label="t('_common.refresh')"
            variant="secondary"
            :busy="refreshing"
            :busy-label="t('ui.overview.refreshing')"
            @click="refresh(true)"
          />
        </div>
      </template>
    </PageHeader>
    <div class="visually-hidden" aria-live="polite" aria-atomic="true">{{ readiness.label }}</div>
    <template v-if="loading">
      <LoadingSkeleton variant="block" height="184px" :label="t('ui.overview.loadingReadiness')" />
      <LoadingSkeleton variant="block" height="320px" :label="t('ui.overview.loadingReadiness')" />
    </template>
    <template v-else>
      <section
        class="readiness-panel"
        :data-tone="readiness.tone"
        aria-labelledby="readiness-title"
      >
        <div class="readiness-panel__state">
          <span class="readiness-panel__icon" aria-hidden="true"
            ><UiIcon :name="readinessIcon" :size="28"
          /></span>
          <div class="readiness-panel__copy">
            <span class="readiness-panel__eyebrow">{{
              t('ui.overview.readiness.hostStatus')
            }}</span>
            <h2 id="readiness-title">{{ readiness.label }}</h2>
            <p>{{ readiness.detail }}</p>
            <p v-if="isStreaming" class="readiness-panel__sessions">
              {{
                t(
                  (session?.activeSessions ?? 0) === 1
                    ? 'ui.overview.activeSessions.one'
                    : 'ui.overview.activeSessions.other',
                  { count: session?.activeSessions ?? 0 },
                )
              }}
            </p>
          </div>
        </div>
        <div class="readiness-panel__actions">
          <RouterLink class="button button--primary" :to="primaryAction.to">
            <UiIcon :name="primaryAction.icon" />{{ primaryAction.label }}
          </RouterLink>
          <RouterLink v-if="!isStreaming" class="button button--secondary" to="/stream">
            <UiIcon name="play" />{{ t('ui.overview.actions.startBrowserStream') }}
          </RouterLink>
        </div>
      </section>

      <div v-if="warnings.length" class="overview-notices">
        <InlineAlert
          v-for="warning in warnings"
          :key="warning.key"
          tone="warning"
          :title="warning.title"
        >
          {{ warning.detail }}
          <template #actions
            ><RouterLink :to="warning.to">{{ warning.action }}</RouterLink></template
          >
        </InlineAlert>
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
              <p :class="{ 'overview-stale': statsStale && hostStats }">
                {{
                  t(
                    statsStale && hostStats
                      ? 'ui.overview.hostLoad.staleDescription'
                      : 'ui.overview.hostLoad.description',
                  )
                }}
              </p>
            </div>
            <RouterLink class="overview-detail-link" to="/stats"
              >{{ t('ui.overview.actions.viewDetails') }}<UiIcon name="chevron-right"
            /></RouterLink>
          </div>
          <dl v-if="hostStats" class="metric-grid">
            <div
              v-for="metric in metrics"
              :key="metric.label"
              :class="{ 'metric--unavailable': !validPercent(metric.value) }"
            >
              <dt>{{ metric.label }}</dt>
              <dd>{{ percent(metric.value) }}</dd>
              <div class="metric-track" aria-hidden="true">
                <span
                  :style="{ width: `${validPercent(metric.value) ? metric.value : 0}%` }"
                  :data-high="validPercent(metric.value) && metric.value >= 95"
                />
              </div>
            </div>
          </dl>
          <EmptyState
            v-else
            compact
            icon="activity"
            :title="t('ui.overview.hostLoad.unavailableTitle')"
            :description="t('ui.overview.hostLoad.unavailableDescription')"
          />
          <p
            v-if="hostStats && hostStats.ram_total_bytes > 0 && hostStats.ram_used_bytes >= 0"
            class="metric-footnote"
          >
            {{
              t('ui.overview.hostLoad.memoryInUse', {
                total: formatBytes(hostStats.ram_total_bytes, locale),
                used: formatBytes(hostStats.ram_used_bytes, locale),
              })
            }}
          </p>
          <dl class="host-details" :aria-label="t('ui.overview.hostDetails')">
            <div>
              <dt>{{ t('ui.overview.processor') }}</dt>
              <dd>{{ hostInfo?.cpu_model || t('ui.overview.unavailable') }}</dd>
            </div>
            <div>
              <dt>{{ t('ui.overview.graphics') }}</dt>
              <dd>{{ hostInfo?.gpu_model || t('ui.overview.unavailable') }}</dd>
            </div>
          </dl>
        </section>

        <section class="overview-panel workspace-panel" aria-labelledby="workspace-title">
          <div class="overview-panel__heading">
            <h2 id="workspace-title">{{ t('ui.overview.quickActions.title') }}</h2>
          </div>
          <RouterLink
            v-for="action in quickActions"
            :key="action.key"
            class="workspace-link"
            :to="action.to"
          >
            <span class="workspace-link__icon"><UiIcon :name="action.icon" :size="20" /></span>
            <span class="workspace-link__copy"
              ><strong>{{ t(`ui.overview.quickActions.${action.key}`) }}</strong
              ><span>{{ t(`ui.overview.quickActions.${action.key}Detail`) }}</span></span
            >
            <UiIcon name="chevron-right" :size="16" />
          </RouterLink>
        </section>
      </div>
      <LinuxCaptureStatus
        v-if="system.metadata?.platform === 'linux' && supportsManagedLinuxDisplay(system.metadata)"
        :metadata="system.metadata"
        :virtual-mode="
          system.metadata.capture_status?.virtual_display_configured === false
            ? 'disabled'
            : undefined
        "
      />
      <footer class="overview-footer">
        <span
          >{{ t('ui.overview.installedVersion') }}
          <strong>{{ system.metadata?.version || t('_common.unknown') }}</strong></span
        >
        <nav :aria-label="t('ui.overview.support')">
          <a
            href="https://github.com/Nonary/Vibepollo/issues/new/choose"
            target="_blank"
            rel="noopener noreferrer"
            >{{ t('ui.overview.actions.reportBug') }}<UiIcon name="external-link" :size="14"
          /></a>
          <a
            href="https://github.com/Nonary/Vibepollo/releases/latest"
            target="_blank"
            rel="noopener noreferrer"
            >{{ t('ui.overview.actions.checkUpdates') }}<UiIcon name="external-link" :size="14"
          /></a>
        </nav>
      </footer>
    </template>
  </div>
</template>

<style scoped>
.overview-page {
  display: grid;
  gap: var(--vs-space-24);
}
.overview-refresh {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-16);
}
.overview-updated {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}
.readiness-panel,
.overview-panel {
  min-width: 0;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}
.readiness-panel {
  --readiness-color: var(--vs-color-status-success);
  display: flex;
  min-height: 184px;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-32);
  padding: var(--vs-space-32);
}
.readiness-panel[data-tone='info'] {
  --readiness-color: var(--vs-color-status-info);
}
.readiness-panel[data-tone='warning'] {
  --readiness-color: var(--vs-color-status-warning);
}
.readiness-panel[data-tone='neutral'] {
  --readiness-color: var(--vs-color-text-muted);
}
.readiness-panel__state {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: var(--vs-space-20);
}
.readiness-panel__copy {
  min-width: 0;
}
.readiness-panel__eyebrow {
  color: var(--readiness-color);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-medium);
}
.readiness-panel__icon {
  display: grid;
  width: var(--vs-space-64);
  height: var(--vs-space-64);
  flex: none;
  place-items: center;
  border: 1px solid color-mix(in srgb, var(--readiness-color) 22%, transparent);
  border-radius: var(--vs-radius-dialog);
  background: color-mix(in srgb, var(--readiness-color) 8%, transparent);
  color: var(--readiness-color);
}
.readiness-panel h2 {
  margin: var(--vs-space-4) 0 var(--vs-space-8);
  font-size: 28px;
  line-height: 36px;
}
.readiness-panel p {
  max-width: 32rem;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-control);
}
.readiness-panel__sessions {
  margin-top: var(--vs-space-8);
}
.readiness-panel__actions {
  display: flex;
  flex: 0 0 auto;
  flex-direction: column;
  gap: var(--vs-space-8);
}
.overview-notices {
  display: grid;
  gap: var(--vs-space-12);
}
.overview-detail-grid {
  display: grid;
  grid-template-columns: minmax(0, 1.25fr) minmax(0, 1fr);
  gap: var(--vs-space-24);
}
.overview-panel {
  padding: var(--vs-space-24);
}
.overview-panel__heading {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-12);
}
.overview-panel h2 {
  font-size: 16px;
  line-height: 24px;
}
.overview-panel__heading p {
  margin-top: var(--vs-space-4);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}
.overview-panel__heading .overview-stale {
  color: var(--vs-color-status-warning);
}
.overview-detail-link {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  font-size: 12px;
  text-decoration: none;
}
.metric-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--vs-space-20);
  margin-top: var(--vs-space-32);
}
.metric-grid > div {
  min-width: 0;
}
.metric-grid dt {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}
.metric-grid dd {
  margin: var(--vs-space-8) 0 var(--vs-space-16);
  font-size: 28px;
  line-height: 36px;
  font-weight: var(--vs-type-weight-medium);
  letter-spacing: -0.04em;
  font-variant-numeric: tabular-nums;
}
.metric-grid .metric--unavailable dd {
  font-size: var(--vs-type-size-helper);
  letter-spacing: normal;
}
.metric-track {
  height: 4px;
  overflow: hidden;
  border-radius: var(--vs-radius-pill);
  background: var(--vs-color-border-subtle);
}
.metric-track > span {
  display: block;
  height: 100%;
  background: var(--vs-color-accent-default);
  border-radius: inherit;
}
.metric-track > span[data-high='true'] {
  background: var(--vs-color-status-warning);
}
.metric-footnote {
  margin-top: var(--vs-space-16);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
.host-details {
  display: grid;
  gap: var(--vs-space-12);
  padding-top: var(--vs-space-20);
  margin-top: var(--vs-space-24);
  border-top: 1px solid var(--vs-color-border-subtle);
  font-size: var(--vs-type-size-metadata);
}
.host-details > div {
  display: flex;
  flex-wrap: wrap;
  justify-content: space-between;
  gap: var(--vs-space-8) var(--vs-space-16);
}
.host-details dt {
  color: var(--vs-color-text-muted);
}
.host-details dd {
  color: var(--vs-color-text-secondary);
  overflow-wrap: anywhere;
}
.workspace-panel {
  padding-bottom: var(--vs-space-12);
}
.workspace-panel .overview-panel__heading {
  margin-bottom: var(--vs-space-12);
}
.workspace-link {
  display: flex;
  align-items: center;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16) 0;
  color: var(--vs-color-text-muted);
  text-decoration: none;
}
.workspace-link + .workspace-link {
  border-top: 1px solid var(--vs-color-border-subtle);
}
.workspace-link__icon {
  display: grid;
  flex: none;
  width: 40px;
  height: 40px;
  place-items: center;
  background: var(--vs-color-bg-subtle);
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-text-secondary);
}
.workspace-link__copy {
  min-width: 0;
  flex: 1;
}
.workspace-link strong {
  display: block;
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
}
.workspace-link__copy > span {
  display: block;
  margin-top: var(--vs-space-4);
  font-size: var(--vs-type-size-metadata);
  line-height: 20px;
}
.workspace-link:hover strong,
.workspace-link:hover > svg,
.workspace-link:hover .workspace-link__icon {
  color: var(--vs-color-accent-default);
}
.overview-page :deep(.linux-capture) {
  margin-bottom: 0;
}
.overview-footer {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-16);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
.overview-footer strong {
  margin-left: var(--vs-space-8);
  font-weight: var(--vs-type-weight-medium);
  color: var(--vs-color-text-secondary);
}
.overview-footer nav {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-24);
}
.overview-footer a {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-muted);
  text-decoration: none;
}
.overview-footer a:hover {
  color: var(--vs-color-accent-default);
}
@media (max-width: 1199px) {
  .overview-detail-grid {
    grid-template-columns: minmax(0, 1fr);
  }
}
@media (max-width: 767px) {
  .readiness-panel {
    padding: var(--vs-space-24);
    align-items: stretch;
    flex-direction: column;
    gap: var(--vs-space-24);
  }
  .readiness-panel__state {
    align-items: flex-start;
  }
  .readiness-panel__icon {
    width: 44px;
    height: 44px;
  }
  .readiness-panel h2 {
    font-size: 24px;
    line-height: 32px;
  }
  .overview-panel {
    padding: var(--vs-space-20);
  }
  .metric-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: var(--vs-space-24);
  }
}
@media (max-width: 359px) {
  .readiness-panel__state {
    flex-direction: column;
  }
}
@media (forced-colors: active) {
  .readiness-panel__icon,
  .metric-track {
    border: 1px solid CanvasText;
  }
  .metric-track > span {
    background: Highlight;
    forced-color-adjust: none;
  }
}
</style>
