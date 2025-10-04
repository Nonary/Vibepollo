<template>
  <div class="max-w-3xl mx-auto px-6 py-8 space-y-4">
    <!-- Toolbar aligned to card -->
    <div class="flex items-center justify-between">
      <h2 class="text-sm font-semibold uppercase tracking-wider">{{ $t('apps.applications_title') }}</h2>
      <!-- Toolbar: one Primary + one secondary, 8-pt spacing -->
      <n-space align="center" :size="16" class="items-center">
        <!-- Windows + Playnite secondary action -->
        <template v-if="isWindows">
          <n-button
            v-if="playniteEnabled"
            size="small"
            type="default"
            strong
            class="h-10 px-3 rounded-md"
            :loading="syncBusy"
            :disabled="syncBusy"
            @click="forceSync"
            :aria-label="$t('playnite.force_sync')"
          >
            <svg
              class="w-4 h-4 mr-2 inline-block"
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
            {{ $t('playnite.force_sync') }}
          </n-button>

          <!-- Setup Playnite when disabled -->
          <n-button
            v-else
            size="small"
            type="default"
            strong
            @click="gotoPlaynite"
            class="h-10 px-3"
          >
            <svg
              class="w-4 h-4 mr-2 inline-block"
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
            {{ $t('playnite.setup_integration') }}
          </n-button>
        </template>

        <!-- Primary: Add -->
        <n-button type="primary" size="small" strong class="h-10 px-4 rounded-md" @click="openAdd">
          <svg
            class="w-4 h-4 mr-2 inline-block"
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
          {{ $t('_common.add') }}
        </n-button>
      </n-space>
    </div>

    <!-- Redesigned list view -->
    <div
      class="rounded-2xl overflow-hidden border border-dark/10 dark:border-light/10 bg-light/80 dark:bg-surface/80 backdrop-blur"
    >
      <template v-if="orderedApps.length">
        <div class="px-6 py-3 text-[11px] uppercase tracking-wide opacity-60 flex items-center justify-between">
          <span>{{ $t('apps.reorder_hint') }}</span>
          <span v-if="reorderDirty" class="font-medium text-primary">{{ $t('apps.reorder_unsaved') }}</span>
        </div>
        <div class="divide-y divide-black/5 dark:divide-white/10">
          <div
            v-for="(app, i) in orderedApps"
            :key="appKey(app, i)"
            class="relative"
            @dragover.prevent="handleDragOver(i, $event)"
            @drop.prevent="handleDrop($event)"
          >
            <div v-if="dragInsertIndex === i" class="drag-indicator" style="top: -1px;"></div>
            <div
              class="app-row w-full text-left focus:outline-none focus-visible:ring-2 focus-visible:ring-primary/40"
              role="button"
              tabindex="0"
              draggable="true"
              @dragstart="handleDragStart(i, $event)"
              @dragend="handleDragEnd"
              @click="openEdit(app, i)"
              @keydown.enter.prevent="openEdit(app, i)"
              @keydown.space.prevent="openEdit(app, i)"
            >
              <div
                class="flex items-center gap-4 px-6 py-4 min-h-[56px] hover:bg-dark/10 dark:hover:bg-light/10"
                :class="{ 'dragging-row': draggingIndex === i }"
              >
                <div
                  class="drag-handle"
                  @click.stop
                  :aria-label="$t('apps.drag_handle_label')"
                  tabindex="-1"
                  role="button"
                >
                  <svg
                    class="w-3.5 h-3.5"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                  >
                    <path
                      stroke-linecap="round"
                      stroke-linejoin="round"
                      stroke-width="1.6"
                      d="M7 6h0m0 6h0m0 6h0m10-12h0m0 6h0m0 6h0"
                    />
                  </svg>
                </div>
                <div class="app-icon-container">
                  <img
                    v-if="resolveAppIcon(app)"
                    :src="resolveAppIcon(app)"
                    class="app-icon-image"
                    :alt="(app.name || 'Application') + ' icon'"
                    loading="lazy"
                  />
                  <div v-else class="app-icon-fallback">{{ appInitial(app) }}</div>
                </div>
                <div class="min-w-0 flex-1">
                  <div class="text-sm font-semibold truncate flex items-center gap-2">
                    <span class="truncate">{{ app.name || $t('apps.untitled') }}</span>
                    <!-- Playnite or Custom badges -->
                    <template v-if="app['playnite-id']">
                      <n-tag
                        size="small"
                        class="!px-2 !py-0.5 text-xs bg-slate-700 border-none text-slate-200"
                        >{{ $t('apps.playnite_badge') }}</n-tag
                      >
                      <span
                        v-if="app['playnite-managed'] === 'manual'"
                        class="text-[10px] opacity-70"
                        >{{ $t('apps.playnite_label_manual') }}</span
                      >
                      <span
                        v-else-if="(app['playnite-source'] || '') === 'recent'"
                        class="text-[10px] opacity-70"
                        >{{ $t('apps.playnite_label_recent') }}</span
                      >
                      <span
                        v-else-if="(app['playnite-source'] || '') === 'category'"
                        class="text-[10px] opacity-70"
                        >{{ $t('apps.playnite_label_category') }}</span
                      >
                      <span
                        v-else-if="(app['playnite-source'] || '') === 'recent+category'"
                        class="text-[10px] opacity-70"
                        >{{ $t('apps.playnite_label_recent_category') }}</span
                      >
                      <span v-else class="text-[10px] opacity-70">{{ $t('apps.playnite_label_managed') }}</span>
                    </template>
                    <template v-else>
                      <n-tag
                        size="small"
                        class="!px-2 !py-0.5 text-xs bg-slate-700/70 border-none text-slate-200"
                        >{{ $t('apps.custom_badge') }}</n-tag
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
            </div>
            <div v-if="dragInsertIndex === i + 1" class="drag-indicator" style="bottom: -1px;"></div>
          </div>
        </div>
        <div
          v-if="orderedApps.length > 1"
          class="flex items-center justify-between px-6 py-4 border-t border-black/5 dark:border-white/10 bg-dark/5 dark:bg-light/5"
        >
          <div class="text-xs opacity-70">
            <span v-if="reorderDirty">{{ $t('apps.reorder_dirty_notice') }}</span>
            <span v-else>{{ $t('apps.reorder_clean_notice') }}</span>
          </div>
          <n-space :size="8" align="center">
            <n-button
              tertiary
              size="small"
              @click="alphabetize"
              :disabled="reorderSaving || orderedApps.length < 2"
            >
              {{ $t('apps.alphabetize') }}
            </n-button>
            <n-button
              size="small"
              type="default"
              strong
              @click="resetOrder"
              :disabled="reorderSaving || !reorderDirty"
            >
              {{ $t('apps.reorder_reset') }}
            </n-button>
            <n-button
              type="primary"
              size="small"
              strong
              @click="saveOrder"
              :loading="reorderSaving"
              :disabled="!reorderDirty"
            >
              {{ $t('apps.reorder_save') }}
            </n-button>
          </n-space>
        </div>
      </template>
      <div v-else class="px-6 py-10 text-center text-sm opacity-60">
        {{ $t('apps.none_configured') }}
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
const AppEditModal = defineAsyncComponent(() => import('@/components/AppEditModal.vue'));
import { useAppsStore } from '@/stores/apps';
import { storeToRefs } from 'pinia';
import { NButton, NSpace, NTag, useMessage } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';
import { useRouter } from 'vue-router';
import { useAuthStore } from '@/stores/auth';
import type { App } from '@/stores/apps';
import { useI18n } from 'vue-i18n';

