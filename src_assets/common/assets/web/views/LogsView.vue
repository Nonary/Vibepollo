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

interface LogSearchResult {
  id: number;
  line: LogLine;
  snippet: LogLine[];
}

interface LogSegment {
  text: string;
  match: boolean;
}

const source = ref<LogSource>('sunshine');
const severity = ref<SeverityFilter>('all');
const search = ref('');
const rawText = ref('');
const latestText = ref('');
const paused = ref(false);
const autoscroll = ref(true);
const loading = ref(true);
const refreshing = ref(false);
const error = ref('');
const notice = ref('');
const lastLoaded = ref<number | null>(null);
const viewer = ref<HTMLElement | null>(null);
const activeMatchIndex = ref(0);
const selectedLineNumber = ref<number | null>(null);
const highlightedLineNumber = ref<number | null>(null);
const resultRefs = new Map<number, HTMLElement>();
let refreshTimer: number | undefined;
let highlightTimer: number | undefined;
let latestRequest = 0;

const searchContextLines = 3;
const searchResultLimit = 50;
const logTailLines = 2000;
const displayedLineLimit = 1000;

function splitLogLines(text: string): string[] {
  if (!text) return [];
  const lines = text.split(/\r?\n/);
  if (lines.at(-1) === '') lines.pop();
  return lines;
}

function tailLogText(text: string, maxLines: number): string {
  if (!text || maxLines <= 0) return text;
  let cursor = text.length;
  if (text.endsWith('\n')) cursor -= 1;
  let tailStart = 0;
  for (let line = 0; line < maxLines && cursor > 0; line += 1) {
    const separator = text.lastIndexOf('\n', cursor - 1);
    if (separator < 0) return text;
    tailStart = separator + 1;
    cursor = separator;
  }
  return tailStart > 0 ? text.slice(tailStart) : text;
}

const allLines = computed<LogLine[]>(() => {
  return splitLogLines(rawText.value).map((text, index) => ({
    number: index + 1,
    text,
    severity: detectSeverity(text),
  }));
});

const latestLineCount = computed(() => splitLogLines(latestText.value).length);
const unseenLines = computed(() => Math.max(0, latestLineCount.value - allLines.value.length));
const newLogsAvailable = computed(
  () => unseenLines.value > 0 || latestText.value !== rawText.value,
);
const isAtBottom = ref(true);
const showJumpToLatest = computed(
  () => newLogsAvailable.value || !isAtBottom.value || !autoscroll.value,
);
const normalizedSearch = computed(() => search.value.trim().toLocaleLowerCase(locale.value));
const searchActive = computed(() => normalizedSearch.value.length > 0);

const filteredLines = computed(() => {
  const needle = normalizedSearch.value;
  return allLines.value.filter((line) => {
    const severityMatches = severity.value === 'all' || line.severity === severity.value;
    const textMatches = !needle || line.text.toLocaleLowerCase(locale.value).includes(needle);
    return severityMatches && textMatches;
  });
});

const matchingLineIndexes = computed(() => {
  const needle = normalizedSearch.value;
  if (!needle) return [];
  const matches: number[] = [];
  for (let index = 0; index < allLines.value.length; index += 1) {
    const line = allLines.value[index];
    if (!line) continue;
    if (severity.value !== 'all' && line.severity !== severity.value) continue;
    if (line.text.toLocaleLowerCase(locale.value).includes(needle)) matches.push(index);
  }
  return matches;
});

const matchOccurrenceCount = computed(() => {
  const needle = normalizedSearch.value;
  if (!needle) return 0;
  let count = 0;
  for (const lineIndex of matchingLineIndexes.value) {
    const text = allLines.value[lineIndex]?.text.toLocaleLowerCase(locale.value) ?? '';
    let offset = 0;
    while (offset <= text.length) {
      const matchIndex = text.indexOf(needle, offset);
      if (matchIndex < 0) break;
      count += 1;
      offset = matchIndex + needle.length;
    }
  }
  return count;
});

const searchWindow = computed(() => {
  const total = matchingLineIndexes.value.length;
  if (total <= searchResultLimit) return { start: 0, end: total };
  const half = Math.floor(searchResultLimit / 2);
  const start = Math.max(0, Math.min(activeMatchIndex.value - half, total - searchResultLimit));
  return { start, end: start + searchResultLimit };
});

const searchResults = computed<LogSearchResult[]>(() => {
  const { start, end } = searchWindow.value;
  return matchingLineIndexes.value.slice(start, end).map((lineIndex, offset) => ({
    id: start + offset,
    line: allLines.value[lineIndex]!,
    snippet: allLines.value.slice(
      Math.max(0, lineIndex - searchContextLines),
      Math.min(allLines.value.length, lineIndex + searchContextLines + 1),
    ),
  }));
});

