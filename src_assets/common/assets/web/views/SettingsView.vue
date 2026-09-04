<script setup lang="ts">
import {
  providerSupported,
  supportsManagedLinuxDisplay,
  settingsCapabilitySupported,
} from '@/utils/providerCapabilities';
import { computed, nextTick, onMounted, onBeforeUnmount, reactive, ref, toRaw, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute, useRouter, onBeforeRouteLeave } from 'vue-router';
import { useSystemStore, type HostMetadata } from '@/stores/system';
import LinuxCaptureStatus from '@/components/settings/LinuxCaptureStatus.vue';
import { acknowledgeSettings, configBoolean, settingError } from '@/utils/settings';

import { ApiError, apiGet, apiPatch, apiPost } from '@/api/client';
import DisplayModeOverrides from '@/components/settings/DisplayModeOverrides.vue';
import DisplayRecoverySettings from '@/components/settings/DisplayRecoverySettings.vue';
import GlobalPrepCommands from '@/components/settings/GlobalPrepCommands.vue';
import ServerCommands from '@/components/settings/ServerCommands.vue';
import SettingsIntegrationPath from '@/components/settings/SettingsIntegrationPath.vue';
import {
  SettingRow,
  ConfirmDialog,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
  UiIcon,
} from '@/components/ui';
import {
  fieldForPlatform,
  matchesPlatform,
  encoderFamilyFor,
  optionsForPlatform,
  settingsFields,
  settingsDestinations,
  captureOptionsForPlatform,
  frameGenerationOptionsForPlatform,
  restartRequiredKeys,
  settingsCategories,
  settingsDefaults,
  type SettingsField,
  type SettingsGroup,
  type SettingsOption,
  type SettingsVisibility,
} from '@/configs/settingsSchema';
import { serializeCommandRows, serializeServerCommandRows } from '@/utils/v2Parity';

const { locale, t, te } = useI18n();
const system = useSystemStore();
const route = useRoute();
const router = useRouter();
const confirmation = ref<'restart' | 'reset' | null>(null);
const resetting = ref(false);
const form = ref<HTMLFormElement | null>(null);

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

type MetadataResponse = HostMetadata;

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
const configLoaded = ref(false);
const saving = ref(false);
const restarting = ref(false);
const restartAvailable = computed({
  get: () => system.restartRequired,
  set: (value: boolean) => {
    system.restartRequired = value;
  },
});
const displayOverridesValid = ref(true);
const error = ref('');
const notice = ref('');
const search = ref(typeof route.query.q === 'string' ? route.query.q : '');
const activeCategory = ref(
  settingsCategories.some((item) => item.id === route.query.category)
    ? String(route.query.category)
    : settingsCategories[0].id,
);
const hostMetadata = ref<MetadataResponse>({});
const values = reactive<Record<string, unknown>>({});
const original = ref<Record<string, unknown>>({});
const displayDevices = ref<DisplayDevice[]>([]);
const displayDevicesLoading = ref(false);
const displayDevicesLoaded = ref(false);
const displayDevicesError = ref('');
const metadataUnavailable = ref(false);

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
    ? encoderFamilyFor(configuredEncoder)
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
  if (!isWindowsHost.value) return t('ui.settings.options.encoder.auto');
  const family = preferredAutomaticEncoderFamily.value;
  const gpuName = preferredGpu.value?.description?.trim() ?? '';
  const encoderKey =
    family === 'nvidia'
      ? 'ui.settings.options.encoder.nvenc'
      : family === 'intel'
        ? 'ui.settings.options.encoder.quicksync'
        : family === 'amd'
          ? 'ui.settings.options.encoder.amdvce_ffmpeg'
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
const isLinuxHost = computed(() =>
  String(hostMetadata.value.platform ?? '')
    .toLocaleLowerCase()
    .includes('linux'),
);
const virtualDisplayUnavailable = computed(
  () =>
    isLinuxHost.value &&
    (hostMetadata.value.virtual_display?.capable === false ||
      hostMetadata.value.virtual_display?.ready === false),
);
const supportsDisplayDeviceEnumeration = computed(
  () => isWindowsHost.value || supportsManagedLinuxDisplay(hostMetadata.value),
);

