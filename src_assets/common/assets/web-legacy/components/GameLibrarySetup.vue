<script setup lang="ts">
import { providerSupported } from '../../web/utils/providerCapabilities';
import { computed, nextTick, reactive, ref, watch } from 'vue';

const props = defineProps<{
  open: boolean;
  platform: string;
  metadata?: unknown;
  request: (
    method: 'GET' | 'POST' | 'PATCH',
    path: string,
    body?: Record<string, unknown>,
  ) => Promise<any>;
}>();
const emit = defineEmits<{
  (event: 'update:open', value: boolean): void;
  (event: 'saved'): void;
}>();
const dialog = ref<HTMLDialogElement>();
const busy = ref(false);
const loading = ref(false);
const settingsLoaded = ref(false);
const error = ref('');
const notice = ref('');
const config = ref<Record<string, unknown>>({});
const playniteInstalled = ref(false);
const selected = reactive<Record<string, boolean>>({
  playnite: false,
  steam: false,
  lutris: false,
});
const windows = computed(() => props.platform.toLowerCase().includes('windows'));
const linux = computed(() => props.platform.toLowerCase().includes('linux'));
const libraries = computed(() => [
  ...(windows.value
    ? [
        {
          id: 'playnite',
          name: 'Playnite',
          benefit:
            'Bring games from multiple stores into one Windows library, with Playnite artwork and launch settings.',
          detail:
            'Requires Playnite and the Vibepollo extension. Setup installs the extension for you.',
          recommended: true,
        },
      ]
    : []),
  {
    id: 'steam',
    name: 'Steam',
    benefit: 'Sync your recent Steam games directly, with Steam box art and launch support.',
    detail: windows.value
      ? 'Use it instead of Playnite, or alongside it. No Playnite extension required.'
      : 'Uses your local Steam library. No extra library manager required.',
    recommended: false,
  },
  ...(linux.value
    ? [
        {
          id: 'lutris',
          name: 'Lutris',
          benefit:
            'Add installed Linux games from Lutris, including games using Wine and other runners.',
          detail:
            'Works alongside Steam. Steam games in Lutris are excluded from automatic sync by default.',
          recommended: false,
        },
      ]
    : []),
].filter((library) => library.id === 'playnite' ? windows.value : providerSupported(props.metadata, library.id)));
function flag(value: unknown, fallback: boolean): boolean {
  if (value === undefined) return fallback;
  return (
    value === true ||
    value === 1 ||
    ['true', 'on', '1', 'yes'].includes(String(value).toLowerCase())
  );
}
async function load() {
  loading.value = true;
  settingsLoaded.value = false;
  error.value = '';
  notice.value = '';
  try {
    const [settings, playnite] = await Promise.all([
      props.request('GET', '/api/config'),
      windows.value ? props.request('GET', '/api/playnite/status') : Promise.resolve({}),
    ]);
    if (settings.status === false) throw new Error('Unable to load settings');
    config.value = settings;
    playniteInstalled.value = playnite.installed === true || playnite.active === true;
    for (const library of libraries.value) {
      selected[library.id] =
        flag(settings[library.id + '_enabled'], true) &&
        flag(settings[library.id + '_auto_sync'], library.id !== 'steam') &&
        (library.id !== 'playnite' || playniteInstalled.value);
    }
    settingsLoaded.value = true;
  } catch {
    error.value = 'Unable to load game library settings. Close this window and try again.';
  } finally {
    loading.value = false;
  }
}
async function save() {
  busy.value = true;
  error.value = '';
  notice.value = '';
  try {
    let installed = false;
    if (windows.value && selected['playnite'] && !playniteInstalled.value) {
      const result = await props.request('POST', '/api/playnite/install', { restart: false });
      if (result.status === false || result.success === false)
        throw new Error(
          result.error ||
            'Unable to install the Playnite extension. Install Playnite, then try again.',
        );
      installed = true;
      playniteInstalled.value = true;
    }
    const patch: Record<string, unknown> = {};
    for (const library of libraries.value) {
      // Linux always exposes Steam discovery; its synchronization is opt-in.
      if (!(linux.value && library.id === 'steam') &&
          (library.id !== 'playnite' || providerSupported(props.metadata, 'playnite_toggle')))
        patch[library.id + '_enabled'] = selected[library.id];
      patch[library.id + '_auto_sync'] = selected[library.id];
      if (
        library.id !== 'lutris' &&
        selected[library.id] &&
        !flag(config.value[library.id + '_auto_sync'], library.id === 'playnite')
      ) {
        patch[library.id + '_sync_all_installed'] = false;
      }
    }
    const result = await props.request('PATCH', '/api/config', patch);
    if (result.status === false)
      throw new Error(result.error || 'Unable to save game library settings.');
    Object.assign(config.value, patch);
    notice.value = installed
      ? 'Libraries saved. Open or restart Playnite to finish connecting the extension.'
      : 'Game library settings saved.';
    emit('saved');
  } catch (cause) {
    error.value =
      cause instanceof Error ? cause.message : 'Unable to set up game libraries. Try again.';
  } finally {
    busy.value = false;
  }
}
function close() {
  if (!busy.value) emit('update:open', false);
}
watch(
  () => props.open,
  async (open) => {
    await nextTick();
    if (open) {
      if (!dialog.value?.open) dialog.value?.showModal();
      void load();
    } else dialog.value?.close();
  },
  { immediate: true },
);
</script>

