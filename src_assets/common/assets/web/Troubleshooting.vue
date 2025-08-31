<template>
  <div class="px-4 py-6">
    <h1 class="mb-6 text-3xl font-semibold tracking-tight text-dark dark:text-light">
      {{ $t('troubleshooting.troubleshooting') }}
    </h1>

    <!-- Actions grid -->
    <n-grid cols="24" x-gap="16" y-gap="16" responsive="screen">
      <!-- Force Close App -->
      <n-gi :span="24" :lg="12">
        <section
          class="rounded-2xl border border-dark/10 bg-white p-6 shadow-sm dark:border-light/10 dark:bg-surface"
        >
          <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
            <div>
              <h2 id="close_apps" class="text-xl font-medium text-dark dark:text-light">
                {{ $t('troubleshooting.force_close') }}
              </h2>
              <p class="mt-2 text-sm opacity-70">
                {{ $t('troubleshooting.force_close_desc') }}
              </p>
            </div>
            <n-button secondary :disabled="closeAppPressed" @click="closeApp">
              {{ $t('troubleshooting.force_close') }}
            </n-button>
          </header>

          <transition name="fade">
            <p
              v-if="closeAppStatus === true"
              class="mt-4 alert alert-success rounded-lg px-4 py-2 text-sm"
            >
              {{ $t('troubleshooting.force_close_success') }}
            </p>
          </transition>
          <transition name="fade">
            <p
              v-if="closeAppStatus === false"
              class="mt-4 alert alert-danger rounded-lg px-4 py-2 text-sm"
            >
              {{ $t('troubleshooting.force_close_error') }}
            </p>
          </transition>
        </section>
      </n-gi>

      <!-- Restart Sunshine -->
      <n-gi :span="24" :lg="12">
        <section
          class="rounded-2xl border border-dark/10 bg-white p-6 shadow-sm dark:border-light/10 dark:bg-surface"
        >
          <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
            <div>
              <h2 id="restart" class="text-xl font-medium text-dark dark:text-light">
                {{ $t('troubleshooting.restart_sunshine') }}
              </h2>
              <p class="mt-2 text-sm opacity-70">
                {{ $t('troubleshooting.restart_sunshine_desc') }}
              </p>
            </div>
            <n-button secondary :disabled="restartPressed" @click="restart">
              {{ $t('troubleshooting.restart_sunshine') }}
            </n-button>
          </header>

          <transition name="fade">
            <p
              v-if="restartPressed === true"
              class="mt-4 alert alert-success rounded-lg px-4 py-2 text-sm"
            >
              {{ $t('troubleshooting.restart_sunshine_success') }}
            </p>
          </transition>
        </section>
      </n-gi>

      <!-- Reset persistent display device settings (Windows only) -->
      <n-gi v-if="platform === 'windows'" :span="24">
        <section
          v-if="platform === 'windows'"
          class="lg:col-span-2 rounded-2xl border border-dark/10 bg-white p-6 shadow-sm dark:border-light/10 dark:bg-surface"
        >
          <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
            <div>
              <h2 id="dd_reset" class="text-xl font-medium text-dark dark:text-light">
                {{ $t('troubleshooting.dd_reset') }}
              </h2>
              <p class="mt-2 whitespace-pre-line text-sm opacity-70">
                {{ $t('troubleshooting.dd_reset_desc') }}
              </p>
            </div>
            <n-button secondary :disabled="ddResetPressed" @click="ddResetPersistence">
              {{ $t('troubleshooting.dd_reset') }}
            </n-button>
          </header>

          <transition name="fade">
            <p
              v-if="ddResetStatus === true"
              class="mt-4 alert alert-success rounded-lg px-4 py-2 text-sm"
            >
              {{ $t('troubleshooting.dd_reset_success') }}
            </p>
          </transition>
          <transition name="fade">
            <p
              v-if="ddResetStatus === false"
              class="mt-4 alert alert-danger rounded-lg px-4 py-2 text-sm"
            >
              {{ $t('troubleshooting.dd_reset_error') }}
            </p>
          </transition>
        </section>
      </n-gi>

      <!-- Export Playnite Logs (Windows only) -->
      <n-gi v-if="platform === 'windows'" :span="24" :lg="12">
        <section
          class="rounded-2xl border border-dark/10 bg-white p-6 shadow-sm dark:border-light/10 dark:bg-surface"
        >
          <header class="flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
            <div>
              <h2 id="collect_playnite_logs" class="text-xl font-medium text-dark dark:text-light">
                {{ $t('troubleshooting.collect_playnite_logs') || 'Export Playnite Logs' }}
              </h2>
              <p class="mt-2 text-sm opacity-70">
                {{
                  $t('troubleshooting.collect_playnite_logs_desc') ||
                  'Export Playnite and plugin logs for troubleshooting.'
                }}
              </p>
            </div>
            <n-button secondary @click="downloadPlayniteLogs">
              {{ $t('troubleshooting.collect_playnite_logs') || 'Export Playnite Logs' }}
            </n-button>
          </header>
        </section>
      </n-gi>

      
    </n-grid>

    <!-- Logs -->
    <section
      class="mt-8 rounded-2xl border border-dark/10 bg-white shadow-sm dark:border-light/10 dark:bg-surface"
    >
      <div class="flex flex-col gap-4 p-6 md:flex-row md:items-center md:justify-between">
        <div>
          <h2 id="logs" class="text-xl font-medium text-dark dark:text-light">
            {{ $t('troubleshooting.logs') }}
          </h2>
          <p class="mt-1 text-sm opacity-70">
            {{ $t('troubleshooting.logs_desc') }}
          </p>
        </div>

        <div class="flex flex-col items-stretch gap-2 sm:flex-row sm:items-center">
          <n-input
            v-model:value="logFilter"
            :placeholder="$t('troubleshooting.logs_find')"
            @input="handleFilterInput"
          />
          <n-button secondary @click="toggleWrap">
            <i :class="wrapLongLines ? 'fas fa-align-left' : 'fas fa-ellipsis-h'"></i>
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
            <i class="fas fa-copy"></i>
            <span>{{ $t('troubleshooting.copy_logs') }}</span>
          </n-button>
        </div>
      </div>

      <!-- Logs console -->
      <div class="relative">
        <!-- New logs banner (appears at bottom of viewport inside console) -->
        <transition name="slide-up">
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
            <i class="fas fa-arrow-down ml-2"></i>
          </n-button>
        </transition>

        <!-- Scroll container -->
        <div
          ref="logContainer"
          class="h-[520px] overflow-auto border-t border-dark/10 bg-light font-mono text-[13px] leading-5 text-dark dark:border-light/10 dark:bg-dark dark:text-light"
          @scroll.passive="onLogScroll"
        >
          <pre class="m-0 whitespace-pre p-4" :class="{ 'whitespace-pre-wrap': wrapLongLines }">{{
            actualLogs
          }}</pre>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onBeforeUnmount, nextTick } from 'vue';
