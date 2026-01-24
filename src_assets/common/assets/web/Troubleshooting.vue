<template>
  <div class="troubleshoot-root">
    <h1 class="text-2xl font-semibold tracking-tight text-dark dark:text-light">
      {{ $t('troubleshooting.troubleshooting') }}
    </h1>

    <div class="troubleshoot-grid">
      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ $t('troubleshooting.force_close') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{ $t('troubleshooting.force_close_desc') }}
            </p>
          </div>
          <n-button type="primary" strong :disabled="closeAppPressed" @click="closeApp">
            {{ $t('troubleshooting.force_close') }}
          </n-button>
        </div>
        <n-alert v-if="closeAppStatus === true" type="success" class="mt-3">
          {{ $t('troubleshooting.force_close_success') }}
        </n-alert>
        <n-alert v-else-if="closeAppStatus === false" type="error" class="mt-3">
          {{ $t('troubleshooting.force_close_error') }}
        </n-alert>
      </section>

      <section class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ $t('troubleshooting.restart_sunshine') }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{ $t('troubleshooting.restart_sunshine_desc') }}
            </p>
          </div>
          <n-button type="primary" strong :disabled="restartPressed" @click="restart">
            {{ $t('troubleshooting.restart_sunshine') }}
          </n-button>
        </div>
        <n-alert v-if="restartPressed === true" type="success" class="mt-3">
          {{ $t('troubleshooting.restart_sunshine_success') }}
        </n-alert>
      </section>

      <section v-if="platform === 'windows'" class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ $t('troubleshooting.collect_playnite_logs') || 'Export Logs' }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                $t('troubleshooting.collect_playnite_logs_desc') ||
                'Export Sunshine, Playnite, plugin, and display-helper logs.'
              }}
            </p>
          </div>
          <n-button type="primary" strong @click="exportLogs">
            {{ $t('troubleshooting.collect_playnite_logs') || 'Export Logs' }}
          </n-button>
        </div>
      </section>

      <section v-if="platform === 'windows' && crashDumpAvailable" class="troubleshoot-card">
        <div class="flex items-start justify-between gap-4 flex-wrap">
          <div>
            <h2 class="text-base font-semibold text-dark dark:text-light">
              {{ $t('troubleshooting.export_crash_bundle') || 'Export Crash Bundle' }}
            </h2>
            <p class="text-xs opacity-70 leading-snug">
              {{
                $t('troubleshooting.export_crash_bundle_desc') ||
                'Download logs and the most recent Sunshine crash dump for issue reports.'
              }}
            </p>
          </div>
          <n-button type="error" strong @click="exportCrashBundle">
            {{ $t('troubleshooting.export_crash_bundle') || 'Export Crash Bundle' }}
          </n-button>
        </div>
      </section>
    </div>

    <section class="troubleshoot-card space-y-4">
      <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
        <div>
          <h2 class="text-base font-semibold text-dark dark:text-light">
            {{ $t('troubleshooting.logs') }}
          </h2>
          <p class="text-xs opacity-70 leading-snug">
            {{ $t('troubleshooting.logs_desc') }}
          </p>
        </div>
        <div class="flex flex-col sm:flex-row gap-2">
          <n-select
            v-if="logSourceOptions.length > 1"
            v-model:value="logSource"
            class="min-w-[200px]"
            :options="logSourceOptions"
            :placeholder="translate('troubleshooting.logs_source', 'Log source')"
          />
          <n-input v-model:value="logFilter" :placeholder="$t('troubleshooting.logs_find')" />
          <n-button
            type="primary"
            :aria-label="$t('troubleshooting.export_logs')"
            @click="exportLogs"
          >
            <i class="fas fa-download" />
            <span>{{ $t('troubleshooting.export_logs') }}</span>
          </n-button>
        </div>
      </div>

      <div
        v-if="rawSearchActive"
        class="flex flex-wrap items-center gap-2 text-xs text-dark/70 dark:text-light/70"
      >
        <span class="font-semibold text-dark/80 dark:text-light/80">
          {{ matchCountLabel }}
        </span>
        <n-button
          size="small"
          type="default"
          :disabled="matchCount === 0 || searchPending"
          @click="jumpToPreviousMatch"
        >
          {{ translate('troubleshooting.search_prev', 'Prev') }}
        </n-button>
        <n-button
          size="small"
          type="default"
          :disabled="matchCount === 0 || searchPending"
          @click="jumpToNextMatch"
        >
          {{ translate('troubleshooting.search_next', 'Next') }}
        </n-button>
        <n-button
          size="small"
          type="default"
          :disabled="logFilter.length === 0"
          @click="clearSearch"
        >
          {{ translate('troubleshooting.search_clear', 'Clear') }}
        </n-button>
        <span class="text-[11px] opacity-60">
          {{ searchContextLabel }}
        </span>
      </div>

      <div class="relative">
        <n-button
          v-if="newLogsAvailable && !rawSearchActive"
          class="absolute bottom-4 left-1/2 z-20 -translate-x-1/2 rounded-full px-4 py-2 text-sm font-medium shadow-lg"
          type="primary"
          strong
          @click="jumpToLatest"
        >
          {{ $t('troubleshooting.new_logs_available') }}
          <span
            v-if="unseenLines > 0"
            class="ml-2 rounded bg-dark/10 dark:bg-light/10 px-2 py-0.5 text-xs"
          >
            +{{ unseenLines }}
          </span>
          <i class="fas fa-arrow-down ml-2" />
        </n-button>
        <n-button
          v-else-if="showJumpToLatest && !rawSearchActive"
          class="absolute bottom-4 left-1/2 z-20 -translate-x-1/2 rounded-full px-4 py-2 text-sm font-medium shadow-lg"
          type="primary"
          strong
          @click="jumpToLatest"
        >
          {{ $t('troubleshooting.jump_to_latest') }}
          <i class="fas fa-arrow-down ml-2" />
        </n-button>

        <n-scrollbar
          ref="logScrollbar"
          style="height: 520px"
          class="border border-dark/10 dark:border-light/10 rounded-lg"
          @scroll="onLogScroll"
          @wheel="pauseAutoScroll"
          @mousedown="pauseAutoScroll"
          @touchstart="pauseAutoScroll"
        >
          <div
            class="m-0 bg-light dark:bg-dark font-mono text-[13px] leading-5 text-dark dark:text-light p-4 whitespace-pre-wrap break-words"
            @mousedown="pauseAutoScroll"
          >
            <div v-if="!searchActive" class="log-lines" :style="{ '--log-line-number-width': lineNumberWidth }">
              <div
                v-for="(line, index) in logLines"
                :key="index"
                :ref="setLineRef(index)"
                class="log-line"
              >
                <span class="log-line-number">{{ index + 1 }}</span>
                <span class="log-line-text">{{ line.length === 0 ? ' ' : line }}</span>
              </div>
            </div>
            <div v-else class="space-y-2">
              <div class="flex items-center justify-between text-xs font-semibold text-dark/80 dark:text-light/80">
                <span>{{ translate('troubleshooting.search_results', 'Results') }}</span>
                <span class="text-[11px] opacity-60">{{ searchContextLabel }}</span>
              </div>
              <div v-if="matchCount === 0" class="rounded-md border border-dark/10 dark:border-light/10 p-3 text-sm opacity-70">
                {{ translate('troubleshooting.search_no_matches', 'No matches') }}
              </div>
              <button
                v-for="result in searchResults"
                :key="result.id"
                type="button"
                class="w-full rounded-md border border-dark/10 dark:border-light/10 bg-white/80 dark:bg-surface/60 p-2 text-left transition hover:bg-dark/5 dark:hover:bg-light/5"
                :class="{
                  'border-amber-400/70 bg-amber-100/60 dark:bg-amber-500/10':
                    result.id === activeMatchIndex,
                }"
                :ref="setResultRef(result.id)"
                @click="openSearchResult(result.id)"
              >
                <div class="text-[11px] font-semibold text-dark/70 dark:text-light/70">
                  {{ translate('troubleshooting.search_line', 'Line') }} {{ result.lineIndex + 1 }}
                </div>
                <div
                  class="mt-1 font-mono text-[12px] leading-4 text-dark dark:text-light whitespace-pre-wrap break-words"
                  :style="{ '--log-line-number-width': lineNumberWidth }"
                >
                  <div v-for="snippetLine in result.snippet" :key="snippetLine.lineIndex" class="log-line">
                    <span class="log-line-number">
                      {{ snippetLine.lineIndex + 1 }}
                    </span>
                    <span class="log-line-text">
                      <template
                        v-for="(segment, sIndex) in getLineSegments(snippetLine.text, snippetLine.lineIndex)"
                        :key="sIndex"
                      >
                        <span
                          :class="
                            segment.isActive ? 'log-match-active' : segment.isMatch ? 'log-match' : ''
                          "
                          >{{ segment.text }}</span
                        >
                      </template>
                    </span>
                  </div>
                </div>
              </button>
            </div>
          </div>
        </n-scrollbar>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onBeforeUnmount, nextTick, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { NButton, NInput, NAlert, NScrollbar, NSelect } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import type { CrashDumpStatus } from '@/utils/crashDump';
