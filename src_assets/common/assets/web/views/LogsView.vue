<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiGet } from '@/api/client';
import {
  AppButton,
  EmptyState,
  InlineAlert,
  PageHeader,
  StatusBadge,
  UiIcon,
  type StatusTone,
} from '@/components/ui';
import { formatRelativeTime } from '@/utils/format';

const { locale, t } = useI18n();

const logSources = [
  { value: 'sunshine', labelKey: 'troubleshooting.logs_source_sunshine' },
  { value: 'display_helper', labelKey: 'troubleshooting.logs_source_display_helper' },
  { value: 'playnite', labelKey: 'troubleshooting.logs_source_playnite' },
  { value: 'playnite_launcher', labelKey: 'troubleshooting.logs_source_playnite_launcher' },
  { value: 'wgc', labelKey: 'troubleshooting.logs_source_wgc' },
] as const;

const severityOptions = [
  { value: 'all', labelKey: 'ui.logs.severity.all' },
  { value: 'error', labelKey: 'ui.logs.severity.errors' },
  { value: 'warning', labelKey: 'ui.logs.severity.warnings' },
  { value: 'info', labelKey: 'config.min_log_level_2' },
  { value: 'debug', labelKey: 'config.min_log_level_1' },
  { value: 'trace', labelKey: 'ui.logs.severity.trace' },
] as const;

type LogSource = (typeof logSources)[number]['value'];
type SeverityFilter = (typeof severityOptions)[number]['value'];
type LogSeverity = Exclude<SeverityFilter, 'all'> | 'other';

interface LogLine {
  number: number;
  text: string;
  severity: LogSeverity;
}

const source = ref<LogSource>('sunshine');
const severity = ref<SeverityFilter>('all');
const search = ref('');
const rawText = ref('');
const paused = ref(false);
const autoscroll = ref(true);
const loading = ref(true);
const refreshing = ref(false);
const error = ref('');
const notice = ref('');
const lastLoaded = ref<number | null>(null);
const viewer = ref<HTMLElement | null>(null);
let refreshTimer: number | undefined;
let latestRequest = 0;

const allLines = computed<LogLine[]>(() => {
  if (!rawText.value) return [];
  const textLines = rawText.value.split(/\r?\n/);
  if (textLines.at(-1) === '') textLines.pop();
  return textLines.map((text, index) => ({
    number: index + 1,
    text,
    severity: detectSeverity(text),
  }));
});

const filteredLines = computed(() => {
  const needle = search.value.trim().toLocaleLowerCase(locale.value);
  return allLines.value.filter((line) => {
    const severityMatches = severity.value === 'all' || line.severity === severity.value;
    const textMatches =
      !needle || line.text.toLocaleLowerCase(locale.value).includes(needle);
    return severityMatches && textMatches;
  });
});

const displayedLines = computed(() => filteredLines.value.slice(-5000));
const omittedLines = computed(() => filteredLines.value.length - displayedLines.value.length);

const counts = computed(() => {
  const result: Record<LogSeverity, number> = {
    trace: 0,
    debug: 0,
    info: 0,
    warning: 0,
    error: 0,
    other: 0,
  };
  for (const line of allLines.value) result[line.severity] += 1;
  return result;
});

const sourceLabel = computed(
  () => {
    const option = logSources.find((candidate) => candidate.value === source.value);
    return option ? t(option.labelKey) : source.value;
  },
);

const summary = computed(() => {
  if (!allLines.value.length) {
    return t('ui.logs.summary.empty', { source: sourceLabel.value });
  }
  if (counts.value.error) {
    return `${t(
      'ui.logs.summary.errors',
      { count: counts.value.error },
      counts.value.error,
    )} ${t(
      'ui.logs.summary.warnings_also',
      { count: counts.value.warning },
      counts.value.warning,
    )}`;
  }
  if (counts.value.warning) {
    return t(
      'ui.logs.summary.warnings_only',
      { count: counts.value.warning },
      counts.value.warning,
    );
  }
  return t(
    'ui.logs.summary.clear',
    { count: allLines.value.length.toLocaleString(locale.value || undefined) },
    allLines.value.length,
  );
});

function detectSeverity(line: string): LogSeverity {
  const match = line.match(/(?:^|[\[\]\s:])(fatal|error|warning|warn|info|debug|trace)(?=[:\]\s])/i);
  const value = match?.[1]?.toLocaleLowerCase();
  if (value === 'fatal' || value === 'error') return 'error';
  if (value === 'warning' || value === 'warn') return 'warning';
  if (value === 'info') return 'info';
  if (value === 'debug') return 'debug';
  if (value === 'trace') return 'trace';
  return 'other';
}

