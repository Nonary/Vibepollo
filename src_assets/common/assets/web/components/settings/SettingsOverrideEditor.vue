<script setup lang="ts">
import { computed, nextTick, ref, toRaw } from 'vue';
import { useI18n } from 'vue-i18n';

import {
  clientOverrideableKeys,
  settingsCategories,
  settingsDefaults,
  type SettingsField,
  type SettingsOption,
} from '@/configs/settingsSchema';
import { AppButton, SettingRow, StatusBadge, UiIcon } from '@/components/ui';

interface OverrideMetadata {
  gpus?: Array<{ description?: string; pnp_id?: string }>;
  platform?: string;
}

const props = withDefaults(
  defineProps<{
    modelValue?: Record<string, unknown>;
    baseValues?: Record<string, unknown>;
    metadata?: OverrideMetadata;
    scope?: 'application' | 'client';
    displaySelection?: 'physical' | 'virtual';
    hiddenKeys?: string[];
    controlIdPrefix?: string;
  }>(),
  {
    modelValue: () => ({}),
    baseValues: () => ({}),
    metadata: () => ({}),
    scope: 'client',
    hiddenKeys: () => [],
    controlIdPrefix: 'settings-override',
  },
);

const emit = defineEmits<{
  'update:modelValue': [value: Record<string, unknown>];
}>();

const { t, te } = useI18n();
const search = ref('');
const announcement = ref('');
const catalogOpen = ref(false);

function cloneValue<T>(value: T): T {
  if (value === undefined) return value;
  try {
    return structuredClone(toRaw(value));
  } catch {
    return value;
  }
}

function emitOverrides(value: Record<string, unknown>): void {
  emit('update:modelValue', cloneValue(value));
}

function messageExists(key: string): boolean {
  return te(key) || te(key, 'en');
}

function humanize(key: string): string {
  const acronyms = new Map<string, string>([
    ['amd', 'AMD'],
    ['av1', 'AV1'],
    ['fps', 'FPS'],
    ['gpu', 'GPU'],
    ['hdr', 'HDR'],
    ['hevc', 'HEVC'],
    ['nvenc', 'NVENC'],
    ['qsv', 'QSV'],
    ['rtss', 'RTSS'],
    ['vaapi', 'VA-API'],
    ['wgc', 'WGC'],
  ]);
  return key
    .split(/[_-]+/)
    .filter(Boolean)
    .map(
      (part) =>
        acronyms.get(part.toLocaleLowerCase()) ?? `${part[0]?.toUpperCase()}${part.slice(1)}`,
    )
    .join(' ');
}

const fieldsByKey = computed(() => {
  const fields = new Map<string, SettingsField>();
  for (const category of settingsCategories) {
    for (const group of category.groups) {
      for (const field of group.fields) fields.set(field.key, field);
    }
  }
  return fields;
});

function fieldFor(key: string): SettingsField | undefined {
  return fieldsByKey.value.get(key);
}

function isAllowed(key: string): boolean {
  // Both application and client overrides are runtime-scoped by the backend
  // allowlist; global identity, network, filesystem, and updater settings are
  // never safe to expose from an override editor.
  return clientOverrideableKeys.has(key);
}

function labelFor(key: string): string {
  const field = fieldFor(key);
  const translationKey = field?.labelKey || `config.${key}`;
  return messageExists(translationKey) ? t(translationKey) : humanize(key);
}

function descriptionFor(key: string): string {
  const field = fieldFor(key);
  const candidates = [
    field?.descriptionKey,
    `config.${key}_desc`,
    `ui.settings.fields.${key}.description`,
  ].filter((candidate): candidate is string => Boolean(candidate));
  const translationKey = candidates.find((candidate) => messageExists(candidate));
  return translationKey ? t(translationKey) : '';
}

function optionLabel(option: SettingsOption): string {
  return option.labelKey && messageExists(option.labelKey)
    ? t(option.labelKey, { value: option.value })
    : humanize(option.value);
}