const appsStore = useAppsStore();
const { apps } = storeToRefs(appsStore);
const configStore = useConfigStore();
const auth = useAuthStore();
const router = useRouter();
const message = useMessage();
const { t } = useI18n();

const orderedApps = ref<App[]>([]);
const reorderDirty = ref(false);
const reorderSaving = ref(false);
const draggingIndex = ref<number | null>(null);
const dragInsertIndex = ref<number | null>(null);
const suppressClick = ref(false);

watch(
  apps,
  (next) => {
    const list = Array.isArray(next) ? next : [];
    if (!reorderDirty.value) {
      orderedApps.value = list.slice();
      return;
    }

    const byUuid = new Map<string, App>();
    list.forEach((item) => {
      if (item && typeof item === 'object' && item.uuid) {
        byUuid.set(item.uuid, item);
      }
    });

    const updated: App[] = [];
    const seen = new Set<string>();

    orderedApps.value.forEach((item) => {
      if (item?.uuid && byUuid.has(item.uuid)) {
        updated.push(byUuid.get(item.uuid)!);
        seen.add(item.uuid);
      } else if (!item?.uuid) {
        updated.push(item);
      }
    });

    list.forEach((item) => {
      if (item?.uuid && !seen.has(item.uuid)) {
        updated.push(item);
      }
    });

    orderedApps.value = updated;
  },
  { immediate: true },
);

