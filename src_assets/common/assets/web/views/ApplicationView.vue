<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, reactive, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute, useRouter } from 'vue-router';

import { ApiError, apiGet, apiPost } from '@/api/client';
import AppEditCoverModal from '@/components/app-edit/AppEditCoverModal.vue';
import type { CoverCandidate } from '@/components/app-edit/AppEditCoverModal.types';
import {
  AppButton,
  ConfirmDialog,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  SettingRow,
  StatusBadge,
  UiIcon,
  type StatusTone,
  type UiIconName,
} from '@/components/ui';
import {
  appCoverUrl,
  appUuid,
  AppServiceError,
  deleteApp,
  fetchApps,
  saveApp,
  type AppRecord,
} from '@/services/apps';
import { searchCovers, updatePlayniteCover, uploadCover } from '@/services/covers';
import {
  settingsCategories,
  settingsDefaults,
  type SettingsField,
  type SettingsOption,
} from '@/configs/settingsSchema';

interface PrepEntry {
  do: string;
  undo: string;
  elevated: boolean;
  extras: Record<string, unknown>;
}

interface EditorForm {
  uuid: string;
  name: string;
  output: string;
  displayOutput: string;
  cmd: string;
  workingDir: string;
  imagePath: string;
  playniteIconPath: string;
  playniteId: string;
  playniteManaged: string;
  elevated: boolean;
  autoDetach: boolean;
  waitAll: boolean;
  excludeGlobalPrepCmd: boolean;
  exitTimeout: string;
  virtualScreen: boolean;
  virtualDisplayMode: string;
  virtualDisplayLayout: string;
  ddConfigurationOption: string;
  frameGenerationProvider: string;
  frameGenerationMode: string;
  gen1FramegenFix: boolean;
  gen2FramegenFix: boolean;
  frameGenLimiterFix: boolean;
  losslessScalingEnabled: boolean;
  losslessScalingFramegen: boolean;
  losslessScalingTargetFps: string;
  losslessScalingRtssLimit: string;
  losslessScalingProfile: string;
  losslessScalingLaunchDelay: string;
  rtxHdrMode: RtxHdrMode;
  rtxHdrValuesOverride: boolean;
  rtxHdrSdrBrightness: number;
  rtxHdrPeakBrightness: number;
  rtxHdrMiddleGray: number;
  rtxHdrContrast: number;
  rtxHdrSaturation: number;
  prepCmd: PrepEntry[];
  detachedText: string;
  configOverridesJson: string;
  advancedJson: string;
}

type RtxHdrMode = 'inherit' | 'enabled' | 'disabled';
type RtxHdrLiveStatus = 'idle' | 'queued' | 'applying' | 'applied' | 'error';
type RtxHdrCalibrationKey =
  | 'rtxHdrSdrBrightness'
  | 'rtxHdrPeakBrightness'
  | 'rtxHdrMiddleGray'
  | 'rtxHdrContrast'
  | 'rtxHdrSaturation';

interface RtxHdrCalibrationField {
  key: RtxHdrCalibrationKey;
  labelKey: string;
  descriptionKey: string;
  min: number;
  max: number;
  step: number;
  unit?: string;
}

interface SelectOption {
  value: string;
  label: string;
}

interface PlayniteGame {
  id: string;
  name: string;
  installed?: boolean;
}

interface PlayniteStatus {
  active?: boolean;
  installed?: boolean | null;
}

interface FrameGenConfig extends Record<string, unknown> {
  output_name?: unknown;
  virtual_display_mode?: unknown;
  capture?: unknown;
}

interface FrameGenMetadata {
  gpus?: Array<{
    description?: string;
    pnp_id?: string;
    vendor_id?: number | string;
  }>;
  has_nvidia_gpu?: boolean;
  platform?: unknown;
  windows_build_number?: unknown;
}

interface RtssStatus {
  hooks_found?: unknown;
  path_exists?: unknown;
  process_running?: unknown;
}

interface DisplayDevice {
  device_id?: unknown;
  display_name?: unknown;
  friendly_name?: unknown;
  info?: {
    active?: unknown;
    refresh_rate?: unknown;
    refreshRate?: unknown;
  };
  supported_refresh_rates?: unknown;
  supportedRefreshRates?: unknown;
}

interface EdidTarget {
  hz?: unknown;
  supported?: unknown;
}

interface EdidRefresh {
  max_timing_hz?: unknown;
  max_vertical_hz?: unknown;
  status?: unknown;
  targets?: unknown;
}

interface VirtualDisplayResolution {
  usingVirtual: boolean | null;
  output: string;
  hasAppOutput: boolean;
}

interface RtxHdrOverrideState {
  rest: Record<string, unknown>;
  mode: RtxHdrMode;
  valuesOverride: boolean;
  sdrBrightness: number;
  peakBrightness: number;
  middleGray: number;
  contrast: number;
  saturation: number;
}

type AppDisplaySelection = 'physical' | 'virtual';

type FrameGenRequirementStatus = 'pass' | 'configured' | 'warn' | 'fail' | 'unknown';

const RTX_HDR_OVERRIDE_KEYS = [
  'rtx_hdr',
  'rtx_hdr_sdr_brightness',
  'rtx_hdr_peak_brightness',
  'rtx_hdr_middle_gray',
  'rtx_hdr_contrast',
  'rtx_hdr_saturation',
] as const;
const RTX_HDR_LIVE_DEBOUNCE_MS = 200;

interface FrameGenHealth {
  checkedAt: number;
  os: {
    status: FrameGenRequirementStatus;
    message: string;
  };
  capture: {
    status: FrameGenRequirementStatus;
    message: string;
  };
  rtss: {
    status: FrameGenRequirementStatus;
    message: string;
  };
  display: {
    status: FrameGenRequirementStatus;
    label: string;
    message: string;
    usingVirtual: boolean | null;
    capabilities: Array<{
      hz: number;
      supported: boolean | null;
    }>;
  };
}

const route = useRoute();
const router = useRouter();
const { t, te } = useI18n();
const loading = ref(true);
const saving = ref(false);
const deleting = ref(false);
const loadError = ref('');
const saveError = ref('');
const initialSnapshot = ref('');
const sourceApp = ref<AppRecord | null>(null);
const commandWasArray = ref(false);
const coverFailed = ref(false);
const selectedCoverPreview = ref('');
const coverPickerOpen = ref(false);
const coverSearching = ref(false);
const coverBusy = ref(false);
const coverCandidates = ref<CoverCandidate[]>([]);
const coverError = ref('');
const coverSearchQuery = ref('');
const deleteOpen = ref(false);
const deleteError = ref('');
const errors = reactive<Record<string, string>>({});
const playnitePickerOpen = ref(false);
const playniteGames = ref<PlayniteGame[]>([]);
const playniteGamesLoaded = ref(false);
const playniteGamesLoading = ref(false);
const playniteGamesError = ref('');
const playniteGamesUnavailable = ref(false);
const playniteActiveIndex = ref(-1);
const frameGenHealth = ref<FrameGenHealth | null>(null);
const frameGenHealthError = ref('');
const frameGenHealthLoading = ref(false);
let frameGenHealthEpoch = 0;
let frameGenHealthRequest: { epoch: number; promise: Promise<void> } | null = null;
let formHydrating = false;
let formHydrationEpoch = 0;
let playniteCloseTimer: number | null = null;
const form = reactive<EditorForm>(emptyForm());
const overrideMetadata = ref<FrameGenMetadata>({});
const originalRtxHdrLiveOverrides = ref<Record<string, unknown>>({});
const liveRtxHdrStatus = ref<RtxHdrLiveStatus>('idle');
const liveRtxHdrError = ref('');
let liveRtxHdrTimer: ReturnType<typeof setTimeout> | null = null;
let liveRtxHdrSuppress = false;
let liveRtxHdrLastSentKey = '';
let liveRtxHdrQueue: Promise<void> = Promise.resolve();
const displayDevices = ref<DisplayDevice[]>([]);
const displayDevicesLoading = ref(false);
const displayDevicesLoaded = ref(false);
const displayDevicesError = ref('');

const frameGenerationModes = computed<SelectOption[]>(() => [
  { value: '', label: t('ui.application.options.hostDefault') },
  { value: 'off', label: t('ui.application.options.frameMode.off') },
  { value: 'lossless-scaling', label: t('ui.application.options.frameMode.lossless') },
  { value: 'nvidia-smooth-motion', label: t('ui.application.options.frameMode.nvidia') },
  { value: 'game-provided', label: t('ui.application.options.frameMode.game') },
]);
const virtualDisplayModes = computed<SelectOption[]>(() => [
  { value: '', label: t('ui.application.options.hostDefault') },
  { value: 'disabled', label: t('_common.disabled') },
  { value: 'per_client', label: t('ui.application.options.displayMode.perClient') },
  { value: 'shared', label: t('ui.application.options.displayMode.shared') },
]);
const virtualDisplayLayouts = computed<SelectOption[]>(() => [
  { value: '', label: t('ui.application.options.hostDefault') },
  { value: 'exclusive', label: t('ui.application.options.displayLayout.exclusive') },
  { value: 'extended', label: t('ui.application.options.displayLayout.extended') },
  { value: 'extended_primary', label: t('ui.application.options.displayLayout.extendedPrimary') },
  { value: 'extended_isolated', label: t('ui.application.options.displayLayout.extendedIsolated') },
  {
    value: 'extended_primary_isolated',
    label: t('ui.application.options.displayLayout.extendedPrimaryIsolated'),
  },
]);
const displayConfigurationOptions = computed<SelectOption[]>(() => [
  { value: '', label: t('ui.application.options.hostDefault') },
  { value: 'disabled', label: t('ui.application.options.displayAction.disabled') },
  { value: 'verify_only', label: t('ui.application.options.displayAction.verify') },
  { value: 'ensure_active', label: t('ui.application.options.displayAction.ensureActive') },
  { value: 'ensure_primary', label: t('ui.application.options.displayAction.ensurePrimary') },
  { value: 'ensure_only_display', label: t('ui.application.options.displayAction.ensureOnly') },
]);

const displaySelection = computed<AppDisplaySelection>({
  get: () => {
    const output = effectiveAppOutput();
    if (isVirtualDisplaySelection(output)) return 'virtual';
    if (output) return 'physical';
    return form.virtualScreen || virtualDisplayModeUsesVirtual(form.virtualDisplayMode) === true
      ? 'virtual'
      : 'physical';
  },
  set: (selection) => applyDisplaySelection(selection),
});

const physicalDisplayOutput = computed<string>({
  get: () => form.displayOutput.trim() || form.output.trim(),
  set: (value) => {
    const normalized = value.trim();
    form.displayOutput = normalized;
    form.output = '';
    form.virtualScreen = false;
    form.virtualDisplayMode = 'disabled';
    form.virtualDisplayLayout = '';
  },
});

const displayDeviceOptions = computed(() => {
  const seen = new Set<string>();
  const options = displayDevices.value.flatMap((device) => {
    const value = asString(device.device_id).trim() || asString(device.display_name).trim();
    if (!value || seen.has(value)) return [];
    seen.add(value);
    const friendly =
      asString(device.friendly_name).trim() ||
      asString(device.display_name).trim() ||
      t('config.app_display_physical_label');
    const active = typeof device.info?.active === 'boolean' ? device.info.active : null;
    const suffix =
      active === null
        ? ''
        : active
          ? ` (${t('config.app_display_status_active')})`
          : ` (${t('config.app_display_status_inactive')})`;
    return [{ label: `${friendly} - ${value}${suffix}`, value }];
  });
  const current = physicalDisplayOutput.value;
  if (current && !seen.has(current)) {
    options.unshift({
      label: t('ui.application.options.currentValue', { value: current }),
      value: current,
    });
  }
  return options;
});
const rtxHdrModeOptions = computed<
  Array<{ value: RtxHdrMode; label: string; description: string; icon: UiIconName }>
>(() => [
  {
    value: 'inherit',
    label: t('ui.application.rtxHdr.modes.inherit.label'),
    description: t('ui.application.rtxHdr.modes.inherit.description'),
    icon: 'settings',
  },
  {
    value: 'enabled',
    label: t('ui.application.rtxHdr.modes.enabled.label'),
    description: t('ui.application.rtxHdr.modes.enabled.description'),
    icon: 'check-circle',
  },
  {
    value: 'disabled',
    label: t('ui.application.rtxHdr.modes.disabled.label'),
    description: t('ui.application.rtxHdr.modes.disabled.description'),
    icon: 'x-circle',
  },
]);
const rtxHdrCalibrationFields = computed<RtxHdrCalibrationField[]>(() => [
  {
    key: 'rtxHdrPeakBrightness',
    labelKey: 'config.rtx_hdr_peak_brightness',
    descriptionKey: 'config.rtx_hdr_peak_brightness_desc',
    min: 400,
    max: 2000,
    step: 1,
    unit: 'nits',
  },
  {
    key: 'rtxHdrSdrBrightness',
    labelKey: 'config.rtx_hdr_sdr_brightness',
    descriptionKey: 'config.rtx_hdr_sdr_brightness_desc',
    min: 0,
    max: 100,
    step: 1,
  },
  {
    key: 'rtxHdrMiddleGray',
    labelKey: 'config.rtx_hdr_middle_gray',
    descriptionKey: 'config.rtx_hdr_middle_gray_desc',
    min: 10,
    max: 100,
    step: 1,
  },
  {
    key: 'rtxHdrContrast',
    labelKey: 'config.rtx_hdr_contrast',
    descriptionKey: 'config.rtx_hdr_contrast_desc',
    min: -100,
    max: 100,
    step: 1,
  },
  {
    key: 'rtxHdrSaturation',
    labelKey: 'config.rtx_hdr_saturation',
    descriptionKey: 'config.rtx_hdr_saturation_desc',
    min: -100,
    max: 100,
    step: 1,
  },
]);
const isPlayniteLinked = computed(() => Boolean(form.playniteId.trim()));
const filteredPlayniteGames = computed(() => {
  const query = form.name.trim().toLocaleLowerCase();
  const games = playniteGames.value.filter((game) => game.installed !== false);
  return query ? games.filter((game) => game.name.toLocaleLowerCase().includes(query)) : games;
});
const frameGenerationEnabled = computed(() => {
  if (form.frameGenerationMode === 'off') return false;
  return Boolean(form.frameGenerationMode);
});
const isWindowsHost = computed(() =>
  asString(overrideMetadata.value.platform).toLocaleLowerCase().includes('windows'),
);
const hasNvidiaGpu = computed(() => {
  if (typeof overrideMetadata.value.has_nvidia_gpu === 'boolean') {
    return overrideMetadata.value.has_nvidia_gpu;
  }
  const gpus = overrideMetadata.value.gpus ?? [];
  if (gpus.length) {
    return gpus.some((gpu) => Number(gpu.vendor_id) === 0x10de);
  }
  // Keep the feature visible if metadata was unavailable; the host still owns the
  // final capability decision and will reject unsupported runtime updates.
  return true;
});
const showRtxHdrSection = computed(() => isWindowsHost.value && hasNvidiaGpu.value);
const rtxHdrModeLabel = computed(() => {
  const option = rtxHdrModeOptions.value.find((candidate) => candidate.value === form.rtxHdrMode);
  return option?.label ?? t('ui.application.rtxHdr.modes.inherit.label');
});
const rtxHdrModeTone = computed<StatusTone>(() => {
  if (form.rtxHdrMode === 'enabled') return 'success';
  if (form.rtxHdrMode === 'disabled') return 'warning';
  return 'neutral';
});
const rtxHdrLiveStatusLabel = computed(() => {
  switch (liveRtxHdrStatus.value) {
    case 'queued':
      return t('ui.application.rtxHdr.live.queued');
    case 'applying':
      return t('ui.application.rtxHdr.live.applying');
    case 'applied':
      return t('ui.application.rtxHdr.live.applied');
    case 'error':
      return t('ui.application.rtxHdr.live.error');
    default:
      return '';
  }
});
const rtxHdrLiveStatusTone = computed<StatusTone>(() => {
  switch (liveRtxHdrStatus.value) {
    case 'queued':
    case 'applying':
      return 'info';
    case 'applied':
      return 'success';
    case 'error':
      return 'danger';
    default:
      return 'neutral';
  }
});
const frameGenHealthRows = computed(() => {
  if (!frameGenHealth.value) return [];
  return [
    {
      id: 'os',
      label: t('apps.framegen.req_os_label'),
      ...frameGenHealth.value.os,
    },
    {
      id: 'capture',
      label: t('apps.framegen.req_capture_label'),
      ...frameGenHealth.value.capture,
    },
    {
      id: 'rtss',
      label: t('apps.framegen.req_rtss_label'),
      ...frameGenHealth.value.rtss,
    },
    {
      id: 'display',
      ...frameGenHealth.value.display,
    },
  ];
});