function readValue(key: string): unknown {
  return props.modelValue[key];
}

function hasOverride(key: string): boolean {
  return Object.prototype.hasOwnProperty.call(props.modelValue, key);
}

function inheritedValue(key: string): unknown {
  return props.baseValues[key] ?? settingsDefaults[key];
}

function displayValue(value: unknown): string {
  if (value === undefined || value === null || value === '') return t('_common.auto');
  if (typeof value === 'boolean') return value ? t('_common.enabled') : t('_common.disabled');
  if (typeof value === 'object') return JSON.stringify(value);
  return String(value);
}

function inheritedDisplayValue(key: string): string {
  const value = inheritedValue(key);
  const selected = selectOptions(key).find((option) => option.value === String(value ?? ''));
  return selected?.label ?? displayValue(value);
}

const virtualDisplayOnlyKeys = new Set([
  'virtual_display_mode',
  'virtual_display_layout',
  'dd_virtual_display_scale',
  'dd_activate_virtual_display',
  'dd_virtual_display_permanent_count',
  'dd_paused_virtual_display_timeout_secs',
  'frame_limiter_auto_virtual_framegen',
  'dd_use_sunshine_virtual_display_driver',
  'vulkan_hdr_layer',
]);

function isHiddenForDisplay(key: string): boolean {
  return (
    props.hiddenKeys.includes(key) ||
    (props.displaySelection === 'physical' && virtualDisplayOnlyKeys.has(key))
  );
}

const overrideKeys = computed(() =>
  Object.keys(props.modelValue)
    .filter((key) => key !== 'adapter_pnp_id' && !isHiddenForDisplay(key))
    .sort((a, b) => labelFor(a).localeCompare(labelFor(b))),
);

const catalogGroups = computed(() => {
  const seen = new Set<string>();
  return settingsCategories
    .map((category) => ({
      id: category.id,
      label: messageExists(`ui.settings.categories.${category.id}.label`)
        ? t(`ui.settings.categories.${category.id}.label`)
        : humanize(category.id),
      fields: category.groups
        .flatMap((group) => group.fields)
        .filter((field) => {
          if (
            !isAllowed(field.key) ||
            isHiddenForDisplay(field.key) ||
            field.key === 'adapter_pnp_id' ||
            seen.has(field.key)
          ) {
            return false;
          }
          seen.add(field.key);
          return true;
        })
        .sort((a, b) => labelFor(a.key).localeCompare(labelFor(b.key))),
    }))
    .filter((category) => category.fields.length);
});

const filteredCatalogGroups = computed(() => {
  const query = search.value.trim().toLocaleLowerCase();
  if (!query) return catalogGroups.value;
  return catalogGroups.value
    .map((category) => ({
      ...category,
      fields: category.fields.filter((field) =>
        `${labelFor(field.key)} ${descriptionFor(field.key)} ${field.key}`
          .toLocaleLowerCase()
          .includes(query),
      ),
    }))
    .filter((category) => category.fields.length);
});

const catalogCount = computed(() =>
  catalogGroups.value.reduce((count, category) => count + category.fields.length, 0),
);

const filteredCatalogCount = computed(() =>
  filteredCatalogGroups.value.reduce((count, category) => count + category.fields.length, 0),
);

const activeGroups = computed(() => {
  const remaining = new Set(overrideKeys.value);
  const groups = catalogGroups.value
    .map((category) => ({
      id: category.id,
      label: category.label,
      keys: category.fields.map((field) => field.key).filter((key) => remaining.delete(key)),
    }))
    .filter((category) => category.keys.length);
  if (remaining.size) {
    groups.push({
      id: 'additional',
      label: t('apps.overrides.override_editor'),
      keys: [...remaining],
    });
  }
  return groups;
});

