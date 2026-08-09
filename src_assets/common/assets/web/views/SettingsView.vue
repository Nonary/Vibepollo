<script setup lang="ts">
import { computed, onMounted, reactive, ref, toRaw } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet, apiPatch, apiPost } from '@/api/client';
import DisplayModeOverrides from '@/components/settings/DisplayModeOverrides.vue';
import DisplayRecoverySettings from '@/components/settings/DisplayRecoverySettings.vue';
import GlobalPrepCommands from '@/components/settings/GlobalPrepCommands.vue';
import SettingsIntegrationPath from '@/components/settings/SettingsIntegrationPath.vue';
import { InlineAlert, LoadingSkeleton, PageHeader, StatusBadge, UiIcon } from '@/components/ui';
import {
  restartRequiredKeys,
  settingsCategories,
  settingsDefaults,
  type SettingsField,
  type SettingsGroup,
  type SettingsOption,
  type SettingsVisibility,
} from '@/configs/settingsSchema';

const { locale, t, te } = useI18n();

function messageExists(key: string): boolean {
  return te(key) || te(key, 'en');
}

interface ConfigResponse extends Record<string, unknown> {
  status?: boolean;
}

interface SaveResult {
  appliedNow?: boolean;
  deferred?: boolean;
  restartRequired?: boolean;
  status?: boolean;
}

interface GpuMetadata {
  description?: string;
  pnp_id?: string;
  vendor_id?: number | string;
  dedicated_video_memory?: number | string;
}

interface MetadataResponse {
  gpus?: GpuMetadata[];
  platform?: string;
  prerelease?: string;
  windows_build_number?: number;
  windows_major_version?: number;
}

interface DisplaySettingsGroup extends SettingsGroup {
  categoryId: string;
}

interface GpuOption extends SettingsOption {
  adapterName: string;
  pnpId: string;
}

interface DisplayDevice {
  device_id?: unknown;
  display_name?: unknown;
  friendly_name?: unknown;
  info?: {
    active?: unknown;
  };
}

const loading = ref(true);
const saving = ref(false);
const restarting = ref(false);
const restartAvailable = ref(false);
const displayOverridesValid = ref(true);
const error = ref('');
const notice = ref('');
const search = ref('');
const activeCategory = ref(settingsCategories[0].id);
const hostMetadata = ref<MetadataResponse>({});
const values = reactive<Record<string, unknown>>({});
const original = ref<Record<string, unknown>>({});
const displayDevices = ref<DisplayDevice[]>([]);
const displayDevicesLoading = ref(false);
const displayDevicesLoaded = ref(false);
const displayDevicesError = ref('');

function cloneSettings(value: Record<string, unknown>): Record<string, unknown> {
  return structuredClone(toRaw(value));
}

function numericMetadataValue(value: unknown): number {
  const parsed = Number(value ?? 0);
  return Number.isFinite(parsed) ? parsed : 0;
}

const preferredGpu = computed<GpuMetadata | null>(() => {
  const gpus = hostMetadata.value.gpus ?? [];
  const configuredName = String(values.adapter_name ?? '').trim();
  const configuredPnpId = String(values.adapter_pnp_id ?? '').trim();
  const configured = gpus.find((gpu) => {
    const pnpId = gpu.pnp_id?.trim() ?? '';
    const name = gpu.description?.trim() ?? '';
    return configuredPnpId
      ? pnpId.toLocaleLowerCase() === configuredPnpId.toLocaleLowerCase()
      : configuredName && name === configuredName;
  });
  if (configured) return configured;
  if (configuredName || configuredPnpId) return null;

  return (
    gpus.reduce<GpuMetadata | null>((best, gpu) => {
      if (!best) return gpu;
      return numericMetadataValue(gpu.dedicated_video_memory) >
        numericMetadataValue(best.dedicated_video_memory)
        ? gpu
        : best;
    }, null) ?? null
  );
});

function encoderFamily(encoder: string): SettingsField['encoderFamily'] | undefined {
  if (encoder === 'nvenc') return 'nvidia';
  if (encoder === 'quicksync') return 'intel';
  if (encoder === 'amdvce' || encoder === 'amdvce_legacy') return 'amd';
  return undefined;
}

const preferredAutomaticEncoderFamily = computed<SettingsField['encoderFamily'] | undefined>(() => {
  switch (numericMetadataValue(preferredGpu.value?.vendor_id)) {
    case 0x10de:
      return 'nvidia';
    case 0x8086:
      return 'intel';
    case 0x1002:
    case 0x1022:
      return 'amd';
    default:
      return undefined;
  }
});

const effectiveEncoderFamily = computed<SettingsField['encoderFamily'] | undefined>(() => {
  const configuredEncoder = String(values.encoder ?? '');
  return configuredEncoder
    ? encoderFamily(configuredEncoder)
    : preferredAutomaticEncoderFamily.value;
});

const automaticCaptureLabel = computed(() => {
  const platform = String(hostMetadata.value.platform ?? '').toLocaleLowerCase();
  if (!platform.includes('windows')) return t('_common.auto');
  return t(
    Number(hostMetadata.value.windows_build_number ?? 0) >= 22631
      ? 'ui.settings.options.capture.auto_wgc'
      : 'ui.settings.options.capture.auto_ddx',
  );
});

const automaticEncoderLabel = computed(() => {
  const family = preferredAutomaticEncoderFamily.value;
  const gpuName = preferredGpu.value?.description?.trim() ?? '';
  const encoderKey =
    family === 'nvidia'
      ? 'ui.settings.options.encoder.nvenc'
      : family === 'intel'
        ? 'ui.settings.options.encoder.quicksync'
        : family === 'amd'
          ? 'ui.settings.options.encoder.amdvce'
          : '';
  if (!encoderKey || !gpuName) return t('ui.settings.options.encoder.auto');
  return t('ui.settings.options.encoder.auto_selected', {
    encoder: t(encoderKey),
    name: gpuName,
  });
});

