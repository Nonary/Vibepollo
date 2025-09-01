<template>
  <n-modal :show="open" :mask-closable="true" @update:show="(v) => emit('update:modelValue', v)">
    <n-card
      :bordered="false"
      :content-style="{
        display: 'flex',
        flexDirection: 'column',
        minHeight: 0,
        overflow: 'hidden',
      }"
      class="overflow-hidden"
      style="
        max-width: 56rem;
        width: 100%;
        height: min(85dvh, calc(100dvh - 2rem));
        max-height: calc(100dvh - 2rem);
      "
    >
      <template #header>
        <div class="flex items-center justify-between gap-3">
          <div class="flex items-center gap-3">
            <div
              class="h-14 w-14 rounded-full bg-gradient-to-br from-primary/20 to-primary/10 text-primary flex items-center justify-center shadow-inner"
            >
              <i class="fas fa-window-restore text-xl" />
            </div>
            <div class="flex flex-col">
              <span class="text-xl font-semibold">{{
                form.index === -1 ? 'Add Application' : 'Edit Application'
              }}</span>
            </div>
          </div>
          <div class="shrink-0">
            <span
              v-if="isPlaynite"
              class="inline-flex items-center px-2 py-0.5 rounded bg-primary/15 text-primary text-[11px] font-semibold"
            >
              Playnite
            </span>
            <span
              v-else
              class="inline-flex items-center px-2 py-0.5 rounded bg-dark/10 dark:bg-light/10 text-[11px] font-semibold"
            >
              Custom
            </span>
          </div>
        </div>
      </template>

      <div
        ref="bodyRef"
        class="relative flex-1 min-h-0 overflow-auto pr-1"
        style="padding-bottom: calc(env(safe-area-inset-bottom) + 0.5rem)"
      >
        <!-- Scroll affordance shadows: appear when more content is available -->
        <div v-if="showTopShadow" class="scroll-shadow-top" aria-hidden="true"></div>
        <div v-if="showBottomShadow" class="scroll-shadow-bottom" aria-hidden="true"></div>

        <form
          class="space-y-6 text-sm"
          @submit.prevent="save"
          @keydown.ctrl.enter.stop.prevent="save"
        >
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <div class="space-y-1 md:col-span-2">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Name</label>
              <!-- Unified combobox: type any name; suggestions from Playnite if available -->
              <div class="flex items-center gap-2 mb-1">
                <n-select
                  v-model:value="nameSelectValue"
                  :options="nameSelectOptions"
                  :loading="gamesLoading"
                  filterable
                  clearable
                  :placeholder="'Type to search or enter a custom name'"
                  class="flex-1"
                  :fallback-option="fallbackOption"
                  @focus="onNameFocus"
                  @search="onNameSearch"
                  @update:value="onNamePicked"
                />
              </div>
              <!-- When adding a new app on Windows, allow picking a Playnite game (disabled if plugin not installed) -->
              <template v-if="isNew && isWindows && newAppSource === 'playnite'">
                <div class="flex items-center gap-2">
                  <n-select
                    v-model:value="selectedPlayniteId"
                    :options="playniteOptions"
                    :loading="gamesLoading"
                    filterable
                    :disabled="lockPlaynite || !playniteInstalled"
                    :placeholder="
                      playniteInstalled ? 'Select a Playnite game…' : 'Playnite plugin not detected'
                    "
                    class="flex-1"
                    @focus="loadPlayniteGames"
                    @update:value="onPickPlaynite"
                  />
                  <n-button v-if="lockPlaynite" size="small" tertiary @click="unlockPlaynite">
                    Change
                  </n-button>
                </div>
              </template>
              <div class="text-[11px] opacity-60">
                {{ isPlaynite ? 'Linked to Playnite' : 'Custom application' }}
              </div>
            </div>
            <div v-if="!isPlaynite" class="space-y-1 md:col-span-2">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Command</label
              >
              <n-input
                v-model:value="cmdText"
                type="textarea"
                :autosize="{ minRows: 4, maxRows: 8 }"
                placeholder="Executable command line"
              />
              <p class="text-[11px] opacity-60">Enter the full command line (single string).</p>
            </div>
            <div v-if="!isPlaynite" class="space-y-1 md:col-span-1">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Working Dir</label
              >
              <n-input
                v-model:value="form.workingDir"
                class="font-mono"
                placeholder="C:/Games/App"
              />
            </div>
            <div class="space-y-1 md:col-span-1">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Exit Timeout</label
              >
              <div class="flex items-center gap-2">
                <n-input-number v-model:value="form.exitTimeout" :min="0" class="w-28" />
                <span class="text-xs opacity-60">seconds</span>
              </div>
            </div>
            <div v-if="!isPlaynite" class="space-y-1 md:col-span-2">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Image Path</label
              >
              <div class="flex items-center gap-2">
                <n-input
                  v-model:value="form.imagePath"
                  class="font-mono flex-1"
                  placeholder="/path/to/image.png"
                />
                <n-button tertiary :disabled="!form.name" @click="openCoverFinder">
                  <i class="fas fa-image" /> Find Cover
                </n-button>
              </div>
              <p class="text-[11px] opacity-60">
                Optional; stored only and not fetched by Sunshine.
              </p>
            </div>
          </div>

          <div class="grid grid-cols-2 gap-3">
            <n-checkbox v-model:checked="form.excludeGlobalPrepCmd" size="small">
              Exclude Global Prep
            </n-checkbox>
            <n-checkbox v-if="!isPlaynite" v-model:checked="form.autoDetach" size="small">
              Auto Detach
            </n-checkbox>
            <n-checkbox v-if="!isPlaynite" v-model:checked="form.waitAll" size="small"
              >Wait All</n-checkbox
            >
            <n-checkbox
              v-if="isWindows && !isPlaynite"
              v-model:checked="form.elevated"
              size="small"
            >
              Elevated
            </n-checkbox>
          </div>

          <section class="space-y-3">
            <div class="flex items-center justify-between">
              <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">
                Prep Commands
              </h3>
              <n-button size="small" type="primary" @click="addPrep">
                <i class="fas fa-plus" /> Add
              </n-button>
            </div>
            <div v-if="form.prepCmd.length === 0" class="text-[12px] opacity-60">None</div>
            <div v-else class="space-y-2">
              <div
                v-for="(p, i) in form.prepCmd"
                :key="i"
                class="rounded-md border border-dark/10 dark:border-light/10 p-2"
              >
                <div class="flex items-center justify-between gap-2 mb-2">
                  <div class="text-xs opacity-70">Step {{ i + 1 }}</div>
                  <div class="flex items-center gap-2">
                    <n-checkbox v-if="isWindows" v-model:checked="p.elevated" size="small">
                      {{ $t('_common.elevated') }}
                    </n-checkbox>
                    <n-button size="small" secondary @click="form.prepCmd.splice(i, 1)">
                      <i class="fas fa-trash" />
                    </n-button>
                  </div>
                </div>
                <div class="grid grid-cols-1 gap-2">
                  <div>
                    <label class="text-[11px] opacity-60">{{ $t('_common.do_cmd') }}</label>
                    <n-input
                      v-model:value="p.do"
                      type="textarea"
                      :autosize="{ minRows: 1, maxRows: 3 }"
                      class="font-mono"
                      placeholder="Command to run before start"
                    />
                  </div>
                  <div>
                    <label class="text-[11px] opacity-60">{{ $t('_common.undo_cmd') }}</label>
                    <n-input
                      v-model:value="p.undo"
                      type="textarea"
                      :autosize="{ minRows: 1, maxRows: 3 }"
                      class="font-mono"
                      placeholder="Command to run on stop"
                    />
                  </div>
                </div>
              </div>
            </div>
          </section>

          <section v-if="!isPlaynite" class="space-y-3">
            <div class="flex items-center justify-between">
              <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">
                Detached Commands
              </h3>
              <n-button size="small" type="primary" @click="addDetached">
                <i class="fas fa-plus" /> Add
              </n-button>
            </div>
            <div v-if="form.detached.length === 0" class="text-[12px] opacity-60">None</div>
            <div v-else class="space-y-2">
              <div v-for="(d, i) in form.detached" :key="i" class="flex gap-2 items-center">
                <n-input v-model:value="form.detached[i]" class="font-mono flex-1" />
                <n-button secondary @click="form.detached.splice(i, 1)">
                  <i class="fas fa-times" />
                </n-button>
              </div>
            </div>
          </section>
          <section class="sr-only">
            <!-- hidden submit to allow Enter to save within fields -->
            <button type="submit" tabindex="-1" aria-hidden="true"></button>
          </section>
        </form>
      </div>

      <template #footer>
        <div
          class="flex items-center justify-end w-full gap-2 border-t border-dark/10 dark:border-light/10 bg-light/80 dark:bg-surface/80 backdrop-blur px-2 py-2"
        >
          <n-button tertiary @click="close">{{ $t('_common.cancel') }}</n-button>
          <n-button
            v-if="form.index !== -1"
            type="error"
            :disabled="saving"
            @click="showDeleteConfirm = true"
          >
            <i class="fas fa-trash" /> {{ $t('apps.delete') }}
          </n-button>
          <n-button type="primary" :loading="saving" :disabled="saving" @click="save">
            <i class="fas fa-save" /> {{ $t('_common.save') }}
          </n-button>
        </div>
      </template>

      <n-modal
        :show="showCoverModal"
        :z-index="3300"
        :mask-style="{ backgroundColor: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }"
        @update:show="(v) => (showCoverModal = v)"
      >
        <n-card :bordered="false" style="max-width: 48rem; width: 100%">
          <template #header>
            <div class="flex items-center justify-between w-full">
              <span class="font-semibold">Covers Found</span>
              <n-button quaternary size="small" @click="showCoverModal = false"> Close </n-button>
            </div>
          </template>
          <div class="min-h-[160px]">
            <div v-if="coverSearching" class="flex items-center justify-center py-10">
              <n-spin size="large">Loading…</n-spin>
            </div>
            <div v-else>
              <div
                class="grid grid-cols-2 sm:grid-cols-3 md:grid-cols-4 gap-3 max-h-[420px] overflow-auto pr-1"
              >
                <div
                  v-for="(cover, i) in coverCandidates"
                  :key="i"
                  class="cursor-pointer group"
                  @click="useCover(cover)"
                >
                  <div
                    class="relative rounded overflow-hidden aspect-[3/4] bg-black/5 dark:bg-white/5"
                  >
                    <img :src="cover.url" class="absolute inset-0 w-full h-full object-cover" />
                    <div
                      v-if="coverBusy"
                      class="absolute inset-0 bg-black/20 dark:bg-white/10 flex items-center justify-center"
                    >
                      <n-spin size="small" />
                    </div>
                  </div>
                  <div class="mt-1 text-xs text-center truncate" :title="cover.name">
                    {{ cover.name }}
                  </div>
                </div>
                <div
                  v-if="!coverCandidates.length"
                  class="col-span-full text-center opacity-70 py-8"
                >
                  No results. Try adjusting the app name.
                </div>
              </div>
            </div>
          </div>
        </n-card>
      </n-modal>

      <n-modal
        :show="showDeleteConfirm"
        :z-index="3300"
        :mask-style="{ backgroundColor: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }"
        @update:show="(v) => (showDeleteConfirm = v)"
      >
        <n-card
          :title="
            isPlayniteAuto
              ? 'Remove and Exclude from Auto‑Sync?'
              : ($t('apps.confirm_delete_title_named', { name: form.name || '' }) as any)
          "
          :bordered="false"
          style="max-width: 32rem; width: 100%"
        >
          <div class="text-sm text-center space-y-2">
            <template v-if="isPlayniteAuto">
              <div>
                This application is managed by Playnite. Removing it will also add it to the
                Excluded Games list so it won’t be auto‑synced back.
              </div>
              <div class="opacity-80">
                You can bring it back later by manually adding it in Applications, or by removing
                the exclusion under Settings → Playnite.
              </div>
              <div class="opacity-70">Do you want to continue?</div>
            </template>
            <template v-else>
              {{ $t('apps.confirm_delete_message_named', { name: form.name || '' }) }}
            </template>
          </div>
          <template #footer>
            <div class="w-full flex items-center justify-center gap-3">
              <n-button tertiary @click="showDeleteConfirm = false">{{
                $t('_common.cancel')
              }}</n-button>
              <n-button secondary @click="del">{{ $t('apps.delete') }}</n-button>
            </div>
          </template>
        </n-card>
      </n-modal>
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { computed, ref, watch, onMounted, onBeforeUnmount } from 'vue';
import { http } from '@/http';
import { NModal, NCard, NButton, NInput, NInputNumber, NCheckbox, NSelect, NSpin } from 'naive-ui';
import { useConfigStore } from '@/stores/config';