const overrideFieldsByKey = computed(() => {
  const fields = new Map<string, SettingsField>();
  for (const category of settingsCategories) {
    for (const group of category.groups) {
      for (const field of group.fields) fields.set(field.key, field);
    }
  }
  return fields;
});
const virtualDisplayOnlyOverrideKeys = new Set([
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
function overrideVisibleForDisplay(key: string): boolean {
  return displaySelection.value !== 'physical' || !virtualDisplayOnlyOverrideKeys.has(key);
}
const overrideSearch = ref('');
const overrideAnnouncement = ref('');
const overrideKeys = computed(() =>
  Object.keys(readConfigOverrides())
    .filter((key) => key !== 'adapter_pnp_id' && overrideVisibleForDisplay(key))
    .sort(),
);
const overrideCatalogGroups = computed(() => {
  const seen = new Set<string>();
  return settingsCategories
    .map((category) => ({
      id: category.id,
      label: overrideMessageExists(`ui.settings.categories.${category.id}.label`)
        ? t(`ui.settings.categories.${category.id}.label`)
        : humanizeOverrideText(category.id),
      fields: category.groups
        .flatMap((group) => group.fields)
        .filter((field) => {
          if (
            !overrideVisibleForDisplay(field.key) ||
            field.key === 'adapter_pnp_id' ||
            seen.has(field.key)
          )
            return false;
          seen.add(field.key);
          return true;
        })
        .sort((a, b) => overrideLabel(a.key).localeCompare(overrideLabel(b.key))),
    }))
    .filter((category) => category.fields.length > 0);
});
const overrideCatalogCount = computed(() =>
  overrideCatalogGroups.value.reduce((count, category) => count + category.fields.length, 0),
);
const filteredOverrideCatalogGroups = computed(() => {
  const query = overrideSearch.value.trim().toLocaleLowerCase();
  if (!query) return overrideCatalogGroups.value;
  return overrideCatalogGroups.value
    .map((category) => ({
      ...category,
      fields: category.fields.filter((field) =>
        `${overrideLabel(field.key)} ${overrideDescription(field.key)} ${field.key}`
          .toLocaleLowerCase()
          .includes(query),
      ),
    }))
    .filter((category) => category.fields.length > 0);
});
const filteredOverrideCatalogCount = computed(() =>
  filteredOverrideCatalogGroups.value.reduce(
    (count, category) => count + category.fields.length,
    0,
  ),
);
const activeOverrideGroups = computed(() => {
  const remaining = new Set(overrideKeys.value);
  const groups = overrideCatalogGroups.value
    .map((category) => {
      const keys = category.fields.map((field) => field.key).filter((key) => remaining.delete(key));
      return { id: category.id, label: category.label, keys };
    })
    .filter((category) => category.keys.length > 0);
  if (remaining.size) {
    groups.push({
      id: 'additional',
      label: t('apps.overrides.override_editor'),
      keys: [...remaining].sort(),
    });
  }
  return groups;
});

function readConfigOverrides(): Record<string, unknown> {
  try {
    const value = JSON.parse(form.configOverridesJson || '{}') as unknown;
    return value && typeof value === 'object' && !Array.isArray(value)
      ? { ...(value as Record<string, unknown>) }
      : {};
  } catch {
    return {};
  }
}

function writeConfigOverrides(value: Record<string, unknown>): void {
  form.configOverridesJson = JSON.stringify(value, null, 2);
}

function buildRtxHdrConfigOverrides(overrides: Record<string, unknown>): Record<string, unknown> {
  const next = { ...overrides };
  for (const key of RTX_HDR_OVERRIDE_KEYS) delete next[key];

  if (form.rtxHdrMode === 'enabled') {
    next.rtx_hdr = true;
    if (form.rtxHdrValuesOverride) {
      next.rtx_hdr_sdr_brightness = form.rtxHdrSdrBrightness;
      next.rtx_hdr_peak_brightness = form.rtxHdrPeakBrightness;
      next.rtx_hdr_middle_gray = form.rtxHdrMiddleGray;
      next.rtx_hdr_contrast = form.rtxHdrContrast;
      next.rtx_hdr_saturation = form.rtxHdrSaturation;
    }
  } else if (form.rtxHdrMode === 'disabled') {
    next.rtx_hdr = false;
  }

  return Object.fromEntries(
    Object.entries(next).filter(
      ([key, value]) => key.length > 0 && value !== undefined && value !== null,
    ),
  );
}

function buildRtxHdrLiveOverridesPayload(): Record<string, unknown> {
  if (form.rtxHdrMode === 'inherit') return {};
  if (form.rtxHdrMode === 'disabled') return { rtx_hdr: false };

  const overrides: Record<string, unknown> = { rtx_hdr: true };
  if (form.rtxHdrValuesOverride) {
    overrides.rtx_hdr_sdr_brightness = form.rtxHdrSdrBrightness;
    overrides.rtx_hdr_peak_brightness = form.rtxHdrPeakBrightness;
    overrides.rtx_hdr_middle_gray = form.rtxHdrMiddleGray;
    overrides.rtx_hdr_contrast = form.rtxHdrContrast;
    overrides.rtx_hdr_saturation = form.rtxHdrSaturation;
  }
  return overrides;
}

function stableStringify(value: unknown): string {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return JSON.stringify(value) ?? String(value);
  }
  const entries = Object.entries(value as Record<string, unknown>).sort(([left], [right]) =>
    left.localeCompare(right),
  );
  return JSON.stringify(Object.fromEntries(entries));
}

function extractRtxHdrLiveOverrides(app: AppRecord | null): Record<string, unknown> {
  const source = clonePlainRecord(app?.['config-overrides']);
  return Object.fromEntries(
    RTX_HDR_OVERRIDE_KEYS.filter((key) => Object.prototype.hasOwnProperty.call(source, key)).map(
      (key) => [key, source[key]],
    ),
  );
}

function clearLiveRtxHdrTimer(): void {
  if (liveRtxHdrTimer === null) return;
  clearTimeout(liveRtxHdrTimer);
  liveRtxHdrTimer = null;
}

function primeLiveRtxHdrState(app: AppRecord | null): void {
  originalRtxHdrLiveOverrides.value = extractRtxHdrLiveOverrides(app);
  liveRtxHdrLastSentKey = stableStringify(buildRtxHdrLiveOverridesPayload());
  liveRtxHdrStatus.value = 'idle';
  liveRtxHdrError.value = '';
  clearLiveRtxHdrTimer();
}

function apiErrorMessage(cause: unknown, fallbackKey: string): string {
  if (cause instanceof ApiError) {
    const payload = cause.payload;
    if (payload && typeof payload === 'object' && !Array.isArray(payload)) {
      const message = (payload as { error?: unknown }).error;
      if (typeof message === 'string' && message.trim()) return message;
    }
  }
  return cause instanceof Error && cause.message ? cause.message : t(fallbackKey);
}

async function postRtxHdrLiveOverrides(
  overrides: Record<string, unknown>,
  key: string,
): Promise<void> {
  if (isNew.value || !form.uuid) return;

  liveRtxHdrStatus.value = 'applying';
  liveRtxHdrError.value = '';
  const response = await apiPost<{ status?: boolean; applied?: boolean }>(
    '/api/apps/rtx_hdr/live',
    {
      uuid: form.uuid,
      'config-overrides': overrides,
    },
  );
  if (response?.status === false) {
    throw new Error(t('rtx_hdr_live_update_failed'));
  }
  liveRtxHdrLastSentKey = key;
  liveRtxHdrStatus.value = 'applied';
}

function enqueueRtxHdrLivePost(overrides: Record<string, unknown>, key: string): Promise<void> {
  liveRtxHdrQueue = liveRtxHdrQueue
    .catch(() => {})
    .then(() => postRtxHdrLiveOverrides(overrides, key))
    .catch((cause) => {
      liveRtxHdrStatus.value = 'error';
      liveRtxHdrError.value = apiErrorMessage(cause, 'rtx_hdr_live_update_failed');
    });
  return liveRtxHdrQueue;
}

function scheduleRtxHdrLiveUpdate(): void {
  if (
    liveRtxHdrSuppress ||
    formHydrating ||
    loading.value ||
    isNew.value ||
    !form.uuid ||
    !showRtxHdrSection.value
  ) {
    return;
  }

  const key = stableStringify(buildRtxHdrLiveOverridesPayload());
  if (key === liveRtxHdrLastSentKey) {
    clearLiveRtxHdrTimer();
    if (liveRtxHdrStatus.value === 'queued') liveRtxHdrStatus.value = 'idle';
    return;
  }

  clearLiveRtxHdrTimer();
  liveRtxHdrStatus.value = 'queued';
  liveRtxHdrError.value = '';
  liveRtxHdrTimer = setTimeout(() => {
    liveRtxHdrTimer = null;
    const overrides = buildRtxHdrLiveOverridesPayload();
    void enqueueRtxHdrLivePost(overrides, stableStringify(overrides));
  }, RTX_HDR_LIVE_DEBOUNCE_MS);
}

async function restoreOriginalRtxHdrLiveOverrides(): Promise<void> {
  clearLiveRtxHdrTimer();
  if (isNew.value || !form.uuid || !sourceApp.value) return;

  await liveRtxHdrQueue.catch(() => {});
  const original = clonePlainRecord(originalRtxHdrLiveOverrides.value);
  const originalKey = stableStringify(original);
  if (originalKey === liveRtxHdrLastSentKey) return;
  await enqueueRtxHdrLivePost(original, originalKey);
}

function commitRtxHdrLiveState(): void {
  clearLiveRtxHdrTimer();
  const current = buildRtxHdrLiveOverridesPayload();
  originalRtxHdrLiveOverrides.value = clonePlainRecord(current);
  liveRtxHdrLastSentKey = stableStringify(current);
  liveRtxHdrStatus.value = 'idle';
  liveRtxHdrError.value = '';
}

function rtxHdrCalibrationValue(key: RtxHdrCalibrationKey): number {
  return form[key];
}

function rtxHdrBoundary(value: number, field: RtxHdrCalibrationField): string {
  return field.unit ? `${value} ${field.unit}` : String(value);
}

function updateRtxHdrCalibrationValue(
  key: RtxHdrCalibrationKey,
  field: RtxHdrCalibrationField,
  event: Event,
): void {
  const raw = Number((event.target as HTMLInputElement).value);
  if (!Number.isFinite(raw)) return;
  form[key] = Math.min(field.max, Math.max(field.min, Math.round(raw)));
}

function resetRtxHdrCalibration(): void {
  form.rtxHdrSdrBrightness = 0;
  form.rtxHdrPeakBrightness = 1000;
  form.rtxHdrMiddleGray = 50;
  form.rtxHdrContrast = 0;
  form.rtxHdrSaturation = 0;
}

function overrideField(key: string): SettingsField | undefined {
  return overrideFieldsByKey.value.get(key);
}

function overrideMessageExists(key: string): boolean {
  return te(key) || te(key, 'en');
}

function humanizeOverrideText(value: string): string {
  const acronyms = new Map<string, string>([
    ['amd', 'AMD'],
    ['av1', 'AV1'],
    ['fps', 'FPS'],
    ['gpu', 'GPU'],
    ['h264', 'H.264'],
    ['hdr', 'HDR'],
    ['hevc', 'HEVC'],
    ['nvenc', 'NVENC'],
    ['qsv', 'QSV'],
    ['rtss', 'RTSS'],
    ['vaapi', 'VA-API'],
    ['wgc', 'WGC'],
  ]);
  return value
    .split(/[_-]+/)
    .filter(Boolean)
    .map(
      (part) =>
        acronyms.get(part.toLocaleLowerCase()) ?? `${part[0]?.toLocaleUpperCase()}${part.slice(1)}`,
    )
    .join(' ');
}

function overrideLabel(key: string): string {
  const field = overrideField(key);
  const translationKey = field?.labelKey || `config.${key}`;
  return overrideMessageExists(translationKey) ? t(translationKey) : humanizeOverrideText(key);
}

function overrideDescription(key: string): string {
  const field = overrideField(key);
  const candidates = [
    field?.descriptionKey,
    `config.${key}_desc`,
    `ui.settings.fields.${key}.description`,
  ].filter((candidate): candidate is string => Boolean(candidate));
  const translationKey = candidates.find((candidate) => overrideMessageExists(candidate));
  return translationKey ? t(translationKey) : '';
}

function overrideOptionLabel(option: SettingsOption): string {
  return option.labelKey && overrideMessageExists(option.labelKey)
    ? t(option.labelKey)
    : humanizeOverrideText(option.value);
}

function overrideGpuOptions(): Array<{
  adapterName: string;
  label: string;
  pnpId: string;
  value: string;
}> {
  const options = [
    {
      label: t('ui.settings.options.gpu.auto'),
      value: '',
      adapterName: '',
      pnpId: '',
    },
    ...(overrideMetadata.value.gpus ?? [])
      .map((gpu) => ({
        label: gpu.description?.trim() ?? '',
        value: gpu.pnp_id?.trim() || gpu.description?.trim() || '',
        adapterName: gpu.description?.trim() ?? '',
        pnpId: gpu.pnp_id?.trim() ?? '',
      }))
      .filter((option) => option.adapterName),
  ];
  const currentName = String(overrideValue('adapter_name') ?? '');
  const currentPnpId = String(overrideValue('adapter_pnp_id') ?? '');
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

function overrideSelectOptions(key: string): Array<{ label: string; value: string }> {
  const field = overrideField(key);
  if (field?.source === 'gpu') {
    return overrideGpuOptions().map(({ label, value }) => ({ label, value }));
  }

  let declaredOptions = field?.options ?? [];
  if (key === 'encoder') {
    const auto: SettingsOption = { value: '', labelKey: '_common.auto' };
    const platform = String(overrideMetadata.value.platform ?? '').toLocaleLowerCase();
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
      : platform.includes('mac')
        ? [
            auto,
            { value: 'videotoolbox', labelKey: 'ui.settings.options.encoder.videotoolbox' },
            { value: 'software', labelKey: 'ui.settings.options.encoder.software' },
          ]
        : platform
          ? [
              auto,
              { value: 'nvenc', labelKey: 'ui.settings.options.encoder.nvenc' },
              { value: 'vulkan', labelKey: 'ui.settings.options.encoder.vulkan' },
              { value: 'vaapi', labelKey: 'ui.settings.options.encoder.vaapi' },
              { value: 'software', labelKey: 'ui.settings.options.encoder.software' },
            ]
          : [auto];
  }

  const options = declaredOptions.map((option) => ({
    label: overrideOptionLabel(option),
    value: String(option.value),
  }));
  const current = String(overrideValue(key) ?? '');
  if (options.length && current && !options.some((option) => option.value === current)) {
    options.push({
      label: t('ui.application.options.currentValue', { value: current }),
      value: current,
    });
  }
  return options;
}

function overrideControlValue(key: string): string {
  if (overrideField(key)?.source === 'gpu') {
    return String(overrideValue('adapter_pnp_id') || overrideValue('adapter_name') || '');
  }
  return String(overrideValue(key) ?? '');
}

function overridePlaceholder(key: string): string | undefined {
  const placeholderKey = overrideField(key)?.placeholderKey;
  return placeholderKey && overrideMessageExists(placeholderKey) ? t(placeholderKey) : undefined;
}

function overrideValue(key: string): unknown {
  return readConfigOverrides()[key];
}

function setOverrideValue(key: string, value: unknown): void {
  const overrides = readConfigOverrides();
  overrides[key] = value;
  writeConfigOverrides(overrides);
}

function overrideBooleanValue(key: string): boolean {
  const value = overrideValue(key);
  return (
    value === true ||
    ['1', 'true', 'yes', 'on', 'enabled'].includes(String(value).toLocaleLowerCase())
  );
}

function updateOverrideFromEvent(
  key: string,
  field: SettingsField | undefined,
  event: Event,
): void {
  const target = event.target as HTMLInputElement | HTMLSelectElement;
  if (field?.source === 'gpu') {
    const selected = overrideGpuOptions().find((option) => option.value === target.value);
    const overrides = readConfigOverrides();
    overrides.adapter_name = selected?.adapterName ?? '';
    overrides.adapter_pnp_id = selected?.pnpId ?? '';
    writeConfigOverrides(overrides);
  } else if (field?.kind === 'boolean') {
    setOverrideValue(key, (target as HTMLInputElement).checked);
  } else if (field?.kind === 'number') {
    setOverrideValue(key, target.value === '' ? '' : Number(target.value));
  } else {
    setOverrideValue(key, target.value);
  }
}

function overrideIsConfigured(key: string): boolean {
  return overrideKeys.value.includes(key);
}

async function focusOverride(key: string): Promise<void> {
  await nextTick();
  document.getElementById(`app-override-${key}`)?.focus();
}

async function chooseOverride(key: string): Promise<void> {
  if (!overrideIsConfigured(key)) await addOverride(key);
  else await focusOverride(key);
}

async function addOverride(key: string): Promise<void> {
  if (!key || overrideIsConfigured(key)) return;
  const field = overrideField(key);
  const options = field?.options ?? [];
  const defaultValue =
    settingsDefaults[key] ??
    (field?.kind === 'boolean' ? false : field?.kind === 'number' ? 0 : (options[0]?.value ?? ''));
  setOverrideValue(key, defaultValue);
  overrideAnnouncement.value = `${t('apps.overrides.add_setting')}: ${overrideLabel(key)}`;
  await focusOverride(key);
}

async function removeOverride(key: string): Promise<void> {
  const overrides = readConfigOverrides();
  delete overrides[key];
  if (key === 'adapter_name') delete overrides.adapter_pnp_id;
  writeConfigOverrides(overrides);
  overrideAnnouncement.value = `${t('_common.remove')}: ${overrideLabel(key)}`;
  await nextTick();
  document.getElementById(`app-override-catalog-${key}`)?.focus();
}

const editableKeys = new Set([
  'uuid',
  'name',
  'output',
  'display-output',
  'cmd',
  'working-dir',
  'image-path',
  'playnite-icon-path',
  'playnite-id',
  'playnite-managed',
  'elevated',
  'auto-detach',
  'wait-all',
  'exclude-global-prep-cmd',
  'exit-timeout',
  'virtual-screen',
  'virtual-display-mode',
  'virtual-display-layout',
  'dd-configuration-option',
  'frame-generation-provider',
  'frame-generation-mode',
  'gen1-framegen-fix',
  'gen2-framegen-fix',
  'frame-gen-limiter-fix',
  'lossless-scaling-enabled',
  'lossless-scaling-framegen',
  'lossless-scaling-target-fps',
  'lossless-scaling-rtss-limit',
  'lossless-scaling-profile',
  'lossless-scaling-launch-delay',
  'prep-cmd',
  'detached',
  'config-overrides',
]);
const transientKeys = new Set(['id', 'index', 'image-version', 'playnite-icon-version']);

function newUuid(): string {
  return crypto.randomUUID();
}

function emptyForm(): EditorForm {
  return {
    uuid: newUuid(),
    name: '',
    output: '',
    displayOutput: '',
    cmd: '',
    workingDir: '',
    imagePath: '',
    playniteIconPath: '',
    playniteId: '',
    playniteManaged: '',
    elevated: false,
    autoDetach: false,
    waitAll: false,
    excludeGlobalPrepCmd: false,
    exitTimeout: '',
    virtualScreen: false,
    virtualDisplayMode: '',
    virtualDisplayLayout: '',
    ddConfigurationOption: '',
    frameGenerationProvider: '',
    frameGenerationMode: '',
    gen1FramegenFix: false,
    gen2FramegenFix: false,
    frameGenLimiterFix: false,
    losslessScalingEnabled: false,
    losslessScalingFramegen: false,
    losslessScalingTargetFps: '',
    losslessScalingRtssLimit: '',
    losslessScalingProfile: '',
    losslessScalingLaunchDelay: '',
    rtxHdrMode: 'inherit',
    rtxHdrValuesOverride: false,
    rtxHdrSdrBrightness: 0,
    rtxHdrPeakBrightness: 1000,
    rtxHdrMiddleGray: 50,
    rtxHdrContrast: 0,
    rtxHdrSaturation: 0,
    prepCmd: [],
    detachedText: '',
    configOverridesJson: '{}',
    advancedJson: '{}',
  };
}

const routeId = computed(() => (typeof route.params.id === 'string' ? route.params.id : ''));
const isNew = computed(() => route.name === 'application-new' || !routeId.value);
const pageTitle = computed(() =>
  isNew.value
    ? t('ui.application.page.addTitle')
    : form.name || t('ui.application.page.fallbackTitle'),
);
const isDirty = computed(
  () =>
    isNew.value ||
    (Boolean(initialSnapshot.value) && JSON.stringify(form) !== initialSnapshot.value),
);
const errorMessages = computed(() => Object.values(errors));
const sourceCoverUrl = computed(() => (sourceApp.value ? appCoverUrl(sourceApp.value) : ''));
const editorCoverUrl = computed(() => selectedCoverPreview.value || sourceCoverUrl.value);

function asString(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function asBoolean(value: unknown): boolean {
  if (typeof value === 'boolean') return value;
  return ['1', 'true', 'yes', 'on', 'enabled'].includes(String(value).toLocaleLowerCase());
}

function asNumberText(value: unknown): string {
  return typeof value === 'number' || typeof value === 'string' ? String(value) : '';
}

function localizedError(cause: unknown, fallbackKey: string): string {
  if (cause instanceof AppServiceError && cause.code === 'missing-app-uuid') {
    return t('ui.application.errors.missingUuid');
  }
  if (cause instanceof ApiError) return t(fallbackKey);
  return cause instanceof Error ? cause.message : t(fallbackKey);
}

function jsonText(value: unknown): string {
  return JSON.stringify(value && typeof value === 'object' ? value : {}, null, 2);
}

function clonePlainRecord(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {};
  try {
    return JSON.parse(JSON.stringify(value)) as Record<string, unknown>;
  } catch {
    return { ...(value as Record<string, unknown>) };
  }
}

function parseBooleanOverride(value: unknown, fallback: boolean): boolean {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return value !== 0;
  const normalized = String(value ?? '')
    .trim()
    .toLocaleLowerCase();
  if (['true', '1', 'enabled', 'enable', 'yes', 'on'].includes(normalized)) return true;
  if (['false', '0', 'disabled', 'disable', 'no', 'off'].includes(normalized)) return false;
  return fallback;
}

function parseNumberOverride(value: unknown, fallback: number, min: number, max: number): number {
  const parsed = typeof value === 'number' ? value : Number(value);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.min(max, Math.max(min, Math.round(parsed)));
}

function extractRtxHdrOverrides(overrides: Record<string, unknown>): RtxHdrOverrideState {
  const rest = { ...overrides };
  const hasRtxHdrOverride = RTX_HDR_OVERRIDE_KEYS.some((key) =>
    Object.prototype.hasOwnProperty.call(rest, key),
  );
  const hasRtxHdrValueOverride = RTX_HDR_OVERRIDE_KEYS.filter((key) => key !== 'rtx_hdr').some(
    (key) => Object.prototype.hasOwnProperty.call(rest, key),
  );
  const mode: RtxHdrMode = !hasRtxHdrOverride
    ? 'inherit'
    : parseBooleanOverride(rest.rtx_hdr, true)
      ? 'enabled'
      : 'disabled';
  const sdrBrightness = parseNumberOverride(rest.rtx_hdr_sdr_brightness, 0, 0, 100);
  const peakBrightness = parseNumberOverride(rest.rtx_hdr_peak_brightness, 1000, 400, 2000);
  const middleGray = parseNumberOverride(rest.rtx_hdr_middle_gray, 50, 10, 100);
  const contrast = parseNumberOverride(rest.rtx_hdr_contrast, 0, -100, 100);
  const saturation = parseNumberOverride(rest.rtx_hdr_saturation, 0, -100, 100);

  for (const key of RTX_HDR_OVERRIDE_KEYS) delete rest[key];

  return {
    rest,
    mode,
    valuesOverride: hasRtxHdrValueOverride,
    sdrBrightness,
    peakBrightness,
    middleGray,
    contrast,
    saturation,
  };
}

function optionIsCustom(options: SelectOption[], value: string): boolean {
  return Boolean(value) && !options.some((option) => option.value === value);
}

function normalizeFrameGenerationMode(value: string): string {
  const normalized = value.toLocaleLowerCase().replace(/[^a-z0-9]/g, '');
  if (!normalized) return '';
  if (['off', 'none', 'disabled'].includes(normalized)) return 'off';
  if (['nvidia', 'smoothmotion', 'nvidiasmoothmotion'].includes(normalized)) {
    return 'nvidia-smooth-motion';
  }
  if (['game', 'gameprovided', 'gameprovider'].includes(normalized)) return 'game-provided';
  return 'lossless-scaling';
}

function frameGenerationModeFor(app: AppRecord): string {
  const configured = normalizeFrameGenerationMode(asString(app['frame-generation-mode']));
  if (configured) return configured;

  const provider = normalizeFrameGenerationMode(asString(app['frame-generation-provider']));
  if (provider === 'lossless-scaling' && asBoolean(app['lossless-scaling-framegen'])) {
    return provider;
  }
  if (['nvidia-smooth-motion', 'game-provided'].includes(provider)) return provider;
  return asBoolean(app['lossless-scaling-framegen']) ? 'lossless-scaling' : '';
}

function clearFrameGenHealth(): void {
  frameGenHealthEpoch += 1;
  frameGenHealth.value = null;
  frameGenHealthError.value = '';
  frameGenHealthLoading.value = false;
}

function beginFormSynchronizationDeferral(): number {
  formHydrating = true;
  formHydrationEpoch += 1;
  return formHydrationEpoch;
}

function endFormSynchronizationDeferral(epoch: number): void {
  void nextTick(() => {
    if (epoch === formHydrationEpoch) formHydrating = false;
  });
}

function prepEntry(value: unknown): PrepEntry {
  const source =
    value && typeof value === 'object' && !Array.isArray(value)
      ? (value as Record<string, unknown>)
      : {};
  const extras = Object.fromEntries(
    Object.entries(source).filter(([key]) => !['do', 'undo', 'elevated'].includes(key)),
  );
  return {
    do: asString(source.do),
    undo: asString(source.undo),
    elevated: asBoolean(source.elevated),
    extras,
  };
}

function hydrate(app: AppRecord): void {
  const synchronizationEpoch = beginFormSynchronizationDeferral();
  sourceApp.value = structuredClone(app);
  commandWasArray.value = Array.isArray(app.cmd);
  const command = Array.isArray(app.cmd)
    ? app.cmd.filter((part): part is string => typeof part === 'string').join('\n')
    : asString(app.cmd);
  const unknown = Object.fromEntries(
    Object.entries(app).filter(([key]) => !editableKeys.has(key) && !transientKeys.has(key)),
  );
  const rtxHdrOverrides = extractRtxHdrOverrides(clonePlainRecord(app['config-overrides']));

  Object.assign(form, {
    uuid: appUuid(app),
    name: asString(app.name),
    output: asString(app.output),
    displayOutput: asString(app['display-output']),
    cmd: command,
    workingDir: asString(app['working-dir']),
    imagePath: asString(app['image-path']),
    playniteIconPath: asString(app['playnite-icon-path']),
    playniteId: asString(app['playnite-id']),
    playniteManaged: asString(app['playnite-managed']),
    elevated: asBoolean(app.elevated),
    autoDetach: asBoolean(app['auto-detach']),
    waitAll: asBoolean(app['wait-all']),
    excludeGlobalPrepCmd: asBoolean(app['exclude-global-prep-cmd']),
    exitTimeout: asNumberText(app['exit-timeout']),
    virtualScreen: asBoolean(app['virtual-screen']),
    virtualDisplayMode: asString(app['virtual-display-mode']),
    virtualDisplayLayout: asString(app['virtual-display-layout']),
    ddConfigurationOption: asString(app['dd-configuration-option']),
    frameGenerationProvider: asString(app['frame-generation-provider']),
    frameGenerationMode: frameGenerationModeFor(app),
    gen1FramegenFix: asBoolean(app['gen1-framegen-fix']),
    gen2FramegenFix: asBoolean(app['gen2-framegen-fix']),
    frameGenLimiterFix: asBoolean(app['frame-gen-limiter-fix']),
    losslessScalingEnabled: asBoolean(app['lossless-scaling-enabled']),
    losslessScalingFramegen: asBoolean(app['lossless-scaling-framegen']),
    losslessScalingTargetFps: asNumberText(app['lossless-scaling-target-fps']),
    losslessScalingRtssLimit: asNumberText(app['lossless-scaling-rtss-limit']),
    losslessScalingProfile: asString(app['lossless-scaling-profile']),
    losslessScalingLaunchDelay: asNumberText(app['lossless-scaling-launch-delay']),
    rtxHdrMode: rtxHdrOverrides.mode,
    rtxHdrValuesOverride: rtxHdrOverrides.valuesOverride,
    rtxHdrSdrBrightness: rtxHdrOverrides.sdrBrightness,
    rtxHdrPeakBrightness: rtxHdrOverrides.peakBrightness,
    rtxHdrMiddleGray: rtxHdrOverrides.middleGray,
    rtxHdrContrast: rtxHdrOverrides.contrast,
    rtxHdrSaturation: rtxHdrOverrides.saturation,
    prepCmd: Array.isArray(app['prep-cmd']) ? app['prep-cmd'].map(prepEntry) : [],
    detachedText: Array.isArray(app.detached)
      ? app.detached.filter((value): value is string => typeof value === 'string').join('\n')
      : '',
    configOverridesJson: jsonText(rtxHdrOverrides.rest),
    advancedJson: jsonText(unknown),
  } satisfies EditorForm);
  coverFailed.value = false;
  selectedCoverPreview.value = '';
  coverPickerOpen.value = false;
  clearErrors();
  initialSnapshot.value = JSON.stringify(form);
  cancelPlayniteClose();
  playnitePickerOpen.value = false;
  playniteActiveIndex.value = -1;
  clearFrameGenHealth();
  liveRtxHdrSuppress = true;
  primeLiveRtxHdrState(app);
  endFormSynchronizationDeferral(synchronizationEpoch);
  void nextTick(() => {
    liveRtxHdrSuppress = false;
  });
}

function hydrateNew(): void {
  const synchronizationEpoch = beginFormSynchronizationDeferral();
  const next = emptyForm();
  Object.assign(form, next);
  sourceApp.value = null;
  commandWasArray.value = false;
  coverFailed.value = true;
  selectedCoverPreview.value = '';
  coverPickerOpen.value = false;
  clearErrors();
  initialSnapshot.value = JSON.stringify(form);
  cancelPlayniteClose();
  playnitePickerOpen.value = false;
  playniteActiveIndex.value = -1;
  clearFrameGenHealth();
  liveRtxHdrSuppress = true;
  primeLiveRtxHdrState(null);
  endFormSynchronizationDeferral(synchronizationEpoch);
  void nextTick(() => {
    liveRtxHdrSuppress = false;
  });
}

async function load(): Promise<void> {
  loading.value = true;
  loadError.value = '';
  saveError.value = '';
  const metadataPromise = apiGet<FrameGenMetadata>('/api/metadata').catch(
    (): FrameGenMetadata => ({}),
  );
  if (isNew.value) {
    overrideMetadata.value = await metadataPromise;
    hydrateNew();
    loading.value = false;
    return;
  }

  try {
    const [appList, metadata] = await Promise.all([fetchApps(), metadataPromise]);
    overrideMetadata.value = metadata;
    const app = appList.find(
      (candidate) => appUuid(candidate).toLocaleLowerCase() === routeId.value.toLocaleLowerCase(),
    );
    if (!app) throw new Error(t('ui.application.errors.notFound'));
    hydrate(app);
  } catch (cause) {
    loadError.value = localizedError(cause, 'ui.application.errors.load');
  } finally {
    loading.value = false;
  }
}

function clearErrors(): void {
  for (const key of Object.keys(errors)) delete errors[key];
}

async function validate(): Promise<boolean> {
  clearErrors();
  if (!form.name.trim()) errors.name = t('ui.application.validation.nameRequired');
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(form.uuid)) {
    errors.uuid = t('ui.application.validation.uuidInvalid');
  }

  const firstKey = Object.keys(errors)[0];
  if (!firstKey) return true;
  await nextTick();
  document.querySelector<HTMLElement>(`[data-field-key="${firstKey}"]`)?.focus();
  return false;
}

function setOptionalString(payload: AppRecord, key: string, value: string): void {
  const normalized = value.trim();
  if (normalized) payload[key] = normalized;
  else delete payload[key];
}

function setOptionalInteger(payload: AppRecord, key: string, value: string): void {
  if (value.trim()) payload[key] = Number(value);
  else delete payload[key];
}

function buildPayload(): AppRecord {
  const advanced = JSON.parse(form.advancedJson || '{}') as Record<string, unknown>;
  const configOverrides = buildRtxHdrConfigOverrides(
    JSON.parse(form.configOverridesJson || '{}') as Record<string, unknown>,
  );
  const payload: AppRecord = {
    ...advanced,
    uuid: form.uuid,
    name: form.name.trim(),
    cmd: commandWasArray.value
      ? form.cmd.split(/\r?\n/).filter((line) => line.length > 0)
      : form.cmd,
    elevated: form.elevated,
    'auto-detach': form.autoDetach,
    'wait-all': form.waitAll,
    'exclude-global-prep-cmd': form.excludeGlobalPrepCmd,
    'virtual-screen': form.virtualScreen,
    'gen1-framegen-fix': form.gen1FramegenFix,
    'gen2-framegen-fix': form.gen2FramegenFix,
    'frame-gen-limiter-fix': form.frameGenLimiterFix,
    'lossless-scaling-enabled': form.losslessScalingEnabled,
    'lossless-scaling-framegen': form.losslessScalingFramegen,
    'prep-cmd': form.prepCmd
      .filter((entry) => entry.do.trim() || entry.undo.trim() || Object.keys(entry.extras).length)
      .map((entry) => ({
        ...entry.extras,
        do: entry.do,
        undo: entry.undo,
        elevated: entry.elevated,
      })),
    detached: form.detachedText
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter(Boolean),
    'config-overrides': configOverrides,
  };

  setOptionalString(payload, 'output', form.output);
  setOptionalString(payload, 'display-output', form.displayOutput);
  setOptionalString(payload, 'working-dir', form.workingDir);
  setOptionalString(payload, 'image-path', form.imagePath);
  setOptionalString(payload, 'playnite-icon-path', form.playniteIconPath);
  setOptionalString(payload, 'playnite-id', form.playniteId);
  setOptionalString(payload, 'playnite-managed', form.playniteManaged);
  setOptionalString(payload, 'virtual-display-mode', form.virtualDisplayMode);
  setOptionalString(payload, 'virtual-display-layout', form.virtualDisplayLayout);
  setOptionalString(payload, 'dd-configuration-option', form.ddConfigurationOption);
  setOptionalString(payload, 'frame-generation-provider', form.frameGenerationProvider);
  setOptionalString(payload, 'frame-generation-mode', form.frameGenerationMode);
  setOptionalString(payload, 'lossless-scaling-profile', form.losslessScalingProfile);
  setOptionalInteger(payload, 'exit-timeout', form.exitTimeout);
  setOptionalInteger(payload, 'lossless-scaling-target-fps', form.losslessScalingTargetFps);
  setOptionalInteger(payload, 'lossless-scaling-rtss-limit', form.losslessScalingRtssLimit);
  setOptionalInteger(payload, 'lossless-scaling-launch-delay', form.losslessScalingLaunchDelay);
  return payload;
}

function clearPlayniteLink(): void {
  form.playniteId = '';
  form.playniteManaged = '';
}

function cancelPlayniteClose(): void {
  if (playniteCloseTimer === null) return;
  window.clearTimeout(playniteCloseTimer);
  playniteCloseTimer = null;
}

function waitForPlaynite(milliseconds: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, milliseconds));
}