const physicalDisplaySelected = computed(
  () => String(values.virtual_display_mode ?? '') === 'disabled',
);

const hostPlatform = computed(() => String(hostMetadata.value.platform ?? ''));

const physicalDisplayDescription = computed(() =>
  t(
    isWindowsHost.value
      ? 'config.output_name_desc_windows'
      : isLinuxHost.value
        ? 'config.output_name_desc_linux'
        : 'config.output_name_desc_unix',
  ),
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
  () =>
    configLoaded.value &&
    (displayOverridesValid.value || !dirtyKeys.value.includes('dd_mode_remapping')),
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
      .filter(
        (group) => matchesPlatform(group, hostPlatform.value) && (query || groupIsVisible(group)),
      )
      .map<DisplaySettingsGroup>((group) => ({
        ...group,
        categoryId: settingsCategory.id,
        fields: group.fields
          .map((field) => fieldForPlatform(field, hostPlatform.value))
          .filter((field) => {
            const matches =
              !query ||
              `${categoryLabel(settingsCategory.id)} ${groupTitle(group.id)} ${fieldLabel(field)} ${fieldDescription(field)} ${field.key}`
                .toLocaleLowerCase(locale.value)
                .includes(query);
            const matchesEncoder =
              !field.encoderFamily ||
              !effectiveEncoderFamily.value ||
              field.encoderFamily === effectiveEncoderFamily.value ||
              Boolean(query) ||
              route.hash === `#setting-${field.key}`;
            if (
              !matches ||
              !fieldMatchesPlatform(field) ||
              !matchesEncoder ||
              (!query && route.hash !== `#setting-${field.key}` && !fieldIsVisible(field))
            ) {
              return false;
            }
            if (seenKeys.has(field.key)) return false;
            seenKeys.add(field.key);
            return true;
          }),
      }))
      .filter((group) => group.fields.length),
  );
});

const destinationResults = computed(() => {
  const q = search.value.trim().toLocaleLowerCase(locale.value);
  if (!q) return [];
  return settingsDestinations.filter(
    (item) =>
      matchesPlatform(item, hostPlatform.value) &&
      (!item.to.includes('#integration-') ||
        providerSupported(hostMetadata.value, item.to.split('#integration-')[1])) &&
      `${t(item.labelKey)} ${item.keys.join(' ')}`.toLocaleLowerCase(locale.value).includes(q),
  );
});

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
  return (
    matchesPlatform(field, hostPlatform.value) &&
    settingsCapabilitySupported(field.key, hostMetadata.value)
  );
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
    (!field.encoderFamily ||
      !effectiveEncoderFamily.value ||
      field.encoderFamily === effectiveEncoderFamily.value)
  );
}

function fieldIsInactive(field: SettingsField): boolean {
  return Boolean(isSearching.value && field.visibleWhen && !visibilityMatches(field.visibleWhen));
}

function fieldByKey(key: string): SettingsField | undefined {
  return settingsFields.get(key);
}

function categoryLabel(id: string): string {
  return t(`ui.settings.categories.${id}.label`);
}

function groupTitle(id: string): string {
  return t(`ui.settings.groups.${id}.title`);
}

function groupDescription(id: string): string {
  const platform = String(hostMetadata.value.platform ?? '').toLocaleLowerCase();
  const candidates = [
    platform.includes('windows') ? `ui.settings.groups.${id}.description_windows` : '',
    platform.includes('linux') ? `ui.settings.groups.${id}.description_linux` : '',
    platform.includes('mac') ? `ui.settings.groups.${id}.description_macos` : '',
    `ui.settings.groups.${id}.description`,
  ].filter(Boolean);
  const key = candidates.find((candidate) => messageExists(candidate));
  return key ? t(key) : '';
}