watch(reorderDirty, (dirty) => {
  if (!dirty) {
    orderedApps.value = (Array.isArray(apps.value) ? apps.value : []).slice();
  }
});

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

function resetDragState(): void {
  draggingIndex.value = null;
  dragInsertIndex.value = null;
  window.setTimeout(() => {
    suppressClick.value = false;
  }, 0);
}

function handleDragStart(index: number, event: DragEvent): void {
  if (reorderSaving.value) return;
  if (index < 0 || index >= orderedApps.value.length) return;
  draggingIndex.value = index;
  dragInsertIndex.value = index;
  suppressClick.value = true;
  const uuid = orderedApps.value[index]?.uuid;
  if (uuid) {
    event.dataTransfer?.setData('text/plain', uuid);
  }
  if (event.dataTransfer) {
    event.dataTransfer.effectAllowed = 'move';
  }
}

function handleDragOver(index: number, event: DragEvent): void {
  if (draggingIndex.value === null) return;
  event.preventDefault();
  const target = event.currentTarget as HTMLElement | null;
  if (!target) return;
  const rect = target.getBoundingClientRect();
  const offset = event.clientY - rect.top;
  const position = offset > rect.height / 2 ? index + 1 : index;
  if (dragInsertIndex.value !== position) {
    dragInsertIndex.value = position;
  }
  if (event.dataTransfer) {
    event.dataTransfer.dropEffect = 'move';
  }
}

function applyReorder(): void {
  const from = draggingIndex.value;
  let to = dragInsertIndex.value;
  if (from === null || to === null) {
    resetDragState();
    return;
  }
  const list = orderedApps.value.slice();
  if (from < 0 || from >= list.length) {
    resetDragState();
    return;
  }
  if (to > list.length) {
    to = list.length;
  }
  if (to === from || to === from + 1) {
    resetDragState();
    return;
  }
  const [moved] = list.splice(from, 1);
  if (!moved) {
    resetDragState();
    return;
  }
  if (to > from) {
    to -= 1;
  }
  list.splice(to, 0, moved);
  orderedApps.value = list;
  reorderDirty.value = true;
  resetDragState();
}

function handleDrop(event: DragEvent): void {
  if (draggingIndex.value === null || dragInsertIndex.value === null) {
    resetDragState();
    return;
  }
  event.preventDefault();
  applyReorder();
}

function handleDragEnd(): void {
  resetDragState();
}

function resetOrder(): void {
  orderedApps.value = (Array.isArray(apps.value) ? apps.value : []).slice();
  reorderDirty.value = false;
  resetDragState();
}

function alphabetize(): void {
  if (orderedApps.value.length < 2) return;
  const snapshot = orderedApps.value.slice();
  const sorted = snapshot.slice().sort((a, b) => {
    const nameA = a?.name || '';
    const nameB = b?.name || '';
    return nameA.localeCompare(nameB, undefined, { sensitivity: 'base', numeric: true });
  });
  const changed = sorted.some((item, idx) => item !== snapshot[idx]);
  if (!changed) {
    message.info(t('apps.alphabetize_done'));
    return;
  }
  orderedApps.value = sorted;
  reorderDirty.value = true;
  resetDragState();
}

async function saveOrder(): Promise<void> {
  if (!reorderDirty.value || reorderSaving.value) return;
  const uuids = orderedApps.value
    .map((item) => item?.uuid)
    .filter((uuid): uuid is string => typeof uuid === 'string' && uuid.length > 0);
  if (!uuids.length) {
    message.warning(t('apps.reorder_none'));
    return;
  }
  reorderSaving.value = true;
  const result = await appsStore.reorderApps(uuids);
  if (result.ok) {
    message.success(t('apps.reorder_saved'));
    reorderDirty.value = false;
  } else {
    message.error(result.error || t('apps.reorder_save_failed'));
  }
  reorderSaving.value = false;
}

async function reload(): Promise<void> {
  reorderDirty.value = false;
  await appsStore.loadApps(true);
}

function openAdd(): void {
  currentApp.value = null;
  currentIndex.value = -1;
  showModal.value = true;
}