async function loadPlayniteGames(): Promise<void> {
  if (playniteGamesLoading.value || playniteGamesLoaded.value) return;

  playniteGamesLoading.value = true;
  playniteGamesError.value = '';
  playniteGamesUnavailable.value = false;
  try {
    let sawActiveConnection = false;
    for (let attempt = 0; attempt < 8; attempt += 1) {
      const status = await apiGet<PlayniteStatus>('/api/playnite/status');
      if (status.installed !== true && status.active !== true) {
        playniteGames.value = [];
        playniteGamesUnavailable.value = true;
        playniteGamesLoaded.value = true;
        return;
      }

      sawActiveConnection ||= status.active === true;
      const payload = await apiGet<unknown>('/api/playnite/games');
      const games = Array.isArray(payload) ? payload : [];
      playniteGames.value = games
        .filter(
          (game): game is Record<string, unknown> =>
            Boolean(game) && typeof game === 'object' && !Array.isArray(game),
        )
        .map((game) => ({
          id: asString(game.id),
          name: asString(game.name) || asString(game.id),
          installed: game.installed === undefined ? undefined : asBoolean(game.installed),
        }))
        .filter((game) => Boolean(game.id) && Boolean(game.name) && game.installed !== false)
        .sort((left, right) => left.name.localeCompare(right.name));
      if (playniteGames.value.length) {
        playniteGamesLoaded.value = true;
        return;
      }
      if (attempt < 7) await waitForPlaynite(500);
    }

    playniteGamesUnavailable.value = !sawActiveConnection;
    playniteGamesLoaded.value = sawActiveConnection;
  } catch {
    playniteGamesError.value = t('ui.application.playnite.loadFailed');
  } finally {
    playniteGamesLoading.value = false;
  }
}