const isWindowsHost = computed(() =>
  String(hostMetadata.value.platform ?? '')
    .toLocaleLowerCase()
    .includes('windows'),
);

const physicalDisplaySelected = computed(
  () => String(values.virtual_display_mode ?? '') === 'disabled',
);

const hostPlatform = computed(() => String(hostMetadata.value.platform ?? ''));

const physicalDisplayDescription = computed(() =>
  t(isWindowsHost.value ? 'config.output_name_desc_windows' : 'config.output_name_desc_unix'),
);

const displayDeviceOptions = computed(() => {
  const seen = new Set<string>();
  const options = displayDevices.value.flatMap((device) => {
    const value = String(device.device_id ?? device.display_name ?? '').trim();
    if (!value || seen.has(value)) return [];
    seen.add(value);
    const friendly =
      String(device.friendly_name ?? '').trim() ||
      String(device.display_name ?? '').trim() ||
      t('config.output_name');
    const active = typeof device.info?.active === 'boolean' ? device.info.active : null;
    const suffix =
      active === null
        ? ''
        : active
          ? ` (${t('config.app_display_status_active')})`
          : ` (${t('config.app_display_status_inactive')})`;
    return [{ label: `${friendly} - ${value}${suffix}`, value }];
  });
  const current = String(values.output_name ?? '').trim();
  if (current && !seen.has(current)) {
    options.unshift({
      label: t('ui.application.options.currentValue', { value: current }),
      value: current,
    });
  }
  return options;
});

function comparableValue(value: unknown): string {
  if (value && typeof value === 'object') return JSON.stringify(value);
  return String(value ?? '');
}

const dirtyKeys = computed(() => {
  const keys = new Set([...Object.keys(original.value), ...Object.keys(values)]);
  return [...keys].filter(
    (key) => comparableValue(values[key]) !== comparableValue(original.value[key]),
  );
});

const isDirty = computed(() => dirtyKeys.value.length > 0);
const saveAllowed = computed(
  () => displayOverridesValid.value || !dirtyKeys.value.includes('dd_mode_remapping'),
);
const restartPending = computed(() => dirtyKeys.value.some((key) => restartRequiredKeys.has(key)));

const category = computed(
  () =>
    settingsCategories.find((candidate) => candidate.id === activeCategory.value) ??
    settingsCategories[0],
);

const isSearching = computed(() => search.value.trim().length > 0);

const categoryDescription = computed(() =>
  isSearching.value
    ? t('ui.settings.search_description')
    : t(`ui.settings.categories.${category.value.id}.description`),
);

const filteredGroups = computed(() => {
  const query = search.value.trim().toLocaleLowerCase(locale.value);
  const categories = query ? settingsCategories : [category.value];
  const seenKeys = new Set<string>();

  return categories.flatMap((settingsCategory) =>
    settingsCategory.groups
      .filter((group) => query || groupIsVisible(group))
      .map<DisplaySettingsGroup>((group) => ({
        ...group,
        categoryId: settingsCategory.id,
        fields: group.fields.filter((field) => {
          const matches =
            !query ||
            `${categoryLabel(settingsCategory.id)} ${groupTitle(group.id)} ${fieldLabel(field)} ${fieldDescription(field)} ${field.key}`
              .toLocaleLowerCase(locale.value)
              .includes(query);
          if (!matches || !fieldMatchesPlatform(field) || (!query && !fieldIsVisible(field))) {
            return false;
          }
          if (query && seenKeys.has(field.key)) return false;
          if (query) seenKeys.add(field.key);
          return true;
        }),
      }))
      .filter((group) => group.fields.length),
  );
});

const everydaySummary = computed(() => [
  {
    label: t('ui.settings.summary.display'),
    value: optionLabel('virtual_display_mode', t('ui.settings.summary.host_default')),
  },
  {
    label: t('ui.settings.summary.capture'),
    value: optionLabel('capture', automaticCaptureLabel.value),
  },
  {
    label: t('ui.settings.summary.game_smoothness'),
    value:
      String(values.virtual_display_mode ?? '') === 'disabled'
        ? t('ui.settings.summary.physical_pacing')
        : String(values.dd_refresh_rate_option ?? 'auto') === 'manual'
          ? t('ui.settings.summary.manual_refresh_pacing')
          : String(values.frame_limiter_provider ?? 'auto') === 'none' &&
              String(values.frame_limiter_auto_virtual_framegen ?? 'enabled') !== 'disabled'
            ? t(
                String(values.frame_limiter_auto_virtual_framegen ?? 'enabled') === 'legacy'
                  ? 'ui.settings.summary.compatibility_pacing_limiter_off'
                  : 'ui.settings.summary.automatic_pacing_limiter_off',
              )
            : t(
                String(values.frame_limiter_auto_virtual_framegen ?? 'enabled') === 'enabled'
                  ? 'ui.settings.summary.automatic_pacing'
                  : String(values.frame_limiter_auto_virtual_framegen ?? '') === 'legacy'
                    ? 'ui.settings.summary.compatibility_pacing'
                    : 'ui.settings.summary.pacing_off',
              ),
  },
]);