// Types for form and server payload
interface PrepCmd {
  do: string;
  undo: string;
  elevated?: boolean;
}
interface AppForm {
  index: number;
  name: string;
  output: string;
  cmd: string;
  workingDir: string;
  imagePath: string;
  excludeGlobalPrepCmd: boolean;
  elevated: boolean;
  autoDetach: boolean;
  waitAll: boolean;
  exitTimeout: number;
  prepCmd: PrepCmd[];
  detached: string[];
  playniteId?: string;
  playniteManaged?: 'manual' | string;
}
interface ServerApp {
  name?: string;
  output?: string;
  cmd?: string | string[];
  uuid?: string;
  'working-dir'?: string;
  'image-path'?: string;
  'exclude-global-prep-cmd'?: boolean;
  elevated?: boolean;
  'auto-detach'?: boolean;
  'wait-all'?: boolean;
  'exit-timeout'?: number;
  'prep-cmd'?: Array<{ do?: string; undo?: string; elevated?: boolean }>;
  detached?: string[];
  'playnite-id'?: string;
  'playnite-managed'?: 'manual' | string;
}

interface AppEditModalProps {
  modelValue: boolean;
  app?: ServerApp | null;
  index?: number;
}

const props = defineProps<AppEditModalProps>();
const emit = defineEmits<{
  (e: 'update:modelValue', v: boolean): void;
  (e: 'saved'): void;
  (e: 'deleted'): void;
}>();
const open = computed<boolean>(() => !!props.modelValue);
function fresh(): AppForm {
  return {
    index: -1,
    name: '',
    cmd: '',
    workingDir: '',
    imagePath: '',
    excludeGlobalPrepCmd: false,
    elevated: false,
    autoDetach: true,
    waitAll: true,
    exitTimeout: 5,
    prepCmd: [],
    detached: [],
    output: '',
  };
}
const form = ref<AppForm>(fresh());