function openPlaynitePicker(): void {
  if (!isNew.value) return;
  cancelPlayniteClose();
  if (playniteGamesUnavailable.value) playniteGamesLoaded.value = false;
  playnitePickerOpen.value = true;
  playniteActiveIndex.value = -1;
  void loadPlayniteGames();
}

function closePlaynitePicker(): void {
  cancelPlayniteClose();
  playniteCloseTimer = window.setTimeout(() => {
    playnitePickerOpen.value = false;
    playniteActiveIndex.value = -1;
    playniteCloseTimer = null;
  }, 120);
}

function selectPlayniteGame(game: PlayniteGame): void {
  cancelPlayniteClose();
  form.name = game.name;
  form.playniteId = game.id;
  form.playniteManaged = 'manual';
  form.cmd = '';
  form.workingDir = '';
  form.imagePath = '';
  selectedCoverPreview.value = '';
  coverPickerOpen.value = false;
  form.playniteIconPath = '';
  playnitePickerOpen.value = false;
  playniteActiveIndex.value = -1;
}

async function openCoverPicker(): Promise<void> {
  if (!form.name.trim() || coverSearching.value) return;
  coverSearchQuery.value = form.name;
  coverCandidates.value = [];
  coverError.value = '';
  coverPickerOpen.value = true;
  await runCoverSearch();
}

async function runCoverSearch(): Promise<void> {
  if (!coverSearchQuery.value.trim() || coverSearching.value) return;
  coverSearching.value = true;
  coverError.value = '';
  try {
    coverCandidates.value = await searchCovers(coverSearchQuery.value);
  } catch {
    coverCandidates.value = [];
    coverError.value = t('ui.application.coverPicker.searchError');
  } finally {
    coverSearching.value = false;
  }
}

async function chooseCover(cover: CoverCandidate): Promise<void> {
  if (coverBusy.value) return;
  coverError.value = '';
  coverBusy.value = true;
  try {
    const imagePath = await uploadCover(cover);
    if (isPlayniteLinked.value) {
      await updatePlayniteCover(form.playniteId, cover);
    }
    form.imagePath = imagePath;
    selectedCoverPreview.value = cover.url;
    coverFailed.value = false;
    coverPickerOpen.value = false;
  } catch {
    coverError.value = t(
      isPlayniteLinked.value
        ? 'ui.application.coverPicker.playniteUpdateError'
        : 'ui.application.coverPicker.uploadError',
    );
  } finally {
    coverBusy.value = false;
  }
}

function useCustomApplication(): void {
  cancelPlayniteClose();
  clearPlayniteLink();
  playnitePickerOpen.value = false;
  playniteActiveIndex.value = -1;
}

function handleNameInput(): void {
  if (isPlayniteLinked.value) clearPlayniteLink();
  if (!isNew.value) return;
  playniteActiveIndex.value = -1;
  openPlaynitePicker();
}

function movePlayniteActiveOption(delta: number): void {
  const count = filteredPlayniteGames.value.length;
  if (!count) return;
  const current = playniteActiveIndex.value;
  if (current < 0) {
    playniteActiveIndex.value = delta < 0 ? count - 1 : 0;
    return;
  }
  playniteActiveIndex.value = (current + delta + count) % count;
}

function handleNameKeydown(event: KeyboardEvent): void {
  if (!isNew.value) return;
  if (event.key === 'ArrowDown') {
    event.preventDefault();
    if (!playnitePickerOpen.value) openPlaynitePicker();
    movePlayniteActiveOption(1);
  } else if (event.key === 'ArrowUp') {
    event.preventDefault();
    if (!playnitePickerOpen.value) openPlaynitePicker();
    movePlayniteActiveOption(-1);
  } else if (event.key === 'Enter' && playnitePickerOpen.value) {
    event.preventDefault();
    const game = filteredPlayniteGames.value[playniteActiveIndex.value];
    if (game) {
      selectPlayniteGame(game);
    }
  } else if (event.key === 'Escape') {
    playnitePickerOpen.value = false;
    playniteActiveIndex.value = -1;
  }
}

function healthTone(
  status: FrameGenRequirementStatus,
): 'success' | 'warning' | 'danger' | 'neutral' {
  if (status === 'pass') return 'success';
  if (status === 'configured') return 'neutral';
  if (status === 'warn') return 'warning';
  if (status === 'fail') return 'danger';
  return 'neutral';
}

function healthStatusLabel(status: FrameGenRequirementStatus): string {
  if (status === 'pass') return t('apps.framegen.status_ready');
  if (status === 'configured') return t('ui.application.framegenHealth.configured');
  if (status === 'warn') return t('apps.framegen.status_warn');
  if (status === 'fail') return t('apps.framegen.status_fail');
  return t('apps.framegen.status_unknown');
}

function parseRefreshHz(raw: unknown): number | null {
  if (typeof raw === 'number') return Number.isFinite(raw) && raw > 0 ? raw : null;
  if (typeof raw === 'string') {
    const normalized = raw.trim().replace(/(hz|fps|frames|refresh)/gi, '');
    const fraction = normalized.match(/^([-+]?\d+(?:\.\d+)?)\s*\/\s*([-+]?\d+(?:\.\d+)?)/);
    if (fraction) {
      const numerator = Number(fraction[1]);
      const denominator = Number(fraction[2]);
      if (Number.isFinite(numerator) && Number.isFinite(denominator) && denominator !== 0) {
        return numerator / denominator;
      }
    }
    const value = normalized.match(/[-+]?\d+(?:\.\d+)?/);
    return value && Number.isFinite(Number(value[0])) && Number(value[0]) > 0
      ? Number(value[0])
      : null;
  }
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;

  const value = raw as Record<string, unknown>;
  for (const key of ['hz', 'value']) {
    const parsed = parseRefreshHz(value[key]);
    if (parsed !== null) return parsed;
  }
  const numerator = Number(value.numerator ?? value.m_numerator ?? value.num ?? value.n);
  const denominator = Number(value.denominator ?? value.m_denominator ?? value.den ?? 1);
  return Number.isFinite(numerator) &&
    Number.isFinite(denominator) &&
    numerator > 0 &&
    denominator > 0
    ? numerator / denominator
    : null;
}

function parseRefreshRates(raw: unknown): number[] {
  const source = Array.isArray(raw) ? raw : raw === undefined || raw === null ? [] : [raw];
  return [
    ...new Set(source.map(parseRefreshHz).filter((value): value is number => value !== null)),
  ].sort((left, right) => left - right);
}

function normalizedDeviceId(value: unknown): string {
  return asString(value).trim().toLocaleLowerCase();
}

function isVirtualDisplaySelection(value: string): boolean {
  const normalized = value.trim().toLocaleLowerCase();
  return (
    normalized === 'sunshine:virtual_display' || normalized === 'sunshine:sudovda_virtual_display'
  );
}

function effectiveAppOutput(): string {
  return form.displayOutput.trim() || form.output.trim();
}

function virtualDisplayModeUsesVirtual(mode: string): boolean | null {
  if (mode === 'disabled') return false;
  if (mode === 'per_client' || mode === 'shared') return true;
  return null;
}

function resolveVirtualDisplay(
  config: FrameGenConfig,
  windowsBuild: number | null,
): VirtualDisplayResolution {
  // An app output override wins over both per-app and host virtual-display settings.
  const appOutput = effectiveAppOutput();
  if (appOutput) {
    return {
      usingVirtual: isVirtualDisplaySelection(appOutput),
      output: appOutput,
      hasAppOutput: true,
    };
  }

  if (form.virtualScreen) return { usingVirtual: true, output: '', hasAppOutput: false };

  const appMode = virtualDisplayModeUsesVirtual(form.virtualDisplayMode);
  if (appMode !== null) return { usingVirtual: appMode, output: '', hasAppOutput: false };

  const configuredMode = virtualDisplayModeUsesVirtual(asString(config.virtual_display_mode));
  if (configuredMode !== null) {
    return {
      usingVirtual: configuredMode,
      output: asString(config.output_name),
      hasAppOutput: false,
    };
  }

  const globalOutput = asString(config.output_name);
  if (isVirtualDisplaySelection(globalOutput)) {
    return { usingVirtual: true, output: globalOutput, hasAppOutput: false };
  }

  // The server defaults to per-client virtual displays on Windows 11 and to physical
  // capture on Windows 10 when no mode is saved. Keep an unknown build honest.
  return {
    usingVirtual: windowsBuild === null ? null : windowsBuild >= 22000,
    output: globalOutput,
    hasAppOutput: false,
  };
}

function configWithFrameGenOverrides(config: FrameGenConfig): FrameGenConfig {
  try {
    const overrides = JSON.parse(form.configOverridesJson || '{}') as unknown;
    if (!overrides || typeof overrides !== 'object' || Array.isArray(overrides)) return config;
    const capture = (overrides as Record<string, unknown>).capture;
    return capture === undefined ? config : { ...config, capture };
  } catch {
    return config;
  }
}