function openEdit(app: App, i: number): void {
  if (suppressClick.value) return;
  const uuid = app?.uuid;
  if (uuid) {
    const idx = apps.value.findIndex((item) => item?.uuid === uuid);
    currentIndex.value = idx >= 0 ? idx : i;
    currentApp.value = idx >= 0 ? apps.value[idx] : app;
  } else {
    currentIndex.value = i;
    currentApp.value = app;
  }
  showModal.value = true;
}
function appKey(app: App | null | undefined, index: number) {
  const id = app?.uuid || '';
  return `${app?.name || 'app'}|${id}|${index}`;
}

function resolveAppIcon(app: App | null | undefined): string | null {
  if (!app) return null;
  const direct = typeof app['image-path'] === 'string' ? app['image-path'].trim() : '';
  if (direct) {
    if (/^https?:\/\//i.test(direct)) return direct;
    if (direct.startsWith('/')) return direct;
    if (direct.startsWith('covers/')) return `./${direct}`;
    if (direct.startsWith('./')) return direct;
  }
  const coverUrl = (() => {
    const candidate = (app as Record<string, unknown>)['cover-url'];
    if (typeof candidate === 'string' && candidate.trim().length > 0) return candidate.trim();
    const playniteCover = (app as Record<string, unknown>)['playnite-cover'];
    if (typeof playniteCover === 'string' && playniteCover.trim().length > 0) {
      return playniteCover.trim();
    }
    return '';
  })();
  if (coverUrl) return coverUrl;
  const uuid = app.uuid?.trim();
  if (uuid && (direct || app['playnite-id'])) {
    return `./api/apps/${encodeURIComponent(uuid)}/cover`;
  }
  return null;
}

function appInitial(app: App | null | undefined): string {
  const source = app?.name?.trim();
  if (source && source.length > 0) {
    return source.charAt(0).toUpperCase();
  }
  return '?';
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
  }
}

async function fetchPlayniteStatus(): Promise<void> {
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
  } finally {
    playniteStatusReady.value = true;
  }
}

onMounted(async () => {
  try {
    await configStore.fetchConfig?.();
  } catch {}
  if (auth.isAuthenticated) {
    void fetchPlayniteStatus();
  } else {
    playniteStatusReady.value = false;
  }
  try {
    await appsStore.loadApps(true);
  } catch {}
});

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
.drag-handle {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 2.25rem;
  height: 2.25rem;
  border-radius: 9999px;
  color: rgba(0, 0, 0, 0.35);
  cursor: grab;
  transition: background-color 0.2s ease, color 0.2s ease;
}

.drag-handle:hover {
  background-color: rgba(0, 0, 0, 0.06);
}

.drag-handle:active {
  cursor: grabbing;
}

.app-row {
  cursor: grab;
}

.app-row:active {
  cursor: grabbing;
}

:global(.dark) .drag-handle {
  color: rgba(255, 255, 255, 0.45);
}

:global(.dark) .drag-handle:hover {
  background-color: rgba(255, 255, 255, 0.08);
}

.app-icon-container {
  width: 2.75rem;
  height: 2.75rem;
  border-radius: 0.8rem;
  overflow: hidden;
  background: rgba(0, 0, 0, 0.08);
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: inset 0 0 0 1px rgba(15, 23, 42, 0.08);
}

.app-icon-image {
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
}

.app-icon-fallback {
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 600;
  font-size: 0.875rem;
  color: rgba(15, 23, 42, 0.7);
  background: linear-gradient(135deg, rgba(226, 232, 240, 0.9), rgba(203, 213, 225, 0.9));
}

:global(.dark) .app-icon-container {
  background: rgba(255, 255, 255, 0.06);
  box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.06);
}

:global(.dark) .app-icon-fallback {
  color: rgba(255, 255, 255, 0.85);
  background: linear-gradient(135deg, rgba(100, 116, 139, 0.65), rgba(71, 85, 105, 0.65));
}

.drag-indicator {
  pointer-events: none;
  position: absolute;
  left: 24px;
  right: 24px;
  height: 2px;
  border-radius: 999px;
  background: rgba(253, 184, 19, 0.85);
}

:global(.dark) .drag-indicator {
  background: rgba(77, 163, 255, 0.85);
}

.dragging-row {
  opacity: 0.7;
}
</style>
