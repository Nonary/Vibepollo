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
              <!-- When adding a new app on Windows, allow picking a Playnite game (disabled if plugin not installed) -->
              <template v-if="isNew && isWindows">
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
                <div class="text-[11px] opacity-60" v-if="!lockPlaynite">
                  Pick from your Playnite library (installed only). Once selected, it locks in.
                </div>
              </template>
              <template v-else>
                <n-input v-model:value="form.name" placeholder="Game or App Name" />
              </template>
            </div>
            <div class="space-y-1 md:col-span-2">
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
            <div class="space-y-1 md:col-span-1">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Working Dir</label
              >
              <n-input
                v-model:value="form['working-dir']"
                class="font-mono"
                placeholder="C:/Games/App"
              />
            </div>
            <div class="space-y-1 md:col-span-1">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Exit Timeout</label
              >
              <div class="flex items-center gap-2">
                <n-input-number v-model:value="form['exit-timeout']" :min="0" class="w-28" />
                <span class="text-xs opacity-60">seconds</span>
              </div>
            </div>
            <div class="space-y-1 md:col-span-2">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Image Path</label
              >
              <n-input
                v-model:value="form['image-path']"
                class="font-mono"
                placeholder="/path/to/image.png"
              />
              <p class="text-[11px] opacity-60">
                Optional; stored only and not fetched by Sunshine.
              </p>
            </div>
          </div>

          <div class="grid grid-cols-2 gap-3">
            <n-checkbox v-model:checked="form['exclude-global-prep-cmd']" size="small">
              Exclude Global Prep
            </n-checkbox>
            <n-checkbox v-model:checked="form['auto-detach']" size="small">
              Auto Detach
            </n-checkbox>
            <n-checkbox v-model:checked="form['wait-all']" size="small">Wait All</n-checkbox>
            <n-checkbox v-if="platform === 'windows'" v-model:checked="form.elevated" size="small">
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
            <div v-if="form['prep-cmd'].length === 0" class="text-[12px] opacity-60">None</div>
            <div v-else class="space-y-2">
              <div v-for="(p, i) in form['prep-cmd']" :key="i" class="grid md:grid-cols-12 gap-2">
                <n-input v-model:value="p.do" placeholder="do" class="font-mono md:col-span-5" />
                <n-input
                  v-model:value="p.undo"
                  placeholder="undo"
                  class="font-mono md:col-span-5"
                />
                <div class="flex items-center gap-2 md:col-span-2">
                  <n-checkbox
                    v-if="platform === 'windows'"
                    v-model:checked="p.elevated"
                    size="small"
                  >
                    Elev
                  </n-checkbox>
                  <n-button size="small" secondary @click="form['prep-cmd'].splice(i, 1)">
                    <i class="fas fa-trash" />
                  </n-button>
                </div>
              </div>
            </div>
          </section>

          <section class="space-y-3">
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
              <div v-for="(d, i) in form.detached" :key="i" class="flex gap-2 items-start">
                <n-input v-model:value="form.detached[i]" class="font-mono flex-1" />
                <n-button size="small" secondary @click="form.detached.splice(i, 1)">
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
            :disabled="saving.v"
            @click="showDeleteConfirm = true"
          >
            <i class="fas fa-trash" /> {{ $t('apps.delete') }}
          </n-button>
          <n-button type="primary" :loading="saving.v" :disabled="saving.v" @click="save">
            <i class="fas fa-save" /> {{ $t('_common.save') }}
          </n-button>
        </div>
      </template>

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
import { reactive, watch, computed, ref } from 'vue';
import { http } from '@/http';
import { NModal, NCard, NButton, NInput, NInputNumber, NCheckbox, NSelect } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
const props = defineProps({
  modelValue: Boolean,
  platform: String,
  app: Object,
  index: { type: Number, default: -1 },
});
const emit = defineEmits(['update:modelValue', 'saved', 'deleted']);
const open = computed(() => props.modelValue);
function fresh() {
  return {
    name: '',
    output: '',
    cmd: '',
    index: -1,
    'exclude-global-prep-cmd': false,
    elevated: false,
    'auto-detach': true,
    'wait-all': true,
    'exit-timeout': 5,
    'prep-cmd': [],
    detached: [],
    'image-path': '',
    'working-dir': '',
  };
}
const form = reactive(fresh());
// Normalize cmd to single string; if older data has array, join with spaces
watch(
  () => props.app,
  (val) => {
    if (!open.value) return;
    const copy = JSON.parse(JSON.stringify(val || {}));
    if (Array.isArray(copy.cmd)) copy.cmd = copy.cmd.join(' ');
    Object.assign(form, fresh(), copy);
    form.index = props.index ?? -1;
  },
  { immediate: true },
);
const cmdText = computed({
  get() {
    return form.cmd || '';
  },
  set(v) {
    form.cmd = v;
  },
});
const isPlaynite = computed(() => !!(form as any)['playnite-id']);
const isPlayniteAuto = computed(
  () => isPlaynite.value && (form as any)['playnite-managed'] !== 'manual',
);
watch(open, (o) => {
  if (o) {
    const copy = JSON.parse(JSON.stringify(props.app || {}));
    if (Array.isArray(copy.cmd)) copy.cmd = copy.cmd.join(' ');
    Object.assign(form, fresh(), copy);
    form.index = props.index ?? -1;
    // reset playnite picker state when opening
    selectedPlayniteId.value = '';
    lockPlaynite.value = false;
    // refresh Playnite status early so the picker can enable itself
    refreshPlayniteStatus().then(() => {
      if (playniteInstalled.value) loadPlayniteGames();
    });
    // if editing an existing Playnite-managed app, keep name field simple
    // Update scroll shadows after content paints
    requestAnimationFrame(() => updateShadows());
  }
});
function close() {
  emit('update:modelValue', false);
}
function addPrep() {
  form['prep-cmd'].push({
    do: '',
    undo: '',
    ...(props.platform === 'windows' ? { elevated: false } : {}),
  });
  requestAnimationFrame(() => updateShadows());
}
const saving = reactive({ v: false });
const showDeleteConfirm = ref(false);