import { isCrashDumpEligible, sanitizeCrashDumpStatus } from '@/utils/crashDump';

const store = useConfigStore();
const authStore = useAuthStore();
const { t } = useI18n();
const platform = computed(() => store.metadata.platform);

const crashDump = ref<CrashDumpStatus | null>(null);
const crashDumpAvailable = computed(() => isCrashDumpEligible(crashDump.value));

const closeAppPressed = ref(false);
const closeAppStatus = ref(null as null | boolean);
const restartPressed = ref(false);

const latestLogs = ref('Loading...');
const displayedLogs = ref('Loading...');
const logFilter = ref('');
const searchTerm = ref('');
const logSource = ref('sunshine');

const translate = (key: string, fallback: string) => {
  const value = t(key);
  return value === key ? fallback : value;
};

const tCount = (key: string, fallback: string, count: number) => {
  const value = t(key, { count });
  return value === key ? fallback.replace('{count}', String(count)) : value;
};

const logSourceOptions = computed(() => {
  const options = [
    { label: translate('troubleshooting.logs_source_sunshine', 'Sunshine'), value: 'sunshine' },
  ];
  if (platform.value === 'windows') {
    options.push(
      {
        label: translate('troubleshooting.logs_source_display_helper', 'Display helper'),
        value: 'display_helper',
      },
      {
        label: translate('troubleshooting.logs_source_playnite', 'Playnite'),
        value: 'playnite',
      },
      {
        label: translate('troubleshooting.logs_source_playnite_launcher', 'Playnite launcher'),
        value: 'playnite_launcher',
      },
      {
        label: translate('troubleshooting.logs_source_wgc', 'WGC helper'),
        value: 'wgc',
      },
    );
  }
  return options;
});

