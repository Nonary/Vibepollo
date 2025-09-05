<template>
  <main ref="mainEl" class="flex-1 px-0 md:px-2 xl:px-6 py-2 md:py-6 space-y-6 overflow-x-hidden">
    <header
      class="sticky top-0 z-20 -mx-0 md:-mx-2 xl:-mx-6 px-0 md:px-2 xl:px-6 py-3 bg-light/70 dark:bg-dark/60 backdrop-blur supports-[backdrop-filter]:bg-light/50 supports-[backdrop-filter]:dark:bg-dark/40 border-b border-dark/10 dark:border-light/10"
    >
      <div class="flex items-center justify-between gap-4 flex-wrap">
        <div class="min-w-0">
          <h2 class="text-sm font-semibold uppercase tracking-wider">Settings</h2>
          <p class="text-[11px] opacity-60">
            Configuration auto-saves; restart to apply runtime changes.
          </p>
        </div>

        <div class="relative flex-1 max-w-2xl min-w-[260px]">
          <n-input
            v-model:value="searchQuery"
            type="text"
            placeholder="Search settings... (Enter to jump)"
            @focus="onSearchFocus"
            @blur="onSearchBlur"
            @keydown.enter.prevent="jumpFirstResult"
          >
            <template #suffix>
              <i class="fas fa-magnifying-glass text-[12px] opacity-60" />
            </template>
          </n-input>
          <transition name="fade">
            <div
              v-if="searchOpen"
              class="absolute mt-2 w-full z-20 bg-light/95 dark:bg-surface/95 backdrop-blur rounded-md shadow-lg border border-dark/10 dark:border-light/10 max-h-80 overflow-auto"
            >
              <div v-if="searchResults.length === 0" class="px-3 py-2 text-[12px] opacity-60">
                No results
              </div>
              <button
                v-for="(r, idx) in searchResults"
                :key="idx"
                class="w-full text-left px-3 py-2 hover:bg-dark/10 dark:hover:bg-light/10 text-[13px] flex items-start gap-2"
                @click="goTo(r)"
              >
                <span class="shrink-0 mt-0.5">
                  <i class="fas fa-compass text-primary text-[11px]" />
                </span>
                <span class="min-w-0">
                  <span class="block font-medium truncate">{{ r.label }}</span>
                  <span class="block text-[11px] opacity-60 truncate">{{ r.path }}</span>
                  <span
                    v-if="r.desc"
                    class="block text-[11px] opacity-70 line-clamp-2 break-words"
                    >{{ r.desc }}</span
                  >
                  <span
                    v-if="r.options && r.options.length"
                    class="block text-[11px] opacity-60 mt-1 break-words"
                    >Options:
                    {{
                      r.options
                        .map((o) =>
                          o.text && o.value ? `${o.text} (${o.value})` : o.text || o.value,
                        )
                        .filter(Boolean)
                        .join(', ')
                    }}</span
                  >
                </span>
              </button>
            </div>
          </transition>
        </div>

        <div v-if="showSave" class="flex items-center gap-3">
          <n-button v-if="saveState === 'saved' && !restarted" tertiary @click="apply"
            >Apply</n-button
          >
        </div>
        <div v-else class="text-[11px] font-medium min-h-[1rem] flex items-center gap-2">
          <transition name="fade"><span v-if="saveState === 'saving'">Saving…</span></transition>
          <transition name="fade">
            <span v-if="saveState === 'saved'" class="text-success">Saved</span>
          </transition>
        </div>
      </div>
    </header>

    <div v-if="isReady" class="space-y-4">
      <section
        v-for="tab in tabsFiltered"
        :id="tab.id"
        :key="tab.id"
        :ref="(el) => setSectionRef(tab.id, el)"
        class="scroll-mt-24"
      >
        <button
          type="button"
          class="w-full flex items-center justify-between px-3 py-2 rounded-lg bg-dark/5 dark:bg-light/10 hover:bg-dark/10 dark:hover:bg-light/15 transition text-left"
          @click="toggle(tab.id)"
        >
          <span class="font-semibold">{{ tab.name }}</span>
          <i
            :class="[
              'fas text-xs transition-transform',
              isOpen(tab.id) ? 'fa-chevron-up' : 'fa-chevron-down',
            ]"
          />
        </button>
        <transition name="fade">
          <div
            v-show="isOpen(tab.id)"
            class="mt-2 bg-light/80 dark:bg-surface/70 backdrop-blur-sm border border-dark/10 dark:border-light/10 rounded-xl shadow-sm p-6 space-y-6"
          >
            <component :is="tab.component" />
          </div>
        </transition>
      </section>
    </div>

    <div v-else class="text-xs opacity-60 space-y-2">
      <div v-if="isLoading">Loading...</div>
      <div v-else-if="isError" class="text-danger space-y-2">
        <div>Failed to load configuration.</div>
        <n-button :disabled="isLoading" @click="store.reloadConfig?.()">Retry</n-button>
      </div>
      <div v-else class="opacity-60">No configuration loaded.</div>
    </div>

    <div class="text-[11px]">
      <transition name="fade">
        <div v-if="saveState === 'saved' && !restarted && !autoSave" class="text-success">
          Saved. Click Apply to restart.
        </div>
      </transition>
      <transition name="fade">
        <div v-if="restarted" class="text-success">Restart triggered.</div>
      </transition>
    </div>
    <transition name="slide-fade">
      <div
        v-if="(dirty && !autoSave) || store.manualDirty === true"
        class="fixed bottom-4 right-6 z-30"
      >
        <div
          class="bg-light/90 dark:bg-surface/90 backdrop-blur rounded-lg shadow border border-dark/10 dark:border-light/10 px-4 py-2"
        >
          <div class="flex items-center gap-3">
            <span class="text-[11px] font-medium">Unsaved changes</span>
            <n-button :disabled="saveState === 'saving'" @click="save">Save</n-button>
          </div>
          <div v-if="saveState === 'error'" class="mt-1 text-[11px] text-danger leading-snug">
            {{ store.validationError || 'Save failed. Check fields for errors.' }}
          </div>
        </div>
      </div>
    </transition>
  </main>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch, markRaw, defineAsyncComponent } from 'vue';
