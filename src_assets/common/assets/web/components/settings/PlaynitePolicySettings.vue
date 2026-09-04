<script setup lang="ts">
import { useUnsavedChanges } from '@/composables/useUnsavedChanges';
import { computed, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { useI18n } from 'vue-i18n';
import { apiGet, apiPatch } from '@/api/client';
import { AppButton, InlineAlert, SettingRow } from '@/components/ui';
import { extendedDefaults, playnitePolicyFields } from '@/configs/extendedSettings';
import { configBoolean } from '@/utils/settings';
interface Entry {
  id: string;
  name: string;
}
const { t, te } = useI18n();
const route = useRoute();
const values = ref<Record<string, unknown>>({});
const original = ref<Record<string, unknown>>({});
const loaded = ref(false);
const loading = ref(true),
  saving = ref(false),
  failed = ref(false);
const lists = [
  'playnite_sync_categories',
  'playnite_sync_plugins',
  'playnite_exclude_categories',
  'playnite_exclude_plugins',
  'playnite_exclude_games',
];
const suggestions = ref<Record<string, Entry[]>>({});
const dirty = computed(() => JSON.stringify(values.value) !== JSON.stringify(original.value));
useUnsavedChanges(computed(() => loaded.value && dirty.value));
function label(key: string): string {
  const candidate = `playnite.${key.replace('playnite_', '')}`;
  return te(candidate) || te(candidate, 'en')
    ? t(candidate)
    : key.replace('playnite_', '').replaceAll('_', ' ');
}
function entries(key: string): Entry[] {
  return Array.isArray(values.value[key]) ? (values.value[key] as Entry[]) : [];
}
function add(key: string) {
  values.value[key] = [...entries(key), { id: '', name: '' }];
}
function remove(key: string, index: number) {
  values.value[key] = entries(key).filter((_, i) => i !== index);
}
async function load() {
  loading.value = true;
  failed.value = false;
  try {
    const config = await apiGet<Record<string, unknown>>('/api/config');
    if (config.status === false) throw new Error('config-load-rejected');
    const next: Record<string, unknown> = {};
    for (const field of playnitePolicyFields)
      next[field.key] =
        field.kind === 'boolean'
          ? configBoolean(config[field.key] ?? extendedDefaults[field.key])
          : (config[field.key] ?? extendedDefaults[field.key]);
    for (const key of lists) {
      const value = config[key] ?? [];
      const parsed = typeof value === 'string' ? JSON.parse(value) : value;
      if (!Array.isArray(parsed)) throw new Error('invalid-policy-list');
      next[key] = parsed.map((entry: Entry | string) =>
        typeof entry === 'string' ? { id: entry, name: entry } : entry,
      );
    }
    values.value = next;
    original.value = JSON.parse(JSON.stringify(next));
    loaded.value = true;
  } catch {
    failed.value = true;
  } finally {
    loading.value = false;
  }
}
async function discover() {
  try {
    const [categories, games] = await Promise.all([
      apiGet<Entry[]>('/api/playnite/categories'),
      apiGet<Array<Entry & { pluginId?: string; pluginName?: string }>>('/api/playnite/games'),
    ]);
    const plugins = [
      ...new Map(
        games
          .filter((game) => game.pluginId)
          .map((game) => [
            game.pluginId!,
            { id: game.pluginId!, name: game.pluginName || game.pluginId! },
          ]),
      ).values(),
    ];
    suggestions.value = { categories, games, plugins };
  } catch {
    /* Explicit entries remain editable when Playnite is offline. */
  }
}
function suggestionsFor(key: string): Entry[] {
  return suggestions.value[key.split('_').at(-1)!] ?? [];
}
function selectEntry(key: string, index: number, event: Event) {
  const id = (event.target as HTMLInputElement).value;
  entries(key)[index] = suggestionsFor(key).find((entry) => entry.id === id) ?? { id, name: id };
}
async function save() {
  if (!loaded.value || saving.value || !dirty.value) return;
  saving.value = true;
  failed.value = false;
  const submitted = JSON.parse(JSON.stringify(values.value));
  const patch = Object.fromEntries(
    Object.keys(submitted)
      .filter((key) => JSON.stringify(submitted[key]) !== JSON.stringify(original.value[key]))
      .map((key) => [
        key,
        lists.includes(key)
          ? submitted[key].filter((entry: Entry) => entry.id.trim())
          : submitted[key],
      ]),
  );
  try {
    const result = await apiPatch<{ status?: boolean }>('/api/config', patch);
    if (result.status === false) throw new Error('save-rejected');
    original.value = submitted;
  } catch {
    failed.value = true;
  } finally {
    saving.value = false;
  }
}
onMounted(() => {
  void load();
  void discover();
});
</script>
<template>
  <details
    id="playnite-policies"
    class="integration-settings"
    :open="route.hash === '#playnite-policies'"
  >
    <summary>{{ t('ui.integrations.playnite.policies_title') }}</summary>
    <form @submit.prevent="save">
      <InlineAlert
        v-if="failed"
        tone="warning"
        :title="t(loaded ? 'ui.settings.errors.save' : 'ui.settings.errors.load')"
      />
      <fieldset :disabled="loading || saving || !loaded" class="policy-fields">
        <div class="vs-settings-group">
          <SettingRow
            v-for="field in playnitePolicyFields"
            :key="field.key"
            :label="label(field.key)"
            :control-id="field.key"
          >
            <input
              v-if="field.kind === 'boolean'"
              :id="field.key"
              v-model="values[field.key]"
              type="checkbox"
            />
            <input
              v-else
              :id="field.key"
              v-model="values[field.key]"
              class="vs-input"
              :type="field.kind === 'number' ? 'number' : 'text'"
              :min="field.min"
              :max="field.max"
              :step="field.step"
            />
          </SettingRow>
        </div>
        <details v-for="key in lists" :key="key" class="policy-list">
          <summary>{{ label(key) }} ({{ entries(key).length }})</summary>
          <div v-for="(entry, index) in entries(key)" :key="index" class="policy-entry">
            <label :for="`${key}-${index}`">{{ entry.name || label(key) }}</label>
            <input
              :id="`${key}-${index}`"
              class="vs-input"
              :value="entry.id"
              :list="`${key}-choices`"
              @change="selectEntry(key, index, $event)"
            />
            <AppButton
              :label="t('_common.remove')"
              variant="tertiary"
              @click="remove(key, index)"
            />
          </div>
          <datalist :id="`${key}-choices`">
            <option v-for="entry in suggestionsFor(key)" :key="entry.id" :value="entry.id">
              {{ entry.name }}
            </option>
          </datalist>
          <AppButton :label="t('_common.add')" variant="secondary" @click="add(key)" />
        </details>
      </fieldset>
      <div class="policy-actions">
        <AppButton
          :label="t('ui.settings.reload')"
          variant="tertiary"
          :disabled="dirty || saving"
          @click="load"
        />
        <AppButton
          :label="t('ui.settings.discard')"
          variant="secondary"
          :disabled="!dirty || saving"
          @click="values = JSON.parse(JSON.stringify(original))"
        />
        <button
          class="button button--primary"
          type="submit"
          :disabled="loading || saving || !loaded || !dirty"
        >
          {{ t(saving ? 'ui.settings.saving' : 'ui.settings.save') }}
        </button>
      </div>
    </form>
  </details>
</template>
<style scoped>
.policy-fields {
  border: 0;
  padding: 0;
  margin: 0;
  min-width: 0;
}
.policy-list {
  padding: var(--vs-space-16);
  border-bottom: 1px solid var(--vs-color-border-subtle);
}
summary {
  cursor: pointer;
  margin-bottom: var(--vs-space-12);
}
.policy-entry {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
  margin-bottom: var(--vs-space-8);
}
.policy-entry input {
  flex: 1;
  min-width: 12rem;
}
.policy-actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  padding: var(--vs-space-16);
}
</style>