const gpuOptions = computed<GpuOption[]>(() => {
  const options: GpuOption[] = [
    {
      labelKey: 'ui.settings.options.gpu.auto',
      value: '',
      adapterName: '',
      pnpId: '',
    },
  ];
  for (const gpu of hostMetadata.value.gpus ?? []) {
    const adapterName = gpu.description?.trim() ?? '';
    const pnpId = gpu.pnp_id?.trim() ?? '';
    if (!adapterName) continue;
    options.push({
      labelKey: '',
      value: pnpId || adapterName,
      adapterName,
      pnpId,
    });
  }

  const currentName = String(values.adapter_name ?? '');
  const currentPnpId = String(values.adapter_pnp_id ?? '');
  if (
    currentName &&
    !options.some(
      (option) =>
        option.adapterName === currentName && (!currentPnpId || option.pnpId === currentPnpId),
    )
  ) {
    options.push({
      labelKey: 'ui.settings.options.gpu.current',
      value: currentPnpId || currentName,
      adapterName: currentName,
      pnpId: currentPnpId,
    });
  }
  return options;
});

function isTrue(value: unknown): boolean {
  if (typeof value === 'boolean') return value;
  return ['1', 'true', 'yes', 'on', 'enabled'].includes(String(value).toLocaleLowerCase());
}

function valuesMatch(current: unknown, expected: string | boolean): boolean {
  return typeof expected === 'boolean'
    ? isTrue(current) === expected
    : String(current ?? '') === expected;
}

function fieldMatchesPlatform(field: SettingsField): boolean {
  if (field.platform) {
    const platform = String(hostMetadata.value.platform ?? '').toLocaleLowerCase();
    return platform.includes(field.platform);
  }
  return true;
}

function visibilityMatches(condition?: SettingsVisibility): boolean {
  if (!condition) return true;
  if (condition.equals !== undefined) {
    return valuesMatch(values[condition.key], condition.equals);
  }
  if (condition.notEquals !== undefined) {
    return !valuesMatch(values[condition.key], condition.notEquals);
  }
  return true;
}

function groupIsVisible(group: SettingsGroup): boolean {
  return visibilityMatches(group.visibleWhen);
}

function fieldIsVisible(field: SettingsField): boolean {
  return (
    fieldMatchesPlatform(field) &&
    visibilityMatches(field.visibleWhen) &&
    (!field.encoderFamily || field.encoderFamily === effectiveEncoderFamily.value)
  );
}

function fieldByKey(key: string): SettingsField | undefined {
  for (const settingsCategory of settingsCategories) {
    for (const group of settingsCategory.groups) {
      const field = group.fields.find((candidate) => candidate.key === key);
      if (field) return field;
    }
  }
  return undefined;
}

function categoryLabel(id: string): string {
  return t(`ui.settings.categories.${id}.label`);
}

function groupTitle(id: string): string {
  return t(`ui.settings.groups.${id}.title`);
}

function groupDescription(id: string): string {
  const key = `ui.settings.groups.${id}.description`;
  return messageExists(key) ? t(key) : '';
}

function fieldLabel(field: SettingsField): string {
  const configKey = `config.${field.key}`;
  const key =
    field.labelKey ??
    (messageExists(configKey) ? configKey : `ui.settings.fields.${field.key}.label`);
  return t(key);
}

function fieldDescription(field: SettingsField): string {
  if (field.descriptionKey) return t(field.descriptionKey);
  const platform = String(hostMetadata.value.platform ?? '').toLocaleLowerCase();
  const candidates = [
    platform.includes('windows') ? `config.${field.key}_desc_windows` : '',
    platform.includes('linux') ? `config.${field.key}_desc_linux` : '',
    platform.includes('mac') ? `config.${field.key}_desc_macos` : '',
    `config.${field.key}_desc`,
    `ui.settings.fields.${field.key}.description`,
  ].filter(Boolean);
  const key = candidates.find((candidate) => messageExists(candidate));
  return key ? t(key) : '';
}

function optionText(option: SettingsOption, fieldKey = ''): string {
  if (!option.value && fieldKey === 'capture') return automaticCaptureLabel.value;
  if (!option.value && fieldKey === 'encoder') return automaticEncoderLabel.value;
  const gpu = gpuOptions.value.find((candidate) => candidate.value === option.value);
  if (!option.labelKey) {
    return gpu?.adapterName || option.value;
  }
  return t(option.labelKey, { name: gpu?.adapterName ?? '', value: option.value });
}

function localizedOption(value: string, labelKey: string): SettingsOption {
  return { value, labelKey };
}

function optionLabel(key: string, fallback: string): string {
  const field = fieldByKey(key);
  const value = String(values[key] ?? '');
  const selected = field ? optionsFor(field).find((option) => option.value === value) : undefined;
  return selected ? optionText(selected, key) : fallback;
}

function optionsFor(field: SettingsField): SettingsOption[] {
  if (field.source === 'gpu') return gpuOptions.value;

  const platform = String(hostMetadata.value.platform ?? '').toLocaleLowerCase();
  const current = String(values[field.key] ?? '');
  let options = field.options ?? [];

  if (field.key === 'encoder') {
    const common = [localizedOption('', 'ui.settings.options.encoder.auto')];
    options = common;
    if (platform.includes('windows')) {
      options = [
        ...common,
        localizedOption('nvenc', 'ui.settings.options.encoder.nvenc'),
        localizedOption('quicksync', 'ui.settings.options.encoder.quicksync'),
        localizedOption('amdvce', 'ui.settings.options.encoder.amdvce'),
        localizedOption('amdvce_legacy', 'ui.settings.options.encoder.amdvce_legacy'),
        localizedOption('mediafoundation', 'ui.settings.options.encoder.mediafoundation'),
        localizedOption('software', 'ui.settings.options.encoder.software'),
      ];
    } else if (platform.includes('mac')) {
      options = [
        ...common,
        localizedOption('videotoolbox', 'ui.settings.options.encoder.videotoolbox'),
        localizedOption('software', 'ui.settings.options.encoder.software'),
      ];
    } else if (platform) {
      options = [
        ...common,
        localizedOption('nvenc', 'ui.settings.options.encoder.nvenc'),
        localizedOption('vulkan', 'ui.settings.options.encoder.vulkan'),
        localizedOption('vaapi', 'ui.settings.options.encoder.vaapi'),
        localizedOption('software', 'ui.settings.options.encoder.software'),
      ];
    }
  } else if (field.key === 'capture' && !platform.includes('windows')) {
    options = [localizedOption('', '_common.auto')];
  }

  if (current && !options.some((option) => option.value === current)) {
    return [...options, localizedOption(current, 'ui.settings.options.current')];
  }
  return options;
}