function fromServerApp(src?: ServerApp | null, idx: number = -1): AppForm {
  const base = fresh();
  if (!src) return { ...base, index: idx };
  const cmdStr = Array.isArray(src.cmd) ? src.cmd.join(' ') : (src.cmd ?? '');
  const prep = Array.isArray(src['prep-cmd'])
    ? src['prep-cmd'].map((p) => ({
        do: String(p?.do ?? ''),
        undo: String(p?.undo ?? ''),
        elevated: !!p?.elevated,
      }))
    : [];
  return {
    index: idx,
    name: String(src.name ?? ''),
    output: String(src.output ?? ''),
    cmd: String(cmdStr ?? ''),
    workingDir: String(src['working-dir'] ?? ''),
    imagePath: String(src['image-path'] ?? ''),
    excludeGlobalPrepCmd: !!src['exclude-global-prep-cmd'],
    elevated: !!src.elevated,
    autoDetach: src['auto-detach'] !== undefined ? !!src['auto-detach'] : base.autoDetach,
    waitAll: src['wait-all'] !== undefined ? !!src['wait-all'] : base.waitAll,
    exitTimeout: typeof src['exit-timeout'] === 'number' ? src['exit-timeout'] : base.exitTimeout,
    prepCmd: prep,
    detached: Array.isArray(src.detached) ? src.detached.map((s) => String(s)) : [],
    playniteId: src['playnite-id'] || undefined,
    playniteManaged: src['playnite-managed'] || undefined,
  };
}