import { NInput, NButton, useMessage } from 'naive-ui';
import { useRoute, useRouter } from 'vue-router';
import General from '@/configs/tabs/General.vue';
import Inputs from '@/configs/tabs/Inputs.vue';
import Network from '@/configs/tabs/Network.vue';
import Files from '@/configs/tabs/Files.vue';
import Advanced from '@/configs/tabs/Advanced.vue';
import Playnite from '@/configs/tabs/Playnite.vue';
import AudioVideo from '@/configs/tabs/AudioVideo.vue';
import ContainerEncoders from '@/configs/tabs/ContainerEncoders.vue';
import RTSS from '@/configs/tabs/RTSS.vue';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import { storeToRefs } from 'pinia';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const platform = computed(() => (metadata.value?.platform || '').toLowerCase());
const message = useMessage();
// Auth store (top-level, single instance)
const auth = useAuthStore();

// derive loading/error/ready from the store instead of local flags
const isLoading = computed(() => store.loading === true);
const isError = computed(() => store.error != null);
const isReady = computed(() => !!config.value && !isLoading.value && !isError.value);

const saveState = computed(() => store.savingState || 'idle');
const restarted = ref(false);
const dirty = ref(false);
const autoSave = ref(true);
const showSave = computed(() => store.manualDirty === true || !autoSave.value);

const mainEl = ref(null);
const searchQuery = ref('');
const searchOpen = ref(false);
const searchResults = ref([]);
const searchIndex = ref([]); // { sectionId, label, path, el }
const sectionRefs = new Map();

function setSectionRef(id, el) {
  if (el) sectionRefs.set(id, el);
  else sectionRefs.delete(id);
}