import { NGrid, NGi, NButton, NInput } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';

const store = useConfigStore();
const platform = computed(() => store.metadata.platform);

const closeAppPressed = ref(false);
const closeAppStatus = ref(null as null | boolean);
const ddResetPressed = ref(false);
const ddResetStatus = ref(null as null | boolean);
const restartPressed = ref(false);

const logs = ref('Loading...');
const logFilter = ref('');
const wrapLongLines = ref(true);

const logContainer = ref<HTMLElement | null>(null);
const autoScrollEnabled = ref(true);
const newLogsAvailable = ref(false);
const unseenLines = ref(0);

let logInterval: number | null = null;
let lastLineCount = 0;

const filteredLines = computed(() => {
  if (!logFilter.value) return logs.value;
  const term = logFilter.value;
  return logs.value
    .split('\n')
    .filter((l) => l.includes(term))
    .join('\n');
});

const actualLogs = computed(() => filteredLines.value);

// Banner text now localized directly in template

// ==== Behavior ====
function handleFilterInput() {
  // Keep autoscroll if we’re at bottom when filtering; otherwise respect user scroll
  nextTick(() => {
    if (!logContainer.value) return;
    const atBottom = isNearBottom(logContainer.value);
    if (atBottom && autoScrollEnabled.value) {
      scrollToBottom();
    }
  });
}