function toServerPayload(f: AppForm): Record<string, any> {
  const payload: Record<string, any> = {
    name: f.name,
    output: f.output,
    cmd: f.cmd,
    'working-dir': f.workingDir,
    'image-path': String(f.imagePath || '').replace(/\"/g, ''),
    'exclude-global-prep-cmd': !!f.excludeGlobalPrepCmd,
    elevated: !!f.elevated,
    'auto-detach': !!f.autoDetach,
    'wait-all': !!f.waitAll,
    'exit-timeout': Number.isFinite(f.exitTimeout) ? f.exitTimeout : 5,
    'prep-cmd': f.prepCmd.map((p) => ({
      do: p.do,
      undo: p.undo,
      ...(isWindows.value ? { elevated: !!p.elevated } : {}),
    })),
    detached: Array.isArray(f.detached) ? f.detached : [],
  };
  if (f.playniteId) payload['playnite-id'] = f.playniteId;
  if (f.playniteManaged) payload['playnite-managed'] = f.playniteManaged;
  return payload;
}
// Normalize cmd to single string; rehydrate typed form when props.app changes while open
watch(
  () => props.app,
  (val) => {
    if (!open.value) return;
    form.value = fromServerApp(val as ServerApp | undefined, props.index ?? -1);
  },
  { immediate: true },
);
const cmdText = computed<string>({
  get: () => form.value.cmd || '',
  set: (v: string) => {
    form.value.cmd = v;
  },
});
const isPlaynite = computed<boolean>(() => !!form.value.playniteId);
const isPlayniteAuto = computed<boolean>(
  () => isPlaynite.value && form.value.playniteManaged !== 'manual',
);
// Unified name combobox state (supports Playnite suggestions + free-form)
const nameSelectValue = ref<string>('');
const nameOptions = ref<{ label: string; value: string }[]>([]);
const fallbackOption = (value: unknown) => {
  const v = String(value ?? '');
  const label = String(form.value.name || '').trim() || v;
  return { label, value: v };
};
const nameSearchQuery = ref('');
const nameSelectOptions = computed(() => {
  // Prefer dynamically built options (from search)
  if (nameOptions.value.length) return nameOptions.value;
  const list: { label: string; value: string }[] = [];
  const cur = String(form.value.name || '').trim();
  if (cur) list.push({ label: `Custom: "${cur}"`, value: `__custom__:${cur}` });
  if (playniteOptions.value.length) {
    list.push(...playniteOptions.value.slice(0, 20));
  }
  return list;
});

// Populate suggestions immediately on focus so dropdown isn't empty
async function onNameFocus() {
  // Show a friendly placeholder immediately to avoid "No Data"
  if (!playniteOptions.value.length) {
    nameOptions.value = [
      { label: 'Loading Playnite games…', value: '__loading__', disabled: true } as any,
    ];
  }
  // Kick off loading (don’t block the UI), then refresh list
  loadPlayniteGames()
    .catch(() => {})
    .finally(() => {
      onNameSearch(nameSearchQuery.value);
    });
}

function ensureNameSelectionFromForm() {
  const currentName = String(form.value.name || '').trim();
  const opts: { label: string; value: string }[] = [];
  if (currentName) {
    opts.push({ label: `Custom: "${currentName}"`, value: `__custom__:${currentName}` });
  }
  const pid = form.value.playniteId;
  if (pid) {
    const found = playniteOptions.value.find((o) => o.value === String(pid));
    if (found) opts.push(found);
    else if (currentName) opts.push({ label: currentName, value: String(pid) });
  }
  nameOptions.value = opts;
  nameSelectValue.value = pid ? String(pid) : currentName ? `__custom__:${currentName}` : '';
}
watch(open, (o) => {
  if (o) {
    form.value = fromServerApp(props.app ?? undefined, props.index ?? -1);
    // reset playnite picker state when opening
    selectedPlayniteId.value = '';
    lockPlaynite.value = false;
    newAppSource.value = 'custom';
    // refresh Playnite status early so the picker can enable itself
    refreshPlayniteStatus().then(() => {
      if (playniteInstalled.value) void loadPlayniteGames();
    });
    // Update scroll shadows after content paints
    requestAnimationFrame(() => updateShadows());
    // Initialize unified name combobox selection
    ensureNameSelectionFromForm();
  }
});
function close() {
  emit('update:modelValue', false);
}
function addPrep() {
  form.value.prepCmd.push({
    do: '',
    undo: '',
    ...(isWindows.value ? { elevated: false } : {}),
  });
  requestAnimationFrame(() => updateShadows());
}
const saving = ref(false);
const showDeleteConfirm = ref(false);

// Cover finder state (disabled for Playnite-managed apps)
type CoverCandidate = { name: string; key: string; url: string; saveUrl: string };
const showCoverModal = ref(false);
const coverSearching = ref(false);
const coverBusy = ref(false);
const coverCandidates = ref<CoverCandidate[]>([]);

function getSearchBucket(name: string) {
  const prefix = (name || '')
    .substring(0, Math.min((name || '').length, 2))
    .toLowerCase()
    .replace(/[^a-z\d]/g, '');
  return prefix || '@';
}

async function searchCovers(name: string): Promise<CoverCandidate[]> {
  if (!name) return [];
  const searchName = name.replace(/\s+/g, '.').toLowerCase();
  // Use raw.githubusercontent.com to avoid CORS issues
  const dbUrl = 'https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages';
  const bucket = getSearchBucket(name);
  const res = await fetch(`${dbUrl}/buckets/${bucket}.json`);
  if (!res.ok) return [];
  const maps = await res.json();
  const ids = Object.keys(maps || {});
  const promises = ids.map(async (id) => {
    const item = maps[id];
    if (!item?.name) return null;
    if (String(item.name).replace(/\s+/g, '.').toLowerCase().startsWith(searchName)) {
      try {
        const r = await fetch(`${dbUrl}/games/${id}.json`);
        return await r.json();
      } catch {
        return null;
      }
    }
    return null;
  });
  const results = (await Promise.all(promises)).filter(Boolean);
  return results
    .filter((item) => item && item.cover && item.cover.url)
    .map((game) => {
      const thumb: string = game.cover.url;
      const dotIndex = thumb.lastIndexOf('.');
      const slashIndex = thumb.lastIndexOf('/');
      if (dotIndex < 0 || slashIndex < 0) return null as any;
      const slug = thumb.substring(slashIndex + 1, dotIndex);
      return {
        name: game.name,
        key: `igdb_${game.id}`,
        url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${slug}.jpg`,
        saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${slug}.png`,
      } as CoverCandidate;
    })
    .filter(Boolean);
}

async function openCoverFinder() {
  if (isPlaynite.value) return;
  coverCandidates.value = [];
  showCoverModal.value = true;
  coverSearching.value = true;
  try {
    coverCandidates.value = await searchCovers(String(form.value.name || ''));
  } finally {
    coverSearching.value = false;
  }
}

async function useCover(cover: CoverCandidate) {
  if (!cover || coverBusy.value) return;
  coverBusy.value = true;
  try {
    const r = await http.post(
      './api/covers/upload',
      { key: cover.key, url: cover.saveUrl },
      { headers: { 'Content-Type': 'application/json' }, validateStatus: () => true },
    );
    if (r.status >= 200 && r.status < 300 && r.data && r.data.path) {
      form.value.imagePath = String(r.data.path || '');
      showCoverModal.value = false;
    }
  } finally {
    coverBusy.value = false;
  }
}

// Platform + Playnite detection
const configStore = useConfigStore();
const isWindows = computed(
  () => (configStore.metadata?.platform || '').toLowerCase() === 'windows',
);
const playniteInstalled = ref(false);
const isNew = computed(() => form.value.index === -1);
// New app source: 'custom' or 'playnite' (Windows only)
const newAppSource = ref<'custom' | 'playnite'>('custom');

// Playnite picker state
const gamesLoading = ref(false);
const playniteOptions = ref<{ label: string; value: string }[]>([]);
const selectedPlayniteId = ref('');
const lockPlaynite = ref(false);

async function loadPlayniteGames() {
  if (!isWindows.value || gamesLoading.value || playniteOptions.value.length) return;
  // Ensure we have up-to-date install status
  await refreshPlayniteStatus();
  if (!playniteInstalled.value) return;
  gamesLoading.value = true;
  try {
    const r = await http.get('/api/playnite/games');
    const games: any[] = Array.isArray(r.data) ? r.data : [];
    playniteOptions.value = games
      .filter((g) => !!g.installed)
      .map((g) => ({ label: g.name || g.id, value: g.id }))
      .sort((a, b) => a.label.localeCompare(b.label));
  } catch (_) {}
  gamesLoading.value = false;
  // Refresh suggestions (replace placeholder with actual items)
  try {
    onNameSearch(nameSearchQuery.value);
  } catch {}
}

async function refreshPlayniteStatus() {
  try {
    const r = await http.get('/api/playnite/status', { validateStatus: () => true });
    if (r.status === 200 && r.data && typeof r.data === 'object' && r.data !== null) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      playniteInstalled.value = !!(r.data as any).installed;
    }
  } catch (_) {}
}