function gpuOptions(): Array<{ label: string; value: string; adapterName: string; pnpId: string }> {
  const options = [
    { label: t('ui.settings.options.gpu.auto'), value: '', adapterName: '', pnpId: '' },
    ...(props.metadata.gpus ?? [])
      .map((gpu) => ({
        label: gpu.description?.trim() ?? '',
        value: gpu.pnp_id?.trim() || gpu.description?.trim() || '',
        adapterName: gpu.description?.trim() ?? '',
        pnpId: gpu.pnp_id?.trim() ?? '',
      }))
      .filter((option) => option.adapterName),
  ];
  const currentName = String(readValue('adapter_name') ?? '');
  const currentPnpId = String(readValue('adapter_pnp_id') ?? '');
  if (
    currentName &&
    !options.some(
      (option) =>
        option.adapterName === currentName && (!currentPnpId || option.pnpId === currentPnpId),
    )
  ) {
    options.push({
      label: t('ui.application.options.currentValue', { value: currentName }),
      value: currentPnpId || currentName,
      adapterName: currentName,
      pnpId: currentPnpId,
    });
  }
  return options;
}

function selectOptions(key: string): Array<{ label: string; value: string }> {
  const field = fieldFor(key);
  if (field?.source === 'gpu') {
    return gpuOptions().map(({ label, value }) => ({ label, value }));
  }

  let declaredOptions = field?.options ?? [];
  if (key === 'encoder') {
    const auto: SettingsOption = { value: '', labelKey: '_common.auto' };
    const platform = String(props.metadata.platform ?? '').toLocaleLowerCase();
    declaredOptions = platform.includes('windows')
      ? [
          auto,
          { value: 'nvenc', labelKey: 'ui.settings.options.encoder.nvenc' },
          { value: 'quicksync', labelKey: 'ui.settings.options.encoder.quicksync' },
          { value: 'amdvce', labelKey: 'ui.settings.options.encoder.amdvce' },
          { value: 'amdvce_legacy', labelKey: 'ui.settings.options.encoder.amdvce_legacy' },
          { value: 'mediafoundation', labelKey: 'ui.settings.options.encoder.mediafoundation' },
          { value: 'software', labelKey: 'ui.settings.options.encoder.software' },
        ]
      : [auto, { value: 'software', labelKey: 'ui.settings.options.encoder.software' }];
  }

  const options = declaredOptions.map((option) => ({
    label: optionLabel(option),
    value: String(option.value),
  }));
  const current = String(readValue(key) ?? '');
  if (options.length && current && !options.some((option) => option.value === current)) {
    options.push({
      label: t('ui.application.options.currentValue', { value: current }),
      value: current,
    });
  }
  return options;
}

function controlValue(key: string): string {
  if (fieldFor(key)?.source === 'gpu') {
    return String(readValue('adapter_pnp_id') || readValue('adapter_name') || '');
  }
  return String(readValue(key) ?? '');
}

function booleanValue(key: string): boolean {
  const value = readValue(key);
  return (
    value === true ||
    ['1', 'true', 'yes', 'on', 'enabled'].includes(String(value).toLocaleLowerCase())
  );
}

function setValue(key: string, value: unknown): void {
  const next = { ...props.modelValue, [key]: cloneValue(value) };
  emitOverrides(next);
}

function updateFromEvent(key: string, event: Event): void {
  const field = fieldFor(key);
  const target = event.target as HTMLInputElement | HTMLSelectElement;
  if (field?.source === 'gpu') {
    const selected = gpuOptions().find((option) => option.value === target.value);
    const next = {
      ...props.modelValue,
      adapter_name: selected?.adapterName ?? '',
      adapter_pnp_id: selected?.pnpId ?? '',
    };
    emitOverrides(next);
  } else if (field?.kind === 'boolean') {
    setValue(key, (target as HTMLInputElement).checked);
  } else if (field?.kind === 'number') {
    setValue(key, target.value === '' ? '' : Number(target.value));
  } else {
    setValue(key, target.value);
  }
}