const logScrollbar = ref<InstanceType<typeof NScrollbar> | null>(null);
const autoScrollEnabled = ref(true);
const latestLineCount = ref(0);
const displayedLineCount = ref(0);
const isAtBottom = ref(true);

let logInterval: number | null = null;
let loginDisposer: (() => void) | null = null;
let searchDebounce: number | null = null;

const lineRefs = new Map<number, HTMLElement>();
const resultRefs = new Map<number, HTMLElement>();
const pendingJumpLine = ref<number | null>(null);

const setLineRef = (index: number) => (el: HTMLElement | null) => {
  if (el) {
    lineRefs.set(index, el);
  } else {
    lineRefs.delete(index);
  }
};

const setResultRef = (index: number) => (el: HTMLElement | null) => {
  if (el) {
    resultRefs.set(index, el);
  } else {
    resultRefs.delete(index);
  }
};

const rawSearch = computed(() => logFilter.value.trim());
const rawSearchActive = computed(() => rawSearch.value.length > 0);
const searchActive = computed(() => searchTerm.value.length > 0);
const logLines = computed(() => (displayedLogs.value ?? '').split('\n'));
const logLinesLower = computed(() => logLines.value.map((line) => line.toLowerCase()));
const lineNumberWidth = computed(() => {
  const digits = Math.max(3, String(logLines.value.length || 0).length);
  return `${digits}ch`;
});
const contextLines = 5;

const searchIndex = computed(() => {
  const term = searchTerm.value;
  const matchesByLine = new Map<number, Array<{ start: number; end: number }>>();
  const matchLines: number[] = [];
  let matchCount = 0;
  if (!term) return { matchesByLine, matchLines, matchCount };
  const needle = term.toLowerCase();
  if (!needle) return { matchesByLine, matchLines, matchCount };
  logLinesLower.value.forEach((lower, lineIndex) => {
    let fromIndex = 0;
    let lineHasMatch = false;
    while (fromIndex <= lower.length) {
      const matchIndex = lower.indexOf(needle, fromIndex);
      if (matchIndex === -1) break;
      matchCount += 1;
      lineHasMatch = true;
      const lineMatches = matchesByLine.get(lineIndex);
      if (lineMatches) {
        lineMatches.push({ start: matchIndex, end: matchIndex + needle.length });
      } else {
        matchesByLine.set(lineIndex, [{ start: matchIndex, end: matchIndex + needle.length }]);
      }
      fromIndex = matchIndex + needle.length;
    }
    if (lineHasMatch) matchLines.push(lineIndex);
  });
  return { matchesByLine, matchLines, matchCount };
});