function onPickPlaynite(id: string) {
  const opt = playniteOptions.value.find((o) => o.value === id);
  if (!opt) return;
  // Lock in selection and set fields
  form.value.name = opt.label;
  form.value.playniteId = id;
  form.value.playniteManaged = 'manual';
  // clear command by default for Playnite managed entries
  if (!form.value.cmd) form.value.cmd = '';
  lockPlaynite.value = true;
  // Reflect selection in unified combobox
  ensureNameSelectionFromForm();
  // Default Exit Timeout for Playnite apps to 10s if unset or left at 5
  const et = form.value.exitTimeout;
  if (typeof et === 'undefined' || et === 5) form.value.exitTimeout = 10;
}
function unlockPlaynite() {
  lockPlaynite.value = false;
}
// When switching to custom source, clear Playnite-specific markers
watch(newAppSource, (v) => {
  if (v === 'custom') {
    form.value.playniteId = undefined;
    form.value.playniteManaged = undefined;
    lockPlaynite.value = false;
    selectedPlayniteId.value = '';
  }
});
function addDetached() {
  form.value.detached.push('');
  requestAnimationFrame(() => updateShadows());
}

// Scroll affordance logic for modal body
const bodyRef = ref<HTMLElement | null>(null);
const showTopShadow = ref(false);
const showBottomShadow = ref(false);