function severityLabel(value: LogSeverity): string {
  const labels: Record<LogSeverity, string> = {
    error: 'ui.logs.severity.error',
    warning: 'ui.logs.severity.warning',
    info: 'config.min_log_level_2',
    debug: 'config.min_log_level_1',
    trace: 'ui.logs.severity.trace',
    other: 'ui.logs.severity.other',
  };
  return t(labels[value]);
}

function severityTone(value: LogSeverity): StatusTone {
  if (value === 'error') return 'danger';
  if (value === 'warning') return 'warning';
  if (value === 'info') return 'info';
  return 'neutral';
}

async function scrollToLatest(): Promise<void> {
  if (!autoscroll.value || paused.value) return;
  await nextTick();
  const element = viewer.value;
  if (element) element.scrollTop = element.scrollHeight;
}

async function refreshLogs(silent = false): Promise<void> {
  const requestId = ++latestRequest;
  const requestedSource = source.value;
  if (!silent) refreshing.value = true;

  try {
    const response = await apiGet<string>(
      `/api/logs?source=${encodeURIComponent(requestedSource)}`,
    );
    if (requestId !== latestRequest) return;
    rawText.value = typeof response === 'string' ? response : String(response ?? '');
    error.value = '';
    lastLoaded.value = Date.now();
    await scrollToLatest();
  } catch (cause) {
    if (requestId !== latestRequest) return;
    error.value =
      cause instanceof ApiError
        ? t('ui.logs.error.load')
        : cause instanceof Error
          ? cause.message
          : t('ui.logs.error.load');
  } finally {
    if (requestId === latestRequest) {
      refreshing.value = false;
      loading.value = false;
    }
  }
}

function exportedText(): string {
  const width = String(allLines.value.length || 1).length;
  return filteredLines.value
    .map((line) => `${String(line.number).padStart(width, ' ')}  ${line.text}`)
    .join('\n');
}

async function copyVisible(): Promise<void> {
  error.value = '';
  try {
    if (!navigator.clipboard) throw new Error(t('ui.logs.error.clipboard_unavailable'));
    await navigator.clipboard.writeText(exportedText());
    notice.value = t(
      'ui.logs.notice.copied',
      { count: filteredLines.value.length.toLocaleString(locale.value || undefined) },
      filteredLines.value.length,
    );
  } catch (cause) {
    error.value =
      cause instanceof ApiError
        ? t('ui.logs.error.copy')
        : cause instanceof Error
          ? cause.message
          : t('ui.logs.error.copy');
  }
}

function downloadVisible(): void {
  error.value = '';
  const blob = new Blob([exportedText()], { type: 'text/plain;charset=utf-8' });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
  anchor.href = url;
  anchor.download = `vibepollo-${source.value}-${timestamp}.log`;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  URL.revokeObjectURL(url);
  notice.value = t(
    'ui.logs.notice.downloaded',
    { count: filteredLines.value.length.toLocaleString(locale.value || undefined) },
    filteredLines.value.length,
  );
}

watch(source, () => {
  rawText.value = '';
  loading.value = true;
  error.value = '';
  void refreshLogs();
});

watch(paused, (isPaused) => {
  if (!isPaused) void refreshLogs();
});

watch(autoscroll, (enabled) => {
  if (enabled) void scrollToLatest();
});

onMounted(() => {
  void refreshLogs();
  refreshTimer = window.setInterval(() => {
    if (!paused.value && !document.hidden) void refreshLogs(true);
  }, 3000);
});

onBeforeUnmount(() => {
  latestRequest += 1;
  if (refreshTimer !== undefined) window.clearInterval(refreshTimer);
});
</script>