function fieldLabel(field: SettingsField): string {
  const configKey = `config.${field.key}`;
  const key =
    field.labelKey ??
    (messageExists(configKey) ? configKey : `ui.settings.fields.${field.key}.label`);
  return messageExists(key) ? t(key) : field.key.replaceAll('_', ' ');
}

function fieldDescription(field: SettingsField): string {
  const linuxKey = `ui.settings.linux.fields.${field.key}`;
  if (isLinuxHost.value && messageExists(linuxKey)) return t(linuxKey);
  if (field.descriptionKey) return t(field.descriptionKey);
  const platform = String(hostMetadata.value.platform ?? '').toLocaleLowerCase();
  const candidates = [
    platform.includes('windows') ? `config.${field.key}_desc_windows` : '',
    platform.includes('linux') ? `config.${field.key}_desc_linux` : '',
    platform.includes('mac') ? `config.${field.key}_desc_macos` : '',
    platform.includes('windows') ? `ui.settings.fields.${field.key}.description_windows` : '',
    platform.includes('linux') ? `ui.settings.fields.${field.key}.description_linux` : '',
    platform.includes('mac') ? `ui.settings.fields.${field.key}.description_macos` : '',
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
  return messageExists(option.labelKey)
    ? t(option.labelKey, { name: gpu?.adapterName ?? '', value: option.value })
    : option.labelKey;
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
  let options = optionsForPlatform(field, platform);

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

function fieldWarningIsVisible(field: SettingsField): boolean {
  if (!field.warningKey) return false;
  return field.kind === 'boolean' ? isTrue(values[field.key]) : Number(values[field.key]) > 0;
}

function fieldDescriptionIds(field: SettingsField): string | undefined {
  const ids = [
    fieldDescription(field) ? `setting-${field.key}-description` : '',
    fieldWarningIsVisible(field) ? `setting-${field.key}-warning` : '',
    dependencyHint(field) ? `setting-${field.key}-dependency` : '',
  ].filter(Boolean);
  return ids.length ? ids.join(' ') : undefined;
}

function selectCategory(id: string): void {
  activeCategory.value = id;
  search.value = '';
  void router.push({ query: { category: id }, hash: '' });
}

function updateCategory(event: Event): void {
  selectCategory((event.target as HTMLSelectElement).value);
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

function saveValue(key: string): unknown {
  const value = values[key];
  if (key === 'global_prep_cmd' || key === 'global_state_cmd') {
    return serializeCommandRows(value, hostPlatform.value).filter(
      (row) => row.do.trim() || row.undo.trim(),
    );
  }
  if (key === 'server_cmd') {
    return serializeServerCommandRows(value, hostPlatform.value).filter(
      (row) => row.name.trim() && row.cmd.trim(),
    );
  }
  if (
    ['keybindings', 'dd_snapshot_exclude_devices'].includes(key) &&
    typeof value === 'string' &&
    value.trim()
  )
    return JSON.parse(value);
  return value === '' ? null : value;
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
  metadataUnavailable.value = false;
  try {
    const [response, metadata] = await Promise.all([
      apiGet<ConfigResponse>('/api/config'),
      apiGet<MetadataResponse>('/api/metadata').catch((): MetadataResponse => {
        metadataUnavailable.value = true;
        return {};
      }),
    ]);
    if (response.status === false) throw new Error('config-load-rejected');
    hostMetadata.value = metadata;
    system.metadata = metadata;
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
    if (
      metadata.platform === 'linux' &&
      (metadata.virtual_display?.capable === false || metadata.virtual_display?.ready === false) &&
      configured.virtual_display_mode === undefined
    ) {
      defaults.virtual_display_mode = 'disabled';
    }
    if (metadata.prerelease) defaults.min_log_level = 1;

    const normalized = { ...defaults, ...configured };
    for (const [key, field] of settingsFields)
      if (field.kind === 'boolean' && key in normalized)
        normalized[key] = configBoolean(normalized[key]);
    Object.keys(values).forEach((key) => delete values[key]);
    Object.assign(values, normalized);
    original.value = cloneSettings(normalized);
    configLoaded.value = true;
  } catch {
    error.value = t('ui.settings.errors.load');
  } finally {
    loading.value = false;
    void focusLinkedField();
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
  if (!isDirty.value || saving.value || !saveAllowed.value || !form.value?.reportValidity()) return;
  const invalid = dirtyKeys.value
    .map((key) => ({ key, error: settingError(settingsFields.get(key), values[key]) }))
    .find((item) => item.error);
  if (invalid) {
    error.value = `${fieldLabel(settingsFields.get(invalid.key)!)}: ${t(invalid.error!)}`;
    return;
  }
  saving.value = true;
  error.value = '';
  notice.value = '';
  try {
    const submitted = Object.fromEntries(
      dirtyKeys.value.map((key) => [key, cloneValueForSave(values[key])]),
    );
    const patch = Object.fromEntries(Object.keys(submitted).map((key) => [key, saveValue(key)]));
    const result = await apiPatch<SaveResult>('/api/config', patch);
    if (result.status === false) throw new Error('save-rejected');
    original.value = acknowledgeSettings(original.value, submitted);
    restartAvailable.value ||= Boolean(result.restartRequired);
    notice.value = restartAvailable.value
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
}

async function restart(): Promise<void> {
  if (restarting.value) return;
  restarting.value = true;
  notice.value = t('ui.settings.notices.restarting');
  try {
    await apiPost('/api/restart');
  } catch (cause) {
    if (cause instanceof ApiError) {
      error.value = t('ui.maintenance.errors.actionFailed');
      restarting.value = false;
      return;
    }
  }
  window.setTimeout(() => window.location.reload(), 3500);
}

async function resetDisplayPersistence(): Promise<void> {
  if (resetting.value) return;
  resetting.value = true;
  error.value = '';
  notice.value = '';
  try {
    const result = await apiPost<{ status?: boolean }>('/api/reset-display-device-persistence');
    if (result.status === false) throw new Error('reset-rejected');
    notice.value = t('ui.settings.notices.display_state_cleared');
  } catch {
    error.value = t('ui.settings.errors.reset_display');
  } finally {
    resetting.value = false;
  }
}

function cloneValueForSave(value: unknown): unknown {
  return value === undefined ? undefined : structuredClone(toRaw(value));
}

async function focusLinkedField(): Promise<void> {
  await nextTick();
  let id: string;
  try {
    id = decodeURIComponent(route.hash.slice(1));
  } catch {
    return;
  }
  if (!id.startsWith('setting-')) return;
  const element = document.getElementById(id);
  for (let parent = element?.parentElement; parent; parent = parent.parentElement) {
    if (parent instanceof HTMLDetailsElement) parent.open = true;
  }
  await nextTick();
  element?.scrollIntoView({ block: 'center' });
  element?.focus({ preventScroll: true });
}
watch(search, (q) => {
  if (q === (route.query.q ?? '')) return;
  void router.replace({ query: { ...route.query, q: q || undefined } });
});
watch(
  () => route.fullPath,
  () => {
    if (settingsCategories.some((item) => item.id === route.query.category))
      activeCategory.value = String(route.query.category);
    search.value = typeof route.query.q === 'string' ? route.query.q : '';
    void focusLinkedField();
  },
);
function beforeUnload(event: BeforeUnloadEvent): void {
  if (isDirty.value || saving.value) {
    event.preventDefault();
    event.returnValue = '';
  }
}
onBeforeRouteLeave(
  () => (!isDirty.value && !saving.value) || window.confirm(t('ui.settings.leave_warning')),
);
onMounted(() => {
  void load();
  window.addEventListener('beforeunload', beforeUnload);
});
onBeforeUnmount(() => window.removeEventListener('beforeunload', beforeUnload));
</script>

<template>
  <form
    ref="form"
    class="page settings-page"
    :class="{ 'settings-page--dirty': isDirty }"
    @submit.prevent="save"
  >
    <PageHeader :title="t('ui.settings.title')" :description="t('ui.settings.description')">
      <template #actions>
        <button
          class="button button--secondary"
          type="button"
          :disabled="loading || saving || isDirty"
          @click="load"
        >
          <UiIcon name="refresh" />
          {{ t('ui.settings.reload') }}
        </button>
      </template>
    </PageHeader>

    <InlineAlert
      v-if="error"
      tone="danger"
      announce="assertive"
      :title="t('ui.settings.errors.title')"
    >
      {{ error }}
    </InlineAlert>
    <InlineAlert
      v-else-if="notice || restartAvailable"
      tone="success"
      announce="polite"
      :title="t('ui.settings.notices.title')"
    >
      {{ notice || t('ui.settings.notices.saved_restart') }}
      <template v-if="restartAvailable" #actions>
        <button
          class="button button--secondary button--compact"
          type="button"
          :disabled="restarting"
          :aria-busy="restarting"
          @click="confirmation = 'restart'"
        >
          {{ t(restarting ? 'ui.settings.notices.restarting' : 'ui.settings.restart_now') }}
        </button>
      </template>
    </InlineAlert>
    <InlineAlert
      v-else-if="restartPending"
      tone="warning"
      announce="polite"
      :title="t('ui.settings.restart_required')"
    >
      {{ t('ui.settings.restart_required_description') }}
    </InlineAlert>
    <InlineAlert
      v-if="metadataUnavailable && !error"
      tone="warning"
      announce="polite"
      :title="t('ui.settings.metadata_unavailable.title')"
    >
      {{ t('ui.settings.metadata_unavailable.description') }}
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
          @keydown.enter.prevent
        />
      </label>
      <StatusBadge v-if="isDirty" tone="warning">
        {{ t('ui.settings.unsaved_short', { count: dirtyKeys.length }) }}
      </StatusBadge>
    </div>

    <div class="settings-layout">
      <label class="settings-category-picker">
        <span>{{ t('ui.settings.categories_label') }}</span>
        <select
          class="vs-select"
          :value="isSearching ? '' : activeCategory"
          @change="updateCategory"
        >
          <option v-if="isSearching" disabled value="">
            {{ t('ui.settings.search_results') }}
          </option>
          <option v-for="item in settingsCategories" :key="item.id" :value="item.id">
            {{ categoryLabel(item.id) }}
          </option>
        </select>
      </label>

      <nav class="settings-nav" :aria-label="t('ui.settings.categories_label')">
        <button
          v-for="item in settingsCategories"
          :key="item.id"
          type="button"
          :class="{
            'settings-nav__item--active': !isSearching && activeCategory === item.id,
          }"
          :aria-current="!isSearching && activeCategory === item.id ? 'page' : undefined"
          @click="selectCategory(item.id)"
        >
          {{ categoryLabel(item.id) }}
        </button>
      </nav>

      <div class="settings-content">
        <div
          v-if="loading"
          class="settings-group vs-settings-group"
          :aria-label="t('ui.settings.loading')"
        >
          <LoadingSkeleton v-for="index in 6" :key="index" height="64px" />
        </div>

        <template v-else-if="configLoaded">
          <header class="settings-category-heading" aria-live="polite" aria-atomic="true">
            <h2>
              {{ isSearching ? t('ui.settings.search_results') : categoryLabel(category.id) }}
            </h2>
            <p>{{ categoryDescription }}</p>
          </header>

          <nav
            v-if="destinationResults.length"
            class="settings-destinations"
            :aria-label="t('ui.settings.more_options')"
          >
            <RouterLink v-for="item in destinationResults" :key="item.to" :to="item.to"
              >{{ t(item.labelKey) }} →</RouterLink
            >
          </nav>
          <LinuxCaptureStatus
            v-if="
              isLinuxHost &&
              supportsManagedLinuxDisplay(hostMetadata) &&
              !isSearching &&
              ['everyday', 'display'].includes(activeCategory)
            "
            :metadata="hostMetadata"
            :virtual-mode="String(values.virtual_display_mode ?? '')"
          />

          <section
            v-for="group in filteredGroups"
            :key="`${group.categoryId}-${group.id}`"
            :id="`settings-group-${group.categoryId}-${group.id}`"
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
              <InlineAlert
                v-if="
                  !isSearching &&
                  virtualDisplayUnavailable &&
                  (group.id === 'everyday_display' || group.id === 'display_virtual')
                "
                class="settings-section__alert"
                tone="warning"
                announce="polite"
                :title="t('ui.settings.virtual_display_unavailable.title')"
              >
                {{ t('ui.settings.virtual_display_unavailable.description') }}
              </InlineAlert>
              <div class="settings-group vs-settings-group">
                <SettingRow
                  v-for="field in group.fields"
                  :key="field.key"
                  :label="fieldLabel(field)"
                  :control-id="`setting-${field.key}`"
                  :stacked="field.stacked || field.kind === 'display-recovery'"
                  :disabled="fieldIsInactive(field)"
                  :restart-required="field.restartRequired"
                >
                  <template #label>
                    <span :id="`setting-${field.key}-label`">{{ fieldLabel(field) }}</span>
                  </template>
                  <template #description>
                    <span v-if="fieldDescription(field)" :id="`setting-${field.key}-description`">{{
                      fieldDescription(field)
                    }}</span>
                    <span
                      v-if="fieldWarningIsVisible(field)"
                      :id="`setting-${field.key}-warning`"
                      class="settings-row__warning"
                      >{{ t(field.warningKey ?? '') }}</span
                    >
                    <span v-if="dependencyHint(field)" :id="`setting-${field.key}-dependency`">{{
                      dependencyHint(field)
                    }}</span>
                  </template>

                  <label v-if="field.kind === 'boolean'" class="vs-switch">
                    <input
                      :id="`setting-${field.key}`"
                      type="checkbox"
                      :checked="isTrue(values[field.key])"
                      :disabled="fieldIsInactive(field)"
                      :aria-labelledby="`setting-${field.key}-label`"
                      :aria-describedby="fieldDescriptionIds(field)"
                      @change="updateBoolean(field.key, $event)"
                    />
                    <span class="vs-switch__track" aria-hidden="true" />
                  </label>

                  <select
                    v-else-if="field.kind === 'select' || field.kind === 'duration'"
                    :id="`setting-${field.key}`"
                    class="vs-select"
                    :value="controlValue(field)"
                    :title="optionLabel(field.key, '')"
                    :disabled="fieldIsInactive(field)"
                    :aria-labelledby="`setting-${field.key}-label`"
                    :aria-describedby="fieldDescriptionIds(field)"
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
                    :value="
                      typeof values[field.key] === 'object'
                        ? JSON.stringify(values[field.key], null, 2)
                        : String(values[field.key] ?? '')
                    "
                    :placeholder="field.placeholderKey ? t(field.placeholderKey) : undefined"
                    :disabled="fieldIsInactive(field)"
                    :aria-labelledby="`setting-${field.key}-label`"
                    :aria-describedby="fieldDescriptionIds(field)"
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

                  <ServerCommands
                    v-else-if="field.kind === 'server-commands'"
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
                    :value="
                      typeof values[field.key] === 'object'
                        ? JSON.stringify(values[field.key], null, 2)
                        : String(values[field.key] ?? '')
                    "
                    :placeholder="field.placeholderKey ? t(field.placeholderKey) : undefined"
                    :disabled="fieldIsInactive(field)"
                    :aria-labelledby="`setting-${field.key}-label`"
                    :aria-describedby="fieldDescriptionIds(field)"
                    @input="updateValue(field.key, $event, field)"
                  />
                </SettingRow>
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
                <div class="settings-group vs-settings-group">
                  <SettingRow
                    :label="t('config.output_name')"
                    :description="physicalDisplayDescription"
                    control-id="setting-output_name"
                    v-slot="{ labelId, descriptionId }"
                  >
                    <div class="settings-physical-display__control">
                      <select
                        v-if="supportsDisplayDeviceEnumeration && !displayDevicesError"
                        id="setting-output_name"
                        class="vs-select"
                        :value="String(values.output_name ?? '')"
                        :aria-labelledby="labelId"
                        :aria-describedby="descriptionId"
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
                        :aria-labelledby="labelId"
                        :aria-describedby="
                          displayDevicesError
                            ? `${descriptionId} setting-output_name-error`
                            : descriptionId
                        "
                        @input="updateValue('output_name', $event)"
                      />
                      <button
                        v-if="supportsDisplayDeviceEnumeration"
                        class="button button--secondary button--compact"
                        type="button"
                        :disabled="displayDevicesLoading"
                        :aria-label="t('_common.refresh')"
                        @click="loadDisplayDevices(true)"
                      >
                        <UiIcon name="refresh" />
                        {{ t('_common.refresh') }}
                      </button>
                      <span
                        v-if="displayDevicesError"
                        id="setting-output_name-error"
                        class="settings-physical-display__error"
                        role="alert"
                      >
                        {{ displayDevicesError }}
                      </span>
                    </div>
                  </SettingRow>
                </div>
              </div>
              <RouterLink v-if="group.link" class="settings-more" :to="group.link">{{
                t('ui.settings.more_options')
              }}</RouterLink>
              <p v-if="group.id === 'everyday_audio'" class="settings-more">
                {{ t(isLinuxHost ? 'ui.settings.linux.audio' : 'ui.settings.audio_summary') }}
              </p>
            </component>
          </section>

          <div
            v-if="filteredGroups.length === 0 && destinationResults.length === 0"
            class="settings-empty"
          >
            {{ t('ui.settings.no_results', { query: search }) }}
          </div>
        </template>

        <section
          v-if="supportsDisplayDeviceEnumeration && activeCategory === 'display' && !isSearching"
          class="danger-zone"
          aria-labelledby="display-recovery-title"
        >
          <div>
            <h2 id="display-recovery-title">{{ t('ui.settings.display_recovery.title') }}</h2>
            <p>{{ t('ui.settings.display_recovery.description') }}</p>
          </div>
          <button
            class="button button--danger-text"
            type="button"
            :disabled="resetting"
            @click="confirmation = 'reset'"
          >
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
      aria-live="polite"
      aria-atomic="true"
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
        <button class="button button--primary" type="submit" :disabled="saving || !saveAllowed">
          <UiIcon name="check" />
          {{ t(saving ? 'ui.settings.saving' : 'ui.settings.save') }}
        </button>
      </div>
    </div>
    <ConfirmDialog
      :open="confirmation !== null"
      :title="
        t(
          confirmation === 'restart'
            ? 'ui.maintenance.confirm.restartTitle'
            : 'ui.settings.display_recovery.title',
        )
      "
      :description="
        t(
          confirmation === 'restart'
            ? 'ui.maintenance.confirm.restartDescription'
            : 'ui.settings.reset_warning',
        )
      "
      :confirm-label="t('_common.continue')"
      tone="danger"
      @update:open="!$event && (confirmation = null)"
      @confirm="confirmation === 'restart' ? restart() : resetDisplayPersistence()"
    />
  </form>
</template>

<style scoped>
.settings-page {
  max-width: 1200px;
  padding-bottom: var(--vs-space-80);
}

.settings-page--dirty {
  padding-bottom: calc(var(--vs-space-80) + var(--vs-space-32));
}

.settings-tools,
.settings-layout,
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
  gap: var(--vs-space-4);
  padding-right: var(--vs-space-16);
  border-right: 1px solid var(--vs-color-border-subtle);
}

.settings-category-picker {
  display: none;
}

.settings-nav button {
  min-height: 40px;
  padding: var(--vs-space-8) var(--vs-space-12);
  font-size: var(--vs-type-size-metadata);
  line-height: 20px;
  border: 0;
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-text-secondary);
  text-align: left;
  background: transparent;
}

.settings-nav button:hover {
  color: var(--vs-color-text-primary);
  background: var(--vs-color-bg-subtle);
}
.settings-nav button.settings-nav__item--active {
  color: var(--vs-color-accent-default);
  background: color-mix(in srgb, var(--vs-color-accent-default) 10%, transparent);
  font-weight: var(--vs-type-weight-medium);
}

.settings-content {
  container-type: inline-size;
  min-width: 0;
  flex: 1;
}

.settings-category-heading {
  margin-bottom: var(--vs-space-20);
}

.settings-category-heading > span,
.settings-section__heading > span {
  color: var(--vs-color-accent-default);
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
}

.settings-category-heading h2 {
  margin: var(--vs-space-4) 0 0;
  font-size: var(--vs-type-size-panel);
  line-height: var(--vs-type-line-height-panel);
}

.settings-category-heading p {
  max-width: 680px;
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-secondary);
}

.settings-section + .settings-section,
.danger-zone {
  margin-top: var(--vs-space-24);
}

.settings-section__heading {
  margin-bottom: var(--vs-space-12);
}

.settings-section__alert {
  margin-bottom: var(--vs-space-12);
}

.settings-section__heading h2,
.settings-section__heading h3,
.danger-zone h2 {
  margin: 0;
  font-size: 16px;
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
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.settings-row__warning {
  display: block;
  margin-top: var(--vs-space-8);
  color: var(--vs-color-status-warning);
  font-size: 13px;
  line-height: 18px;
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
  width: 100%;
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
  bottom: var(--vs-space-24);
  right: var(--vs-space-32);
  left: calc(var(--vs-navigation-width-expanded) + var(--vs-space-32));
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-24);
  width: auto;
  padding: var(--vs-space-12) var(--vs-space-16);
  margin-inline: 0;
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-raised);
  box-shadow: var(--vs-shadow-overlay);
}