const tabs = [
  { id: 'general', name: 'General', component: markRaw(General) },
  { id: 'input', name: 'Input', component: markRaw(Inputs) },
  { id: 'av', name: 'Audio / Video', component: markRaw(AudioVideo) },
  { id: 'encoders', name: 'Encoders', component: markRaw(ContainerEncoders) },
  { id: 'network', name: 'Network', component: markRaw(Network) },
  { id: 'files', name: 'Files', component: markRaw(Files) },
  { id: 'advanced', name: 'Advanced', component: markRaw(Advanced) },
  { id: 'rtss', name: 'Frame Limiter', component: markRaw(RTSS) },
  { id: 'playnite', name: 'Playnite', component: markRaw(Playnite) },
];

const tabsFiltered = computed(() =>
  tabs.filter((t) => (t.id === 'rtss' ? platform.value === 'windows' : true)),
);

const openSections = ref(new Set(['general']));
const isOpen = (id) => openSections.value.has(id);
const toggle = (id) => {
  const s = new Set(openSections.value);
  s.has(id) ? s.delete(id) : s.add(id);
  openSections.value = s;
  // When expanding, (cheaply) rebuild index so new controls are searchable
  if (s.has(id)) queueBuildIndex();
};

const route = useRoute();
const router = useRouter();

onMounted(async () => {
  try {
    if (auth && typeof auth.init === 'function') await auth.init();
  } catch (err) {
    console.warn('auth.init failed', err);
  }

  // Wait for authentication before calling APIs during mount
  await auth.waitForAuthentication();
  await store.fetchConfig();
  if (config.value) queueBuildIndex();

  // If a target section is in the URL, scroll to it after initial render
  if (typeof route.query.sec === 'string') {
    setTimeout(() => scrollToOpen(route.query.sec), 0);
  }
});

// When auth becomes ready or authenticated, rebuild index (debounced a bit)
let authTimer = null;
watch(
  () => ({ ready: auth.ready, authed: auth.isAuthenticated }),
  () => {
    clearTimeout(authTimer);
    authTimer = setTimeout(() => queueBuildIndex(), 120);
  },
  { deep: true },
);
onUnmounted(() => {
  if (authTimer) clearTimeout(authTimer);
});

async function save() {
  // Guard autosave/background save when not authenticated
  if (!auth.isAuthenticated) return;
  if (!config.value) return;
  restarted.value = false;
  const ok = await (store.save ? store.save() : Promise.resolve(false));
  if (ok) {
    dirty.value = false;
  } else {
    try {
      message.error(store.validationError || 'Save failed. Check fields for errors.', {
        duration: 5000,
      });
    } catch {}
  }
}

async function apply() {
  await save();
  if (saveState.value !== 'saved') return;
  restarted.value = true;
  try {
    const res = await http.post(
      '/api/restart',
      {},
      { headers: { 'Content-Type': 'application/json' }, validateStatus: () => true },
    );
    if (!res || res.status >= 400) {
      console.warn('Restart request failed', res?.status);
      restarted.value = false;
    }
  } catch (err) {
    console.warn('Restart failed', err);
    restarted.value = false;
  } finally {
    setTimeout(() => {
      // state will settle back to idle via the store
      restarted.value = false;
    }, 5000);
  }
}

// Mark dirty / autosave when version increments (user changed something)
watch(
  () => store.version,
  (v, oldV) => {
    if (!isReady.value || oldV === undefined) return; // ignore before ready
    dirty.value = true;
    if (store.savingState !== undefined) store.savingState = 'dirty';
  },
);

const goSection = (id) => {
  const dest = { path: '/settings', query: { sec: id } };
  route.path === '/settings' ? router.replace(dest) : router.push(dest);
};

function scrollToOpen(id) {
  if (!id) return;
  if (!isOpen(id)) toggle(id);
  const el = sectionRefs.get(id);
  if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' });
}
watch(
  () => route.query.sec,
  (id) => typeof id === 'string' && scrollToOpen(id),
);