function addOverride(key: string): void {
  if (hasOverride(key)) {
    catalogOpen.value = false;
    void focusOverride(key);
    return;
  }
  const field = fieldFor(key);
  const options = selectOptions(key);
  const fallback =
    field?.kind === 'boolean' ? false : field?.kind === 'number' ? 0 : (options[0]?.value ?? '');
  setValue(key, cloneValue(inheritedValue(key) ?? settingsDefaults[key] ?? fallback));
  announcement.value = `${t('apps.overrides.add_setting')}: ${labelFor(key)}`;
  catalogOpen.value = false;
  void focusOverride(key);
}

function removeOverride(key: string): void {
  const next = { ...props.modelValue };
  delete next[key];
  if (key === 'adapter_name') delete next.adapter_pnp_id;
  emitOverrides(next);
  announcement.value = `${t('_common.remove')}: ${labelFor(key)}`;
  void nextTick(() => document.getElementById(`${props.controlIdPrefix}-catalog-${key}`)?.focus());
}

async function focusOverride(key: string): Promise<void> {
  await nextTick();
  document.getElementById(`${props.controlIdPrefix}-${key}`)?.focus();
}
</script>

<template>
  <section class="settings-overrides" :aria-label="t('apps.overrides.title')">
    <div class="settings-overrides__heading">
      <div>
        <h3>{{ t('apps.overrides.title') }}</h3>
        <p>
          {{ t('apps.overrides.adjustment_hint', { scope: t(`apps.overrides.scope_${scope}`) }) }}
        </p>
      </div>
      <div class="settings-overrides__heading-actions">
        <StatusBadge
          tone="neutral"
          compact
          :label="t('apps.overrides.configured_count', { count: overrideKeys.length })"
        />
        <AppButton
          :id="`${controlIdPrefix}-catalog-toggle`"
          variant="secondary"
          size="compact"
          :icon="catalogOpen ? 'x' : 'plus'"
          :label="catalogOpen ? t('_common.close') : t('apps.overrides.add_setting')"
          :aria-expanded="catalogOpen"
          :aria-controls="`${controlIdPrefix}-catalog`"
          @click="catalogOpen = !catalogOpen"
        />
      </div>
    </div>

    <aside
      v-if="catalogOpen"
      :id="`${controlIdPrefix}-catalog`"
      class="settings-overrides__catalog"
      :aria-label="t('apps.overrides.browse_available')"
    >
      <div class="settings-overrides__catalog-header">
        <div class="settings-overrides__pane-heading">
          <div>
            <h4>{{ t('apps.overrides.browse_available') }}</h4>
            <p>{{ t('apps.overrides.browse_available_hint') }}</p>
          </div>
          <span class="settings-overrides__result-count">
            {{
              t('apps.overrides.showing_count', {
                shown: filteredCatalogCount,
                total: catalogCount,
              })
            }}
          </span>
        </div>
        <label class="vs-sr-only" :for="`${controlIdPrefix}-search`">
          {{ t('apps.overrides.browse_available') }}
        </label>
        <div class="settings-overrides__search">
          <UiIcon name="search" :size="16" aria-hidden="true" />
          <input
            :id="`${controlIdPrefix}-search`"
            v-model="search"
            class="vs-input"
            type="search"
            :placeholder="t('apps.overrides.filter_placeholder')"
          />
        </div>
      </div>

      <div v-if="filteredCatalogGroups.length" class="settings-overrides__catalog-list">
        <section v-for="category in filteredCatalogGroups" :key="category.id">
          <h5>{{ category.label }}</h5>
          <div class="settings-overrides__catalog-grid">
            <button
              v-for="field in category.fields"
              :id="`${controlIdPrefix}-catalog-${field.key}`"
              :key="field.key"
              type="button"
              class="settings-overrides__catalog-item"
              :class="{ 'settings-overrides__catalog-item--active': hasOverride(field.key) }"
              @click="addOverride(field.key)"
            >
              <span class="settings-overrides__catalog-copy">
                <strong>{{ labelFor(field.key) }}</strong>
                <span v-if="descriptionFor(field.key)">{{ descriptionFor(field.key) }}</span>
                <span class="settings-overrides__catalog-inherited">
                  {{ t('_common.inherited') }}: {{ inheritedDisplayValue(field.key) }}
                </span>
              </span>
              <span class="settings-overrides__catalog-action">
                <UiIcon
                  :name="hasOverride(field.key) ? 'check' : 'plus'"
                  :size="14"
                  aria-hidden="true"
                />
                {{ t(hasOverride(field.key) ? '_common.active' : 'apps.overrides.add_setting') }}
              </span>
            </button>
          </div>
        </section>
      </div>
      <div v-else class="settings-overrides__catalog-empty">
        <strong>{{ t('apps.overrides.no_matching_settings') }}</strong>
        <p>{{ t('apps.overrides.no_matching_settings_hint') }}</p>
        <AppButton
          variant="tertiary"
          size="compact"
          :label="t('_common.clear')"
          @click="search = ''"
        />
      </div>
    </aside>

    <div class="settings-overrides__editor">
      <div class="settings-overrides__pane-heading settings-overrides__active-heading">
        <div>
          <h4>{{ t('apps.overrides.active_overrides') }}</h4>
          <p>{{ t('apps.overrides.new_settings_hint') }}</p>
        </div>
      </div>
      <div class="vs-sr-only" aria-live="polite">{{ announcement }}</div>

      <div v-if="overrideKeys.length" class="settings-overrides__active-groups">
        <section v-for="category in activeGroups" :key="category.id">
          <h5>{{ category.label }}</h5>
          <div class="vs-settings-group settings-overrides__list">
            <SettingRow
              v-for="key in category.keys"
              :key="key"
              :class="{
                'settings-overrides__list-row--switch': fieldFor(key)?.kind === 'boolean',
              }"
              :label="labelFor(key)"
              :control-id="`${controlIdPrefix}-${key}`"
              :stacked="fieldFor(key)?.kind !== 'boolean'"
            >
              <template #description>
                <span v-if="descriptionFor(key)">{{ descriptionFor(key) }}</span>
                <span class="settings-overrides__inherited">
                  {{ t('_common.inherited') }}: {{ inheritedDisplayValue(key) }}
                </span>
              </template>
              <template #default="{ descriptionId }">
                <div class="settings-overrides__controls">
                  <label v-if="fieldFor(key)?.kind === 'boolean'" class="vs-switch">
                    <input
                      :id="`${controlIdPrefix}-${key}`"
                      type="checkbox"
                      :checked="booleanValue(key)"
                      :aria-describedby="descriptionId"
                      @change="updateFromEvent(key, $event)"
                    />
                    <span class="vs-switch__track" aria-hidden="true" />
                    <span class="vs-sr-only">{{ labelFor(key) }}</span>
                  </label>
                  <select
                    v-else-if="
                      (fieldFor(key)?.kind === 'select' || fieldFor(key)?.kind === 'duration') &&
                      selectOptions(key).length
                    "
                    :id="`${controlIdPrefix}-${key}`"
                    class="vs-select"
                    :value="controlValue(key)"
                    :aria-describedby="descriptionId"
                    @change="updateFromEvent(key, $event)"
                  >
                    <option
                      v-for="option in selectOptions(key)"
                      :key="option.value"
                      :value="option.value"
                    >
                      {{ option.label }}
                    </option>
                  </select>
                  <textarea
                    v-else-if="fieldFor(key)?.kind === 'textarea'"
                    :id="`${controlIdPrefix}-${key}`"
                    class="vs-textarea"
                    rows="4"
                    :value="String(readValue(key) ?? '')"
                    :aria-describedby="descriptionId"
                    @input="updateFromEvent(key, $event)"
                  />
                  <input
                    v-else
                    :id="`${controlIdPrefix}-${key}`"
                    :class="['vs-input', { 'vs-monospace': fieldFor(key)?.monospace }]"
                    :type="fieldFor(key)?.kind === 'number' ? 'number' : 'text'"
                    :min="fieldFor(key)?.min"
                    :max="fieldFor(key)?.max"
                    :step="fieldFor(key)?.step"
                    :value="String(readValue(key) ?? '')"
                    :placeholder="
                      fieldFor(key)?.placeholderKey && messageExists(fieldFor(key)!.placeholderKey!)
                        ? t(fieldFor(key)!.placeholderKey!)
                        : undefined
                    "
                    :aria-describedby="descriptionId"
                    @input="updateFromEvent(key, $event)"
                  />
                  <AppButton
                    variant="tertiary"
                    size="compact"
                    icon="trash"
                    icon-only
                    :label="`${t('_common.remove')}: ${labelFor(key)}`"
                    @click="removeOverride(key)"
                  />
                </div>
              </template>
            </SettingRow>
          </div>
        </section>
      </div>
      <div v-else class="settings-overrides__empty">
        <UiIcon name="settings" :size="20" aria-hidden="true" />
        <strong>{{ t('apps.overrides.empty_picker') }}</strong>
        <p>{{ t('apps.overrides.new_settings_hint') }}</p>
        <AppButton
          variant="secondary"
          size="compact"
          icon="plus"
          :label="t('apps.overrides.add_setting')"
          @click="catalogOpen = true"
        />
      </div>
    </div>
  </section>