function updateShadows() {
  const el = bodyRef.value;
  if (!el) return;
  const { scrollTop, scrollHeight, clientHeight } = el;
  const hasOverflow = scrollHeight > clientHeight + 1;
  showTopShadow.value = hasOverflow && scrollTop > 4;
  showBottomShadow.value = hasOverflow && scrollTop + clientHeight < scrollHeight - 4;
}

function onBodyScroll() {
  updateShadows();
}

let ro: ResizeObserver | null = null;
onMounted(() => {
  const el = bodyRef.value;
  if (el) {
    el.addEventListener('scroll', onBodyScroll, { passive: true });
  }
  // Update on size/content changes
  try {
    ro = new ResizeObserver(() => updateShadows());
    if (el) ro.observe(el);
  } catch {}
  // Initial calc after next paint
  requestAnimationFrame(() => updateShadows());
});
onBeforeUnmount(() => {
  const el = bodyRef.value;
  if (el) el.removeEventListener('scroll', onBodyScroll as any);
  try {
    ro?.disconnect();
  } catch {}
  ro = null;
});

// Update name options while user searches
function onNameSearch(q: string) {
  nameSearchQuery.value = q || '';
  const query = String(q || '')
    .trim()
    .toLowerCase();
  const list: { label: string; value: string }[] = [];
  if (query.length) {
    list.push({ label: `Custom: "${q}"`, value: `__custom__:${q}` });
  } else {
    const cur = String(form.value.name || '').trim();
    if (cur) list.push({ label: `Custom: "${cur}"`, value: `__custom__:${cur}` });
  }
  if (playniteOptions.value.length) {
    const filtered = (
      query
        ? playniteOptions.value.filter((o) => o.label.toLowerCase().includes(query))
        : playniteOptions.value.slice(0, 100)
    ).slice(0, 100);
    list.push(...filtered);
  }
  nameOptions.value = list;
}

