<template>
  <div class="space-y-6">
    <div class="apps-header flex flex-col sm:flex-row sm:items-center sm:justify-between gap-2">
      <h2 class="text-sm font-semibold uppercase tracking-wider">Applications</h2>
      <div
        class="actions flex items-center gap-2 sm:flex-nowrap sm:justify-end w-full sm:w-auto flex-wrap"
      >
        <!-- Windows + Playnite actions -->
        <template v-if="isWindows">
          <template v-if="playniteStatusReady">
            <n-button v-if="playniteEnabled" size="small" secondary @click="purgeAutoSync">
            <i class="fas fa-trash" />
            {{ $t('playnite.delete_all_autosync') || 'Delete All Auto-Sync' }}
            </n-button>
            <n-button
              v-if="playniteEnabled"
              size="small"
              :loading="syncBusy"
              :disabled="syncBusy"
              @click="forceSync"
            >
              <i class="fas fa-rotate" /> {{ $t('playnite.force_sync') || 'Force Sync' }}
            </n-button>
            <n-button v-else size="small" tertiary @click="gotoPlaynite">
              <i class="fas fa-plug" /> {{ $t('playnite.setup_integration') || 'Setup Playnite' }}
            </n-button>
          </template>
        </template>
        <n-button type="primary" size="small" @click="openAdd">
          <i class="fas fa-plus" /> Add
        </n-button>
      </div>
    </div>

    <!-- Redesigned list view -->
    <div
      class="rounded-2xl overflow-hidden border border-dark/10 dark:border-light/10 bg-light/80 dark:bg-surface/80 backdrop-blur max-w-3xl mx-auto"
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
            class="flex items-center justify-between px-4 py-3 hover:bg-dark/10 dark:hover:bg-light/10"
          >
            <div class="min-w-0 flex-1">
              <div class="text-sm font-semibold truncate flex items-center gap-2">
                <span class="truncate">{{ app.name || '(untitled)' }}</span>
                <!-- Playnite or Custom badges -->
                <template v-if="app['playnite-id']">
                  <span
                    class="inline-flex items-center px-1.5 py-0.5 rounded bg-primary/15 text-primary text-[10px] font-semibold"
                  >
                    Playnite
                  </span>
                  <span v-if="app['playnite-managed'] === 'manual'" class="text-[10px] opacity-70"
                    >manual</span
                  >
                  <span
                    v-else-if="(app['playnite-source'] || '') === 'recent'"
                    class="text-[10px] opacity-70"
                    >recent</span
                  >
                  <span
                    v-else-if="(app['playnite-source'] || '') === 'category'"
                    class="text-[10px] opacity-70"
                    >category</span
                  >
                  <span
                    v-else-if="(app['playnite-source'] || '') === 'recent+category'"
                    class="text-[10px] opacity-70"
                    >recent+category</span
                  >
                  <span v-else class="text-[10px] opacity-70">managed</span>
                </template>
                <template v-else>
                  <span
                    class="inline-flex items-center px-1.5 py-0.5 rounded bg-dark/10 dark:bg-light/10 text-[10px] font-semibold"
                  >
                    Custom
                  </span>
                </template>
              </div>
              <div class="mt-0.5 text-[11px] opacity-60 truncate" v-if="app['working-dir']">
                {{ app['working-dir'] }}
              </div>
            </div>
            <div class="shrink-0 text-dark/50 dark:text-light/70">
              <i class="fas fa-chevron-right" />
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
      :key="modalKey + '|' + (currentIndex ?? -1) + '|' + (currentApp?.uuid || currentApp?.name || 'new')"
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
import { NButton } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';

// Minimal shape used for rendering items returned by the backend
interface AppsListItem {
  uuid?: string;
  name?: string;
  'working-dir'?: string;
  'playnite-id'?: string;
  'playnite-managed'?: string;
  'playnite-source'?: string;
}

const appsStore = useAppsStore();
const { apps } = storeToRefs(appsStore);
const configStore = useConfigStore();
const auth = useAuthStore();

const syncBusy = ref(false);
const isWindows = computed(() => (configStore.metadata?.platform || '').toLowerCase() === 'windows');

const playniteInstalled = ref(false);
const playniteStatusReady = ref(false);
const playniteEnabled = computed(() => playniteInstalled.value);

const showModal = ref(false);
const modalKey = ref(0);
const currentApp = ref<AppsListItem | null>(null);
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

async function purgeAutoSync(): Promise<void> {
  if (typeof window !== 'undefined') {
    const ok = window.confirm('Delete all Playnite auto-synced apps?');
    if (!ok) return;
  }
  try {
    await http.post('./api/apps/purge_autosync', {}, { validateStatus: () => true });
    await reload();
  } catch {
    /* ignore */
  }
}

async function forceSync(): Promise<void> {
  syncBusy.value = true;
  try {
    await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    await reload();
  } catch {
    /* ignore */
  } finally {
    syncBusy.value = false;
  }
}

function gotoPlaynite(): void {
  try {
    if (typeof window !== 'undefined') window.location.href = '/settings#playnite';
  } catch {
    /* ignore */
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
      playniteInstalled.value = !!(r.data as any).installed;
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
  } catch {
    /* ignore */
  }
  // Defer Playnite status until authenticated to avoid 401/canceled requests
  if (auth.isAuthenticated) {
    void fetchPlayniteStatus();
  } else {
    playniteStatusReady.value = false; // not ready yet
  }
  // Also load apps list (safe if already loaded by bootstrap)
  try {
    await appsStore.loadApps(true);
  } catch {
    /* ignore */
  }
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