function controlValue(field: SettingsField): string {
  if (field.source !== 'gpu') return String(values[field.key] ?? '');
  const currentName = String(values.adapter_name ?? '');
  const currentPnpId = String(values.adapter_pnp_id ?? '');
  return (
    gpuOptions.value.find(
      (option) =>
        option.adapterName === currentName && (!currentPnpId || option.pnpId === currentPnpId),
    )?.value ?? ''
  );
}

function dependencyHint(field: SettingsField): string {
  if (!isSearching.value || !field.visibleWhen || fieldIsVisible(field)) return '';
  const dependency = fieldByKey(field.visibleWhen.key);
  return dependency
    ? t('ui.settings.dependency_inactive', { setting: fieldLabel(dependency) })
    : '';
}

function updateBoolean(key: string, event: Event): void {
  values[key] = (event.target as HTMLInputElement).checked;
}

function updateValue(key: string, event: Event, field?: SettingsField): void {
  const raw = (event.target as HTMLInputElement | HTMLSelectElement | HTMLTextAreaElement).value;
  if (field?.source === 'gpu') {
    const option = gpuOptions.value.find((candidate) => candidate.value === raw);
    values.adapter_name = option?.adapterName ?? '';
    values.adapter_pnp_id = option?.pnpId ?? '';
    return;
  }
  values[key] =
    (field?.kind === 'number' || field?.kind === 'duration') && raw !== '' ? Number(raw) : raw;
}

function normalizeConfiguredValues(configured: Record<string, unknown>): Record<string, unknown> {
  const normalized = { ...configured };
  const logLevels: Record<string, number> = {
    verbose: 0,
    debug: 1,
    info: 2,
    warning: 3,
    error: 4,
    fatal: 5,
    none: 6,
  };
  const configuredLevel = String(normalized.min_log_level ?? '').toLocaleLowerCase();
  if (configuredLevel in logLevels) normalized.min_log_level = logLevels[configuredLevel];
  if (normalized.frame_limiter_provider === 'nvidia_control_panel') {
    normalized.frame_limiter_provider = 'nvidia-control-panel';
  }
  if (typeof normalized.dd_mode_remapping === 'string') {
    if (!normalized.dd_mode_remapping.trim()) {
      normalized.dd_mode_remapping = {
        mixed: [],
        resolution_only: [],
        refresh_rate_only: [],
      };
    } else {
      try {
        const parsed = JSON.parse(normalized.dd_mode_remapping);
        if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
          normalized.dd_mode_remapping = parsed;
        }
      } catch {
        // Retain malformed persisted data so the editor can block a destructive overwrite.
      }
    }
  }
  return normalized;
}

async function load(): Promise<void> {
  loading.value = true;
  error.value = '';
  restartAvailable.value = false;
  try {
    const [response, metadata] = await Promise.all([
      apiGet<ConfigResponse>('/api/config'),
      apiGet<MetadataResponse>('/api/metadata').catch((): MetadataResponse => ({})),
    ]);
    hostMetadata.value = metadata;
    const configured = normalizeConfiguredValues(
      Object.fromEntries(Object.entries(response).filter(([key]) => key !== 'status')),
    );
    const defaults = { ...settingsDefaults };
    const buildNumber = Number(metadata.windows_build_number ?? 0);
    const majorVersion = Number(metadata.windows_major_version ?? 0);
    if (
      metadata.platform === 'windows' &&
      !((buildNumber && buildNumber >= 22000) || majorVersion >= 11)
    ) {
      defaults.virtual_display_mode = 'disabled';
    }
    if (metadata.prerelease) defaults.min_log_level = 1;

    const normalized = { ...defaults, ...configured };
    Object.keys(values).forEach((key) => delete values[key]);
    Object.assign(values, normalized);
    original.value = cloneSettings(normalized);
  } catch {
    error.value = t('ui.settings.errors.load');
  } finally {
    loading.value = false;
  }
}

async function loadDisplayDevices(force = false): Promise<void> {
  if (displayDevicesLoading.value || (displayDevicesLoaded.value && !force)) return;
  displayDevicesLoading.value = true;
  displayDevicesError.value = '';
  try {
    const response = await apiGet<unknown>('/api/display-devices?detail=full');
    if (!Array.isArray(response)) throw new Error('invalid-display-device-response');
    displayDevices.value = response as DisplayDevice[];
    displayDevicesLoaded.value = true;
  } catch {
    displayDevices.value = [];
    displayDevicesError.value = t('config.display_devices_load_failed');
  } finally {
    displayDevicesLoading.value = false;
  }
}

async function save(): Promise<void> {
  if (!isDirty.value || saving.value || !saveAllowed.value) return;
  saving.value = true;
  error.value = '';
  notice.value = '';
  try {
    const patch = Object.fromEntries(
      dirtyKeys.value.map((key) => [key, values[key] === '' ? null : values[key]]),
    );
    const result = await apiPatch<SaveResult>('/api/config', patch);
    original.value = cloneSettings(values);
    restartAvailable.value = Boolean(result.restartRequired);
    notice.value = result.restartRequired
      ? t('ui.settings.notices.saved_restart')
      : result.deferred
        ? t('ui.settings.notices.saved_deferred')
        : t('ui.settings.notices.saved');
  } catch {
    error.value = t('ui.settings.errors.save');
  } finally {
    saving.value = false;
  }
}