// Handle picking either a Playnite game or a custom name
function onNamePicked(val: string | null) {
  const v = String(val || '');
  if (!v) {
    nameSelectValue.value = '';
    form.value.name = '';
    form.value.playniteId = undefined;
    form.value.playniteManaged = undefined;
    return;
  }
  if (v.startsWith('__custom__:')) {
    const name = v.substring('__custom__:'.length).trim();
    form.value.name = name;
    form.value.playniteId = undefined;
    form.value.playniteManaged = undefined;
    return;
  }
  const opt = playniteOptions.value.find((o) => o.value === v);
  if (opt) {
    form.value.name = opt.label;
    form.value.playniteId = v;
    form.value.playniteManaged = 'manual';
  }
}

// Cover preview logic removed; Sunshine no longer fetches or proxies images
async function save() {
  saving.value = true;
  try {
    // If on Windows and name exactly matches a Playnite game, auto-link it
    try {
      if (
        isWindows.value &&
        !form.value.playniteId &&
        Array.isArray(playniteOptions.value) &&
        playniteOptions.value.length &&
        typeof form.value.name === 'string'
      ) {
        const target = String(form.value.name || '')
          .trim()
          .toLowerCase();
        const exact = playniteOptions.value.find((o) => o.label.trim().toLowerCase() === target);
        if (exact) {
          form.value.playniteId = exact.value;
          form.value.playniteManaged = 'manual';
          // Ensure default Exit Timeout of 10s for Playnite-linked apps
          if (typeof form.value.exitTimeout === 'undefined' || form.value.exitTimeout === 5) {
            form.value.exitTimeout = 10;
          }
        }
      }
    } catch (_) {}
    const payload = toServerPayload(form.value);
    await http.post('./api/apps', payload, {
      headers: { 'Content-Type': 'application/json' },
      validateStatus: () => true,
    });
    emit('saved');
    close();
  } finally {
    saving.value = false;
  }
}
async function del() {
  saving.value = true;
  try {
    // If Playnite auto-managed, add to exclusion list before removing
    const pid = form.value.playniteId;
    if (isPlayniteAuto.value && pid) {
      try {
        // Ensure config store is loaded
        try {
          // @ts-ignore optional chaining for older runtime
          if (!configStore.config) await (configStore.fetchConfig?.() || Promise.resolve());
        } catch {}
        // Start from current local store state to avoid desync
        const current: Array<{ id: string; name: string }> = Array.isArray(
          (configStore.config as any)?.playnite_exclude_games,
        )
          ? ((configStore.config as any).playnite_exclude_games as any)
          : [];
        const map = new Map(current.map((e) => [String(e.id), String(e.name || '')] as const));
        const name = playniteOptions.value.find((o) => o.value === String(pid))?.label || '';
        map.set(String(pid), name);
        const next = Array.from(map.entries()).map(([id, name]) => ({ id, name }));
        // Update local store (keeps UI in sync) and persist via store API
        configStore.updateOption('playnite_exclude_games', next);
        await configStore.save();
      } catch (_) {
        // best-effort; continue with deletion even if exclusion save fails
      }
    }

    await http.delete(`./api/apps/${form.value.index}`, { validateStatus: () => true });
    // Best-effort force sync on Windows environments
    try {
      await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    } catch (_) {}
    emit('deleted');
    close();
  } finally {
    saving.value = false;
  }
}
</script>
<style scoped>
.mobile-only-hidden {
  display: none;
}