</template>

<style scoped>
.settings-overrides {
  display: grid;
  container-name: settings-overrides;
  container-type: inline-size;
  min-inline-size: 0;
  gap: var(--vs-space-12);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.settings-overrides__heading,
.settings-overrides__pane-heading {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.settings-overrides__heading h3,
.settings-overrides__pane-heading h4 {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
}

.settings-overrides__heading p,
.settings-overrides__pane-heading p,
.settings-overrides__catalog-empty p,
.settings-overrides__empty p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.settings-overrides__search {
  position: relative;
  margin: 0 var(--vs-space-16) var(--vs-space-8);
}

.settings-overrides__search > svg {
  position: absolute;
  inset-inline-start: var(--vs-space-12);
  inset-block-start: 50%;
  color: var(--vs-color-text-muted);
  pointer-events: none;
  transform: translateY(-50%);
}

.settings-overrides__search .vs-input {
  inline-size: 100%;
  padding-inline-start: 2.25rem;
}

.settings-overrides__result-count {
  display: block;
  padding: 0 var(--vs-space-16) var(--vs-space-12);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}

.settings-overrides__catalog-list h5,
.settings-overrides__active-groups h5 {
  padding: var(--vs-space-8) var(--vs-space-16);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
  letter-spacing: 0.04em;
  text-transform: uppercase;
}

.settings-overrides__catalog-item {
  display: grid;
  inline-size: 100%;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-12) var(--vs-space-16);
  border: 0;
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: transparent;
  color: inherit;
  text-align: start;
  cursor: pointer;
}

.settings-overrides__catalog-item:hover,
.settings-overrides__catalog-item:focus-visible {
  background: var(--vs-color-bg-surface);
}

.settings-overrides__catalog-item:focus-visible {
  outline: 2px solid var(--vs-color-accent-default);
  outline-offset: -2px;
}

.settings-overrides__catalog-item--active {
  background: color-mix(in srgb, var(--vs-color-accent-default) 7%, transparent);
}

.settings-overrides__catalog-copy {
  display: grid;
  min-inline-size: 0;
  gap: var(--vs-space-4);
}

.settings-overrides__catalog-copy > span {
  display: -webkit-box;
  overflow: hidden;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  -webkit-box-orient: vertical;
  -webkit-line-clamp: 2;
}

.settings-overrides__catalog-action {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  color: var(--vs-color-accent-default);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
  white-space: nowrap;
}

.settings-overrides__catalog-item--active .settings-overrides__catalog-action {
  color: var(--vs-color-status-success);
}

.settings-overrides__catalog-empty,
.settings-overrides__empty {
  display: grid;
  gap: var(--vs-space-8);
  padding: var(--vs-space-24) var(--vs-space-16);
}

.settings-overrides__active-groups h5 {
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-subtle);
}