function discard(): void {
  const restored = cloneSettings(original.value);
  for (const key of Object.keys(values)) {
    if (!(key in restored)) delete values[key];
  }
  Object.assign(values, restored);
  notice.value = '';
  restartAvailable.value = false;
}

async function restart(): Promise<void> {
  restarting.value = true;
  notice.value = t('ui.settings.notices.restarting');
  try {
    await apiPost('/api/restart');
  } catch {
    // The host may terminate the HTTP connection as part of a successful restart.
  } finally {
    window.setTimeout(() => window.location.reload(), 3500);
  }
}

async function resetDisplayPersistence(): Promise<void> {
  error.value = '';
  notice.value = '';
  try {
    await apiPost('/api/reset-display-device-persistence');
    notice.value = t('ui.settings.notices.display_state_cleared');
  } catch {
    error.value = t('ui.settings.errors.reset_display');
  }
}

onMounted(() => void load());
</script>

<template>
  <div class="page settings-page">
    <PageHeader :title="t('ui.settings.title')" :description="t('ui.settings.description')">
      <template #actions>
        <button class="button button--secondary" type="button" :disabled="loading" @click="load">
          <UiIcon name="refresh" />
          {{ t('ui.settings.reload') }}
        </button>
      </template>
    </PageHeader>

    <InlineAlert v-if="error" tone="danger" :title="t('ui.settings.errors.title')">
      {{ error }}
    </InlineAlert>
    <InlineAlert v-else-if="notice" tone="success" :title="t('ui.settings.notices.title')">
      {{ notice }}
      <template v-if="restartAvailable" #actions>
        <button class="button button--secondary button--compact" type="button" @click="restart">
          {{ t('ui.settings.restart_now') }}
        </button>
      </template>
    </InlineAlert>
    <InlineAlert
      v-else-if="restartPending"
      tone="warning"
      :title="t('ui.settings.restart_required')"
    >
      {{ t('ui.settings.restart_required_description') }}
    </InlineAlert>

    <div class="settings-tools">
      <label class="search-field">
        <UiIcon name="search" />
        <span class="visually-hidden">{{ t('ui.settings.search') }}</span>
        <input
          v-model="search"
          class="vs-input"
          type="search"
          :placeholder="t('ui.settings.search')"
        />
      </label>
      <StatusBadge v-if="isDirty" tone="warning">
        {{ t('ui.settings.unsaved_short', { count: dirtyKeys.length }) }}
      </StatusBadge>
    </div>

    <div class="settings-layout">
      <nav class="settings-nav" :aria-label="t('ui.settings.categories_label')">
        <button
          v-for="item in settingsCategories"
          :key="item.id"
          type="button"
          :class="{ 'settings-nav__item--active': activeCategory === item.id }"
          :aria-current="activeCategory === item.id ? 'page' : undefined"
          @click="activeCategory = item.id"
        >
          {{ categoryLabel(item.id) }}
        </button>
      </nav>

      <div class="settings-content">
        <div v-if="loading" class="settings-group" :aria-label="t('ui.settings.loading')">
          <LoadingSkeleton v-for="index in 6" :key="index" height="64px" />
        </div>

        <template v-else>
          <header class="settings-category-heading">
            <span>{{
              t(isSearching ? 'ui.settings.all_settings' : 'ui.settings.selected_category')
            }}</span>
            <h2>
              {{ isSearching ? t('ui.settings.search_results') : categoryLabel(category.id) }}
            </h2>
            <p>{{ categoryDescription }}</p>
          </header>

          <div
            v-if="activeCategory === 'everyday' && !isSearching"
            class="settings-summary"
            :aria-label="t('ui.settings.summary.label')"
          >
            <div v-for="item in everydaySummary" :key="item.label">
              <span>{{ item.label }}</span>
              <strong>{{ item.value }}</strong>
            </div>
          </div>

          <section
            v-for="group in filteredGroups"
            :key="`${group.categoryId}-${group.id}`"
            class="settings-section"
          >
            <component
              :is="group.collapsed && !isSearching ? 'details' : 'div'"
              :class="{ 'settings-disclosure': group.collapsed && !isSearching }"
            >
              <component
                :is="group.collapsed && !isSearching ? 'summary' : 'div'"
                class="settings-section__heading"
              >
                <span v-if="isSearching">{{ categoryLabel(group.categoryId) }}</span>
                <h3>{{ groupTitle(group.id) }}</h3>
                <p v-if="groupDescription(group.id)">{{ groupDescription(group.id) }}</p>
              </component>
              <div class="settings-group">
                <div
                  v-for="field in group.fields"
                  :key="field.key"
                  class="settings-row"
                  :class="{
                    'settings-row--stacked': field.stacked,
                    'settings-row--recovery': field.kind === 'display-recovery',
                  }"
                >
                  <div v-if="field.kind === 'mode-remapping'" class="settings-row__copy">
                    <span class="settings-row__label">{{ fieldLabel(field) }}</span>
                    <span v-if="fieldDescription(field)" class="settings-row__description">
                      {{ fieldDescription(field) }}
                    </span>
                  </div>

                  <label
                    v-else-if="field.kind !== 'display-recovery'"
                    class="settings-row__copy"
                    :for="`setting-${field.key}`"
                  >
                    <span class="settings-row__label">
                      {{ fieldLabel(field) }}
                      <StatusBadge v-if="field.recommended" tone="success" compact>
                        {{ t('ui.settings.recommended') }}
                      </StatusBadge>
                      <StatusBadge v-if="field.restartRequired" tone="warning" compact>
                        {{ t('ui.settings.restart') }}
                      </StatusBadge>
                    </span>
                    <span v-if="fieldDescription(field)" class="settings-row__description">
                      {{ fieldDescription(field) }}
                    </span>
                    <span
                      v-if="field.warningKey && Number(values[field.key]) > 0"
                      class="settings-row__warning"
                    >
                      {{ t(field.warningKey) }}
                    </span>
                    <span v-if="dependencyHint(field)" class="settings-row__dependency">
                      {{ dependencyHint(field) }}
                    </span>
                  </label>

                  <label v-if="field.kind === 'boolean'" class="vs-switch">
                    <input
                      :id="`setting-${field.key}`"
                      type="checkbox"
                      :checked="isTrue(values[field.key])"
                      @change="updateBoolean(field.key, $event)"
                    />
                    <span class="vs-switch__track" aria-hidden="true" />
                    <span class="visually-hidden">{{ fieldLabel(field) }}</span>
                  </label>

                  <select
                    v-else-if="field.kind === 'select' || field.kind === 'duration'"
                    :id="`setting-${field.key}`"
                    class="vs-select"
                    :value="controlValue(field)"
                    :title="optionLabel(field.key, '')"
                    @change="updateValue(field.key, $event, field)"
                  >
                    <option
                      v-for="option in optionsFor(field)"
                      :key="option.value"
                      :value="option.value"
                    >
                      {{ optionText(option, field.key) }}
                    </option>
                  </select>

                  <textarea
                    v-else-if="field.kind === 'textarea'"
                    :id="`setting-${field.key}`"
                    :class="['vs-textarea', { monospace: field.monospace }]"
                    :value="String(values[field.key] ?? '')"
                    :placeholder="field.placeholderKey ? t(field.placeholderKey) : undefined"
                    rows="4"
                    @input="updateValue(field.key, $event, field)"
                  />

                  <DisplayModeOverrides
                    v-else-if="field.kind === 'mode-remapping'"
                    :model-value="values[field.key]"
                    :resolution-mode="String(values.dd_resolution_option ?? 'auto')"
                    :refresh-mode="String(values.dd_refresh_rate_option ?? 'auto')"
                    :simple="field.simple"
                    @update:model-value="values[field.key] = $event"
                    @validity-change="displayOverridesValid = $event"
                  />

                  <DisplayRecoverySettings
                    v-else-if="field.kind === 'display-recovery'"
                    :hotkey="values.dd_snapshot_restore_hotkey"
                    :modifiers="values.dd_snapshot_restore_hotkey_modifiers"
                    :prefer-golden="values.dd_always_restore_from_golden"
                    @update:hotkey="values.dd_snapshot_restore_hotkey = $event"
                    @update:modifiers="values.dd_snapshot_restore_hotkey_modifiers = $event"
                    @update:prefer-golden="values.dd_always_restore_from_golden = $event"
                  />

                  <GlobalPrepCommands
                    v-else-if="field.kind === 'command-preparations'"
                    :model-value="values[field.key]"
                    :platform="hostPlatform"
                    @update:model-value="values[field.key] = $event"
                  />

                  <SettingsIntegrationPath
                    v-else-if="field.kind === 'integration-path'"
                    :kind="field.integration ?? 'rtss'"
                    :input-id="`setting-${field.key}`"
                    :model-value="values[field.key]"
                    @update:model-value="values[field.key] = $event"
                  />

                  <input
                    v-else
                    :id="`setting-${field.key}`"
                    :class="['vs-input', { monospace: field.monospace }]"
                    :type="field.kind === 'number' ? 'number' : 'text'"
                    :min="field.min"
                    :max="field.max"
                    :step="field.step"
                    :value="String(values[field.key] ?? '')"
                    :placeholder="field.placeholderKey ? t(field.placeholderKey) : undefined"
                    @input="updateValue(field.key, $event, field)"
                  />
                </div>
              </div>
              <div
                v-if="
                  activeCategory === 'everyday' &&
                  !isSearching &&
                  group.id === 'everyday_display' &&
                  physicalDisplaySelected
                "
                class="settings-physical-display"
              >
                <div class="settings-physical-display__heading">
                  <h4>{{ groupTitle('display_target') }}</h4>
                  <p>{{ groupDescription('display_target') }}</p>
                </div>
                <div class="settings-group">
                  <div class="settings-row settings-physical-display__row">
                    <label class="settings-row__copy" for="setting-output_name">
                      <span class="settings-row__label">{{ t('config.output_name') }}</span>
                      <span class="settings-row__description">
                        {{ physicalDisplayDescription }}
                      </span>
                    </label>
                    <div class="settings-physical-display__control">
                      <select
                        v-if="isWindowsHost"
                        id="setting-output_name"
                        class="vs-select"
                        :value="String(values.output_name ?? '')"
                        @focus="loadDisplayDevices()"
                        @change="updateValue('output_name', $event)"
                      >
                        <option value="">{{ t('config.output_name_default') }}</option>
                        <option v-if="displayDevicesLoading" disabled value="__loading">
                          {{ t('_common.loading') }}
                        </option>
                        <option
                          v-if="displayDevicesLoaded && !displayDeviceOptions.length"
                          disabled
                          value="__empty"
                        >
                          {{ t('ui.devices.editor.no_displays') }}
                        </option>
                        <option
                          v-for="option in displayDeviceOptions"
                          :key="option.value"
                          :value="option.value"
                        >
                          {{ option.label }}
                        </option>
                      </select>
                      <input
                        v-else
                        id="setting-output_name"
                        class="vs-input monospace"
                        type="text"
                        :value="String(values.output_name ?? '')"
                        @input="updateValue('output_name', $event)"
                      />
                      <button
                        v-if="isWindowsHost"
                        class="button button--secondary button--compact"
                        type="button"
                        :disabled="displayDevicesLoading"
                        :aria-label="t('_common.refresh')"
                        @click="loadDisplayDevices(true)"
                      >
                        <UiIcon name="refresh" />
                        {{ t('_common.refresh') }}
                      </button>
                      <span v-if="displayDevicesError" class="settings-physical-display__error">
                        {{ displayDevicesError }}
                      </span>
                    </div>
                  </div>
                </div>
              </div>
            </component>
          </section>

          <div v-if="filteredGroups.length === 0" class="settings-empty">
            {{ t('ui.settings.no_results', { query: search }) }}
          </div>
        </template>

        <section
          v-if="activeCategory === 'display' && !isSearching"
          class="danger-zone"
          aria-labelledby="display-recovery-title"
        >
          <div>
            <h2 id="display-recovery-title">{{ t('ui.settings.display_recovery.title') }}</h2>
            <p>{{ t('ui.settings.display_recovery.description') }}</p>
          </div>
          <button class="button button--danger-text" type="button" @click="resetDisplayPersistence">
            {{ t('ui.settings.display_recovery.action') }}
          </button>
        </section>
      </div>
    </div>

    <div
      v-if="isDirty"
      class="save-bar"
      role="region"
      :aria-label="t('ui.settings.unsaved_region')"
    >
      <div>
        <strong>
          {{
            t(dirtyKeys.length === 1 ? 'ui.settings.unsaved_one' : 'ui.settings.unsaved_many', {
              count: dirtyKeys.length,
            })
          }}
        </strong>
        <span>
          {{ t(restartPending ? 'ui.settings.save_restart_hint' : 'ui.settings.save_hint') }}
        </span>
      </div>
      <div class="save-bar__actions">
        <button class="button button--secondary" type="button" :disabled="saving" @click="discard">
          {{ t('ui.settings.discard') }}
        </button>
        <button
          class="button button--primary"
          type="button"
          :disabled="saving || !saveAllowed"
          @click="save"
        >
          <UiIcon name="check" />
          {{ t(saving ? 'ui.settings.saving' : 'ui.settings.save') }}
        </button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.settings-page {
  padding-bottom: var(--vs-space-80);
}