function toggleWrap() {
  wrapLongLines.value = !wrapLongLines.value;
}

function onLogScroll() {
  if (!logContainer.value) return;
  const atBottom = isNearBottom(logContainer.value);
  if (atBottom) {
    autoScrollEnabled.value = true;
    newLogsAvailable.value = false;
    unseenLines.value = 0;
  } else {
    autoScrollEnabled.value = false;
  }
}

function isNearBottom(el: HTMLElement) {
  const threshold = 24; // px tolerance
  return el.scrollTop + el.clientHeight >= el.scrollHeight - threshold;
}

function scrollToBottom() {
  if (!logContainer.value) return;
  logContainer.value.scrollTo({ top: logContainer.value.scrollHeight, behavior: 'smooth' });
}

async function refreshLogs() {
  const authStore = useAuthStore();

  if (!authStore.isAuthenticated) {
    return;
  }

  try {
    const r = await http.get('./api/logs', {
      responseType: 'text',
      transformResponse: [(v) => v],
      validateStatus: () => true,
    });
    if (typeof r.data === 'string') {
      // Count new lines before replacing
      const prev = logs.value || '';
      const prevCount = prev ? prev.split('\n').length : 0;
      const nextText = r.data;
      const nextCount = nextText ? nextText.split('\n').length : 0;

      logs.value = nextText;

      // Track new lines if user is not at bottom
      if (!autoScrollEnabled.value) {
        const delta = Math.max(nextCount - prevCount, 0);
        if (delta > 0) {
          unseenLines.value += delta;
          newLogsAvailable.value = true;
        }
      }

      // Autoscroll if enabled
      await nextTick();
      if (autoScrollEnabled.value) {
        scrollToBottom();
        newLogsAvailable.value = false;
        unseenLines.value = 0;
      }

      lastLineCount = nextCount;
    }
  } catch {
    // ignore errors
  }
}

// ===== Playnite logs export =====
function downloadPlayniteLogs() {
  try {
    if (typeof window !== 'undefined') window.location.href = './api/playnite/logs/export';
  } catch (_) {}
}

// (Removed) Clients unpairing UI and logic

function jumpToLatest() {
  scrollToBottom();
  autoScrollEnabled.value = true;
  newLogsAvailable.value = false;
  unseenLines.value = 0;
}

async function copyLogs() {
  try {
    await navigator.clipboard.writeText(actualLogs.value || '');
  } catch {
    // no-op
  }
}

// ==== Actions ====
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

async function ddResetPersistence() {
  ddResetPressed.value = true;
  try {
    const r = await http.post(
      '/api/reset-display-device-persistence',
      {},
      { validateStatus: () => true },
    );
    ddResetStatus.value = r.data?.status === true;
  } catch {
    ddResetStatus.value = false;
  } finally {
    ddResetPressed.value = false;
    setTimeout(() => (ddResetStatus.value = null), 5000);
  }
}

// ==== Lifecycle ====
onMounted(async () => {
  const authStore = useAuthStore();
  await authStore.waitForAuthentication();

  // Start polling & ensure console starts at bottom
  nextTick(() => {
    if (logContainer.value) scrollToBottom();
  });

  logInterval = window.setInterval(refreshLogs, 5000);
  refreshLogs();
});

onBeforeUnmount(() => {
  if (logInterval) window.clearInterval(logInterval);
});
</script>

<style scoped>
/* Simple transitions using Tailwind-friendly classes */
.fade-enter-active,
.fade-leave-active {
  transition: opacity 200ms ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

.slide-up-enter-active,
.slide-up-leave-active {
  transition:
    transform 200ms ease,
    opacity 200ms ease;
}

.slide-up-enter-from,
.slide-up-leave-to {
  transform: translateY(8px);
  opacity: 0;
}
</style>