const matchesByLine = computed(() => searchIndex.value.matchesByLine);
const matchLines = computed(() => searchIndex.value.matchLines ?? []);
const matchCount = computed(() => searchIndex.value.matchCount ?? 0);
const activeMatchIndex = ref(-1);
const activeLineIndex = computed(() =>
  activeMatchIndex.value >= 0 ? matchLines.value[activeMatchIndex.value] ?? null : null,
);

const searchPending = computed(
  () => rawSearchActive.value && rawSearch.value !== searchTerm.value,
);
const matchCountLabel = computed(() => {
  if (!rawSearchActive.value) return '';
  if (searchPending.value) {
    return translate('troubleshooting.search_pending', 'Searching...');
  }
  if (matchCount.value === 0) {
    return translate('troubleshooting.search_no_matches', 'No matches');
  }
  return tCount('troubleshooting.search_matches', '{count} matches', matchCount.value);
});
const searchContextLabel = computed(() =>
  tCount('troubleshooting.search_context', '{count} lines of context', contextLines),
);

const searchResults = computed(() => {
  if (!searchActive.value) return [];
  const lines = logLines.value;
  return matchLines.value.map((lineIndex, id) => {
    const start = Math.max(0, lineIndex - contextLines);
    const end = Math.min(lines.length - 1, lineIndex + contextLines);
    const snippet = [];
    for (let i = start; i <= end; i += 1) {
      snippet.push({ lineIndex: i, text: lines[i] ?? '' });
    }
    return { id, lineIndex, snippet };
  });
});


function getLineSegments(line: string, lineIndex: number) {
  const matches = matchesByLine.value.get(lineIndex);
  const isActiveLine = activeLineIndex.value === lineIndex;
  if (!matches || matches.length === 0) {
    return [{ text: line.length === 0 ? ' ' : line, isMatch: false, isActive: false }];
  }
  const segments: Array<{ text: string; isMatch: boolean; isActive: boolean }> = [];
  let cursor = 0;
  for (const match of matches) {
    if (match.start > cursor) {
      segments.push({ text: line.slice(cursor, match.start), isMatch: false, isActive: false });
    }
    segments.push({
      text: line.slice(match.start, match.end),
      isMatch: true,
      isActive: isActiveLine,
    });
    cursor = match.end;
  }
  if (cursor < line.length) {
    segments.push({ text: line.slice(cursor), isMatch: false, isActive: false });
  }
  if (segments.length === 0) {
    return [{ text: line.length === 0 ? ' ' : line, isMatch: false, isActive: false }];
  }
  return segments;
}
const unseenLines = computed(() =>
  Math.max(0, latestLineCount.value - displayedLineCount.value),
);
const newLogsAvailable = computed(() => unseenLines.value > 0);
const showJumpToLatest = computed(
  () => !newLogsAvailable.value && !isAtBottom.value && !autoScrollEnabled.value,
);

function resetLogState() {
  latestLogs.value = 'Loading...';
  displayedLogs.value = 'Loading...';
  latestLineCount.value = 0;
  displayedLineCount.value = 0;
  autoScrollEnabled.value = true;
  isAtBottom.value = true;
}

function buildLogUrl() {
  if (logSource.value === 'sunshine') return './api/logs';
  const params = new URLSearchParams();
  params.set('source', logSource.value);
  return `./api/logs?${params.toString()}`;
}

function getLogContainer() {
  // Naive UI's <n-scrollbar> exposes `scrollbarInstRef` (a Vue ref) which is sometimes
  // auto-unwrapped by the component public instance proxy. Handle both shapes.
  const maybe = (logScrollbar.value as any)?.scrollbarInstRef;
  const internal = maybe && typeof maybe === 'object' && 'value' in maybe ? maybe.value : maybe;
  const fromInst = internal?.containerRef ?? null;
  if (fromInst) return fromInst;

  // Fallback: query the DOM in case the internal instance shape changes.
  const rootEl = (logScrollbar.value as any)?.$el as HTMLElement | undefined;
  if (!rootEl) return null;
  return (
    rootEl.querySelector<HTMLElement>('.n-scrollbar-container') ??
    rootEl.querySelector<HTMLElement>('[class*="-scrollbar-container"]') ??
    null
  );
}