// Platform + Playnite detection
const configStore = useConfigStore();
const isWindows = computed(
  () => (configStore.metadata?.platform || '').toLowerCase() === 'windows',
);
const playniteInstalled = ref(false);
const isNew = computed(() => form.index === -1);

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
}

async function refreshPlayniteStatus() {
  try {
    const r = await http.get('/api/playnite/status', { validateStatus: () => true });
    if (r.status === 200 && r.data) playniteInstalled.value = !!(r.data as any).installed;
  } catch (_) {}
}

function onPickPlaynite(id: string) {
  const opt = playniteOptions.value.find((o) => o.value === id);
  if (!opt) return;
  // Lock in selection and set fields
  form.name = opt.label;
  (form as any)['playnite-id'] = id;
  (form as any)['playnite-managed'] = 'manual';
  // clear command by default for Playnite managed entries
  if (!form.cmd) form.cmd = '';
  lockPlaynite.value = true;
}
function unlockPlaynite() {
  lockPlaynite.value = false;
}
function addDetached() {
  form.detached.push('');
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

import { onMounted, onBeforeUnmount } from 'vue';
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

// Cover preview logic removed; Sunshine no longer fetches or proxies images
async function save() {
  saving.v = true;
  const payload = JSON.parse(JSON.stringify(form));
  try {
    await http.post('./api/apps', payload, {
      headers: { 'Content-Type': 'application/json' },
      validateStatus: () => true,
    });
    emit('saved');
    close();
  } finally {
    saving.v = false;
  }
}
async function del() {
  saving.v = true;
  try {
    // If Playnite auto-managed, add to exclusion list before removing
    const pid = (form as any)['playnite-id'];
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
        const name =
          playniteOptions.value.find((o) => o.value === String(pid))?.label || '';
        map.set(String(pid), name);
        const next = Array.from(map.entries()).map(([id, name]) => ({ id, name }));
        // Update local store (keeps UI in sync) and persist via store API
        configStore.updateOption('playnite_exclude_games', next);
        await configStore.save();
      } catch (_) {
        // best-effort; continue with deletion even if exclusion save fails
      }
    }

    await http.delete(`./api/apps/${form.index}`, { validateStatus: () => true });
    // Best-effort force sync on Windows environments
    try {
      await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    } catch (_) {}
    emit('deleted');
    close();
  } finally {
    saving.v = false;
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
