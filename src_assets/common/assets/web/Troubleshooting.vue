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
          <n-button type="primary" strong @click="downloadPlayniteLogs">
            {{ $t('troubleshooting.collect_playnite_logs') || 'Export Logs' }}
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
            :disabled="!actualLogs"
            :aria-label="$t('troubleshooting.copy_logs')"
            @click="copyLogs"
          >
            <i class="fas fa-copy" />
            <span>{{ $t('troubleshooting.copy_logs') }}</span>
          </n-button>
        </div>
      </div>

      <div class="relative">
        <n-button
          v-if="newLogsAvailable"
          class="absolute bottom-4 left-1/2 z-20 -translate-x-1/2 rounded-full px-4 py-2 text-sm font-medium shadow-lg"
          type="warning"
          @click="jumpToLatest"
        >
          {{ $t('troubleshooting.new_logs_available') }}
          <span v-if="unseenLines > 0" class="ml-2 rounded bg-warning/20 px-2 py-0.5 text-xs">
            +{{ unseenLines }}
          </span>
          <i class="fas fa-arrow-down ml-2" />
        </n-button>
        <n-button
          v-else-if="!autoScrollEnabled"
          class="absolute bottom-4 left-1/2 z-20 -translate-x-1/2 rounded-full px-4 py-2 text-sm font-medium shadow-lg"
          type="primary"
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
        >
          <pre
            class="m-0 bg-light dark:bg-dark font-mono text-[13px] leading-5 text-dark dark:text-light p-4"
            :class="{ 'whitespace-pre-wrap': wrapLongLines, 'whitespace-pre': !wrapLongLines }"
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
import type { ScrollbarInst } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';

const store = useConfigStore();
const authStore = useAuthStore();
const platform = computed(() => store.metadata.platform);

const closeAppPressed = ref(false);
const closeAppStatus = ref(null as null | boolean);
const restartPressed = ref(false);

const logs = ref('Loading...');
const logFilter = ref('');
const wrapLongLines = ref(true);

const logScrollbar = ref<ScrollbarInst | null>(null);
const autoScrollEnabled = ref(true);
const newLogsAvailable = ref(false);
const unseenLines = ref(0);
const lastObservedLine = ref('');

let logInterval: number | null = null;
let loginDisposer: (() => void) | null = null;

const filteredLines = computed(() => {
  if (!logFilter.value) return logs.value;
  const term = logFilter.value;
  return logs.value
    .split('\n')
    .filter((l) => l.includes(term))
    .join('\n');
});

const actualLogs = computed(() => filteredLines.value);

function handleFilterInput() {
  nextTick(() => {
    const container = logScrollbar.value?.containerRef;
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
  const container = logScrollbar.value?.containerRef;
  if (!container) return;
  const atBottom = isNearBottom(container);
  if (atBottom) {
    autoScrollEnabled.value = true;
    newLogsAvailable.value = false;
    unseenLines.value = 0;
    lastObservedLine.value = extractLastLine(logs.value);
  } else {
    autoScrollEnabled.value = false;
  }
}

function isNearBottom(el: HTMLElement) {
  const threshold = 24;
  return el.scrollTop + el.clientHeight >= el.scrollHeight - threshold;
}

function scrollToBottom() {
  const container = logScrollbar.value?.containerRef;
  if (!container) return;
  logScrollbar.value?.scrollTo({ top: container.scrollHeight, behavior: 'smooth' });
  lastObservedLine.value = extractLastLine(logs.value);
}

async function refreshLogs() {
  if (!authStore.isAuthenticated) return;
  if (authStore.loggingIn && authStore.loggingIn.value) return;

  try {
    const r = await http.get('./api/logs', {
      responseType: 'text',
      transformResponse: [(v) => v],
      validateStatus: () => true,
    });
    if (r.status !== 200 || typeof r.data !== 'string') return;
    const prev = logs.value || '';
    const prevLines = prev ? prev.split('\n') : [];
    const nextText = r.data;

    logs.value = nextText;

    const nextLines = nextText ? nextText.split('\n') : [];

    if (!autoScrollEnabled.value) {
      const fallbackAnchor = lastLineOf(prevLines);
      const anchor = lastObservedLine.value || fallbackAnchor;
      let unseen = 0;
      if (anchor) {
        const anchorIndex = nextLines.lastIndexOf(anchor);
        unseen = anchorIndex === -1 ? nextLines.length : Math.max(nextLines.length - anchorIndex - 1, 0);
      } else {
        unseen = nextLines.length;
      }

      unseenLines.value = unseen;
      newLogsAvailable.value = unseen > 0;
    }

    await nextTick();
    if (autoScrollEnabled.value) {
      scrollToBottom();
      newLogsAvailable.value = false;
      unseenLines.value = 0;
      lastObservedLine.value = extractLastLine(nextText);
    } else if (!lastObservedLine.value && nextLines.length) {
      lastObservedLine.value = extractLastLine(nextText);
    }
  } catch {
    // ignore errors
  }
}

function downloadPlayniteLogs() {
  try {
    if (typeof window !== 'undefined') window.location.href = './api/logs/export';
  } catch (_) {}
}

function jumpToLatest() {
  scrollToBottom();
  autoScrollEnabled.value = true;
  newLogsAvailable.value = false;
  unseenLines.value = 0;
  lastObservedLine.value = extractLastLine(logs.value);
}

async function copyLogs() {
  try {
    await navigator.clipboard.writeText(actualLogs.value || '');
  } catch {}
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
  });

  await authStore.waitForAuthentication();

  nextTick(() => {
    if (logScrollbar.value?.containerRef) scrollToBottom();
  });

  logInterval = window.setInterval(refreshLogs, 5000);
  refreshLogs();
});

onBeforeUnmount(() => {
  if (logInterval) window.clearInterval(logInterval);
  if (loginDisposer) loginDisposer();
});

function extractLastLine(text: string) {
  if (!text) return '';
  return lastLineOf(text.split('\n'));
}

function lastLineOf(lines: string[]) {
  for (let i = lines.length - 1; i >= 0; i -= 1) {
    if (lines[i]) {
      return lines[i];
    }
  }
  return '';
}
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