<template>
  <div class="vs-page vs-page--fluid logs-page">
    <PageHeader
      :title="t('troubleshooting.logs')"
      :description="t('ui.logs.page.description')"
    >
      <template #meta>
        <StatusBadge
          :label="paused ? t('ui.logs.status.paused') : t('ui.logs.status.following')"
          :tone="paused ? 'warning' : 'success'"
          compact
        />
        <span v-if="lastLoaded" class="last-loaded">
          {{
            t('clients.last_updated', {
              time: formatRelativeTime(lastLoaded, locale, t('_common.unknown')),
            })
          }}
        </span>
      </template>
      <template #actions>
        <AppButton
          :label="paused ? t('ui.logs.action.resume') : t('ui.logs.action.pause')"
          :variant="paused ? 'primary' : 'secondary'"
          @click="paused = !paused"
        />
        <AppButton
          :label="refreshing ? t('ui.logs.action.refreshing') : t('_common.refresh')"
          icon="refresh"
          :disabled="refreshing"
          @click="refreshLogs()"
        />
      </template>
    </PageHeader>

    <div class="logs-stack">
      <InlineAlert
        v-if="error"
        tone="danger"
        :title="t('ui.logs.alert.unavailable_title')"
        announce="polite"
      >
        {{ error }} {{ t('ui.logs.alert.unavailable_description') }}
      </InlineAlert>
      <InlineAlert
        v-if="notice"
        tone="success"
        :title="t('ui.logs.alert.export_ready')"
        announce="polite"
        :dismiss-label="t('_common.dismiss')"
        @dismiss="notice = ''"
      >
        {{ notice }}
      </InlineAlert>

      <section class="log-summary vs-surface" aria-labelledby="log-summary-title">
        <div>
          <h2 id="log-summary-title">
            {{ t('ui.logs.summary.title', { source: sourceLabel }) }}
          </h2>
          <p>{{ summary }}</p>
        </div>
        <div class="summary-badges" :aria-label="t('ui.logs.summary.counts_aria_label')">
          <StatusBadge
            :label="t('ui.logs.count.errors', { count: counts.error }, counts.error)"
            :tone="counts.error ? 'danger' : 'neutral'"
            compact
          />
          <StatusBadge
            :label="t('ui.logs.count.warnings', { count: counts.warning }, counts.warning)"
            :tone="counts.warning ? 'warning' : 'neutral'"
            compact
          />
          <StatusBadge
            :label="t('ui.logs.count.total_lines', { count: allLines.length }, allLines.length)"
            tone="neutral"
            compact
          />
        </div>
      </section>

      <section class="log-workspace vs-surface" aria-labelledby="log-viewer-title">
        <div class="log-toolbar">
          <div class="log-filter-grid">
            <label class="vs-field" for="log-source">
              <span class="vs-field__label">{{ t('troubleshooting.logs_source') }}</span>
              <select id="log-source" v-model="source" class="vs-select">
                <option v-for="option in logSources" :key="option.value" :value="option.value">
                  {{ t(option.labelKey) }}
                </option>
              </select>
            </label>

            <label class="vs-field" for="log-severity">
              <span class="vs-field__label">{{ t('ui.logs.filters.severity') }}</span>
              <select id="log-severity" v-model="severity" class="vs-select">
                <option v-for="option in severityOptions" :key="option.value" :value="option.value">
                  {{ t(option.labelKey) }}
                </option>
              </select>
            </label>

            <label class="vs-field log-search" for="log-search">
              <span class="vs-field__label">{{ t('ui.logs.filters.search_label') }}</span>
              <span class="search-control">
                <UiIcon name="search" :size="16" aria-hidden="true" />
                <input
                  id="log-search"
                  v-model="search"
                  class="vs-input"
                  type="search"
                  autocomplete="off"
                  :placeholder="t('ui.logs.filters.search_placeholder')"
                />
              </span>
            </label>
          </div>

          <div class="log-toolbar__actions">
            <label class="vs-checkbox">
              <input v-model="autoscroll" type="checkbox" />
              <span>{{ t('ui.logs.filters.follow_newest') }}</span>
            </label>
            <span class="log-result-count" aria-live="polite">
              {{
                t(
                  'ui.logs.filters.matching_count',
                  { count: filteredLines.length.toLocaleString(locale || undefined) },
                  filteredLines.length,
                )
              }}
            </span>
            <AppButton
              :label="t('ui.logs.action.copy_visible')"
              icon="copy"
              size="compact"
              :disabled="!filteredLines.length"
              @click="copyVisible"
            />
            <AppButton
              :label="t('ui.logs.action.download_text')"
              icon="download"
              size="compact"
              :disabled="!filteredLines.length"
              @click="downloadVisible"
            />
          </div>
        </div>

        <div class="log-viewer-heading">
          <h2 id="log-viewer-title">{{ t('ui.logs.viewer.title') }}</h2>
          <p v-if="omittedLines">
            {{ t('ui.logs.viewer.truncated', { limit: 5000 }) }}
          </p>
          <p v-else>{{ t('ui.logs.viewer.line_numbers_preserved') }}</p>
        </div>

        <div v-if="loading" class="log-loading" role="status" aria-live="polite">
          {{ t('ui.logs.viewer.loading', { source: sourceLabel }) }}
        </div>

        <EmptyState
          v-else-if="!allLines.length"
          :title="t('ui.logs.empty.title')"
          :description="t('ui.logs.empty.description')"
          icon="logs"
          compact
        />

        <EmptyState
          v-else-if="!filteredLines.length"
          :title="t('ui.logs.empty.filtered_title')"
          :description="t('ui.logs.empty.filtered_description')"
          icon="search"
          compact
        />

        <div
          v-else
          ref="viewer"
          class="log-viewer"
          role="region"
          :aria-label="t('ui.logs.viewer.aria_label')"
          aria-live="off"
          tabindex="0"
        >
          <ol :start="displayedLines[0]?.number">
            <li
              v-for="line in displayedLines"
              :key="line.number"
              class="log-line"
              :data-severity="line.severity"
            >
              <span class="log-line__number" aria-hidden="true">{{ line.number }}</span>
              <span class="log-line__severity" :data-tone="severityTone(line.severity)">
                {{ severityLabel(line.severity) }}
              </span>
              <code>{{ line.text || ' ' }}</code>
            </li>
          </ol>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.logs-page,