function buildSearchIndex() {
  const root = mainEl.value;
  if (!root) return;
  const items = [];
  for (const sec of Array.from(root.querySelectorAll('section[id]'))) {
    const sectionId = sec.getAttribute('id');
    const sectionTitle = sec.querySelector('h3')?.textContent?.trim() || sectionId;
    for (const lbl of Array.from(sec.querySelectorAll('label'))) {
      const text = (lbl.textContent || '').trim();
      if (!text) continue;
      const forId = lbl.getAttribute('for');
      let target = null;
      if (forId) {
        try {
          target = sec.querySelector('#' + CSS.escape(forId));
        } catch (err) {
          console.warn('buildSearchIndex: CSS.escape lookup failed', err);
        }
      }
      if (!target)
        target = lbl.closest('div')?.querySelector('input,select,textarea,.form-control');
      // Attempt to locate a descriptor/description element associated with this label.
      let descText = '';
      try {
        const isDescClass = (cls) =>
          !!cls &&
          (cls.includes('text-[11px]') || cls.includes('form-text') || cls.includes('text-xs'));
        // First search within the immediate container for a small description div.
        const container = lbl.parentElement;
        if (container) {
          const candidate = Array.from(container.querySelectorAll('div,p,small')).find(
            (d) => d !== lbl && isDescClass(d.className) && d.textContent.trim().length > 0,
          );
          if (candidate) descText = candidate.textContent.trim();
        }
        // Fallback: look at following siblings up to a few steps
        if (!descText) {
          let sib = lbl.nextElementSibling;
          let steps = 0;
          while (sib && steps < 6) {
            if (isDescClass(sib.className) && sib.textContent.trim()) {
              descText = sib.textContent.trim();
              break;
            }
            sib = sib.nextElementSibling;
            steps++;
          }
        }
      } catch (err) {
        console.warn('buildSearchIndex: description extraction failed', err);
      }
      // If the control is a select, gather option texts/values to allow matching
      let optionsList = [];
      let optionsText = '';
      try {
        if (target && target.tagName && target.tagName.toLowerCase() === 'select') {
          optionsList = Array.from(target.querySelectorAll('option')).map((o) => ({
            text: (o.textContent || '').trim(),
            value: (o.value || '').trim(),
          }));
        }
        // Also support virtual selects (e.g., Naive UI NSelect) via a data attribute
        // data-search-options="Label::value|Label2::value2|..."
        if ((!optionsList || optionsList.length === 0) && target) {
          const ds = target.getAttribute?.('data-search-options') || '';
          if (ds && typeof ds === 'string') {
            optionsList = ds
              .split('|')
              .map((chunk) => chunk.trim())
              .filter(Boolean)
              .map((pair) => {
                const [textRaw, valRaw] = pair.split('::');
                const text = (textRaw || '').trim();
                const value = (valRaw || '').trim();
                return { text, value };
              })
              .filter((o) => o.text || o.value);
          }
        }
        if (optionsList && optionsList.length) {
          optionsText = optionsList
            .map((o) => `${o.text || ''} ${o.value || ''}`.trim())
            .filter(Boolean)
            .join(' | ');
        }
      } catch (err) {
        optionsList = [];
        optionsText = '';
        console.warn('buildSearchIndex: options extraction failed', err);
      }

      if (target)
        items.push({
          sectionId,
          label: text,
          path: `${sectionTitle} › ${text}`,
          el: target,
          desc: descText,
          options: optionsList,
          optionsText,
        });
    }
  }
  searchIndex.value = items;
}

let buildPending = false;
function queueBuildIndex() {
  if (buildPending) return;
  buildPending = true;
  requestAnimationFrame(() => {
    buildPending = false;
    buildSearchIndex();
  });
}

