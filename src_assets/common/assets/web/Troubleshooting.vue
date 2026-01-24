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
          <n-input
            v-model:value="logFilter"
            :placeholder="$t('troubleshooting.logs_find')"
            @input="handleFilterInput"
          />
          <n-button type="default" strong @click="toggleWrap">
            <i :class="wrapLongLines ? 'fas fa-align-left' : 'fas fa-ellipsis-h'" />
            <span>{{
              wrapLongLines ? $t('troubleshooting.wrap') : $t('troubleshooting.no_wrap')
            }}</span>
          </n-button>
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

      <div class="relative">
        <n-button
          v-if="newLogsAvailable"
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
          v-else-if="showJumpToLatest"
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
          <pre
            class="m-0 bg-light dark:bg-dark font-mono text-[13px] leading-5 text-dark dark:text-light p-4"
            :class="{ 'whitespace-pre-wrap': wrapLongLines, 'whitespace-pre': !wrapLongLines }"
            @mousedown="pauseAutoScroll"
            >{{ actualLogs }}</pre
          >
        </n-scrollbar>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onBeforeUnmount, nextTick } from 'vue';
import { NButton, NInput, NAlert, NScrollbar } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import type { CrashDumpStatus } from '@/utils/crashDump';
import { isCrashDumpEligible, sanitizeCrashDumpStatus } from '@/utils/crashDump';

const store = useConfigStore();
const authStore = useAuthStore();
const platform = computed(() => store.metadata.platform);

const crashDump = ref<CrashDumpStatus | null>(null);
const crashDumpAvailable = computed(() => isCrashDumpEligible(crashDump.value));

const closeAppPressed = ref(false);
const closeAppStatus = ref(null as null | boolean);
const restartPressed = ref(false);

const latestLogs = ref('Loading...');
const displayedLogs = ref('Loading...');
const logFilter = ref('');
const wrapLongLines = ref(true);

const logScrollbar = ref<InstanceType<typeof NScrollbar> | null>(null);
const autoScrollEnabled = ref(true);
const latestLineCount = ref(0);
const displayedLineCount = ref(0);
const isAtBottom = ref(true);

let logInterval: number | null = null;
let loginDisposer: (() => void) | null = null;

const filteredLines = computed(() => {
  if (!logFilter.value) return displayedLogs.value;
  const term = logFilter.value;
  return displayedLogs.value
    .split('\n')
    .filter((l) => l.includes(term))
    .join('\n');
});

const actualLogs = computed(() => filteredLines.value);
const unseenLines = computed(() =>
  Math.max(0, latestLineCount.value - displayedLineCount.value),
);
const newLogsAvailable = computed(() => unseenLines.value > 0);
const showJumpToLatest = computed(
  () => !newLogsAvailable.value && !isAtBottom.value && !autoScrollEnabled.value,
);

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

function handleFilterInput() {
  nextTick(() => {
    const container = getLogContainer();
    if (!container) return;
    const atBottom = isNearBottom(container);
    if (atBottom && autoScrollEnabled.value) {
      scrollToBottom();
    }
  });
}

function toggleWrap() {
  wrapLongLines.value = !wrapLongLines.value;
}

function onLogScroll() {
  const container = getLogContainer();
  if (!container) return;
  const atBottom = isNearBottom(container);
  isAtBottom.value = atBottom;
  if (atBottom) {
    autoScrollEnabled.value = true;
    displayedLogs.value = latestLogs.value;
    displayedLineCount.value = latestLineCount.value;
    scrollToBottom();
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

function pauseAutoScroll() {
  if (!autoScrollEnabled.value) return;
  autoScrollEnabled.value = false;
  displayedLogs.value = latestLogs.value;
  displayedLineCount.value = latestLineCount.value;
}

async function refreshLogs() {
  if (!authStore.isAuthenticated) return;
  if (authStore.loggingIn && authStore.loggingIn.value) return;

  try {
    const r = await http.get('./api/logs', {
      responseType: 'text',
      transformResponse: [(v) => v],
    });
    if (r.status !== 200 || typeof r.data !== 'string') return;
    const nextText = r.data;

    latestLogs.value = nextText;

    const nextLines = nextText ? nextText.split('\n') : [];
    latestLineCount.value = nextLines.length;

    const selectionActive = hasActiveLogSelection();
    const container = getLogContainer();
    const atBottom = container ? isNearBottom(container) : true;
    isAtBottom.value = atBottom;
    if (!atBottom && autoScrollEnabled.value) {
      autoScrollEnabled.value = false;
    }
    const shouldAutoScroll = autoScrollEnabled.value && atBottom && !selectionActive;

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
    const content = actualLogs.value || '';
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
</style>
