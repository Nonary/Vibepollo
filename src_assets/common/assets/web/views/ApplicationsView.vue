<template>
  <div class="max-w-3xl mx-auto px-4 py-6 space-y-4 sm:px-6 sm:py-8 sm:space-y-5">
    <div
      class="flex flex-col gap-3 rounded-2xl border border-dark/10 bg-white/75 p-4 shadow-sm backdrop-blur dark:border-light/10 dark:bg-surface/70 sm:flex-row sm:items-center sm:justify-between sm:rounded-none sm:border-0 sm:bg-transparent sm:p-0 sm:shadow-none sm:backdrop-blur-none"
    >
      <div class="min-w-0 space-y-1">
        <h2 class="text-base font-semibold text-dark dark:text-light sm:text-sm sm:uppercase sm:tracking-wider">
          Applications
        </h2>
        <p class="text-[12px] leading-relaxed opacity-65 sm:hidden">
          Add manual apps or connect Playnite to keep your library ready for streaming.
        </p>
      </div>

      <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center sm:justify-end sm:gap-4">
        <!-- Windows + Playnite secondary action -->
        <template v-if="isWindows">
          <n-button
            v-if="playniteEnabled"
            size="medium"
            type="default"
            strong
            class="h-11 justify-start rounded-xl px-4 text-left sm:h-10 sm:justify-center sm:rounded-md sm:px-3"
            :loading="syncBusy"
            :disabled="syncBusy"
            @click="forceSync"
            aria-label="Force sync now"
          >
            <svg
              class="mr-2 inline-block h-4 w-4 shrink-0"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
            >
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="1.6"
                d="M21 12a9 9 0 11-3.2-6.6M21 3v6h-6"
              />
            </svg>
            <span class="inline-flex flex-col items-start leading-tight sm:flex-row sm:items-center">
              <span>{{ $t('playnite.force_sync') || 'Force Sync' }}</span>
              <span class="text-[11px] opacity-60 sm:hidden">Refresh imported titles</span>
            </span>
          </n-button>

          <!-- Setup Playnite when disabled -->
          <n-button
            v-else
            size="medium"
            type="default"
            strong
            @click="gotoPlaynite"
            class="h-11 justify-start rounded-xl px-4 text-left sm:h-10 sm:justify-center sm:rounded-md sm:px-3"
          >
            <svg
              class="mr-2 inline-block h-4 w-4 shrink-0"
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
            >
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="1.6"
                d="M12 3v3m0 12v3m9-9h-3M6 12H3m13.95 5.657l-2.121-2.121M8.172 8.172 6.05 6.05m11.9 0-2.121 2.121M8.172 15.828 6.05 17.95"
              />
            </svg>
            <span class="inline-flex flex-col items-start leading-tight sm:flex-row sm:items-center">
              <span class="sm:hidden">Connect Playnite</span>
              <span class="hidden sm:inline">{{
                $t('playnite.setup_integration') || 'Setup Playnite'
              }}</span>
              <span class="text-[11px] opacity-60 sm:hidden">Import and manage your library</span>
            </span>
          </n-button>
        </template>

        <!-- Primary: Add -->
        <n-button
          type="primary"
          size="medium"
          strong
          class="h-11 justify-start rounded-xl px-4 text-left sm:h-10 sm:justify-center sm:rounded-md sm:px-4"
          @click="openAdd"
        >
          <svg
            class="mr-2 inline-block h-4 w-4 shrink-0"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
          >
            <path
              stroke-linecap="round"
              stroke-linejoin="round"
              stroke-width="1.6"
              d="M12 5v14M5 12h14"
            />
          </svg>
          <span class="inline-flex flex-col items-start leading-tight sm:flex-row sm:items-center">
            <span class="sm:hidden">Add Application</span>
            <span class="hidden sm:inline">Add</span>
            <span class="text-[11px] opacity-80 sm:hidden">Create a manual entry</span>
          </span>
        </n-button>
      </div>
    </div>

    <!-- Redesigned list view -->
    <div
      class="rounded-2xl overflow-hidden border border-dark/10 dark:border-light/10 bg-light/80 dark:bg-surface/80 backdrop-blur"
    >
      <div v-if="apps && apps.length" class="divide-y divide-black/5 dark:divide-white/10">
        <button
          v-for="(app, i) in apps"
          :key="appKey(app, i)"
          type="button"
          class="w-full text-left focus:outline-none focus-visible:ring-2 focus-visible:ring-primary/40"
          @click="openEdit(app, i)"
          @keydown.enter.prevent="openEdit(app, i)"
          @keydown.space.prevent="openEdit(app, i)"
        >
          <div
            class="flex items-center justify-between px-6 py-4 min-h-[56px] hover:bg-dark/10 dark:hover:bg-light/10"
          >
            <div class="min-w-0 flex-1">
              <div class="text-sm font-semibold truncate flex items-center gap-2">
                <span class="truncate">{{ app.name || '(untitled)' }}</span>
                <!-- Playnite or Custom badges -->
                <template v-if="app['playnite-id']">
                  <n-tag
                    size="small"
                    class="!px-2 !py-0.5 text-xs bg-slate-700 border-none text-slate-200"
                    >Playnite</n-tag
                  >
                  <span v-if="app['playnite-managed'] === 'manual'" class="text-[10px] opacity-70"
                    >manual</span
                  >
                  <span v-else-if="app['playnite-source']" class="text-[10px] opacity-70">{{
                    String(app['playnite-source']).split('+').join(' + ')
                  }}</span>
                  <span v-else class="text-[10px] opacity-70">managed</span>
                </template>
                <template v-else>
                  <n-tag
                    size="small"
                    class="!px-2 !py-0.5 text-xs bg-slate-700/70 border-none text-slate-200"
                    >Custom</n-tag
                  >
                </template>
              </div>
              <div class="mt-0.5 text-[11px] opacity-60 truncate" v-if="app['working-dir']">
                {{ app['working-dir'] }}
              </div>
            </div>
            <div class="shrink-0 text-dark/50 dark:text-light/70">
              <svg
                class="w-4 h-4"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                aria-hidden
              >
                <path
                  stroke-linecap="round"
                  stroke-linejoin="round"
                  stroke-width="1.6"
                  d="M9 6l6 6-6 6"
                />
              </svg>
            </div>
          </div>
        </button>
      </div>
      <div v-else class="px-6 py-10 text-center text-sm opacity-60">
        No applications configured.
      </div>
    </div>

    <AppEditModal
      v-model="showModal"
      :app="currentApp"
      :index="currentIndex"
      :key="
        modalKey +
        '|' +
        (currentIndex ?? -1) +
        '|' +
        (currentApp?.uuid || currentApp?.name || 'new')
      "
      @saved="reload"
      @deleted="reload"
    />
    <!-- Playnite integration removed for now -->
  </div>