const displayedLines = computed(() => {
  const lines = filteredLines.value;
  if (selectedLineNumber.value === null || searchActive.value)
    return lines.slice(-displayedLineLimit);
  const targetIndex = lines.findIndex((line) => line.number === selectedLineNumber.value);
  if (targetIndex < 0) return lines.slice(-displayedLineLimit);
  const halfWindow = Math.floor(displayedLineLimit / 2);
  const start = Math.max(0, Math.min(targetIndex - halfWindow, lines.length - displayedLineLimit));
  return lines.slice(start, start + displayedLineLimit);
});
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

const sourceLabel = computed(() => {
  const option = logSources.find((candidate) => candidate.value === source.value);
  return option ? t(option.labelKey) : source.value;
});

const summary = computed(() => {
  if (!allLines.value.length) {
    return t('ui.logs.summary.empty', { source: sourceLabel.value });
  }
  if (counts.value.error) {
    return `${t('ui.logs.summary.errors', { count: counts.value.error }, counts.value.error)} ${t(
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
  const match = line.match(
    /(?:^|[\[\]\s:])(fatal|error|warning|warn|info|debug|trace)(?=[:\]\s])/i,
  );
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

function isNearBottom(element: HTMLElement): boolean {
  return element.scrollTop + element.clientHeight >= element.scrollHeight - 24;
}

function hasViewerSelection(): boolean {
  const element = viewer.value;
  const selection = window.getSelection();
  if (!element || !selection || selection.isCollapsed) return false;
  return Boolean(
    selection.anchorNode &&
      selection.focusNode &&
      element.contains(selection.anchorNode) &&
      element.contains(selection.focusNode),
  );
}

async function scrollToLatest(): Promise<void> {
  await nextTick();
  const element = viewer.value;
  if (!element) return;
  element.scrollTop = element.scrollHeight;
  isAtBottom.value = true;
}

async function jumpToLatest(): Promise<void> {
  selectedLineNumber.value = null;
  highlightedLineNumber.value = null;
  rawText.value = latestText.value;
  autoscroll.value = true;
  await scrollToLatest();
}

function onViewerScroll(): void {
  const element = viewer.value;
  if (!element) return;
  const atBottom = isNearBottom(element);
  isAtBottom.value = atBottom;
  if (!atBottom) {
    autoscroll.value = false;
    return;
  }
  if (!searchActive.value) void jumpToLatest();
}

function setResultRef(index: number): (element: unknown) => void {
  return (element) => {
    if (element instanceof HTMLElement) resultRefs.set(index, element);
    else resultRefs.delete(index);
  };
}

function setActiveMatch(index: number): void {
  const total = matchingLineIndexes.value.length;
  if (!total) return;
  activeMatchIndex.value = ((index % total) + total) % total;
  void nextTick(() => {
    resultRefs.get(activeMatchIndex.value)?.scrollIntoView({ block: 'center' });
  });
}

async function openSearchResult(index: number): Promise<void> {
  const lineIndex = matchingLineIndexes.value[index];
  const lineNumber = lineIndex === undefined ? undefined : allLines.value[lineIndex]?.number;
  if (lineNumber === undefined) return;

  activeMatchIndex.value = index;
  selectedLineNumber.value = lineNumber;
  highlightedLineNumber.value = lineNumber;
  autoscroll.value = false;
  search.value = '';

  await nextTick();
  const element = viewer.value;
  const line = element?.querySelector<HTMLElement>(`[data-log-line="${lineNumber}"]`);
  if (!element || !line) return;
  element.scrollTop = line.offsetTop - element.clientHeight / 2 + line.offsetHeight / 2;
  isAtBottom.value = isNearBottom(element);

  if (highlightTimer !== undefined) window.clearTimeout(highlightTimer);
  highlightTimer = window.setTimeout(() => {
    if (highlightedLineNumber.value === lineNumber) highlightedLineNumber.value = null;
    highlightTimer = undefined;
  }, 3000);
}

function lineSegments(line: string): LogSegment[] {
  const needle = normalizedSearch.value;
  if (!needle) return [{ text: line || ' ', match: false }];
  const lower = line.toLocaleLowerCase(locale.value);
  const segments: LogSegment[] = [];
  let offset = 0;
  let matchIndex = lower.indexOf(needle);
  while (matchIndex >= 0) {
    if (matchIndex > offset) segments.push({ text: line.slice(offset, matchIndex), match: false });
    segments.push({ text: line.slice(matchIndex, matchIndex + needle.length), match: true });
    offset = matchIndex + needle.length;
    matchIndex = lower.indexOf(needle, offset);
  }
  if (offset < line.length) segments.push({ text: line.slice(offset), match: false });
  return segments.length ? segments : [{ text: line || ' ', match: false }];
}

async function refreshLogs(silent = false): Promise<void> {
  const requestId = ++latestRequest;
  const requestedSource = source.value;
  if (!silent) refreshing.value = true;

  try {
    const response = await apiGet<string>(
      `/api/logs?source=${encodeURIComponent(requestedSource)}&tail=${logTailLines}`,
    );
    if (requestId !== latestRequest) return;
    const responseText = typeof response === 'string' ? response : String(response ?? '');
    // Keep the browser bounded even when connected to an older host that ignores
    // the tail query parameter.
    const nextText = tailLogText(responseText, logTailLines);
    const nextLineCount = splitLogLines(nextText).length;
    const displayedLineCount = allLines.value.length;
    const element = viewer.value;
    const atBottom = element ? isNearBottom(element) : isAtBottom.value;
    const shouldFollow =
      autoscroll.value && atBottom && !searchActive.value && !hasViewerSelection();
    latestText.value = nextText;
    if (!rawText.value || shouldFollow || nextLineCount < displayedLineCount) {
      rawText.value = nextText;
    }
    error.value = '';
    lastLoaded.value = Date.now();
    if (shouldFollow) await scrollToLatest();
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
  latestText.value = '';
  autoscroll.value = true;
  isAtBottom.value = true;
  activeMatchIndex.value = 0;
  selectedLineNumber.value = null;
  highlightedLineNumber.value = null;
  loading.value = true;
  error.value = '';
  void refreshLogs();
});

watch(paused, (isPaused) => {
  if (!isPaused) void refreshLogs();
});

watch(autoscroll, (enabled) => {
  if (enabled) void jumpToLatest();
});

watch(normalizedSearch, (value) => {
  activeMatchIndex.value = 0;
  if (value) {
    selectedLineNumber.value = null;
    highlightedLineNumber.value = null;
    autoscroll.value = false;
  }
});

watch(severity, () => {
  selectedLineNumber.value = null;
  highlightedLineNumber.value = null;
});

watch(matchingLineIndexes, (matches) => {
  if (!matches.length) activeMatchIndex.value = 0;
  else if (activeMatchIndex.value >= matches.length) activeMatchIndex.value = matches.length - 1;
});

onMounted(() => {
  void refreshLogs();
  refreshTimer = window.setInterval(() => {
    if (!paused.value && !document.hidden) void refreshLogs(true);
  }, 5000);
});

onBeforeUnmount(() => {
  latestRequest += 1;
  if (refreshTimer !== undefined) window.clearInterval(refreshTimer);
  if (highlightTimer !== undefined) window.clearTimeout(highlightTimer);
});
</script>

<template>
  <div class="vs-page vs-page--fluid logs-page">
    <PageHeader :title="t('troubleshooting.logs')" :description="t('ui.logs.page.description')">
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
                  {
                    count: (searchActive
                      ? matchOccurrenceCount
                      : filteredLines.length
                    ).toLocaleString(locale || undefined),
                  },
                  searchActive ? matchOccurrenceCount : filteredLines.length,
                )
              }}
            </span>
            <template v-if="searchActive">
              <AppButton
                :label="t('ui.logs.action.previous_match')"
                size="compact"
                :disabled="!matchingLineIndexes.length"
                @click="setActiveMatch(activeMatchIndex - 1)"
              />
              <AppButton
                :label="t('ui.logs.action.next_match')"
                size="compact"
                :disabled="!matchingLineIndexes.length"
                @click="setActiveMatch(activeMatchIndex + 1)"
              />
              <AppButton
                :label="t('ui.logs.action.clear_search')"
                size="compact"
                variant="tertiary"
                @click="search = ''"
              />
            </template>
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
          <p v-if="searchActive">
            {{
              t('ui.logs.viewer.search_context', {
                count: searchContextLines,
                current: matchingLineIndexes.length ? activeMatchIndex + 1 : 0,
                total: matchingLineIndexes.length,
              })
            }}
          </p>
          <p v-else-if="omittedLines">
            {{ t('ui.logs.viewer.truncated', { limit: displayedLineLimit }) }}
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

        <div v-else class="log-viewer-shell">
          <AppButton
            v-if="showJumpToLatest && !searchActive"
            class="log-jump-latest"
            :label="
              newLogsAvailable
                ? t('ui.logs.action.new_lines', { count: unseenLines })
                : t('ui.logs.action.jump_to_latest')
            "
            variant="primary"
            size="compact"
            @click="jumpToLatest"
          />

          <div
            v-if="searchActive"
            class="log-viewer log-search-results"
            role="region"
            :aria-label="t('ui.logs.viewer.search_aria_label')"
            aria-live="off"
            tabindex="0"
          >
            <article
              v-for="result in searchResults"
              :key="result.id"
              :ref="setResultRef(result.id)"
              class="log-search-result"
              :class="{ 'log-search-result--active': result.id === activeMatchIndex }"
              tabindex="0"
              @click="openSearchResult(result.id)"
              @focus="setActiveMatch(result.id)"
            >
              <header>{{ t('ui.logs.viewer.match_line', { line: result.line.number }) }}</header>
              <ol :start="result.snippet[0]?.number">
                <li
                  v-for="line in result.snippet"
                  :key="line.number"
                  class="log-line"
                  :data-severity="line.severity"
                  :data-match-line="line.number === result.line.number || undefined"
                >
                  <span class="log-line__number" aria-hidden="true">{{ line.number }}</span>
                  <span class="log-line__severity" :data-tone="severityTone(line.severity)">
                    {{ severityLabel(line.severity) }}
                  </span>
                  <code>
                    <template v-for="(segment, index) in lineSegments(line.text)" :key="index">
                      <mark v-if="segment.match" class="log-match">{{ segment.text }}</mark>
                      <template v-else>{{ segment.text }}</template>
                    </template>
                  </code>
                </li>
              </ol>
            </article>
          </div>

          <div
            v-else
            ref="viewer"
            class="log-viewer"
            role="region"
            :aria-label="t('ui.logs.viewer.aria_label')"
            aria-live="off"
            tabindex="0"
            @scroll="onViewerScroll"
          >
            <ol :start="displayedLines[0]?.number">
              <li
                v-for="line in displayedLines"
                :key="line.number"
                class="log-line"
                :class="{ 'log-line--highlighted': line.number === highlightedLineNumber }"
                :data-severity="line.severity"
                :data-log-line="line.number"
              >
                <span class="log-line__number" aria-hidden="true">{{ line.number }}</span>
                <span class="log-line__severity" :data-tone="severityTone(line.severity)">
                  {{ severityLabel(line.severity) }}
                </span>
                <code>{{ line.text || ' ' }}</code>
              </li>
            </ol>
          </div>
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

.log-viewer-shell {
  position: relative;
}

.log-jump-latest {
  position: absolute;
  right: var(--vs-space-20);
  bottom: var(--vs-space-20);
  z-index: 3;
  box-shadow: var(--vs-shadow-overlay);
}

.log-viewer {
  height: clamp(24rem, 62vh, 52rem);
  overflow: auto;
  background: var(--vs-color-bg-canvas);
  color: var(--vs-color-text-secondary);
  scrollbar-gutter: stable both-edges;
}

.log-search-results {
  display: grid;
  align-content: start;
  gap: var(--vs-space-12);
  padding: var(--vs-space-12);
}

.log-search-result {
  min-width: max-content;
  overflow: clip;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  cursor: pointer;
}

.log-search-result:hover,
.log-search-result:focus-visible,
.log-search-result--active {
  border-color: var(--vs-color-border-strong);
}

.log-search-result:focus-visible {
  outline: 2px solid var(--vs-color-accent-default);
  outline-offset: 2px;
}

.log-search-result--active {
  box-shadow: inset 3px 0 0 var(--vs-color-accent-default);
}

.log-search-result header {
  position: sticky;
  left: 0;
  padding: var(--vs-space-8) var(--vs-space-12);
  border-bottom: 1px solid var(--vs-color-border-subtle);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-semibold);
}

.log-search-result .log-line[data-match-line='true'] {
  background: color-mix(in srgb, var(--vs-color-accent-default) 10%, transparent);
}

.log-match {
  border-radius: 2px;
  background: color-mix(in srgb, var(--vs-color-status-warning) 38%, transparent);
  color: inherit;
  font: inherit;
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

.log-line--highlighted,
.log-line--highlighted[data-severity] {
  background: color-mix(in srgb, var(--vs-color-accent-default) 20%, transparent);
  box-shadow: inset 0 0 0 2px var(--vs-color-accent-default);
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
    overflow-x: hidden;
    overflow-y: auto;
    scrollbar-gutter: auto;
  }

  .log-viewer ol {
    width: 100%;
    min-width: 0;
  }

  .log-search-results {
    padding: var(--vs-space-8);
  }

  .log-search-result {
    width: 100%;
    min-width: 0;
  }

  .log-search-result header {
    position: static;
  }

  .log-line {
    min-width: 0;
    grid-template-areas:
      'number severity'
      'message message';
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 0 var(--vs-space-8);
    padding: var(--vs-space-4) var(--vs-space-12);
    line-height: 1.45;
    white-space: normal;
  }

  .log-line__number {
    position: static;
    grid-area: number;
    padding: 0;
    border-right: 0;
    background: transparent;
    text-align: left;
  }

  .log-line__severity {
    grid-area: severity;
    padding: 0;
    text-align: right;
  }

  .log-line code {
    min-width: 0;
    grid-area: message;
    padding: var(--vs-space-2) 0 0;
    overflow-wrap: anywhere;
    white-space: pre-wrap;
    word-break: break-word;
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