.settings-tools,
.settings-layout,
.settings-row,
.save-bar,
.save-bar__actions,
.danger-zone {
  display: flex;
}

.settings-tools {
  align-items: center;
  gap: var(--vs-space-12);
  margin: var(--vs-space-24) 0;
}

.search-field {
  position: relative;
  display: block;
  width: min(100%, 480px);
}

.search-field > svg {
  position: absolute;
  z-index: 1;
  top: 50%;
  left: var(--vs-space-12);
  color: var(--vs-color-text-muted);
  transform: translateY(-50%);
  pointer-events: none;
}

.search-field .vs-input {
  padding-left: var(--vs-space-40);
}

.settings-layout {
  align-items: flex-start;
  gap: var(--vs-space-24);
}

.settings-nav {
  position: sticky;
  top: var(--vs-space-24);
  display: grid;
  width: 176px;
  flex: 0 0 176px;
  gap: var(--vs-space-2);
}

.settings-nav button {
  min-height: 36px;
  padding: 0 var(--vs-space-12);
  border: 0;
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-text-secondary);
  text-align: left;
  background: transparent;
}

.settings-nav button:hover,
.settings-nav__item--active {
  color: var(--vs-color-text-primary) !important;
  background: var(--vs-color-bg-subtle) !important;
}