function hasActiveLogSelection() {
  if (typeof window === 'undefined') return false;
  const selection = window.getSelection();
  if (!selection || selection.isCollapsed) return false;
  const container = getLogContainer();
  if (!container) return false;
  const anchor = selection.anchorNode;
  const focus = selection.focusNode;
  return !!anchor && !!focus && container.contains(anchor) && container.contains(focus);
}

function onLogScroll() {
  const container = getLogContainer();
  if (!container) return;
  const atBottom = isNearBottom(container);
  isAtBottom.value = atBottom;
  if (atBottom) {
    if (!rawSearchActive.value) {
      autoScrollEnabled.value = true;
      displayedLogs.value = latestLogs.value;
      displayedLineCount.value = latestLineCount.value;
      scrollToBottom();
    } else {
      autoScrollEnabled.value = false;
    }
  } else {
    autoScrollEnabled.value = false;
  }
}

function isNearBottom(el: HTMLElement) {
  const threshold = 24;
  return el.scrollTop + el.clientHeight >= el.scrollHeight - threshold;
}

function scrollToBottom() {
  const doScroll = () => {
    // Avoid relying on scrollHeight math (and keep types happy).
    // Naive UI's internal scrollbar clamps large values to the bottom.
    logScrollbar.value?.scrollTo({ top: Number.MAX_SAFE_INTEGER, behavior: 'auto' });

    // Fallback for cases where the component method no-ops (e.g., container not ready yet).
    const container = getLogContainer();
    if (!container) return;
    container.scrollTop = container.scrollHeight;
    container.scrollTo?.({ top: container.scrollHeight, behavior: 'auto' });
    isAtBottom.value = isNearBottom(container);
  };

  doScroll();
  if (typeof window !== 'undefined') {
    window.requestAnimationFrame(() => doScroll());
  }
}

function scrollToResult(index: number) {
  const resultEl = resultRefs.get(index);
  if (resultEl?.scrollIntoView) {
    resultEl.scrollIntoView({ block: 'center' });
  }
}

function scrollToLogLine(lineIndex: number) {
  const lineEl = lineRefs.get(lineIndex);
  if (!lineEl) return;
  lineEl.scrollIntoView({ block: 'center' });
  flashLogLine(lineEl);
  const container = getLogContainer();
  if (container) isAtBottom.value = isNearBottom(container);
}

function flashLogLine(lineEl: HTMLElement) {
  lineEl.classList.remove('log-flash');
  // Force reflow to restart animation on rapid repeats.
  void lineEl.offsetWidth;
  lineEl.classList.add('log-flash');
  if (typeof window !== 'undefined') {
    window.setTimeout(() => lineEl.classList.remove('log-flash'), 3000);
  }
}

function pauseAutoScroll() {
  if (!autoScrollEnabled.value) return;
  autoScrollEnabled.value = false;
  displayedLogs.value = latestLogs.value;
  displayedLineCount.value = latestLineCount.value;
}

function setActiveMatch(index: number) {
  if (matchLines.value.length === 0) return;
  const total = matchLines.value.length;
  const nextIndex = ((index % total) + total) % total;
  activeMatchIndex.value = nextIndex;
  autoScrollEnabled.value = false;
  nextTick(() => {
    scrollToResult(nextIndex);
  });
}

function openSearchResult(index: number) {
  const lineIndex = matchLines.value[index];
  if (lineIndex === undefined) return;
  pendingJumpLine.value = lineIndex;
  activeMatchIndex.value = index;
  autoScrollEnabled.value = false;
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  logFilter.value = '';
  searchTerm.value = '';
}

function jumpToPreviousMatch() {
  if (matchLines.value.length === 0) return;
  setActiveMatch(activeMatchIndex.value - 1);
}

function jumpToNextMatch() {
  if (matchLines.value.length === 0) return;
  setActiveMatch(activeMatchIndex.value + 1);
}

function clearSearch() {
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  pendingJumpLine.value = null;
  logFilter.value = '';
  searchTerm.value = '';
}