:global(.app-shell--collapsed) .save-bar {
  left: calc(var(--vs-navigation-width-collapsed) + var(--vs-space-32));
  width: auto;
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
    left: calc(var(--vs-navigation-width-collapsed) + var(--vs-space-32));
    width: auto;
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
    display: none;
  }

  .settings-category-picker {
    display: grid;
    width: 100%;
    gap: var(--vs-space-4);
  }

  .settings-category-picker > span {
    color: var(--vs-color-text-secondary);
    font-size: 13px;
    font-weight: 600;
  }

  .save-bar {
    inset: auto 0 0;
    width: auto;
    border-right: 0;
    border-bottom: 0;
    border-left: 0;
    border-radius: 0;
    padding-bottom: calc(var(--vs-space-12) + env(safe-area-inset-bottom));
    margin-inline: 0;
  }

  .settings-page--dirty {
    padding-bottom: calc(184px + env(safe-area-inset-bottom));
  }

  .save-bar__actions > * {
    flex: 1;
  }
}
.settings-more {
  display: block;
  margin: var(--vs-space-12) 0;
  color: var(--vs-color-text-secondary);
}
.settings-content
  :deep(.vs-setting-row__control > :is(input:not([type='checkbox']), select, textarea)) {
  width: 100%;
  min-width: 0;
}
.settings-content :deep(.vs-setting-row__control) {
  flex-wrap: wrap;
}
.settings-destinations {
  display: grid;
  gap: var(--vs-space-12);
  margin-block: var(--vs-space-24);
}
</style>