.settings-content {
  min-width: 0;
  flex: 1;
}

.settings-category-heading {
  margin-bottom: var(--vs-space-20);
}

.settings-category-heading > span,
.settings-section__heading > span {
  color: var(--vs-color-accent-primary);
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
}

.settings-category-heading h2 {
  margin: var(--vs-space-4) 0 0;
  font-size: 24px;
  line-height: 32px;
}

.settings-category-heading p {
  max-width: 680px;
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-secondary);
}

.settings-summary {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  margin-bottom: var(--vs-space-32);
  border-block: 1px solid var(--vs-color-border-subtle);
}

.settings-summary > div {
  min-width: 0;
  padding: var(--vs-space-12) var(--vs-space-16);
}

.settings-summary > div + div {
  border-left: 1px solid var(--vs-color-border-subtle);
}

.settings-summary span,
.settings-summary strong {
  display: block;
}

.settings-summary span {
  color: var(--vs-color-text-muted);
  font-size: 12px;
}

.settings-summary strong {
  margin-top: var(--vs-space-2);
  color: var(--vs-color-text-primary);
  line-height: 18px;
}

.settings-section + .settings-section,
.danger-zone {
  margin-top: var(--vs-space-24);
}

.settings-section__heading {
  margin-bottom: var(--vs-space-12);
}

.settings-section__heading h2,
.settings-section__heading h3,
.danger-zone h2 {
  margin: 0;
  font-size: 18px;
  line-height: 24px;
}

.settings-section__heading p,
.danger-zone p {
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-secondary);
}

.settings-disclosure > summary {
  position: relative;
  padding: var(--vs-space-16) var(--vs-space-48) var(--vs-space-16) var(--vs-space-20);
  margin-bottom: 0;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
  cursor: pointer;
  list-style: none;
}

.settings-disclosure > summary::-webkit-details-marker {
  display: none;
}