async function inspectFrameGenDisplay(
  resolution: VirtualDisplayResolution,
  config: FrameGenConfig,
  displayResult: PromiseSettledResult<unknown>,
): Promise<FrameGenHealth['display']> {
  if (resolution.usingVirtual === true) {
    return {
      status: 'configured',
      label: t('ui.application.framegenHealth.virtualDisplayLabel'),
      message: t('ui.application.framegenHealth.virtualDisplayConfigured'),
      usingVirtual: true,
      capabilities: [],
    };
  }

  if (resolution.usingVirtual === null) {
    return {
      status: 'unknown',
      label: t('ui.application.framegenHealth.displayTargetLabel'),
      message: t('ui.application.framegenHealth.displayTargetUnknown'),
      usingVirtual: null,
      capabilities: [],
    };
  }

  if (displayResult.status !== 'fulfilled' || !Array.isArray(displayResult.value)) {
    return {
      status: 'unknown',
      label: t('ui.application.framegenHealth.physicalDisplayLabel'),
      message: t('apps.framegen.health_display_helper_unreachable'),
      usingVirtual: false,
      capabilities: [],
    };
  }

  const devices = displayResult.value as DisplayDevice[];
  const candidates = (
    resolution.hasAppOutput
      ? [resolution.output]
      : [resolution.output, asString(config.output_name)]
  )
    .filter(Boolean)
    .map(normalizedDeviceId);
  const matchingTarget = devices.find((device) => {
    const deviceId = normalizedDeviceId(device.device_id);
    const displayName = normalizedDeviceId(device.display_name);
    const friendlyName = normalizedDeviceId(device.friendly_name);
    return (
      candidates.includes(deviceId) ||
      candidates.includes(displayName) ||
      candidates.includes(friendlyName)
    );
  });
  const target =
    matchingTarget ??
    (resolution.hasAppOutput
      ? undefined
      : (devices.find((device) => Boolean(device.info)) ?? devices[0]));

  if (!target) {
    return {
      status: 'unknown',
      label: t('ui.application.framegenHealth.physicalDisplayLabel'),
      message: resolution.hasAppOutput
        ? t('ui.application.framegenHealth.appOutputNotFound')
        : t('apps.framegen.health_display_no_devices'),
      usingVirtual: false,
      capabilities: [],
    };
  }

  const label =
    asString(target.friendly_name) ||
    asString(target.display_name) ||
    t('apps.framegen.req_display_physical_label');
  const currentHz = parseRefreshHz(target.info?.refresh_rate ?? target.info?.refreshRate);
  const supportedRates = parseRefreshRates(
    target.supported_refresh_rates ?? target.supportedRefreshRates,
  );
  const deviceId = asString(target.device_id) || asString(target.display_name);
  const capabilityMap = new Map<number, boolean | null>();
  let edidMaximum: number | null = null;

  if (deviceId) {
    try {
      const query = new URLSearchParams({
        device_id: deviceId,
        targets: '120,180,240,288',
      });
      const edid = await apiGet<EdidRefresh>(`/api/framegen/edid-refresh?${query.toString()}`);
      if (edid.status !== false) {
        const maximums = [
          parseRefreshHz(edid.max_vertical_hz),
          parseRefreshHz(edid.max_timing_hz),
        ].filter((value): value is number => value !== null);
        edidMaximum = maximums.length ? Math.max(...maximums) : null;
        if (Array.isArray(edid.targets)) {
          for (const rawTarget of edid.targets) {
            if (!rawTarget || typeof rawTarget !== 'object') continue;
            const entry = rawTarget as EdidTarget;
            const hz = parseRefreshHz(entry.hz);
            if (hz !== null) {
              capabilityMap.set(hz, typeof entry.supported === 'boolean' ? entry.supported : null);
            }
          }
        }
      }
    } catch {
      // The standard display enumeration remains useful when EDID is unavailable.
    }
  }

  const capabilities = [...capabilityMap.entries()]
    .map(([hz, supported]) => ({ hz, supported }))
    .sort((left, right) => left.hz - right.hz);
  const maximum =
    edidMaximum ?? (supportedRates.length ? supportedRates[supportedRates.length - 1] : currentHz);

  if (maximum === null) {
    return {
      status: capabilities.length ? 'configured' : 'unknown',
      label,
      message: capabilities.length
        ? t('ui.application.framegenHealth.physicalDisplayCapabilitiesOnly')
        : t('apps.framegen.health_display_refresh_unreadable'),
      usingVirtual: false,
      capabilities,
    };
  }
  return {
    status: 'configured',
    label,
    message: t('ui.application.framegenHealth.physicalDisplayConfigured', {
      hz: Math.round(maximum),
    }),
    usingVirtual: false,
    capabilities,
  };
}