async function refreshLogs() {
  if (!authStore.isAuthenticated) return;
  if (authStore.loggingIn && authStore.loggingIn.value) return;

  try {
    const r = await http.get(buildLogUrl(), {
      responseType: 'text',
      transformResponse: [(v) => v],
    });
    if (r.status !== 200 || typeof r.data !== 'string') return;
    const nextText = r.data;

    latestLogs.value = nextText;

    const nextLines = nextText ? nextText.split('\n') : [];
    latestLineCount.value = nextLines.length;

    const selectionActive = hasActiveLogSelection();
    const searchActiveNow = searchActive.value;
    const searchInputActive = rawSearchActive.value;
    const container = getLogContainer();
    const atBottom = container ? isNearBottom(container) : true;
    isAtBottom.value = atBottom;
    if (searchInputActive) {
      autoScrollEnabled.value = false;
    } else if (!atBottom && autoScrollEnabled.value) {
      autoScrollEnabled.value = false;
    }
    const shouldAutoScroll =
      autoScrollEnabled.value && atBottom && !selectionActive && !searchActiveNow;

    if (shouldAutoScroll) {
      displayedLogs.value = nextText;
      displayedLineCount.value = latestLineCount.value;
      await nextTick();
      scrollToBottom();
    } else {
      if (latestLineCount.value < displayedLineCount.value) {
        displayedLogs.value = nextText;
        displayedLineCount.value = latestLineCount.value;
      }
    }
  } catch {
    // ignore errors
  }
}

function exportLogs() {
  try {
    if (typeof window === 'undefined') return;
    if (platform.value === 'windows') {
      window.location.href = './api/logs/export';
      return;
    }
    const content = latestLogs.value || displayedLogs.value || '';
    const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
    const url = window.URL.createObjectURL(blob);
    const link = window.document.createElement('a');
    const timestamp = new Date()
      .toISOString()
      .replace(/[:.]/g, '-')
      .replace('T', '_')
      .replace('Z', '');
    link.href = url;
    link.download = `sunshine-logs-${timestamp}.log`;
    link.click();
    window.URL.revokeObjectURL(url);
  } catch (_) {}
}

async function refreshCrashDumpStatus() {
  try {
    if (platform.value === 'windows') {
      const r = await http.get('/api/health/crashdump', { validateStatus: () => true });
      if (r.status === 200 && r.data) {
        const sanitized = sanitizeCrashDumpStatus(r.data as CrashDumpStatus);
        crashDump.value = sanitized ?? { available: false };
      } else {
        crashDump.value = { available: false };
      }
    } else {
      crashDump.value = null;
    }
  } catch {
    crashDump.value = null;
  }
}

function exportCrashBundle() {
  try {
    if (typeof window !== 'undefined') window.location.href = './api/logs/export_crash';
  } catch {}
}

function jumpToLatest() {
  autoScrollEnabled.value = true;
  displayedLogs.value = latestLogs.value;
  displayedLineCount.value = latestLineCount.value;
  nextTick(() => {
    scrollToBottom();
  });
}

async function closeApp() {
  closeAppPressed.value = true;
  try {
    const r = await http.post('./api/apps/close', {}, { validateStatus: () => true });
    closeAppStatus.value = r.data?.status === true;
  } catch {
    closeAppStatus.value = false;
  } finally {
    closeAppPressed.value = false;
    setTimeout(() => (closeAppStatus.value = null), 5000);
  }
}

function restart() {
  restartPressed.value = true;
  setTimeout(() => (restartPressed.value = false), 5000);
  http.post('./api/restart', {}, { validateStatus: () => true });
}

onMounted(async () => {
  loginDisposer = authStore.onLogin(() => {
    void refreshLogs();
    void refreshCrashDumpStatus();
  });

  await authStore.waitForAuthentication();

  await refreshCrashDumpStatus();

  nextTick(() => {
    if (getLogContainer()) scrollToBottom();
  });

  logInterval = window.setInterval(refreshLogs, 5000);
  refreshLogs();
});

onBeforeUnmount(() => {
  if (logInterval) window.clearInterval(logInterval);
  if (loginDisposer) loginDisposer();
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
});