.logs-stack {
  display: grid;
  gap: var(--vs-space-24);
}

.logs-page :deep(*) {
  animation: none !important;
  scroll-behavior: auto !important;
  transition: none !important;
}

.last-loaded,
.log-result-count,
.log-viewer-heading p {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
  font-variant-numeric: tabular-nums;
}

.log-summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-20);
  padding: var(--vs-space-16) var(--vs-space-20);
}

.log-summary h2,
.log-viewer-heading h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.log-summary p {
  margin-top: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
}

.summary-badges,
.log-toolbar__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.log-workspace {
  min-width: 0;
  overflow: clip;
}

.log-toolbar {
  display: grid;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16) var(--vs-space-20);
  border-bottom: 1px solid var(--vs-color-border-subtle);
}

.log-filter-grid {
  display: grid;
  grid-template-columns: minmax(11rem, 0.55fr) minmax(10rem, 0.45fr) minmax(16rem, 1fr);
  gap: var(--vs-space-12);
}

.log-toolbar__actions {
  justify-content: flex-end;
}

.log-toolbar__actions .vs-checkbox {
  margin-right: auto;
}

.search-control {
  position: relative;
  display: flex;
  align-items: center;
}

.search-control > svg {
  position: absolute;
  left: var(--vs-space-12);
  z-index: 1;
  color: var(--vs-color-text-muted);
  pointer-events: none;
}

.search-control .vs-input {
  padding-left: var(--vs-space-40);
}

.log-viewer-heading {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--vs-space-16);
  padding: var(--vs-space-12) var(--vs-space-20);
  border-bottom: 1px solid var(--vs-color-border-subtle);
}

.log-loading {
  min-height: 18rem;
  display: grid;
  place-items: center;
  color: var(--vs-color-text-secondary);
}

.log-viewer {
  height: clamp(24rem, 62vh, 52rem);
  overflow: auto;
  background: var(--vs-color-bg-canvas);
  color: var(--vs-color-text-secondary);
  scrollbar-gutter: stable both-edges;
}

.log-viewer ol {
  min-width: max-content;
  padding: var(--vs-space-8) 0 var(--vs-space-16);
  margin: 0;
  list-style: none;
  counter-reset: none;
}

.log-line {
  display: grid;
  min-height: 24px;
  grid-template-columns: 5.5rem 4.75rem minmax(max-content, 1fr);
  align-items: baseline;
  border-left: 2px solid transparent;
  font-family: var(--vs-type-family-mono);
  font-size: var(--vs-type-size-metadata);
  line-height: 24px;
  white-space: pre;
}

.log-line[data-severity='error'] {
  border-left-color: var(--vs-color-status-danger);
  background: color-mix(in srgb, var(--vs-color-status-danger) 7%, transparent);
}

.log-line[data-severity='warning'] {
  border-left-color: var(--vs-color-status-warning);
  background: color-mix(in srgb, var(--vs-color-status-warning) 6%, transparent);
}

.log-line__number {
  position: sticky;
  left: 0;
  padding: 0 var(--vs-space-12);
  border-right: 1px solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-canvas);
  color: var(--vs-color-text-muted);
  font-variant-numeric: tabular-nums;
  text-align: right;
  user-select: none;
}

.log-line__severity {
  padding: 0 var(--vs-space-12);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-semibold);
  text-transform: uppercase;
}

.log-line__severity[data-tone='danger'] {
  color: var(--vs-color-status-danger);
}

.log-line__severity[data-tone='warning'] {
  color: var(--vs-color-status-warning);
}

.log-line__severity[data-tone='info'] {
  color: var(--vs-color-status-info);
}

.log-line code {
  padding-right: var(--vs-space-24);
  color: inherit;
  font: inherit;
}

@media (max-width: 767px) {
  .log-summary,
  .log-viewer-heading {
    align-items: stretch;
    flex-direction: column;
  }

  .log-filter-grid {
    grid-template-columns: minmax(0, 1fr);
  }

  .log-toolbar__actions {
    align-items: stretch;
  }

  .log-toolbar__actions .vs-checkbox {
    width: 100%;
  }

  .log-result-count {
    width: 100%;
  }

  .log-viewer {
    height: calc(100vh - 10rem);
    min-height: 24rem;
  }
}

@media (forced-colors: active) {
  .log-line[data-severity='error'],
  .log-line[data-severity='warning'] {
    border-left-color: CanvasText;
    background: Canvas;
  }
}
</style>