watch(searchQuery, (q) => {
  const v = (q || '').trim().toLowerCase();
  const terms = v.split(/\s+/).filter(Boolean);
  searchOpen.value = terms.length > 0;
  if (!terms.length) {
    searchResults.value = [];
    return;
  }

  // Score matches: require all terms to match one of the fields. Label highest, options, path, then desc.
  const scoreFor = (it) => {
    const lv = it.label.toLowerCase();
    const pv = it.path.toLowerCase();
    const dv = (it.desc || '').toLowerCase();
    const ov = (it.optionsText || '').toLowerCase();
    let total = 0;
    for (const term of terms) {
      let s = 0;
      if (lv.includes(term)) {
        s += 100 - lv.indexOf(term);
        if (lv.startsWith(term)) s += 50;
      } else if (ov.includes(term)) {
        s += 60 - ov.indexOf(term) / 10;
      } else if (pv.includes(term)) {
        s += 40 - pv.indexOf(term) / 100;
      } else if (dv.includes(term)) {
        s += 20 - dv.indexOf(term) / 1000;
      } else {
        return 0; // missing term
      }
      total += s;
    }
    // penalize very long descriptions/path/options to prefer concise matches
    total -= (pv.length + dv.length + ov.length) / 1000;
    return total;
  };

  searchResults.value = searchIndex.value
    .map((it) => ({ it, s: scoreFor(it) }))
    .filter((x) => x.s > 0)
    .sort((a, b) => b.s - a.s)
    .slice(0, 15)
    .map((x) => x.it);
});
function jumpFirstResult() {
  if (searchResults.value.length) goTo(searchResults.value[0]);
}
function goTo(item) {
  searchOpen.value = false;
  if (item?.sectionId) goSection(item.sectionId);
  setTimeout(() => {
    try {
      // Prefer flashing the visual wrapper for Naive UI controls
      let target = item.el;
      try {
        const wrapper = target?.closest?.(
          '.n-input, .n-select, .n-input-number, .n-checkbox, .n-switch, .form-control',
        );
        if (wrapper) target = wrapper;
      } catch {}
      target?.scrollIntoView({ behavior: 'smooth', block: 'center' });
      flash(target || item.el);
    } catch (err) {
      console.warn('goTo: scroll/flash failed', err);
    }
  }, 250);
}
function flash(el) {
  // Flash on wrapper if available so the ring isn't hidden by internal structure
  let target = el;
  try {
    const wrapper = target?.closest?.(
      '.n-input, .n-select, .n-input-number, .n-checkbox, .n-switch, .form-control',
    );
    if (wrapper) target = wrapper;
  } catch {}
  target?.classList.add('flash-highlight');
  // Let the CSS animation run to completion before cleanup
  setTimeout(() => target?.classList.remove('flash-highlight'), 5200);
}

function onSearchFocus() {
  searchOpen.value = (searchQuery.value || '').length > 0;
}
function onSearchBlur() {
  setTimeout(() => {
    searchOpen.value = false;
  }, 120);
}
</script>

<style scoped>
.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.25s;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

.slide-fade-enter-active,
.slide-fade-leave-active {
  transition: all 0.25s;
}

.slide-fade-enter-from,
.slide-fade-leave-to {
  opacity: 0;
  transform: translateY(6px);
}

/* Make highlight global so it applies to controls inside child tab components */
:global(.flash-highlight) {
  /* Use theme tokens so it works in light and dark */
  box-shadow:
    0 0 0 3px rgb(var(--color-primary) / 0.45),
    0 0 0 6px rgb(var(--color-primary) / 0.2);
  outline: 2px solid rgb(var(--color-primary) / 0.6);
  outline-offset: 2px;
  border-radius: 6px;
  transition:
    box-shadow 0.25s,
    outline-color 0.25s;
  animation: flash-ring-fade 5s ease-out forwards;
  will-change: box-shadow, outline-color;
}

@keyframes flash-ring-fade {
  0% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0.45),
      0 0 0 6px rgb(var(--color-primary) / 0.2);
    outline-color: rgb(var(--color-primary) / 0.6);
  }
  60% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0.25),
      0 0 0 6px rgb(var(--color-primary) / 0.12);
    outline-color: rgb(var(--color-primary) / 0.35);
  }
  100% {
    box-shadow:
      0 0 0 3px rgb(var(--color-primary) / 0),
      0 0 0 6px rgb(var(--color-primary) / 0);
    outline-color: rgb(var(--color-primary) / 0);
  }
}
</style>