<template>
  <dialog
    ref="dialog"
    class="game-library-setup"
    aria-labelledby="game-library-setup-title"
    @cancel.prevent="close"
  >
    <form @submit.prevent="save">
      <header>
        <span class="game-library-setup__eyebrow">YOUR GAMES, READY TO STREAM</span>
        <h2 id="game-library-setup-title">Setup Game Library Integration</h2>
        <p>Choose one or more libraries. You can change these choices any time.</p>
      </header>
      <p v-if="loading" role="status">Loading your libraries…</p>
      <fieldset :disabled="loading || busy || !settingsLoaded">
        <legend class="game-library-setup__legend">Libraries to sync automatically</legend>
        <label
          v-for="library in libraries"
          :key="library.id"
          class="game-library-setup__card"
          :class="{ 'game-library-setup__card--selected': selected[library.id] }"
        >
          <input v-model="selected[library.id]" type="checkbox" :aria-label="library.name" />
          <span>
            <strong
              >{{ library.name }}
              <small v-if="library.recommended">Recommended on Windows</small></strong
            >
            <span>{{ library.benefit }}</span>
            <span class="game-library-setup__detail">{{ library.detail }}</span>
          </span>
        </label>
      </fieldset>
      <p class="game-library-setup__detail">
        Selected libraries sync automatically. Steam and Playnite start with recent games. When
        adding a game found in several libraries, Playnite is preferred, then Steam, then Lutris;
        you can choose another library.
      </p>
      <p v-if="error" role="alert" class="game-library-setup__error">{{ error }}</p>
      <p v-if="notice" role="status">{{ notice }}</p>
      <footer>
        <button type="button" :disabled="busy" @click="close">
          {{ notice ? 'Done' : 'Cancel' }}
        </button>
        <button
          type="submit"
          class="game-library-setup__save"
          :disabled="loading || busy || !settingsLoaded"
        >
          {{ busy ? 'Setting up…' : 'Save library choices' }}
        </button>
      </footer>
    </form>
  </dialog>
</template>

<style scoped>
.game-library-setup {
  box-sizing: border-box;
  width: min(680px, calc(100vw - 32px));
  max-height: calc(100dvh - 32px);
  margin: auto;
  padding: 28px;
  overflow: auto;
  border: 1px solid #435169;
  border-radius: 20px;
  background: #152033;
  color: #edf3fc;
  box-shadow: 0 24px 90px #0008;
  font-family: inherit;
}
.game-library-setup::backdrop {
  background: #060e1cc2;
  backdrop-filter: blur(5px);
}
.game-library-setup header {
  margin-bottom: 22px;
}
.game-library-setup h2 {
  margin: 8px 0;
  color: #edf3fc;
  font-size: 26px;
  line-height: 1.2;
  font-weight: 650;
}
.game-library-setup p {
  line-height: 1.5;
}
.game-library-setup__eyebrow {
  color: #92c6ff;
  font-size: 11px;
  letter-spacing: 0.12em;
}
.game-library-setup fieldset {
  border: 0;
  padding: 0;
  margin: 0;
  display: grid;
  gap: 12px;
}
.game-library-setup__legend {
  margin-bottom: 10px;
  font-size: 14px;
}
.game-library-setup__card {
  display: flex;
  gap: 14px;
  padding: 16px;
  border: 1px solid #526078;
  border-radius: 12px;
  cursor: pointer;
}
.game-library-setup__card--selected {
  border-color: #8cc4ff;
  background: #203653;
}
.game-library-setup__card input {
  width: 18px;
  height: 18px;
  flex-shrink: 0;
  margin-top: 3px;
  accent-color: #8cc4ff;
}
.game-library-setup__card span span {
  display: block;
  margin-top: 7px;
  line-height: 1.4;
  font-size: 14px;
}
.game-library-setup__card strong {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
  font-size: 18px;
}
.game-library-setup__card small {
  padding: 3px 7px;
  border-radius: 5px;
  background: #365777;
  color: #e2f1ff;
  font-size: 11px;
  font-weight: 500;
}
.game-library-setup__detail {
  color: #bdcbdd;
  font-size: 13px;
}
.game-library-setup fieldset + p {
  margin-top: 14px;
}
.game-library-setup__error {
  color: #ffb7b7;
}
.game-library-setup footer {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 10px;
  margin-top: 22px;
}
.game-library-setup button {
  padding: 10px 16px;
  border: 1px solid #71839e;
  border-radius: 8px;
  background: transparent;
  color: inherit;
  cursor: pointer;
  font: inherit;
}
.game-library-setup .game-library-setup__save {
  background: #98cdff;
  border-color: #98cdff;
  color: #102137;
  font-weight: 600;
}
.game-library-setup :focus-visible {
  outline: 3px solid #c7e3ff;
  outline-offset: 3px;
}
.game-library-setup button:disabled {
  opacity: 0.5;
  cursor: wait;
}
@media (max-width: 480px) {
  .game-library-setup {
    padding: 18px;
  }
  .game-library-setup h2 {
    font-size: 22px;
  }
}
</style>