</template>
<script setup lang="ts">
import { ref, onMounted, computed, watch, defineAsyncComponent } from 'vue';
// Lazy-load the modal when first opened
const AppEditModal = defineAsyncComponent(() => import('@/components/AppEditModal.vue'));
import { useAppsStore } from '@/stores/apps';
import { storeToRefs } from 'pinia';
import { NButton, NTag } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';
import { useRouter } from 'vue-router';
import { useAuthStore } from '@/stores/auth';
import type { App } from '@/stores/apps';

// Minimal shape used for rendering items returned by the backend
// Use shared App type from store for consistency

const appsStore = useAppsStore();
const { apps } = storeToRefs(appsStore);
const configStore = useConfigStore();
const auth = useAuthStore();
const router = useRouter();

const syncBusy = ref(false);
const isWindows = computed(
  () => (configStore.metadata?.platform || '').toLowerCase() === 'windows',
);

const playniteInstalled = ref(false);
const playniteStatusReady = ref(false);
const playniteEnabled = computed(() => playniteInstalled.value);

const showModal = ref(false);
const modalKey = ref(0);
const currentApp = ref<App | null>(null);
const currentIndex = ref<number | null>(-1);

async function reload(): Promise<void> {
  await appsStore.loadApps(true);
}

function openAdd(): void {
  currentApp.value = null;
  currentIndex.value = -1;
  showModal.value = true;
}

function openEdit(app: App, i: number): void {
  currentApp.value = app;
  currentIndex.value = i;
  showModal.value = true;
}
function appKey(app: App | null | undefined, index: number) {
  const id = app?.uuid || '';
  return `${app?.name || 'app'}|${id}|${index}`;
}

async function forceSync(): Promise<void> {
  syncBusy.value = true;
  try {
    await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    await reload();
  } catch {
  } finally {
    syncBusy.value = false;
  }
}

function gotoPlaynite(): void {
  try {
    router.push({ path: '/settings', query: { sec: 'playnite' } });
  } catch {
    // ignore navigation errors
  }
}

async function fetchPlayniteStatus(): Promise<void> {
  // Only attempt when authenticated; http layer blocks otherwise
  if (!auth.isAuthenticated) return;
  try {
    const r = await http.get('/api/playnite/status', { validateStatus: () => true });
    if (
      r.status === 200 &&
      r.data &&
      typeof r.data === 'object' &&
      r.data !== null &&
      'installed' in (r.data as Record<string, unknown>)
    ) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const data = r.data as any;
      playniteInstalled.value = data.installed === true || data.active === true;
    }
  } catch {
    // ignore; will retry on next auth change
  } finally {
    playniteStatusReady.value = true;
  }
}

onMounted(async () => {
  // Ensure metadata/config present for platform + playnite detection
  try {
    await configStore.fetchConfig?.();
  } catch {}
  // Defer Playnite status until authenticated to avoid 401/canceled requests
  if (auth.isAuthenticated) {
    void fetchPlayniteStatus();
  } else {
    playniteStatusReady.value = false; // not ready yet
  }
  // Also load apps list (safe if already loaded by bootstrap)
  try {
    await appsStore.loadApps(true);
  } catch {}
});

// When user logs in while this view is mounted, refresh Playnite status
auth.onLogin(() => {
  playniteStatusReady.value = false;
  void fetchPlayniteStatus();
});
</script>
<style scoped>
.main-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  background: rgba(253, 184, 19, 0.9);
  color: #212121;
  font-size: 11px;
  font-weight: 500;
  padding: 6px 12px;
  border-radius: 6px;
}

.main-btn:hover {
  background: #fdb813;
}

.dark .main-btn {
  background: rgba(77, 163, 255, 0.85);
  color: #050b1e;
}

.dark .main-btn:hover {
  background: #4da3ff;
}
/* Row chevron styling adapts via text color set inline */
</style>