.settings-overrides__active-groups section + section h5 {
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.settings-overrides__list {
  border: 0;
  border-radius: 0;
}

.settings-overrides__inherited {
  display: block;
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-muted);
}

.settings-overrides__empty {
  justify-items: center;
  min-block-size: 12rem;
  text-align: center;
}

.settings-overrides__heading-actions {
  display: flex;
  flex: none;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

.settings-overrides__catalog,
.settings-overrides__editor {
  min-inline-size: 0;
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.settings-overrides__catalog {
  border-inline-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-subtle);
}

.settings-overrides__catalog-header {
  display: grid;
}

.settings-overrides__pane-heading {
  padding: var(--vs-space-16);
}

.settings-overrides__search {
  inline-size: min(calc(100% - (var(--vs-space-16) * 2)), 38rem);
  margin: 0 var(--vs-space-16) var(--vs-space-16);
}

.settings-overrides__result-count {
  align-self: flex-start;
  padding: var(--vs-space-2) 0 0;
  white-space: nowrap;
}

.settings-overrides__catalog-list {
  display: grid;
  max-block-size: 36rem;
  overflow-y: auto;
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.settings-overrides__catalog-list > section + section {
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.settings-overrides__catalog-list h5 {
  position: static;
  border-block-end: 0;
  background: transparent;
}

.settings-overrides__catalog-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 20rem), 1fr));
  gap: var(--vs-space-8);
  padding: 0 var(--vs-space-12) var(--vs-space-12);
}