async function refreshFrameGenHealth(): Promise<void> {
  const epoch = frameGenHealthEpoch;
  if (frameGenHealthRequest?.epoch === epoch) return frameGenHealthRequest.promise;

  const run = async () => {
    frameGenHealthLoading.value = true;
    frameGenHealthError.value = '';
    try {
      const [configResult, metadataResult, rtssResult, displayResult] = await Promise.allSettled([
        apiGet<FrameGenConfig>('/api/config'),
        apiGet<FrameGenMetadata>('/api/metadata'),
        apiGet<RtssStatus>('/api/rtss/status'),
        apiGet<unknown>('/api/display-devices?detail=full'),
      ]);
      const hostConfig = configResult.status === 'fulfilled' ? configResult.value : {};
      const config = configWithFrameGenOverrides(hostConfig);
      const metadata = metadataResult.status === 'fulfilled' ? metadataResult.value : {};
      const platform = asString(metadata.platform).toLocaleLowerCase();
      const build = Number(metadata.windows_build_number);
      const windowsBuild = Number.isFinite(build) && build > 0 ? build : null;
      const displayResolution = resolveVirtualDisplay(config, windowsBuild);
      const capture = asString(config.capture).toLocaleLowerCase();
      const automaticWgc =
        !capture &&
        displayResolution.usingVirtual === true &&
        windowsBuild !== null &&
        windowsBuild >= 22631;
      const gameProvidedVirtual =
        displayResolution.usingVirtual === true && form.frameGenerationMode === 'game-provided';

      let captureStatus: FrameGenRequirementStatus;
      let captureMessage: string;
      if (gameProvidedVirtual) {
        captureStatus = 'pass';
        captureMessage = t('ui.application.framegenHealth.gameProvidedVirtualCapture');
      } else if (capture === 'ddx' && displayResolution.usingVirtual === true) {
        captureStatus = 'fail';
        captureMessage = t('ui.application.framegenHealth.virtualCaptureDdx');
      } else if (capture === 'wgc' || capture === 'wgcc') {
        captureStatus = 'pass';
        captureMessage = t('apps.framegen.health_capture_wgc_active');
      } else if (automaticWgc) {
        captureStatus = 'pass';
        captureMessage = t('apps.framegen.health_capture_wgc_auto');
      } else if (!capture && displayResolution.usingVirtual === false) {
        captureStatus = 'configured';
        captureMessage = t('ui.application.framegenHealth.physicalCaptureDefault');
      } else if (capture === 'ddx') {
        captureStatus = 'configured';
        captureMessage = t('ui.application.framegenHealth.physicalCaptureDdx');
      } else if (!capture) {
        captureStatus = 'warn';
        captureMessage = t('ui.application.framegenHealth.captureAutoUnknown');
      } else {
        captureStatus = 'warn';
        captureMessage = t('ui.application.framegenHealth.captureConfigured', { capture });
      }

      let rtssStatus: FrameGenRequirementStatus;
      let rtssMessage: string;
      if (form.frameGenerationMode !== 'lossless-scaling') {
        rtssStatus = 'configured';
        rtssMessage = t('ui.application.framegenHealth.rtssOptional');
      } else if (rtssResult.status === 'fulfilled') {
        const rtssInstalled = asBoolean(rtssResult.value.path_exists);
        const hooksFound = asBoolean(rtssResult.value.hooks_found);
        if (rtssInstalled && hooksFound) {
          rtssStatus = 'pass';
          rtssMessage = t('apps.framegen.health_rtss_hooks');
        } else if (rtssInstalled) {
          rtssStatus = 'warn';
          rtssMessage = t('apps.framegen.health_rtss_no_hooks');
        } else {
          rtssStatus = 'warn';
          rtssMessage = t('apps.framegen.health_rtss_install');
        }
      } else {
        rtssStatus = 'unknown';
        rtssMessage = t('apps.framegen.health_rtss_unreachable');
      }

      const display = await inspectFrameGenDisplay(displayResolution, config, displayResult);
      const os: FrameGenHealth['os'] =
        platform && platform !== 'windows'
          ? { status: 'unknown', message: t('apps.framegen.health_os_unknown') }
          : windowsBuild === null
            ? { status: 'unknown', message: t('apps.framegen.health_os_unknown') }
            : windowsBuild >= 22000
              ? { status: 'pass', message: t('apps.framegen.health_os_win11') }
              : form.frameGenerationMode === 'lossless-scaling'
                ? { status: 'fail', message: t('apps.framegen.health_os_win10_lossless') }
                : { status: 'warn', message: t('apps.framegen.health_os_win10') };

      if (epoch !== frameGenHealthEpoch) return;
      frameGenHealth.value = {
        checkedAt: Date.now(),
        os,
        capture: { status: captureStatus, message: captureMessage },
        rtss: { status: rtssStatus, message: rtssMessage },
        display,
      };
    } catch (cause) {
      if (epoch !== frameGenHealthEpoch) return;
      frameGenHealth.value = null;
      frameGenHealthError.value =
        cause instanceof Error ? cause.message : t('apps.framegen.health_run_error');
    } finally {
      if (frameGenHealthRequest?.epoch === epoch) {
        frameGenHealthLoading.value = false;
        frameGenHealthRequest = null;
      }
    }
  };

  const promise = run();
  frameGenHealthRequest = { epoch, promise };
  return promise;
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

function applyDisplaySelection(selection: AppDisplaySelection): void {
  if (selection === 'physical') {
    const output = effectiveAppOutput();
    form.virtualScreen = false;
    form.virtualDisplayMode = 'disabled';
    form.virtualDisplayLayout = '';
    if (isVirtualDisplaySelection(output)) {
      form.output = '';
      form.displayOutput = '';
    }
    return;
  }

  form.output = '';
  form.displayOutput = '';
  form.virtualScreen = true;
  if (!['per_client', 'shared'].includes(form.virtualDisplayMode)) {
    form.virtualDisplayMode = 'per_client';
  }
  form.ddConfigurationOption = '';
}

function enableVirtualDisplayForFrameGen(): void {
  if (effectiveAppOutput() && !isVirtualDisplaySelection(effectiveAppOutput())) {
    form.output = '';
    form.displayOutput = '';
  }
  form.virtualScreen = true;
  form.virtualDisplayMode = 'per_client';
}

async function submit(): Promise<void> {
  if (saving.value) return;
  saveError.value = '';
  if (!(await validate())) return;
  saving.value = true;
  try {
    const payload = buildPayload();
    await saveApp(payload);
    commitRtxHdrLiveState();
    await fetchApps().catch(() => []);
    await router.push({ name: 'library' });
  } catch (cause) {
    saveError.value = localizedError(cause, 'ui.application.errors.save');
  } finally {
    saving.value = false;
  }
}

async function cancel(): Promise<void> {
  await restoreOriginalRtxHdrLiveOverrides();
  void router.push({ name: 'library' });
}

function addPrepCommand(): void {
  form.prepCmd.push({ do: '', undo: '', elevated: false, extras: {} });
  nextTick(() => {
    document.querySelector<HTMLInputElement>(`#prep-do-${form.prepCmd.length - 1}`)?.focus();
  });
}

function removePrepCommand(index: number): void {
  form.prepCmd.splice(index, 1);
}

async function confirmDelete(): Promise<void> {
  if (isNew.value || deleting.value) return;
  deleting.value = true;
  loadError.value = '';
  deleteError.value = '';
  try {
    await restoreOriginalRtxHdrLiveOverrides();
    await deleteApp(form.uuid);
    await fetchApps().catch(() => []);
    deleteOpen.value = false;
    await router.push({ name: 'library' });
  } catch (cause) {
    deleteError.value = localizedError(cause, 'ui.application.errors.delete');
  } finally {
    deleting.value = false;
  }
}

watch(
  () => form.frameGenerationMode,
  (mode) => {
    if (loading.value || formHydrating) return;
    clearFrameGenHealth();
    if (!mode || mode === 'off') {
      form.frameGenerationProvider = '';
      form.losslessScalingFramegen = false;
      return;
    }
    if (['lossless-scaling', 'nvidia-smooth-motion', 'game-provided'].includes(mode)) {
      form.frameGenerationProvider = mode;
    }
    form.losslessScalingFramegen = mode === 'lossless-scaling';
    void refreshFrameGenHealth();
  },
);

watch(
  () => [
    form.virtualScreen,
    form.virtualDisplayMode,
    form.output,
    form.displayOutput,
    form.configOverridesJson,
  ],
  () => {
    if (!frameGenHealth.value && !frameGenHealthLoading.value) return;
    clearFrameGenHealth();
    void refreshFrameGenHealth();
  },
);

watch(
  () => [
    form.rtxHdrMode,
    form.rtxHdrValuesOverride,
    form.rtxHdrSdrBrightness,
    form.rtxHdrPeakBrightness,
    form.rtxHdrMiddleGray,
    form.rtxHdrContrast,
    form.rtxHdrSaturation,
  ],
  () => scheduleRtxHdrLiveUpdate(),
);

watch([routeId, () => route.name], () => void load(), { immediate: true });

onBeforeUnmount(() => {
  liveRtxHdrSuppress = true;
  void restoreOriginalRtxHdrLiveOverrides();
});
</script>

<template>
  <div class="vs-page application-page">
    <PageHeader
      :title="pageTitle"
      :description="
        t(isNew ? 'ui.application.page.addDescription' : 'ui.application.page.editDescription')
      "
    >
      <template #meta>
        <StatusBadge v-if="isDirty" tone="warning">
          {{ t('ui.application.status.unsaved') }}
        </StatusBadge>
        <StatusBadge v-else-if="!loading && !loadError" tone="neutral">
          {{ t('ui.application.status.saved') }}
        </StatusBadge>
      </template>
      <template #actions>
        <AppButton variant="secondary" :label="t('ui.application.actions.back')" @click="cancel" />
        <AppButton
          variant="primary"
          icon="check"
          :label="t('ui.application.actions.save')"
          :busy="saving"
          :busy-label="t('ui.application.actions.saving')"
          :disabled="loading || Boolean(loadError) || (!isDirty && !isNew)"
          @click="submit"
        />
      </template>
    </PageHeader>

    <InlineAlert
      v-if="loadError"
      tone="danger"
      :title="t('ui.application.alerts.unavailable')"
      announce="assertive"
    >
      {{ loadError }}
      <template #actions>
        <AppButton
          v-if="!isNew"
          size="compact"
          icon="refresh"
          :label="t('ui.application.actions.tryAgain')"
          @click="load"
        />
        <AppButton
          size="compact"
          variant="tertiary"
          :label="t('ui.application.actions.returnToLibrary')"
          @click="cancel"
        />
      </template>
    </InlineAlert>

    <InlineAlert
      v-else-if="saveError"
      tone="danger"
      :title="t('ui.application.alerts.saveFailed')"
      announce="assertive"
    >
      {{ saveError }}
    </InlineAlert>

    <InlineAlert
      v-else-if="errorMessages.length"
      tone="danger"
      :title="t('ui.application.alerts.correctFields')"
      announce="assertive"
    >
      <ul class="editor-error-list">
        <li v-for="message in errorMessages" :key="message">{{ message }}</li>
      </ul>
    </InlineAlert>

    <template v-if="loading">
      <span class="vs-sr-only" role="status" aria-live="polite">
        {{ t('ui.application.loading') }}
      </span>
      <div class="editor-loading" aria-hidden="true">
        <LoadingSkeleton v-for="index in 5" :key="index" variant="block" height="180px" />
      </div>
    </template>

    <form
      v-else-if="!loadError"
      id="application-form"
      class="editor-form"
      novalidate
      @submit.prevent="submit"
    >
      <section class="editor-section" aria-labelledby="identity-heading">
        <div class="editor-section__heading">
          <h2 id="identity-heading">{{ t('ui.application.sections.identity.title') }}</h2>
          <p>{{ t('ui.application.sections.identity.description') }}</p>
        </div>
        <div class="editor-group editor-grid">
          <div class="vs-field editor-field editor-field--wide">
            <label class="vs-field__label" for="app-name">
              {{ t('ui.application.fields.name.label') }}
            </label>
            <div class="editor-name-control">
              <input
                id="app-name"
                v-model="form.name"
                class="vs-input"
                data-field-key="name"
                type="text"
                :role="isNew ? 'combobox' : undefined"
                autocomplete="off"
                required
                :aria-autocomplete="isNew ? 'list' : undefined"
                :aria-controls="isNew ? 'app-playnite-options' : undefined"
                :aria-expanded="isNew ? playnitePickerOpen : undefined"
                :aria-activedescendant="
                  isNew && playniteActiveIndex >= 0
                    ? `app-playnite-option-${playniteActiveIndex}`
                    : undefined
                "
                :aria-invalid="Boolean(errors.name)"
                :aria-describedby="errors.name ? 'app-name-error' : 'app-name-help'"
                @focus="isNew && openPlaynitePicker()"
                @blur="closePlaynitePicker"
                @input="handleNameInput"
                @keydown="handleNameKeydown"
              />
              <AppButton
                v-if="isNew"
                size="compact"
                icon="library"
                :label="t('ui.application.playnite.browse')"
                :aria-label="t('ui.application.playnite.browse')"
                @mousedown.prevent
                @click="openPlaynitePicker"
              />
            </div>
            <span id="app-name-help" class="vs-field__helper">
              {{
                isPlayniteLinked
                  ? t('ui.application.playnite.linkedHelp')
                  : t('ui.application.fields.name.help')
              }}
            </span>
            <span v-if="errors.name" id="app-name-error" class="vs-field__error">{{
              errors.name
            }}</span>

            <div
              v-if="isNew && playnitePickerOpen"
              id="app-playnite-options"
              class="editor-playnite-picker"
              role="listbox"
              :aria-label="t('ui.application.playnite.resultsLabel')"
            >
              <p v-if="playniteGamesLoading" class="editor-playnite-picker__notice">
                {{ t('ui.application.playnite.loading') }}
              </p>
              <p v-else-if="playniteGamesError" class="editor-playnite-picker__notice">
                {{ playniteGamesError }}
              </p>
              <p v-else-if="playniteGamesUnavailable" class="editor-playnite-picker__notice">
                {{ t('ui.application.playnite.unavailable') }}
              </p>
              <template v-else>
                <button
                  v-for="(game, index) in filteredPlayniteGames"
                  :id="`app-playnite-option-${index}`"
                  :key="game.id"
                  class="editor-playnite-option"
                  :class="{ 'editor-playnite-option--active': playniteActiveIndex === index }"
                  type="button"
                  role="option"
                  :aria-selected="playniteActiveIndex === index"
                  @mousedown.prevent
                  @click="selectPlayniteGame(game)"
                  @mouseenter="playniteActiveIndex = index"
                >
                  <UiIcon name="library" :size="16" aria-hidden="true" />
                  <span>{{ game.name }}</span>
                </button>
                <p v-if="!filteredPlayniteGames.length" class="editor-playnite-picker__notice">
                  {{ t('ui.application.playnite.empty') }}
                </p>
              </template>
            </div>

            <div v-if="isPlayniteLinked" class="editor-playnite-link vs-cluster">
              <StatusBadge :label="t('apps.playnite_badge')" tone="info" compact />
              <span>{{ form.playniteId }}</span>
              <AppButton
                v-if="isPlayniteLinked"
                size="compact"
                variant="tertiary"
                icon="x"
                :label="t('ui.application.playnite.useCustom')"
                @click="useCustomApplication"
              />
            </div>
          </div>

          <label v-if="!isNew" class="vs-field editor-field editor-field--wide" for="app-uuid">
            <span class="vs-field__label">{{ t('ui.application.fields.uuid.label') }}</span>
            <input
              id="app-uuid"
              v-model="form.uuid"
              class="vs-input vs-monospace"
              data-field-key="uuid"
              type="text"
              readonly
              :aria-invalid="Boolean(errors.uuid)"
              :aria-describedby="errors.uuid ? 'app-uuid-error' : 'app-uuid-help'"
            />
            <span id="app-uuid-help" class="vs-field__helper">
              {{ t('ui.application.fields.uuid.help') }}
            </span>
            <span v-if="errors.uuid" id="app-uuid-error" class="vs-field__error">{{
              errors.uuid
            }}</span>
          </label>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="execution-heading">
        <div class="editor-section__heading">
          <h2 id="execution-heading">{{ t('ui.application.sections.execution.title') }}</h2>
          <p>{{ t('ui.application.sections.execution.description') }}</p>
        </div>
        <div
          class="editor-group editor-execution-layout"
          :class="{ 'editor-execution-layout--simple': isNew && !editorCoverUrl }"
        >
          <div
            v-if="!isNew || editorCoverUrl"
            class="editor-artwork"
            :class="{ 'editor-artwork--empty': coverFailed || !editorCoverUrl }"
          >
            <img
              v-if="editorCoverUrl && !coverFailed"
              :src="editorCoverUrl"
              :alt="
                t('ui.application.cover.alt', {
                  name: form.name || t('ui.application.page.fallbackTitle'),
                })
              "
              @error="coverFailed = true"
            />
            <div
              v-else
              role="img"
              :aria-label="
                t('ui.application.cover.unavailableLabel', {
                  name: form.name || t('ui.application.page.fallbackTitle'),
                })
              "
            >
              <UiIcon name="gamepad" :size="40" aria-hidden="true" />
              <span>{{ t('ui.application.cover.unavailable') }}</span>
            </div>
          </div>

          <div class="editor-grid">
            <label
              v-if="!isPlayniteLinked"
              class="vs-field editor-field editor-field--full"
              for="app-command"
            >
              <span class="vs-field__label">{{ t('apps.cmd') }}</span>
              <textarea
                id="app-command"
                v-model="form.cmd"
                class="vs-textarea vs-monospace"
                rows="3"
                :placeholder="t('ui.application.fields.command.placeholder')"
              />
              <span class="vs-field__helper">
                {{
                  t(
                    commandWasArray
                      ? 'ui.application.fields.command.arrayHelp'
                      : 'ui.application.fields.command.help',
                  )
                }}
              </span>
            </label>

            <label v-if="!isPlayniteLinked" class="vs-field editor-field" for="app-working-dir">
              <span class="vs-field__label">{{ t('apps.working_dir') }}</span>
              <input
                id="app-working-dir"
                v-model="form.workingDir"
                class="vs-input vs-monospace"
                type="text"
              />
            </label>

            <div class="vs-field editor-field editor-field--full">
              <label v-if="!isPlayniteLinked" class="vs-field__label" for="app-image-path">
                {{ t('ui.application.fields.imagePath.label') }}
              </label>
              <span v-else class="vs-field__label">
                {{ t('ui.application.fields.imagePath.label') }}
              </span>
              <div class="editor-cover-control">
                <input
                  v-if="!isPlayniteLinked"
                  id="app-image-path"
                  v-model="form.imagePath"
                  class="vs-input vs-monospace"
                  type="text"
                  :placeholder="t('ui.application.fields.imagePath.placeholder')"
                />
                <AppButton
                  variant="secondary"
                  icon="search"
                  :label="t('ui.application.coverPicker.action')"
                  :disabled="!form.name.trim()"
                  @click="openCoverPicker"
                />
              </div>
              <span class="vs-field__helper">
                {{
                  t(
                    isPlayniteLinked
                      ? 'ui.application.coverPicker.playniteWarning'
                      : 'ui.application.fields.imagePath.help',
                  )
                }}
              </span>
            </div>

            <InlineAlert
              v-if="isPlayniteLinked"
              class="editor-field editor-field--full"
              tone="info"
              :title="t('apps.playnite_badge')"
            >
              {{ t('apps.playnite_edit_notice') }}
            </InlineAlert>
          </div>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="frame-generation-heading">
        <div class="editor-section__heading">
          <h2 id="frame-generation-heading">
            {{ t('ui.application.sections.frameGeneration.title') }}
          </h2>
          <p>{{ t('apps.framegen.kind_hint') }}</p>
        </div>
        <div class="editor-group editor-grid">
          <label class="vs-field editor-field editor-field--full" for="app-frame-mode">
            <span class="vs-field__label">{{ t('ui.application.fields.frameMode.label') }}</span>
            <select id="app-frame-mode" v-model="form.frameGenerationMode" class="vs-select">
              <option
                v-if="optionIsCustom(frameGenerationModes, form.frameGenerationMode)"
                :value="form.frameGenerationMode"
              >
                {{ t('ui.application.options.currentValue', { value: form.frameGenerationMode }) }}
              </option>
              <option
                v-for="option in frameGenerationModes"
                :key="option.value"
                :value="option.value"
              >
                {{ option.label }}
              </option>
            </select>
          </label>

          <div v-if="frameGenerationEnabled" class="framegen-health editor-field--full">
            <div class="framegen-health__heading">
              <div>
                <h3>{{ t('apps.framegen.run_health_check') }}</h3>
                <p>{{ t('apps.framegen.health_prompt') }}</p>
              </div>
              <AppButton
                size="compact"
                icon="refresh"
                :label="t('_common.refresh')"
                :busy="frameGenHealthLoading"
                :busy-label="t('apps.framegen.health_checking')"
                @click="refreshFrameGenHealth"
              />
            </div>

            <InlineAlert
              v-if="frameGenHealthError"
              tone="danger"
              :title="t('apps.framegen.run_health_check')"
            >
              {{ frameGenHealthError }}
            </InlineAlert>
            <InlineAlert
              v-else-if="!frameGenHealth && !frameGenHealthLoading"
              tone="info"
              :title="t('apps.framegen.run_health_check')"
            >
              {{ t('apps.framegen.health_prompt') }}
            </InlineAlert>
            <div v-else-if="frameGenHealth" class="framegen-health__rows">
              <div v-for="row in frameGenHealthRows" :key="row.id" class="framegen-health__row">
                <div>
                  <div class="framegen-health__row-title vs-cluster">
                    <strong>{{ row.label }}</strong>
                    <StatusBadge
                      :label="healthStatusLabel(row.status)"
                      :tone="healthTone(row.status)"
                      compact
                    />
                  </div>
                  <p>{{ row.message }}</p>
                </div>
              </div>
              <div
                v-if="frameGenHealth.display.capabilities.length"
                class="framegen-health__coverage"
              >
                <strong>{{ t('ui.application.framegenHealth.refreshCapabilitiesTitle') }}</strong>
                <span
                  v-for="capability in frameGenHealth.display.capabilities"
                  :key="capability.hz"
                >
                  {{
                    t('ui.application.framegenHealth.refreshCapability', {
                      hz: capability.hz,
                      status:
                        capability.supported === true
                          ? t('apps.framegen.target_supported')
                          : capability.supported === false
                            ? t('apps.framegen.target_unsupported')
                            : t('apps.framegen.target_unknown'),
                    })
                  }}
                </span>
              </div>
              <AppButton
                v-if="frameGenHealth.display.usingVirtual !== true"
                size="compact"
                icon="devices"
                :label="t('apps.framegen.use_virtual_display')"
                @click="enableVirtualDisplayForFrameGen"
              />
            </div>
          </div>
        </div>
      </section>

      <section v-if="showRtxHdrSection" class="editor-section" aria-labelledby="rtx-hdr-heading">
        <div class="editor-section__heading editor-section__heading--rtx-hdr">
          <div>
            <h2 id="rtx-hdr-heading">{{ t('ui.application.rtxHdr.title') }}</h2>
            <p>{{ t('ui.application.rtxHdr.description') }}</p>
          </div>
          <div class="rtx-hdr__heading-meta">
            <StatusBadge :label="rtxHdrModeLabel" :tone="rtxHdrModeTone" compact />
            <StatusBadge
              v-if="liveRtxHdrStatus !== 'idle'"
              :label="rtxHdrLiveStatusLabel"
              :tone="rtxHdrLiveStatusTone"
              compact
              announce="polite"
            />
          </div>
        </div>

        <div class="editor-group rtx-hdr-panel">
          <fieldset class="rtx-hdr__mode-fieldset">
            <legend>{{ t('ui.application.rtxHdr.modeLabel') }}</legend>
            <div class="rtx-hdr__mode-grid">
              <label
                v-for="option in rtxHdrModeOptions"
                :key="option.value"
                class="rtx-hdr__mode"
                :class="{ 'rtx-hdr__mode--active': form.rtxHdrMode === option.value }"
              >
                <input v-model="form.rtxHdrMode" type="radio" :value="option.value" />
                <span class="rtx-hdr__mode-icon" aria-hidden="true">
                  <UiIcon :name="option.icon" :size="18" />
                </span>
                <span class="rtx-hdr__mode-copy">
                  <strong>{{ option.label }}</strong>
                  <span>{{ option.description }}</span>
                </span>
              </label>
            </div>
          </fieldset>

          <div v-if="form.rtxHdrMode === 'enabled'" class="rtx-hdr__calibration">
            <div class="rtx-hdr__calibration-heading">
              <div>
                <h3>{{ t('ui.application.rtxHdr.calibration.title') }}</h3>
                <p>{{ t('ui.application.rtxHdr.calibration.description') }}</p>
              </div>
              <AppButton
                size="compact"
                variant="tertiary"
                icon="refresh"
                :label="t('ui.application.rtxHdr.calibration.reset')"
                @click="resetRtxHdrCalibration"
              />
            </div>

            <SettingRow
              :label="t('ui.application.rtxHdr.calibration.overrideLabel')"
              :description="t('ui.application.rtxHdr.calibration.overrideDescription')"
              control-id="app-rtx-hdr-values"
            >
              <label class="vs-switch">
                <input
                  id="app-rtx-hdr-values"
                  v-model="form.rtxHdrValuesOverride"
                  type="checkbox"
                />
                <span class="vs-switch__track" aria-hidden="true" />
                <span class="vs-sr-only">{{
                  t('ui.application.rtxHdr.calibration.overrideLabel')
                }}</span>
              </label>
            </SettingRow>

            <div v-if="form.rtxHdrValuesOverride" class="rtx-hdr__dials">
              <article
                v-for="field in rtxHdrCalibrationFields"
                :key="field.key"
                class="rtx-hdr__dial"
              >
                <div class="rtx-hdr__dial-heading">
                  <label :for="`app-rtx-hdr-${field.key}`">{{ t(field.labelKey) }}</label>
                  <output :for="`app-rtx-hdr-${field.key}`">
                    {{ rtxHdrCalibrationValue(field.key) }}
                    <span v-if="field.unit">{{ field.unit }}</span>
                  </output>
                </div>
                <input
                  :id="`app-rtx-hdr-${field.key}`"
                  class="rtx-hdr__range"
                  type="range"
                  :min="field.min"
                  :max="field.max"
                  :step="field.step"
                  :value="rtxHdrCalibrationValue(field.key)"
                  :aria-label="t(field.labelKey)"
                  @input="updateRtxHdrCalibrationValue(field.key, field, $event)"
                />
                <div class="rtx-hdr__dial-footer">
                  <span>{{ rtxHdrBoundary(field.min, field) }}</span>
                  <input
                    class="vs-input rtx-hdr__number"
                    type="number"
                    :min="field.min"
                    :max="field.max"
                    :step="field.step"
                    :value="rtxHdrCalibrationValue(field.key)"
                    :aria-label="t(field.labelKey)"
                    @input="updateRtxHdrCalibrationValue(field.key, field, $event)"
                  />
                  <span>{{ rtxHdrBoundary(field.max, field) }}</span>
                </div>
                <p>{{ t(field.descriptionKey) }}</p>
              </article>
            </div>
          </div>

          <InlineAlert
            v-if="liveRtxHdrStatus === 'error'"
            tone="danger"
            :title="t('ui.application.rtxHdr.live.failureTitle')"
            announce="assertive"
          >
            {{ liveRtxHdrError || t('rtx_hdr_live_update_failed') }}
          </InlineAlert>

          <p class="rtx-hdr__live-note">
            <UiIcon name="activity" :size="16" aria-hidden="true" />
            {{ t('ui.application.rtxHdr.liveHint') }}
          </p>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="display-heading">
        <div class="editor-section__heading">
          <h2 id="display-heading">{{ t('ui.application.sections.display.title') }}</h2>
          <p>{{ t('ui.application.sections.display.description') }}</p>
        </div>
        <div class="vs-settings-group">
          <fieldset class="app-display-routing">
            <legend>{{ t('config.app_display_override_label') }}</legend>
            <p>{{ t('config.app_display_override_hint') }}</p>
            <div class="app-display-routing__choices">
              <label
                class="app-display-routing__choice"
                :class="{ 'app-display-routing__choice--active': displaySelection === 'physical' }"
              >
                <input
                  v-model="displaySelection"
                  name="app-display-selection"
                  type="radio"
                  value="physical"
                />
                <span>{{ t('config.app_display_override_physical') }}</span>
              </label>
              <label
                class="app-display-routing__choice"
                :class="{ 'app-display-routing__choice--active': displaySelection === 'virtual' }"
              >
                <input
                  v-model="displaySelection"
                  name="app-display-selection"
                  type="radio"
                  value="virtual"
                />
                <span>{{ t('config.app_display_override_virtual') }}</span>
              </label>
            </div>
          </fieldset>

          <template v-if="displaySelection === 'physical'">
            <SettingRow
              :label="t('config.app_display_physical_label')"
              :description="t('config.app_display_physical_hint')"
              control-id="app-physical-display"
            >
              <div class="app-display-control-stack">
                <select
                  id="app-physical-display"
                  v-model="physicalDisplayOutput"
                  class="vs-select"
                  @focus="loadDisplayDevices()"
                >
                  <option value="">{{ t('config.app_display_physical_placeholder') }}</option>
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
                <AppButton
                  variant="tertiary"
                  size="compact"
                  icon="refresh"
                  icon-only
                  :label="t('_common.refresh')"
                  :busy="displayDevicesLoading"
                  @click="loadDisplayDevices(true)"
                />
                <span v-if="displayDevicesError" class="app-display-control-stack__error">
                  {{ displayDevicesError }}
                </span>
              </div>
            </SettingRow>
            <SettingRow
              :label="t('ui.application.fields.displayAction.label')"
              :description="t('ui.application.fields.displayAction.description')"
              control-id="app-display-configuration"
            >
              <select
                id="app-display-configuration"
                v-model="form.ddConfigurationOption"
                class="vs-select"
              >
                <option
                  v-if="optionIsCustom(displayConfigurationOptions, form.ddConfigurationOption)"
                  :value="form.ddConfigurationOption"
                >
                  {{
                    t('ui.application.options.currentValue', { value: form.ddConfigurationOption })
                  }}
                </option>
                <option
                  v-for="option in displayConfigurationOptions"
                  :key="option.value"
                  :value="option.value"
                >
                  {{ option.label }}
                </option>
              </select>
            </SettingRow>
          </template>

          <template v-else>
            <SettingRow
              :label="t('ui.application.fields.displayMode.label')"
              :description="t('ui.application.fields.displayMode.description')"
              control-id="app-display-mode"
            >
              <select id="app-display-mode" v-model="form.virtualDisplayMode" class="vs-select">
                <option
                  v-if="optionIsCustom(virtualDisplayModes, form.virtualDisplayMode)"
                  :value="form.virtualDisplayMode"
                >
                  {{ t('ui.application.options.currentValue', { value: form.virtualDisplayMode }) }}
                </option>
                <option
                  v-for="option in virtualDisplayModes"
                  :key="option.value"
                  :value="option.value"
                >
                  {{ option.label }}
                </option>
              </select>
            </SettingRow>
            <SettingRow
              :label="t('ui.application.fields.displayLayout.label')"
              :description="t('ui.application.fields.displayLayout.description')"
              control-id="app-display-layout"
            >
              <select id="app-display-layout" v-model="form.virtualDisplayLayout" class="vs-select">
                <option
                  v-if="optionIsCustom(virtualDisplayLayouts, form.virtualDisplayLayout)"
                  :value="form.virtualDisplayLayout"
                >
                  {{
                    t('ui.application.options.currentValue', { value: form.virtualDisplayLayout })
                  }}
                </option>
                <option
                  v-for="option in virtualDisplayLayouts"
                  :key="option.value"
                  :value="option.value"
                >
                  {{ option.label }}
                </option>
              </select>
            </SettingRow>
          </template>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="commands-heading">
        <div class="editor-section__heading editor-section__heading--actions">
          <div>
            <h2 id="commands-heading">{{ t('ui.application.sections.commands.title') }}</h2>
            <p>{{ t('ui.application.sections.commands.description') }}</p>
          </div>
          <AppButton
            icon="plus"
            variant="secondary"
            :label="t('ui.application.actions.addPrep')"
            @click="addPrepCommand"
          />
        </div>
        <div v-if="form.prepCmd.length" class="prep-list">
          <fieldset
            v-for="(entry, index) in form.prepCmd"
            :key="index"
            class="editor-group prep-entry"
          >
            <legend>{{ t('ui.application.prep.legend', { number: index + 1 }) }}</legend>
            <label class="vs-field editor-field" :for="`prep-do-${index}`">
              <span class="vs-field__label">{{ t('ui.application.prep.before') }}</span>
              <input
                :id="`prep-do-${index}`"
                v-model="entry.do"
                class="vs-input vs-monospace"
                type="text"
              />
            </label>
            <label class="vs-field editor-field" :for="`prep-undo-${index}`">
              <span class="vs-field__label">{{ t('ui.application.prep.after') }}</span>
              <input
                :id="`prep-undo-${index}`"
                v-model="entry.undo"
                class="vs-input vs-monospace"
                type="text"
              />
            </label>
            <label class="vs-checkbox prep-entry__elevated">
              <input v-model="entry.elevated" type="checkbox" />
              <span>{{ t('ui.application.prep.elevated') }}</span>
            </label>
            <AppButton
              icon="trash"
              variant="tertiary"
              :label="t('ui.application.prep.remove', { number: index + 1 })"
              @click="removePrepCommand(index)"
            />
          </fieldset>
        </div>
        <div v-else class="editor-empty-row">{{ t('ui.application.prep.empty') }}</div>
        <label class="vs-field editor-field editor-field--full" for="app-detached">
          <span class="vs-field__label">{{ t('apps.detached_cmds') }}</span>
          <textarea
            id="app-detached"
            v-model="form.detachedText"
            class="vs-textarea vs-monospace"
            rows="4"
            :placeholder="t('ui.application.fields.detached.placeholder')"
          />
          <span class="vs-field__helper">{{ t('ui.application.fields.detached.help') }}</span>
        </label>
      </section>

      <section class="editor-section" aria-labelledby="overrides-heading">
        <div class="editor-section__heading">
          <h2 id="overrides-heading">{{ t('apps.overrides.title') }}</h2>
          <p>
            {{
              t('apps.overrides.adjustment_hint', { scope: t('apps.overrides.scope_application') })
            }}
          </p>
        </div>
        <div class="application-overrides" aria-labelledby="overrides-heading">
          <aside class="application-overrides__catalog" aria-labelledby="override-catalog-heading">
            <div class="application-overrides__pane-heading">
              <span class="application-overrides__step" aria-hidden="true">1</span>
              <div>
                <h3 id="override-catalog-heading">{{ t('apps.overrides.browse_available') }}</h3>
                <p>{{ t('apps.overrides.browse_available_hint') }}</p>
              </div>
            </div>

            <div class="application-overrides__catalog-tools">
              <label class="vs-sr-only" for="app-override-search">
                {{ t('apps.overrides.browse_available') }}
              </label>
              <div class="application-overrides__search">
                <UiIcon name="search" :size="16" aria-hidden="true" />
                <input
                  id="app-override-search"
                  v-model="overrideSearch"
                  class="vs-input"
                  type="search"
                  :placeholder="t('apps.overrides.filter_placeholder')"
                />
              </div>
              <span class="application-overrides__result-count">
                {{
                  t('apps.overrides.showing_count', {
                    shown: filteredOverrideCatalogCount,
                    total: overrideCatalogCount,
                  })
                }}
              </span>
            </div>

            <div
              v-if="filteredOverrideCatalogGroups.length"
              class="application-overrides__catalog-list"
            >
              <section
                v-for="category in filteredOverrideCatalogGroups"
                :key="category.id"
                class="application-overrides__catalog-group"
                :aria-labelledby="`override-catalog-${category.id}`"
              >
                <h4 :id="`override-catalog-${category.id}`">{{ category.label }}</h4>
                <button
                  v-for="field in category.fields"
                  :id="`app-override-catalog-${field.key}`"
                  :key="field.key"
                  type="button"
                  class="application-overrides__catalog-item"
                  :class="{
                    'application-overrides__catalog-item--active': overrideIsConfigured(field.key),
                  }"
                  :aria-label="`${overrideIsConfigured(field.key) ? t('_common.active') : t('apps.overrides.add_setting')}: ${overrideLabel(field.key)}`"
                  @click="chooseOverride(field.key)"
                >
                  <span class="application-overrides__catalog-copy">
                    <strong>{{ overrideLabel(field.key) }}</strong>
                    <span v-if="overrideDescription(field.key)">
                      {{ overrideDescription(field.key) }}
                    </span>
                  </span>
                  <span class="application-overrides__catalog-action">
                    <UiIcon
                      :name="overrideIsConfigured(field.key) ? 'check' : 'plus'"
                      :size="14"
                      aria-hidden="true"
                    />
                    {{
                      t(
                        overrideIsConfigured(field.key)
                          ? '_common.active'
                          : 'apps.overrides.add_setting',
                      )
                    }}
                  </span>
                </button>
              </section>
            </div>
            <div v-else class="application-overrides__catalog-empty">
              <strong>{{ t('apps.overrides.no_matching_settings') }}</strong>
              <p>{{ t('apps.overrides.no_matching_settings_hint') }}</p>
              <AppButton
                variant="tertiary"
                size="compact"
                :label="t('_common.clear')"
                @click="overrideSearch = ''"
              />
            </div>
          </aside>

          <div class="application-overrides__editor" aria-labelledby="override-editor-heading">
            <div class="application-overrides__pane-heading application-overrides__editor-heading">
              <span class="application-overrides__step" aria-hidden="true">2</span>
              <div>
                <h3 id="override-editor-heading">{{ t('apps.overrides.active_overrides') }}</h3>
                <p>{{ t('apps.overrides.new_settings_hint') }}</p>
              </div>
              <StatusBadge
                tone="neutral"
                compact
                :label="t('apps.overrides.configured_count', { count: overrideKeys.length })"
              />
            </div>
            <div class="vs-sr-only" aria-live="polite">{{ overrideAnnouncement }}</div>

            <div v-if="overrideKeys.length" class="application-overrides__active-groups">
              <section
                v-for="category in activeOverrideGroups"
                :key="category.id"
                class="application-overrides__active-group"
                :aria-labelledby="`override-active-${category.id}`"
              >
                <h4 :id="`override-active-${category.id}`">{{ category.label }}</h4>
                <div class="vs-settings-group application-overrides__list">
                  <SettingRow
                    v-for="key in category.keys"
                    :key="key"
                    :label="overrideLabel(key)"
                    :control-id="`app-override-${key}`"
                    :stacked="
                      overrideField(key)?.kind === 'textarea' || overrideField(key)?.stacked
                    "
                    :restart-required="overrideField(key)?.restartRequired"
                  >
                    <template #description>
                      <span v-if="overrideDescription(key)">{{ overrideDescription(key) }}</span>
                    </template>

                    <template #default="{ descriptionId }">
                      <div class="application-override__controls">
                        <label v-if="overrideField(key)?.kind === 'boolean'" class="vs-switch">
                          <input
                            :id="`app-override-${key}`"
                            type="checkbox"
                            :checked="overrideBooleanValue(key)"
                            :aria-describedby="descriptionId"
                            @change="updateOverrideFromEvent(key, overrideField(key), $event)"
                          />
                          <span class="vs-switch__track" aria-hidden="true" />
                          <span class="vs-sr-only">{{ overrideLabel(key) }}</span>
                        </label>
                        <select
                          v-else-if="
                            overrideField(key)?.kind === 'select' &&
                            overrideSelectOptions(key).length
                          "
                          :id="`app-override-${key}`"
                          class="vs-select"
                          :value="overrideControlValue(key)"
                          :aria-describedby="descriptionId"
                          @change="updateOverrideFromEvent(key, overrideField(key), $event)"
                        >
                          <option
                            v-for="option in overrideSelectOptions(key)"
                            :key="option.value"
                            :value="option.value"
                          >
                            {{ option.label }}
                          </option>
                        </select>
                        <textarea
                          v-else-if="overrideField(key)?.kind === 'textarea'"
                          :id="`app-override-${key}`"
                          :class="[
                            'vs-textarea',
                            { 'vs-monospace': overrideField(key)?.monospace },
                          ]"
                          :value="String(overrideValue(key) ?? '')"
                          :placeholder="overridePlaceholder(key)"
                          :aria-describedby="descriptionId"
                          rows="4"
                          @input="updateOverrideFromEvent(key, overrideField(key), $event)"
                        />
                        <input
                          v-else
                          :id="`app-override-${key}`"
                          :class="['vs-input', { 'vs-monospace': overrideField(key)?.monospace }]"
                          :type="overrideField(key)?.kind === 'number' ? 'number' : 'text'"
                          :min="overrideField(key)?.min"
                          :max="overrideField(key)?.max"
                          :step="overrideField(key)?.step"
                          :value="String(overrideValue(key) ?? '')"
                          :placeholder="overridePlaceholder(key)"
                          :aria-describedby="descriptionId"
                          @input="updateOverrideFromEvent(key, overrideField(key), $event)"
                        />
                        <AppButton
                          variant="tertiary"
                          size="compact"
                          icon="trash"
                          icon-only
                          :label="t('_common.remove')"
                          :aria-label="`${t('_common.remove')}: ${overrideLabel(key)}`"
                          @click="removeOverride(key)"
                        />
                      </div>
                    </template>
                  </SettingRow>
                </div>
              </section>
            </div>
            <div v-else class="application-overrides__empty">
              <span class="application-overrides__empty-icon" aria-hidden="true">
                <UiIcon name="settings" :size="20" />
              </span>
              <strong>{{ t('apps.overrides.empty_picker') }}</strong>
              <p>{{ t('apps.overrides.new_settings_hint') }}</p>
            </div>
          </div>
        </div>
      </section>

      <section v-if="!isNew" class="editor-danger" aria-labelledby="danger-heading">
        <div>
          <h2 id="danger-heading">{{ t('ui.application.delete.sectionTitle') }}</h2>
          <p>{{ t('ui.application.delete.sectionDescription') }}</p>
        </div>
        <AppButton
          variant="danger"
          icon="trash"
          :label="t('ui.application.delete.action')"
          @click="
            deleteError = '';
            deleteOpen = true;
          "
        />
      </section>

      <div class="editor-save-bar" role="region" :aria-label="t('ui.application.saveBar.region')">
        <div>
          <strong>
            {{ t(isDirty ? 'ui.application.saveBar.unsaved' : 'ui.application.saveBar.current') }}
          </strong>
          <span>
            {{ t(isNew ? 'ui.application.saveBar.addHint' : 'ui.application.saveBar.editHint') }}
          </span>
        </div>
        <div class="editor-save-bar__actions">
          <AppButton
            variant="secondary"
            :label="t('_common.cancel')"
            :disabled="saving"
            @click="cancel"
          />
          <AppButton
            type="submit"
            variant="primary"
            icon="check"
            :label="t('ui.application.actions.save')"
            :busy="saving"
            :busy-label="t('ui.application.actions.saving')"
            :disabled="!isDirty && !isNew"
          />
        </div>
      </div>
    </form>

    <AppEditCoverModal
      v-model:open="coverPickerOpen"
      v-model:query="coverSearchQuery"
      :searching="coverSearching"
      :busy="coverBusy"
      :candidates="coverCandidates"
      :error="coverError"
      :playnite-managed="isPlayniteLinked"
      @search="runCoverSearch"
      @pick="chooseCover"
    />

    <ConfirmDialog
      v-model:open="deleteOpen"
      :title="
        t('ui.application.delete.dialogTitle', {
          name: form.name || t('ui.application.delete.fallbackName'),
        })
      "
      :description="t('ui.application.delete.dialogDescription')"
      :confirm-label="t('ui.application.delete.action')"
      :cancel-label="t('_common.cancel')"
      :busy-label="t('ui.application.delete.busy')"
      tone="danger"
      :busy="deleting"
      :close-on-confirm="false"
      @confirm="confirmDelete"
    >
      <InlineAlert
        v-if="deleteError"
        tone="danger"
        :title="t('ui.application.delete.errorTitle')"
        announce="assertive"
      >
        {{ deleteError }}
      </InlineAlert>
    </ConfirmDialog>
  </div>