watch(rawSearch, (value) => {
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
  if (!value) {
    searchTerm.value = '';
    activeMatchIndex.value = -1;
    return;
  }
  autoScrollEnabled.value = false;
  if (typeof window === 'undefined') {
    searchTerm.value = value;
    return;
  }
  searchDebounce = window.setTimeout(() => {
    searchTerm.value = value;
  }, 150);
});

watch(searchActive, (active) => {
  if (active) {
    autoScrollEnabled.value = false;
    return;
  }
  activeMatchIndex.value = -1;
  if (pendingJumpLine.value !== null) {
    const targetLine = pendingJumpLine.value;
    pendingJumpLine.value = null;
    nextTick(() => {
      scrollToLogLine(targetLine);
    });
    return;
  }
  const container = getLogContainer();
  if (container && isNearBottom(container)) {
    autoScrollEnabled.value = true;
    displayedLogs.value = latestLogs.value;
    displayedLineCount.value = latestLineCount.value;
    nextTick(() => scrollToBottom());
  }
});

watch(matchLines, (list) => {
  if (!searchActive.value || list.length === 0) {
    activeMatchIndex.value = -1;
    return;
  }
  if (activeMatchIndex.value >= list.length) {
    activeMatchIndex.value = list.length - 1;
  }
});

watch(searchTerm, (value, oldValue) => {
  if (value !== oldValue) {
    activeMatchIndex.value = -1;
  }
});

watch(logSource, () => {
  resetLogState();
  void refreshLogs();
  nextTick(() => scrollToBottom());
});

</script>

<style scoped>
.troubleshoot-root {
  @apply space-y-6;
}

.troubleshoot-grid {
  @apply grid grid-cols-1 lg:grid-cols-2 gap-4;
}

.troubleshoot-card {
  @apply rounded-2xl border border-dark/10 dark:border-light/10 bg-white/90 dark:bg-surface/80 shadow-sm px-5 py-4 space-y-3;
}

.log-lines {
  @apply space-y-0;
}

.log-line {
  display: grid;
  grid-template-columns: var(--log-line-number-width, 4ch) minmax(0, 1fr);
  column-gap: 0.75rem;
  align-items: start;
  padding: 0.25rem 0;
}

.log-line-number {
  @apply text-left opacity-50 tabular-nums font-mono;
}

.log-line-text {
  @apply min-w-0 break-words;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

.log-match {
  @apply rounded-sm bg-amber-200/70 dark:bg-amber-400/30;
}

.log-match-active {
  @apply rounded-sm bg-amber-400/80 dark:bg-amber-500/50;
}

.log-flash {
  border-radius: 6px;
  box-shadow:
    0 0 0 3px rgb(var(--color-secondary) / 0.5),
    0 0 0 6px rgb(var(--color-secondary) / 0.25);
  outline: 2px solid rgb(var(--color-secondary) / 0.55);
  outline-offset: 2px;
  animation: log-flash-fade 3s ease-out forwards;
}

:deep(.n-scrollbar-container) {
  overflow-x: hidden !important;
}

:deep(.n-scrollbar-rail--horizontal) {
  display: none !important;
}

@keyframes log-flash-fade {
  0% {
    box-shadow:
      0 0 0 3px rgb(var(--color-secondary) / 0.55),
      0 0 0 6px rgb(var(--color-secondary) / 0.28);
    outline-color: rgb(var(--color-secondary) / 0.65);
  }
  70% {
    box-shadow:
      0 0 0 3px rgb(var(--color-secondary) / 0.25),
      0 0 0 6px rgb(var(--color-secondary) / 0.12);
    outline-color: rgb(var(--color-secondary) / 0.35);
  }
  100% {
    box-shadow:
      0 0 0 3px rgb(var(--color-secondary) / 0),
      0 0 0 6px rgb(var(--color-secondary) / 0);
    outline-color: rgb(var(--color-secondary) / 0);
  }
}

.dark .log-flash {
  animation-name: log-flash-fade-dark;
}

@keyframes log-flash-fade-dark {
  0% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0.45),
      0 0 0 6px rgb(var(--color-primary) / 0.22);
    outline-color: rgb(var(--color-primary) / 0.55);
  }
  70% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0.2),
      0 0 0 6px rgb(var(--color-primary) / 0.1);
    outline-color: rgb(var(--color-primary) / 0.3);
  }
  100% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0),
      0 0 0 6px rgb(var(--color-primary) / 0);
    outline-color: rgb(var(--color-primary) / 0);
  }
}
</style>