.settings-disclosure > summary::after {
  position: absolute;
  top: 50%;
  right: var(--vs-space-20);
  width: 8px;
  height: 8px;
  border-right: 2px solid var(--vs-color-text-secondary);
  border-bottom: 2px solid var(--vs-color-text-secondary);
  content: '';
  transform: translateY(-65%) rotate(45deg);
  transition: transform 120ms ease;
}

.settings-disclosure[open] > summary {
  border-radius: var(--vs-radius-card) var(--vs-radius-card) 0 0;
}

.settings-disclosure[open] > summary::after {
  transform: translateY(-35%) rotate(225deg);
}

.settings-disclosure > .settings-group {
  border-top: 0;
  border-radius: 0 0 var(--vs-radius-card) var(--vs-radius-card);
}

.settings-group {
  overflow: hidden;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.settings-row {
  display: grid;
  min-height: var(--vs-size-row-settings);
  grid-template-columns: minmax(240px, 1fr) minmax(300px, 420px);
  align-items: center;
  gap: var(--vs-space-24);
  padding: var(--vs-space-16) var(--vs-space-20);
}

.settings-row + .settings-row {
  border-top: 1px solid var(--vs-color-border-subtle);
}

.settings-row--stacked {
  grid-template-columns: minmax(0, 1fr);
  align-items: stretch;
  gap: var(--vs-space-12);
}

.settings-row--recovery {
  padding: 0;
}

.settings-row__copy {
  min-width: 0;
}

.settings-row__label {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  column-gap: var(--vs-space-8);
  row-gap: var(--vs-space-4);
  color: var(--vs-color-text-primary);
  font-weight: 600;
}

.settings-row__description,
.settings-row__warning,
.settings-row__dependency,
.settings-row code {
  display: block;
  margin-top: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: 13px;
  line-height: 18px;
}

.settings-row__dependency {
  color: var(--vs-color-status-warning);
}

.settings-row__warning {
  display: block;
  margin-top: var(--vs-space-8);
  color: var(--vs-color-status-warning);
  font-size: 13px;
  line-height: 18px;
}

.settings-row input:not([type='checkbox']),
.settings-row select,
.settings-row textarea {
  width: 100%;
  min-width: 0;
}

.settings-row--stacked input:not([type='checkbox']),
.settings-row--stacked select,
.settings-row--stacked textarea {
  width: 100%;
  min-width: 0;
}

.settings-row > .vs-switch {
  justify-self: end;
}

.settings-physical-display {
  margin-top: var(--vs-space-16);
}

.settings-physical-display__heading {
  margin-bottom: var(--vs-space-12);
}

.settings-physical-display__heading h4 {
  margin: 0;
  color: var(--vs-color-text-primary);
  font-size: 16px;
  line-height: 22px;
}

.settings-physical-display__heading p {
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-secondary);
}

.settings-physical-display__control {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: start;
  gap: var(--vs-space-8);
}

.settings-physical-display__control > .vs-input,
.settings-physical-display__control > .vs-select {
  min-width: 0;
}

.settings-physical-display__error {
  grid-column: 1 / -1;
  color: var(--vs-color-status-danger);
  font-size: 13px;
  line-height: 18px;
}

.settings-group--advanced {
  margin-top: var(--vs-space-16);
}

.settings-empty {
  padding: var(--vs-space-24);
  margin: 0;
  color: var(--vs-color-text-secondary);
  text-align: center;
}

.danger-zone {
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-24);
  padding: var(--vs-space-20);
  border: 1px solid var(--vs-color-status-danger);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.save-bar {
  position: fixed;
  z-index: 15;
  inset: auto var(--vs-space-32) var(--vs-space-24)
    calc(var(--vs-navigation-width-expanded) + var(--vs-space-32));
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-24);
  max-width: 960px;
  padding: var(--vs-space-12) var(--vs-space-16);
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-raised);
  box-shadow: var(--vs-shadow-overlay);
}

.save-bar strong,
.save-bar span {
  display: block;
}

.save-bar span {
  color: var(--vs-color-text-secondary);
  font-size: 12px;
}

.save-bar__actions {
  gap: var(--vs-space-8);
}

@media (max-width: 1023px) {
  .save-bar {
    left: calc(var(--vs-navigation-width-collapsed) + var(--vs-space-24));
  }
}

@media (max-width: 899px) {
  .settings-row {
    grid-template-columns: minmax(0, 1fr);
    align-items: stretch;
    gap: var(--vs-space-12);
  }

  .settings-row > .vs-switch {
    justify-self: start;
  }
}

@media (max-width: 767px) {
  .settings-tools,
  .settings-layout,
  .danger-zone,
  .save-bar {
    align-items: stretch;
    flex-direction: column;
  }

  .settings-nav {
    position: static;
    display: flex;
    overflow-x: auto;
    width: 100%;
    flex-basis: auto;
    padding-bottom: var(--vs-space-4);
  }

  .settings-nav button {
    flex: 0 0 auto;
    white-space: nowrap;
  }

  .settings-summary {
    grid-template-columns: 1fr;
  }

  .settings-summary > div + div {
    border-top: 1px solid var(--vs-color-border-subtle);
    border-left: 0;
  }

  .settings-row {
    align-items: stretch;
    flex-direction: column;
    gap: var(--vs-space-12);
  }

  .settings-row input:not([type='checkbox']),
  .settings-row select,
  .settings-row textarea {
    width: 100%;
  }

  .save-bar {
    inset: auto 0 0;
    border-right: 0;
    border-bottom: 0;
    border-left: 0;
    border-radius: 0;
    padding-bottom: calc(var(--vs-space-12) + env(safe-area-inset-bottom));
  }

  .save-bar__actions > * {
    flex: 1;
  }
}
</style>