</template>

<style scoped>
.application-page,
.editor-form,
.editor-section,
.editor-section__heading,
.editor-loading {
  display: grid;
}

.application-page {
  gap: var(--vs-space-20);
  padding-block-end: calc(var(--vs-space-80) * 1.5);
}

.application-page :deep(.vs-page-header) {
  padding-block-end: 0;
}

.editor-form,
.editor-loading {
  gap: var(--vs-space-32);
}

.editor-section {
  gap: var(--vs-space-12);
}

.app-display-routing {
  min-inline-size: 0;
  padding: 0;
  border: 0;
}

.app-display-routing legend {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.app-display-routing p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.app-display-routing__choices {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12);
  margin-block-start: var(--vs-space-12);
}

.app-display-routing__choice {
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
  min-block-size: 3rem;
  padding: var(--vs-space-12) var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  cursor: pointer;
}

.app-display-routing__choice--active {
  border-color: var(--vs-color-accent-default);
  background: color-mix(in srgb, var(--vs-color-accent-default) 8%, var(--vs-color-bg-raised));
}

.app-display-control-stack {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.app-display-control-stack > .vs-select {
  flex: 1 1 16rem;
  min-inline-size: 0;
}

.app-display-control-stack__error {
  flex-basis: 100%;
  color: var(--vs-color-status-danger);
  font-size: var(--vs-type-size-helper);
}

.editor-section__heading {
  gap: var(--vs-space-4);
}

.editor-section__heading h2,
.editor-danger h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.editor-section__heading p,
.editor-danger p {
  color: var(--vs-color-text-secondary);
}

.editor-section__heading--actions,
.editor-danger,
.editor-save-bar,
.editor-save-bar__actions {
  display: flex;
  align-items: center;
}

.editor-section__heading--actions,
.editor-danger,
.editor-save-bar {
  justify-content: space-between;
  gap: var(--vs-space-24);
}

.editor-group {
  padding: var(--vs-space-20);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.editor-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-20);
}