/* Mobile-friendly modal sizing and sticky header/footer */
@media (max-width: 640px) {
  :deep(.n-modal .n-card) {
    border-radius: 0 !important;
    max-width: 100vw !important;
    width: 100vw !important;
    height: 100dvh !important;
    max-height: 100dvh !important;
  }
  :deep(.n-modal .n-card .n-card__header),
  :deep(.n-modal .n-card .n-card-header) {
    position: sticky;
    top: 0;
    z-index: 10;
    backdrop-filter: saturate(1.2) blur(8px);
    background: rgb(var(--color-light) / 0.9);
  }
  :deep(.dark .n-modal .n-card .n-card__header),
  :deep(.dark .n-modal .n-card .n-card-header) {
    background: rgb(var(--color-surface) / 0.9);
  }
  :deep(.n-modal .n-card .n-card__footer),
  :deep(.n-modal .n-card .n-card-footer) {
    position: sticky;
    bottom: 0;
    z-index: 10;
    backdrop-filter: saturate(1.2) blur(8px);
    background: rgb(var(--color-light) / 0.9);
    padding-bottom: calc(env(safe-area-inset-bottom) + 0.5rem) !important;
  }
  :deep(.dark .n-modal .n-card .n-card__footer),
  :deep(.dark .n-modal .n-card .n-card-footer) {
    background: rgb(var(--color-surface) / 0.9);
  }
}
.scroll-shadow-top {
  position: sticky;
  top: 0;
  height: 16px;
  background: linear-gradient(
    to bottom,
    rgb(var(--color-light) / 0.9),
    rgb(var(--color-light) / 0)
  );
  pointer-events: none;
  z-index: 1;
}
.dark .scroll-shadow-top {
  background: linear-gradient(
    to bottom,
    rgb(var(--color-surface) / 0.9),
    rgb(var(--color-surface) / 0)
  );
}
.scroll-shadow-bottom {
  position: sticky;
  bottom: 0;
  height: 20px;
  background: linear-gradient(to top, rgb(var(--color-light) / 0.9), rgb(var(--color-light) / 0));
  pointer-events: none;
  z-index: 1;
}
.dark .scroll-shadow-bottom {
  background: linear-gradient(
    to top,
    rgb(var(--color-surface) / 0.9),
    rgb(var(--color-surface) / 0)
  );
}
.ui-input {
  width: 100%;
  border: 1px solid rgba(0, 0, 0, 0.12);
  background: rgba(255, 255, 255, 0.75);
  padding: 8px 10px;
  border-radius: 8px;
  font-size: 13px;
  line-height: 1.2;
}
.dark .ui-input {
  background: rgba(13, 16, 28, 0.65);
  border-color: rgba(255, 255, 255, 0.14);
  color: #f5f9ff;
}
.ui-checkbox {
  width: 14px;
  height: 14px;
}
</style>