.settings-overrides__catalog-item {
  min-block-size: 5.5rem;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
}

.settings-overrides__catalog-copy > .settings-overrides__catalog-inherited {
  color: var(--vs-color-text-muted);
  -webkit-line-clamp: 1;
}

.settings-overrides__active-heading {
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.settings-overrides__controls {
  display: grid;
  inline-size: 100%;
  min-inline-size: 0;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  justify-content: stretch;
  gap: var(--vs-space-8);
}

.settings-overrides__controls > .vs-switch {
  justify-self: start;
}

.settings-overrides__list :deep(.vs-setting-row.settings-overrides__list-row--switch) {
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
}

.settings-overrides__list
  :deep(.vs-setting-row.settings-overrides__list-row--switch .vs-setting-row__control) {
  min-inline-size: 7rem;
}

.settings-overrides__controls > .vs-input,
.settings-overrides__controls > .vs-select,
.settings-overrides__controls > .vs-textarea {
  inline-size: 100%;
  max-inline-size: 36rem;
}

.settings-overrides__empty {
  min-block-size: 10rem;
}

@container settings-overrides (max-width: 40rem) {
  .settings-overrides__heading,
  .settings-overrides__pane-heading {
    display: grid;
  }

  .settings-overrides__heading-actions {
    justify-content: flex-start;
  }

  .settings-overrides__catalog-grid {
    grid-template-columns: minmax(0, 1fr);
  }

  .settings-overrides__catalog-item {
    grid-template-columns: minmax(0, 1fr);
    align-items: start;
  }

  .settings-overrides__catalog-action {
    justify-self: start;
  }

  .settings-overrides__result-count {
    white-space: normal;
  }
}

@media (max-width: 47.999rem) {
  .settings-overrides {
    padding: var(--vs-space-12);
  }
}
</style>