.editor-name-control,
.editor-cover-control,
.editor-playnite-link,
.framegen-health__heading,
.framegen-health__row-title,
.framegen-health__coverage {
  display: flex;
  align-items: center;
}

.editor-name-control {
  gap: var(--vs-space-8);
}

.editor-name-control .vs-input,
.editor-cover-control .vs-input {
  min-inline-size: 0;
  flex: 1;
}

.editor-cover-control {
  gap: var(--vs-space-8);
}

.editor-playnite-picker {
  overflow: auto;
  max-block-size: 17rem;
  border: var(--vs-border-width) solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  box-shadow: var(--vs-shadow-raised);
}

.editor-playnite-option {
  display: flex;
  inline-size: 100%;
  align-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-10) var(--vs-space-12);
  border: 0;
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: transparent;
  color: var(--vs-color-text-primary);
  font: inherit;
  text-align: start;
  cursor: pointer;
}

.editor-playnite-option:last-of-type {
  border-block-end: 0;
}

.editor-playnite-option:hover,
.editor-playnite-option--active {
  background: var(--vs-color-bg-subtle);
}

.editor-playnite-option:focus-visible {
  position: relative;
  z-index: 1;
  outline: var(--vs-focus-width) solid var(--vs-color-focus);
  outline-offset: calc(var(--vs-focus-width) * -1);
}

.editor-playnite-picker__notice {
  margin: 0;
  padding: var(--vs-space-12);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.editor-playnite-link {
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.editor-field {
  align-content: start;
}

.editor-field--full,
.editor-field--wide {
  grid-column: 1 / -1;
}

.editor-execution-layout {
  display: grid;
  grid-template-columns: minmax(10rem, 13rem) minmax(0, 1fr);
  gap: var(--vs-space-24);
}

.editor-execution-layout--simple {
  grid-template-columns: minmax(0, 1fr);
}

.editor-execution-layout > .editor-grid {
  padding: 0;
  border: 0;
  background: transparent;
}

.editor-artwork {
  aspect-ratio: 2 / 3;
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-subtle);
}

.editor-artwork img {
  inline-size: 100%;
  block-size: 100%;
  object-fit: cover;
}

.editor-artwork > div {
  display: grid;
  block-size: 100%;
  place-content: center;
  place-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-16);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  text-align: center;
}

.framegen-health {
  display: grid;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.framegen-health__heading {
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.framegen-health__heading h3,
.framegen-health__coverage strong {
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.framegen-health__heading p,
.framegen-health__row p {
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.framegen-health__rows {
  display: grid;
  gap: var(--vs-space-8);
}

.framegen-health__row {
  padding: var(--vs-space-12);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
}

.framegen-health__row-title {
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.framegen-health__coverage {
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) var(--vs-space-12);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.framegen-health__coverage strong {
  color: var(--vs-color-text-primary);
}

.editor-section__heading--rtx-hdr {
  align-items: flex-start;
}

.rtx-hdr__heading-meta {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

.rtx-hdr-panel {
  display: grid;
  gap: var(--vs-space-20);
}

.rtx-hdr__mode-fieldset {
  min-inline-size: 0;
  padding: 0;
  margin: 0;
  border: 0;
}

.rtx-hdr__mode-fieldset legend {
  margin-block-end: var(--vs-space-12);
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.rtx-hdr__mode-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.rtx-hdr__mode {
  position: relative;
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  align-items: start;
  gap: var(--vs-space-12);
  min-inline-size: 0;
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  cursor: pointer;
  transition:
    border-color 120ms ease,
    background-color 120ms ease,
    box-shadow 120ms ease;
}

.rtx-hdr__mode:hover {
  border-color: var(--vs-color-border-strong);
}

.rtx-hdr__mode--active {
  border-color: var(--vs-color-accent-default);
  background: color-mix(in srgb, var(--vs-color-accent-default) 8%, var(--vs-color-bg-raised));
  box-shadow: inset 0 0 0 var(--vs-border-width) var(--vs-color-accent-default);
}

.rtx-hdr__mode input {
  position: absolute;
  inline-size: 1px;
  block-size: 1px;
  opacity: 0;
}

.rtx-hdr__mode input:focus-visible + .rtx-hdr__mode-icon {
  outline: var(--vs-focus-width) solid var(--vs-color-focus);
  outline-offset: var(--vs-space-4);
}

.rtx-hdr__mode-icon {
  display: grid;
  inline-size: 2rem;
  block-size: 2rem;
  place-items: center;
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-text-muted);
}

.rtx-hdr__mode--active .rtx-hdr__mode-icon {
  background: color-mix(in srgb, var(--vs-color-accent-default) 16%, transparent);
  color: var(--vs-color-accent-default);
}

.rtx-hdr__mode-copy {
  display: grid;
  min-inline-size: 0;
  gap: var(--vs-space-4);
}

.rtx-hdr__mode-copy strong {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  line-height: var(--vs-type-line-height-control);
}

.rtx-hdr__mode-copy span {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-helper);
}

.rtx-hdr__calibration {
  display: grid;
  gap: var(--vs-space-20);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.rtx-hdr__calibration-heading,
.rtx-hdr__dial-heading,
.rtx-hdr__dial-footer {
  display: flex;
  align-items: center;
}

.rtx-hdr__calibration-heading {
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.rtx-hdr__calibration-heading h3 {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  line-height: var(--vs-type-line-height-control);
}

.rtx-hdr__calibration-heading p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.rtx-hdr__dials {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.rtx-hdr__dial {
  display: grid;
  gap: var(--vs-space-10);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
}

.rtx-hdr__dial-heading {
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.rtx-hdr__dial-heading label {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.rtx-hdr__dial-heading output {
  color: var(--vs-color-accent-default);
  font-size: var(--vs-type-size-control);
  font-variant-numeric: tabular-nums;
  font-weight: var(--vs-type-weight-semibold);
  white-space: nowrap;
}

.rtx-hdr__dial-heading output span {
  margin-inline-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-regular);
}

.rtx-hdr__range {
  inline-size: 100%;
  margin: var(--vs-space-4) 0;
  accent-color: var(--vs-color-accent-default);
}

.rtx-hdr__dial-footer {
  justify-content: space-between;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
  font-variant-numeric: tabular-nums;
}

.rtx-hdr__number {
  inline-size: 5.5rem;
  text-align: end;
}

.rtx-hdr__dial p {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-helper);
}

.rtx-hdr__live-note {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.rtx-hdr__live-note > svg {
  flex: 0 0 auto;
  margin-block-start: 0.1rem;
  color: var(--vs-color-accent-default);
}

.prep-list {
  display: grid;
  gap: var(--vs-space-12);
}

.prep-entry {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  align-items: end;
  gap: var(--vs-space-16);
}

.prep-entry legend {
  padding-inline: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
}

.prep-entry__elevated {
  align-self: center;
}

.prep-entry > .vs-button {
  justify-self: end;
}

.editor-empty-row {
  padding: var(--vs-space-24);
  border: var(--vs-border-width) dashed var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  color: var(--vs-color-text-secondary);
  text-align: center;
}

.application-overrides {
  display: grid;
  overflow: hidden;
  grid-template-columns: minmax(18rem, 0.82fr) minmax(0, 1.18fr);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.application-overrides__catalog,
.application-overrides__editor {
  min-inline-size: 0;
}

.application-overrides__catalog {
  border-inline-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-subtle);
}

.application-overrides__pane-heading {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  align-items: start;
  gap: var(--vs-space-12);
  min-block-size: 5.5rem;
  padding: var(--vs-space-16) var(--vs-space-20);
}

.application-overrides__editor-heading {
  grid-template-columns: auto minmax(0, 1fr) auto;
}

.application-overrides__pane-heading h3 {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  line-height: var(--vs-type-line-height-control);
}

.application-overrides__pane-heading p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.application-overrides__step {
  display: inline-flex;
  inline-size: 1.75rem;
  block-size: 1.75rem;
  align-items: center;
  justify-content: center;
  border-radius: 999px;
  background: var(--vs-color-accent-default);
  color: var(--vs-color-text-on-accent);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
}

.application-overrides__catalog-tools {
  display: grid;
  gap: var(--vs-space-8);
  padding: 0 var(--vs-space-16) var(--vs-space-16);
}

.application-overrides__search {
  position: relative;
}

.application-overrides__search > svg {
  position: absolute;
  z-index: 1;
  inset-inline-start: var(--vs-space-12);
  inset-block-start: 50%;
  color: var(--vs-color-text-muted);
  pointer-events: none;
  transform: translateY(-50%);
}

.application-overrides__search .vs-input {
  inline-size: 100%;
  padding-inline-start: 2.25rem;
}

.application-overrides__result-count {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}

.application-overrides__catalog-list {
  max-block-size: 42rem;
  overflow-y: auto;
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__catalog-group h4,
.application-overrides__active-group > h4 {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
  letter-spacing: 0.04em;
  text-transform: uppercase;
}

.application-overrides__catalog-group h4 {
  position: sticky;
  z-index: 1;
  inset-block-start: 0;
  padding: var(--vs-space-8) var(--vs-space-16);
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-raised);
}

.application-overrides__catalog-group + .application-overrides__catalog-group h4 {
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__catalog-item {
  display: grid;
  inline-size: 100%;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-12);
  padding: var(--vs-space-12) var(--vs-space-16);
  border: 0;
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: transparent;
  color: inherit;
  text-align: start;
  cursor: pointer;
}

.application-overrides__catalog-item:hover,
.application-overrides__catalog-item:focus-visible {
  background: var(--vs-color-bg-surface);
}

.application-overrides__catalog-item:focus-visible {
  outline: 2px solid var(--vs-color-accent-default);
  outline-offset: -2px;
}

.application-overrides__catalog-item--active {
  background: color-mix(in srgb, var(--vs-color-accent-default) 7%, transparent);
}

.application-overrides__catalog-copy {
  display: grid;
  min-inline-size: 0;
  gap: var(--vs-space-4);
}

.application-overrides__catalog-copy strong {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
  line-height: var(--vs-type-line-height-control);
}

.application-overrides__catalog-copy > span {
  display: -webkit-box;
  overflow: hidden;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
  -webkit-box-orient: vertical;
  -webkit-line-clamp: 2;
}

.application-overrides__catalog-action {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  color: var(--vs-color-accent-default);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
  white-space: nowrap;
}

.application-overrides__catalog-item--active .application-overrides__catalog-action {
  color: var(--vs-color-status-success);
}

.application-overrides__catalog-empty {
  display: grid;
  justify-items: start;
  gap: var(--vs-space-8);
  padding: var(--vs-space-24) var(--vs-space-16);
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__catalog-empty p {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.application-overrides__editor {
  overflow: hidden;
  background: var(--vs-color-bg-surface);
}

.application-overrides__active-groups {
  display: grid;
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__active-group > h4 {
  padding: var(--vs-space-8) var(--vs-space-20);
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-subtle);
}

.application-overrides__active-group + .application-overrides__active-group > h4 {
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__list {
  border: 0;
  border-radius: 0;
}

.application-overrides__list :deep(.vs-setting-row__control) {
  min-inline-size: min(100%, 20rem);
}

.application-override__controls {
  display: flex;
  min-inline-size: 0;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

.application-override__controls > .vs-input,
.application-override__controls > .vs-select,
.application-override__controls > .vs-textarea {
  inline-size: min(100%, 18rem);
}

.application-override__controls > .vs-textarea {
  inline-size: min(100%, 30rem);
  resize: vertical;
}

.application-overrides__empty {
  display: grid;
  justify-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-32) var(--vs-space-20);
  min-block-size: 18rem;
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
  text-align: center;
}

.application-overrides__empty-icon {
  display: inline-flex;
  inline-size: 3rem;
  block-size: 3rem;
  align-items: center;
  justify-content: center;
  margin-block-end: var(--vs-space-4);
  border-radius: 999px;
  background: color-mix(in srgb, var(--vs-color-accent-default) 12%, transparent);
  color: var(--vs-color-accent-default);
}

.application-overrides__empty strong {
  color: var(--vs-color-text-primary);
}

.application-overrides__empty p {
  max-inline-size: 38rem;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.editor-danger {
  padding: var(--vs-space-20);
  border: var(--vs-border-width) solid var(--vs-color-status-danger);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.editor-danger p {
  margin-block-start: var(--vs-space-4);
}

.editor-save-bar {
  position: fixed;
  z-index: 12;
  inset-inline-start: calc(var(--vs-navigation-width-expanded) + var(--vs-space-32));
  inset-inline-end: var(--vs-space-32);
  inset-block-end: var(--vs-space-24);
  max-inline-size: var(--vs-content-width-general);
  padding: var(--vs-space-12) var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-raised);
  box-shadow: var(--vs-shadow-overlay);
}

.editor-save-bar strong,
.editor-save-bar span {
  display: block;
}

.editor-save-bar span {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.editor-save-bar__actions {
  gap: var(--vs-space-8);
}

.editor-error-list {
  padding-inline-start: var(--vs-space-20);
}

@media (max-width: 1023px) {
  .application-overrides {
    grid-template-columns: minmax(0, 1fr);
  }

  .rtx-hdr__mode-grid {
    grid-template-columns: minmax(0, 1fr);
  }

  .application-overrides__catalog {
    border-inline-end: 0;
    border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  }

  .application-overrides__catalog-list {
    max-block-size: 28rem;
  }

  .editor-save-bar {
    inset-inline-start: calc(var(--vs-navigation-width-collapsed) + var(--vs-space-24));
    inset-inline-end: var(--vs-space-24);
  }
}

@media (max-width: 767px) {
  .editor-grid,
  .prep-entry,
  .editor-execution-layout,
  .app-display-routing__choices {
    grid-template-columns: minmax(0, 1fr);
  }

  .editor-artwork {
    inline-size: min(100%, 11rem);
  }

  .rtx-hdr__dials {
    grid-template-columns: minmax(0, 1fr);
  }

  .editor-section__heading--actions,
  .editor-danger,
  .editor-save-bar,
  .editor-name-control,
  .editor-cover-control,
  .framegen-health__heading,
  .rtx-hdr__calibration-heading {
    align-items: stretch;
    flex-direction: column;
  }

  .editor-section__heading--actions .vs-button,
  .editor-danger .vs-button,
  .editor-name-control .vs-button,
  .editor-cover-control .vs-button,
  .framegen-health__heading .vs-button {
    align-self: flex-start;
  }

  .application-overrides__list :deep(.vs-setting-row__control),
  .application-override__controls,
  .application-override__controls > .vs-input,
  .application-override__controls > .vs-select,
  .application-override__controls > .vs-textarea {
    inline-size: 100%;
    min-inline-size: 0;
  }

  .application-override__controls > .vs-button {
    flex: 0 0 auto;
  }

  .application-override__controls {
    justify-content: flex-start;
  }

  .application-overrides__editor-heading {
    grid-template-columns: auto minmax(0, 1fr);
  }

  .application-overrides__editor-heading .vs-status-badge {
    grid-column: 2;
    justify-self: start;
  }

  .editor-save-bar {
    inset: auto 0 0;
    max-inline-size: none;
    border-inline: 0;
    border-block-end: 0;
    border-radius: 0;
    padding-block-end: calc(var(--vs-space-12) + env(safe-area-inset-bottom));
  }

  .editor-save-bar__actions > * {
    flex: 1;
  }
}
</style>
