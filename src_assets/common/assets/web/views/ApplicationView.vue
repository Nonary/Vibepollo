<script setup lang="ts">
import { computed, nextTick, reactive, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute, useRouter } from 'vue-router';

import { ApiError, apiGet } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  SettingRow,
  StatusBadge,
  UiIcon,
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
  prepCmd: PrepEntry[];
  detachedText: string;
  configOverridesJson: string;
  advancedJson: string;
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

type FrameGenRequirementStatus = 'pass' | 'configured' | 'warn' | 'fail' | 'unknown';

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
const { t } = useI18n();
const loading = ref(true);
const saving = ref(false);
const deleting = ref(false);
const loadError = ref('');
const saveError = ref('');
const initialSnapshot = ref('');
const sourceApp = ref<AppRecord | null>(null);
const commandWasArray = ref(false);
const coverFailed = ref(false);
const deleteOpen = ref(false);
const deleteError = ref('');
const errors = reactive<Record<string, string>>({});
const advancedOpen = ref(false);
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
  {
    value: 'extended_primary',
    label: t('ui.application.options.displayLayout.extendedPrimary'),
  },
  {
    value: 'extended_isolated',
    label: t('ui.application.options.displayLayout.extendedIsolated'),
  },
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
  {
    value: 'ensure_only_display',
    label: t('ui.application.options.displayAction.ensureOnly'),
  },
]);
const frameGenerationModes = computed<SelectOption[]>(() => [
  { value: '', label: t('ui.application.options.hostDefault') },
  { value: 'off', label: t('ui.application.options.frameMode.off') },
  { value: 'lossless-scaling', label: t('ui.application.options.frameMode.lossless') },
  { value: 'nvidia-smooth-motion', label: t('ui.application.options.frameMode.nvidia') },
  { value: 'game-provided', label: t('ui.application.options.frameMode.game') },
]);
const losslessProfiles = computed<SelectOption[]>(() => [
  { value: '', label: t('ui.application.options.hostDefault') },
  { value: 'recommended', label: t('ui.application.options.profile.recommended') },
  { value: 'custom', label: t('ui.application.options.profile.custom') },
]);
const isPlayniteLinked = computed(() => Boolean(form.playniteId.trim()));
const filteredPlayniteGames = computed(() => {
  const query = form.name.trim().toLocaleLowerCase();
  const games = playniteGames.value.filter((game) => game.installed !== false);
  return query
    ? games.filter((game) => game.name.toLocaleLowerCase().includes(query))
    : games;
});
const frameGenerationEnabled = computed(() => {
  if (form.frameGenerationMode === 'off') return false;
  return Boolean(form.frameGenerationMode) || form.gen1FramegenFix || form.gen2FramegenFix;
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
    prepCmd: [],
    detachedText: '',
    configOverridesJson: '{}',
    advancedJson: '{}',
  };
}

const routeId = computed(() => (typeof route.params.id === 'string' ? route.params.id : ''));
const isNew = computed(() => route.name === 'application-new' || !routeId.value);
const pageTitle = computed(() =>
  isNew.value ? t('ui.application.page.addTitle') : form.name || t('ui.application.page.fallbackTitle'),
);
const isDirty = computed(
  () => isNew.value || (Boolean(initialSnapshot.value) && JSON.stringify(form) !== initialSnapshot.value),
);
const errorMessages = computed(() => Object.values(errors));
const sourceCoverUrl = computed(() => (sourceApp.value ? appCoverUrl(sourceApp.value) : ''));

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
  if (
    provider === 'lossless-scaling' &&
    asBoolean(app['lossless-scaling-framegen'])
  ) {
    return provider;
  }
  if (['nvidia-smooth-motion', 'game-provided'].includes(provider)) return provider;
  return asBoolean(app['lossless-scaling-framegen']) ? 'lossless-scaling' : '';
}

function hasAdvancedConfiguration(): boolean {
  return Boolean(
    form.output ||
      form.displayOutput ||
      form.imagePath ||
      form.playniteIconPath ||
      form.elevated ||
      form.autoDetach ||
      form.waitAll ||
      form.excludeGlobalPrepCmd ||
      form.exitTimeout ||
      form.virtualScreen ||
      form.virtualDisplayMode ||
      form.virtualDisplayLayout ||
      form.ddConfigurationOption ||
      form.gen1FramegenFix ||
      form.gen2FramegenFix ||
      form.frameGenLimiterFix ||
      form.losslessScalingEnabled ||
      form.losslessScalingTargetFps ||
      form.losslessScalingRtssLimit ||
      form.losslessScalingProfile ||
      form.losslessScalingLaunchDelay ||
      form.prepCmd.length ||
      form.detachedText.trim() ||
      jsonHasEntries(form.configOverridesJson) ||
      jsonHasEntries(form.advancedJson),
  );
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

function jsonHasEntries(text: string): boolean {
  try {
    const value = JSON.parse(text || '{}') as unknown;
    return Boolean(value && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).length);
  } catch {
    return Boolean(text.trim() && text.trim() !== '{}');
  }
}

function prepEntry(value: unknown): PrepEntry {
  const source = value && typeof value === 'object' && !Array.isArray(value)
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
    Object.entries(app).filter(
      ([key]) => !editableKeys.has(key) && !transientKeys.has(key),
    ),
  );

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
    prepCmd: Array.isArray(app['prep-cmd']) ? app['prep-cmd'].map(prepEntry) : [],
    detachedText: Array.isArray(app.detached)
      ? app.detached.filter((value): value is string => typeof value === 'string').join('\n')
      : '',
    configOverridesJson: jsonText(app['config-overrides']),
    advancedJson: jsonText(unknown),
  } satisfies EditorForm);
  coverFailed.value = false;
  clearErrors();
  initialSnapshot.value = JSON.stringify(form);
  advancedOpen.value = hasAdvancedConfiguration();
  cancelPlayniteClose();
  playnitePickerOpen.value = false;
  playniteActiveIndex.value = -1;
  clearFrameGenHealth();
  endFormSynchronizationDeferral(synchronizationEpoch);
}

function hydrateNew(): void {
  const synchronizationEpoch = beginFormSynchronizationDeferral();
  const next = emptyForm();
  Object.assign(form, next);
  sourceApp.value = null;
  commandWasArray.value = false;
  coverFailed.value = true;
  clearErrors();
  initialSnapshot.value = JSON.stringify(form);
  advancedOpen.value = false;
  cancelPlayniteClose();
  playnitePickerOpen.value = false;
  playniteActiveIndex.value = -1;
  clearFrameGenHealth();
  endFormSynchronizationDeferral(synchronizationEpoch);
}

async function load(): Promise<void> {
  loading.value = true;
  loadError.value = '';
  saveError.value = '';
  if (isNew.value) {
    hydrateNew();
    loading.value = false;
    return;
  }

  try {
    const appList = await fetchApps();
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

function parseObject(text: string, key: string, labelKey: string): Record<string, unknown> | null {
  const label = t(labelKey);
  try {
    const value = JSON.parse(text || '{}') as unknown;
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
      errors[key] = t('ui.application.validation.jsonObject', { label });
      return null;
    }
    return value as Record<string, unknown>;
  } catch {
    errors[key] = t('ui.application.validation.invalidJson', { label });
    return null;
  }
}

function validateInteger(key: string, labelKey: string, value: string, minimum: number): void {
  if (!value.trim()) return;
  const number = Number(value);
  if (!Number.isInteger(number) || number < minimum) {
    errors[key] = t('ui.application.validation.integerMinimum', {
      label: t(labelKey),
      minimum,
    });
  }
}

async function validate(): Promise<boolean> {
  clearErrors();
  if (!form.name.trim()) errors.name = t('ui.application.validation.nameRequired');
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(form.uuid)) {
    errors.uuid = t('ui.application.validation.uuidInvalid');
  }
  validateInteger('exitTimeout', 'apps.exit_timeout', form.exitTimeout, 0);
  validateInteger(
    'targetFps',
    'ui.application.fields.losslessTarget.label',
    form.losslessScalingTargetFps,
    1,
  );
  validateInteger(
    'rtssLimit',
    'ui.application.fields.losslessRtss.label',
    form.losslessScalingRtssLimit,
    0,
  );
  validateInteger(
    'launchDelay',
    'ui.application.fields.losslessDelay.label',
    form.losslessScalingLaunchDelay,
    0,
  );
  parseObject(
    form.configOverridesJson,
    'configOverrides',
    'ui.application.fields.configOverrides.label',
  );
  parseObject(form.advancedJson, 'advanced', 'ui.application.fields.advanced.label');

  const invalidPrep = form.prepCmd.findIndex(
    (entry) => !entry.do.trim() && !entry.undo.trim() && Object.keys(entry.extras).length > 0,
  );
  if (invalidPrep >= 0) {
    errors.prep = t('ui.application.validation.prepMissingCommand', {
      number: invalidPrep + 1,
    });
  }

  const firstKey = Object.keys(errors)[0];
  if (!firstKey) return true;
  if (['exitTimeout', 'targetFps', 'rtssLimit', 'launchDelay', 'configOverrides', 'advanced', 'prep'].includes(firstKey)) {
    advancedOpen.value = true;
  }
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
  const configOverrides = JSON.parse(form.configOverridesJson || '{}') as Record<string, unknown>;
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

function updateAdvancedOpen(event: Event): void {
  advancedOpen.value = (event.currentTarget as HTMLDetailsElement).open;
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
  form.playniteIconPath = '';
  playnitePickerOpen.value = false;
  playniteActiveIndex.value = -1;
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

function healthTone(status: FrameGenRequirementStatus): 'success' | 'warning' | 'danger' | 'neutral' {
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
  return Number.isFinite(numerator) && Number.isFinite(denominator) && numerator > 0 && denominator > 0
    ? numerator / denominator
    : null;
}

function parseRefreshRates(raw: unknown): number[] {
  const source = Array.isArray(raw) ? raw : raw === undefined || raw === null ? [] : [raw];
  return [...new Set(source.map(parseRefreshHz).filter((value): value is number => value !== null))].sort(
    (left, right) => left - right,
  );
}

function normalizedDeviceId(value: unknown): string {
  return asString(value).trim().toLocaleLowerCase();
}

function isVirtualDisplaySelection(value: string): boolean {
  const normalized = value.trim().toLocaleLowerCase();
  return normalized === 'sunshine:virtual_display' || normalized === 'sunshine:sudovda_virtual_display';
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
  const candidates = (resolution.hasAppOutput
    ? [resolution.output]
    : [resolution.output, asString(config.output_name)])
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
  const target = matchingTarget ?? (resolution.hasAppOutput ? undefined : devices.find((device) => Boolean(device.info)) ?? devices[0]);

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
        const maximums = [parseRefreshHz(edid.max_vertical_hz), parseRefreshHz(edid.max_timing_hz)].filter(
          (value): value is number => value !== null,
        );
        edidMaximum = maximums.length ? Math.max(...maximums) : null;
        if (Array.isArray(edid.targets)) {
          for (const rawTarget of edid.targets) {
            if (!rawTarget || typeof rawTarget !== 'object') continue;
            const entry = rawTarget as EdidTarget;
            const hz = parseRefreshHz(entry.hz);
            if (hz !== null) {
              capabilityMap.set(
                hz,
                typeof entry.supported === 'boolean' ? entry.supported : null,
              );
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
    edidMaximum ??
    (supportedRates.length ? supportedRates[supportedRates.length - 1] : currentHz);

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

function enableVirtualDisplayForFrameGen(): void {
  if (effectiveAppOutput() && !isVirtualDisplaySelection(effectiveAppOutput())) {
    form.output = '';
    form.displayOutput = '';
  }
  form.virtualScreen = true;
  form.virtualDisplayMode = 'per_client';
  advancedOpen.value = true;
}

async function submit(): Promise<void> {
  if (saving.value) return;
  saveError.value = '';
  if (!(await validate())) return;
  saving.value = true;
  try {
    const payload = buildPayload();
    await saveApp(payload);
    await fetchApps().catch(() => []);
    await router.push({ name: 'library' });
  } catch (cause) {
    saveError.value = localizedError(cause, 'ui.application.errors.save');
  } finally {
    saving.value = false;
  }
}

function cancel(): void {
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

watch([routeId, () => route.name], () => void load(), { immediate: true });
</script>

<template>
  <div class="vs-page vs-page--settings application-page">
    <PageHeader
      :title="pageTitle"
      :description="
        t(
          isNew
            ? 'ui.application.page.addDescription'
            : 'ui.application.page.editDescription',
        )
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
        <AppButton
          variant="secondary"
          :label="t('ui.application.actions.back')"
          @click="cancel"
        />
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

    <form v-else-if="!loadError" id="application-form" class="editor-form" novalidate @submit.prevent="submit">
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
                :aria-activedescendant="isNew && playniteActiveIndex >= 0 ? `app-playnite-option-${playniteActiveIndex}` : undefined"
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
            <span v-if="errors.name" id="app-name-error" class="vs-field__error">{{ errors.name }}</span>

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
            <span v-if="errors.uuid" id="app-uuid-error" class="vs-field__error">{{ errors.uuid }}</span>
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
          :class="{ 'editor-execution-layout--simple': isNew && !sourceCoverUrl }"
        >
          <div
            v-if="!isNew || sourceCoverUrl"
            class="editor-artwork"
            :class="{ 'editor-artwork--empty': coverFailed || !sourceCoverUrl }"
          >
            <img
              v-if="sourceCoverUrl && !coverFailed"
              :src="sourceCoverUrl"
              :alt="t('ui.application.cover.alt', { name: form.name || t('ui.application.page.fallbackTitle') })"
              @error="coverFailed = true"
            />
            <div
              v-else
              role="img"
              :aria-label="t('ui.application.cover.unavailableLabel', { name: form.name || t('ui.application.page.fallbackTitle') })"
            >
              <UiIcon name="gamepad" :size="40" aria-hidden="true" />
              <span>{{ t('ui.application.cover.unavailable') }}</span>
            </div>
          </div>

          <div class="editor-grid">
            <label v-if="!isPlayniteLinked" class="vs-field editor-field editor-field--full" for="app-command">
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
              <input id="app-working-dir" v-model="form.workingDir" class="vs-input vs-monospace" type="text" />
            </label>

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
          <h2 id="frame-generation-heading">{{ t('ui.application.sections.frameGeneration.title') }}</h2>
          <p>{{ t('apps.framegen.kind_hint') }}</p>
        </div>
        <div class="editor-group editor-grid">
          <label class="vs-field editor-field editor-field--full" for="app-frame-mode">
            <span class="vs-field__label">{{ t('ui.application.fields.frameMode.label') }}</span>
            <select id="app-frame-mode" v-model="form.frameGenerationMode" class="vs-select">
              <option v-if="optionIsCustom(frameGenerationModes, form.frameGenerationMode)" :value="form.frameGenerationMode">
                {{ t('ui.application.options.currentValue', { value: form.frameGenerationMode }) }}
              </option>
              <option v-for="option in frameGenerationModes" :key="option.value" :value="option.value">{{ option.label }}</option>
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

            <InlineAlert v-if="frameGenHealthError" tone="danger" :title="t('apps.framegen.run_health_check')">
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
                    <StatusBadge :label="healthStatusLabel(row.status)" :tone="healthTone(row.status)" compact />
                  </div>
                  <p>{{ row.message }}</p>
                </div>
              </div>
              <div v-if="frameGenHealth.display.capabilities.length" class="framegen-health__coverage">
                <strong>{{ t('ui.application.framegenHealth.refreshCapabilitiesTitle') }}</strong>
                <span v-for="capability in frameGenHealth.display.capabilities" :key="capability.hz">
                  {{ t('ui.application.framegenHealth.refreshCapability', {
                    hz: capability.hz,
                    status: capability.supported === true
                      ? t('apps.framegen.target_supported')
                      : capability.supported === false
                        ? t('apps.framegen.target_unsupported')
                        : t('apps.framegen.target_unknown'),
                  }) }}
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

      <details class="editor-disclosure" :open="advancedOpen" @toggle="updateAdvancedOpen">
        <summary>
          <span>
            <UiIcon name="settings" :size="18" aria-hidden="true" />
            {{ t('ui.application.advanced.summary') }}
          </span>
          <UiIcon class="editor-disclosure__chevron" name="chevron-down" :size="18" aria-hidden="true" />
        </summary>
        <p class="editor-disclosure__description">{{ t('ui.application.advanced.description') }}</p>
        <div class="editor-disclosure__content">

      <section class="editor-section" aria-labelledby="launch-heading">
        <div class="editor-section__heading">
          <h2 id="launch-heading">{{ t('ui.application.sections.launch.title') }}</h2>
          <p>{{ t('ui.application.sections.launch.description') }}</p>
        </div>
        <div class="vs-settings-group">
          <SettingRow
            :label="t('ui.application.fields.elevated.label')"
            :description="t('ui.application.fields.elevated.description')"
            control-id="app-elevated"
          >
            <label class="vs-switch">
              <input id="app-elevated" v-model="form.elevated" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('ui.application.fields.elevated.label') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.autoDetach.label')"
            :description="t('ui.application.fields.autoDetach.description')"
            control-id="app-auto-detach"
          >
            <label class="vs-switch">
              <input id="app-auto-detach" v-model="form.autoDetach" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('ui.application.fields.autoDetach.label') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.waitAll.label')"
            :description="t('ui.application.fields.waitAll.description')"
            control-id="app-wait-all"
          >
            <label class="vs-switch">
              <input id="app-wait-all" v-model="form.waitAll" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('ui.application.fields.waitAll.label') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.excludePrep.label')"
            :description="t('ui.application.fields.excludePrep.description')"
            control-id="app-exclude-prep"
          >
            <label class="vs-switch">
              <input id="app-exclude-prep" v-model="form.excludeGlobalPrepCmd" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('ui.application.fields.excludePrep.label') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('apps.exit_timeout')"
            :description="t('ui.application.fields.exitTimeout.description')"
            control-id="app-exit-timeout"
          >
            <div class="editor-inline-control">
              <input
                id="app-exit-timeout"
                v-model="form.exitTimeout"
                class="vs-input"
                data-field-key="exitTimeout"
                type="number"
                min="0"
                step="1"
                inputmode="numeric"
                :aria-invalid="Boolean(errors.exitTimeout)"
                :aria-describedby="errors.exitTimeout ? 'app-exit-timeout-error' : undefined"
              />
              <span aria-hidden="true">{{ t('_common.seconds') }}</span>
              <span v-if="errors.exitTimeout" id="app-exit-timeout-error" class="vs-field__error">{{ errors.exitTimeout }}</span>
            </div>
          </SettingRow>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="display-heading">
        <div class="editor-section__heading">
          <h2 id="display-heading">{{ t('ui.application.sections.display.title') }}</h2>
          <p>{{ t('ui.application.sections.display.description') }}</p>
        </div>
        <div class="vs-settings-group">
          <SettingRow
            :label="t('ui.application.fields.virtualScreen.label')"
            :description="t('ui.application.fields.virtualScreen.description')"
            control-id="app-virtual-screen"
          >
            <label class="vs-switch">
              <input id="app-virtual-screen" v-model="form.virtualScreen" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('ui.application.fields.virtualScreen.label') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.displayMode.label')"
            :description="t('ui.application.fields.displayMode.description')"
            control-id="app-display-mode"
          >
            <select id="app-display-mode" v-model="form.virtualDisplayMode" class="vs-select">
              <option v-if="optionIsCustom(virtualDisplayModes, form.virtualDisplayMode)" :value="form.virtualDisplayMode">
                {{ t('ui.application.options.currentValue', { value: form.virtualDisplayMode }) }}
              </option>
              <option v-for="option in virtualDisplayModes" :key="option.value" :value="option.value">{{ option.label }}</option>
            </select>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.displayLayout.label')"
            :description="t('ui.application.fields.displayLayout.description')"
            control-id="app-display-layout"
          >
            <select id="app-display-layout" v-model="form.virtualDisplayLayout" class="vs-select">
              <option v-if="optionIsCustom(virtualDisplayLayouts, form.virtualDisplayLayout)" :value="form.virtualDisplayLayout">
                {{ t('ui.application.options.currentValue', { value: form.virtualDisplayLayout }) }}
              </option>
              <option v-for="option in virtualDisplayLayouts" :key="option.value" :value="option.value">{{ option.label }}</option>
            </select>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.displayAction.label')"
            :description="t('ui.application.fields.displayAction.description')"
            control-id="app-display-configuration"
          >
            <select id="app-display-configuration" v-model="form.ddConfigurationOption" class="vs-select">
              <option v-if="optionIsCustom(displayConfigurationOptions, form.ddConfigurationOption)" :value="form.ddConfigurationOption">
                {{ t('ui.application.options.currentValue', { value: form.ddConfigurationOption }) }}
              </option>
              <option v-for="option in displayConfigurationOptions" :key="option.value" :value="option.value">{{ option.label }}</option>
            </select>
          </SettingRow>
        </div>
        <div class="editor-group editor-grid editor-subgroup">
          <label class="vs-field editor-field" for="app-output">
            <span class="vs-field__label">{{ t('ui.application.fields.output.label') }}</span>
            <input id="app-output" v-model="form.output" class="vs-input vs-monospace" type="text" />
          </label>
          <label class="vs-field editor-field" for="app-display-output">
            <span class="vs-field__label">{{ t('ui.application.fields.displayOutput.label') }}</span>
            <input id="app-display-output" v-model="form.displayOutput" class="vs-input vs-monospace" type="text" />
          </label>
          <label class="vs-field editor-field" for="app-image-path">
            <span class="vs-field__label">{{ t('ui.application.fields.imagePath.label') }}</span>
            <input id="app-image-path" v-model="form.imagePath" class="vs-input vs-monospace" type="text" />
          </label>
          <label class="vs-field editor-field" for="app-icon-path">
            <span class="vs-field__label">{{ t('ui.application.fields.iconPath.label') }}</span>
            <input id="app-icon-path" v-model="form.playniteIconPath" class="vs-input vs-monospace" type="text" />
          </label>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="frame-generation-tuning-heading">
        <div class="editor-section__heading">
          <h2 id="frame-generation-tuning-heading">
            {{ t('ui.application.sections.frameGeneration.title') }}
          </h2>
          <p>{{ t('ui.application.sections.frameGeneration.description') }}</p>
        </div>
        <div class="vs-settings-group editor-subgroup">
          <SettingRow
            :label="t('ui.application.fields.gen1Fix.label')"
            :description="t('ui.application.fields.gen1Fix.description')"
            control-id="app-gen1-fix"
          >
            <label class="vs-switch"><input id="app-gen1-fix" v-model="form.gen1FramegenFix" type="checkbox" /><span class="vs-switch__track" aria-hidden="true" /><span class="vs-sr-only">{{ t('ui.application.fields.gen1Fix.label') }}</span></label>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.gen2Fix.label')"
            :description="t('ui.application.fields.gen2Fix.description')"
            control-id="app-gen2-fix"
          >
            <label class="vs-switch"><input id="app-gen2-fix" v-model="form.gen2FramegenFix" type="checkbox" /><span class="vs-switch__track" aria-hidden="true" /><span class="vs-sr-only">{{ t('ui.application.fields.gen2Fix.label') }}</span></label>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.limiterFix.label')"
            :description="t('ui.application.fields.limiterFix.description')"
            control-id="app-limiter-fix"
          >
            <label class="vs-switch"><input id="app-limiter-fix" v-model="form.frameGenLimiterFix" type="checkbox" /><span class="vs-switch__track" aria-hidden="true" /><span class="vs-sr-only">{{ t('ui.application.fields.limiterFix.label') }}</span></label>
          </SettingRow>
          <SettingRow
            :label="t('ui.application.fields.losslessEnabled.label')"
            :description="t('ui.application.fields.losslessEnabled.description')"
            control-id="app-lossless-enabled"
          >
            <label class="vs-switch"><input id="app-lossless-enabled" v-model="form.losslessScalingEnabled" type="checkbox" /><span class="vs-switch__track" aria-hidden="true" /><span class="vs-sr-only">{{ t('ui.application.fields.losslessEnabled.label') }}</span></label>
          </SettingRow>
        </div>
        <div class="editor-group editor-grid editor-subgroup">
          <label class="vs-field editor-field" for="app-lossless-target">
            <span class="vs-field__label">
              {{ t('ui.application.fields.losslessTarget.label') }}
            </span>
            <input id="app-lossless-target" v-model="form.losslessScalingTargetFps" class="vs-input" data-field-key="targetFps" type="number" min="1" step="1" inputmode="numeric" :aria-invalid="Boolean(errors.targetFps)" :aria-describedby="errors.targetFps ? 'app-lossless-target-error' : undefined" />
            <span v-if="errors.targetFps" id="app-lossless-target-error" class="vs-field__error">{{ errors.targetFps }}</span>
          </label>
          <label class="vs-field editor-field" for="app-lossless-rtss">
            <span class="vs-field__label">
              {{ t('ui.application.fields.losslessRtss.label') }}
            </span>
            <input id="app-lossless-rtss" v-model="form.losslessScalingRtssLimit" class="vs-input" data-field-key="rtssLimit" type="number" min="0" step="1" inputmode="numeric" :aria-invalid="Boolean(errors.rtssLimit)" :aria-describedby="errors.rtssLimit ? 'app-lossless-rtss-error' : undefined" />
            <span v-if="errors.rtssLimit" id="app-lossless-rtss-error" class="vs-field__error">{{ errors.rtssLimit }}</span>
          </label>
          <label class="vs-field editor-field" for="app-lossless-profile">
            <span class="vs-field__label">{{ t('apps.framegen.profile_label') }}</span>
            <select id="app-lossless-profile" v-model="form.losslessScalingProfile" class="vs-select">
              <option v-if="optionIsCustom(losslessProfiles, form.losslessScalingProfile)" :value="form.losslessScalingProfile">{{ t('ui.application.options.currentValue', { value: form.losslessScalingProfile }) }}</option>
              <option v-for="option in losslessProfiles" :key="option.value" :value="option.value">{{ option.label }}</option>
            </select>
          </label>
          <label class="vs-field editor-field" for="app-lossless-delay">
            <span class="vs-field__label">
              {{ t('ui.application.fields.losslessDelay.label') }}
            </span>
            <input id="app-lossless-delay" v-model="form.losslessScalingLaunchDelay" class="vs-input" data-field-key="launchDelay" type="number" min="0" step="1" inputmode="numeric" :aria-invalid="Boolean(errors.launchDelay)" :aria-describedby="errors.launchDelay ? 'app-lossless-delay-error' : undefined" />
            <span class="vs-field__helper">
              {{ t('ui.application.fields.losslessDelay.help') }}
            </span>
            <span v-if="errors.launchDelay" id="app-lossless-delay-error" class="vs-field__error">{{ errors.launchDelay }}</span>
          </label>
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
        <InlineAlert
          v-if="errors.prep"
          tone="danger"
          :title="t('ui.application.alerts.prepAttention')"
        >
          {{ errors.prep }}
        </InlineAlert>
        <div v-if="form.prepCmd.length" class="prep-list">
          <fieldset v-for="(entry, index) in form.prepCmd" :key="index" class="editor-group prep-entry">
            <legend>
              {{ t('ui.application.prep.legend', { number: index + 1 }) }}
            </legend>
            <label class="vs-field editor-field" :for="`prep-do-${index}`">
              <span class="vs-field__label">{{ t('ui.application.prep.before') }}</span>
              <input :id="`prep-do-${index}`" v-model="entry.do" class="vs-input vs-monospace" :data-field-key="errors.prep ? 'prep' : undefined" type="text" />
            </label>
            <label class="vs-field editor-field" :for="`prep-undo-${index}`">
              <span class="vs-field__label">{{ t('ui.application.prep.after') }}</span>
              <input :id="`prep-undo-${index}`" v-model="entry.undo" class="vs-input vs-monospace" type="text" />
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
        <div v-else class="editor-empty-row">
          {{ t('ui.application.prep.empty') }}
        </div>
        <div class="editor-group editor-subgroup">
          <label class="vs-field editor-field editor-field--full" for="app-detached">
            <span class="vs-field__label">{{ t('apps.detached_cmds') }}</span>
            <textarea
              id="app-detached"
              v-model="form.detachedText"
              class="vs-textarea vs-monospace"
              rows="5"
              :placeholder="t('ui.application.fields.detached.placeholder')"
            />
            <span class="vs-field__helper">
              {{ t('ui.application.fields.detached.help') }}
            </span>
          </label>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="overrides-heading">
        <div class="editor-section__heading">
          <h2 id="overrides-heading">{{ t('ui.application.sections.overrides.title') }}</h2>
          <p>{{ t('ui.application.sections.overrides.description') }}</p>
        </div>
        <div class="editor-group">
          <label class="vs-field editor-field editor-field--full" for="app-config-overrides">
            <span class="vs-field__label">
              {{ t('ui.application.fields.configOverrides.label') }}
            </span>
            <textarea
              id="app-config-overrides"
              v-model="form.configOverridesJson"
              class="vs-textarea vs-monospace editor-json"
              data-field-key="configOverrides"
              rows="10"
              spellcheck="false"
              :aria-invalid="Boolean(errors.configOverrides)"
              :aria-describedby="errors.configOverrides ? 'app-config-overrides-error' : 'app-config-overrides-help'"
            />
            <span id="app-config-overrides-help" class="vs-field__helper">
              {{ t('ui.application.fields.configOverrides.help') }}
            </span>
            <span v-if="errors.configOverrides" id="app-config-overrides-error" class="vs-field__error">{{ errors.configOverrides }}</span>
          </label>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="advanced-heading">
        <div class="editor-section__heading">
          <h2 id="advanced-heading">{{ t('ui.application.sections.advanced.title') }}</h2>
          <p>{{ t('ui.application.sections.advanced.description') }}</p>
        </div>
        <InlineAlert tone="warning" :title="t('ui.application.alerts.expertTitle')">
          {{ t('ui.application.alerts.expertDescription') }}
        </InlineAlert>
        <div class="editor-group editor-subgroup">
          <label class="vs-field editor-field editor-field--full" for="app-advanced-json">
            <span class="vs-field__label">{{ t('ui.application.fields.advanced.label') }}</span>
            <textarea
              id="app-advanced-json"
              v-model="form.advancedJson"
              class="vs-textarea vs-monospace editor-json"
              data-field-key="advanced"
              rows="12"
              spellcheck="false"
              :aria-invalid="Boolean(errors.advanced)"
              :aria-describedby="errors.advanced ? 'app-advanced-json-error' : 'app-advanced-json-help'"
            />
            <span id="app-advanced-json-help" class="vs-field__helper">
              {{ t('ui.application.fields.advanced.help') }}
            </span>
            <span v-if="errors.advanced" id="app-advanced-json-error" class="vs-field__error">{{ errors.advanced }}</span>
          </label>
        </div>
      </section>
        </div>
      </details>

      <section v-if="!isNew" class="editor-danger" aria-labelledby="danger-heading">
        <div>
          <h2 id="danger-heading">{{ t('ui.application.delete.sectionTitle') }}</h2>
          <p>{{ t('ui.application.delete.sectionDescription') }}</p>
        </div>
        <AppButton
          variant="danger"
          icon="trash"
          :label="t('ui.application.delete.action')"
          @click="deleteError = ''; deleteOpen = true"
        />
      </section>

      <div
        class="editor-save-bar"
        role="region"
        :aria-label="t('ui.application.saveBar.region')"
      >
        <div>
          <strong>
            {{
              t(
                isDirty
                  ? 'ui.application.saveBar.unsaved'
                  : 'ui.application.saveBar.current',
              )
            }}
          </strong>
          <span>
            {{
              t(
                isNew
                  ? 'ui.application.saveBar.addHint'
                  : 'ui.application.saveBar.editHint',
              )
            }}
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

    <ConfirmDialog
      v-model:open="deleteOpen"
      :title="t('ui.application.delete.dialogTitle', { name: form.name || t('ui.application.delete.fallbackName') })"
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
.editor-save-bar__actions,
.editor-inline-control {
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

.editor-name-control .vs-input {
  min-inline-size: 0;
  flex: 1;
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

.editor-inline-control {
  min-inline-size: min(100%, 15rem);
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
}

.editor-inline-control .vs-input {
  inline-size: 7rem;
}

.editor-inline-control .vs-field__error {
  flex-basis: 100%;
  text-align: end;
}

.editor-subgroup {
  margin-block-start: var(--vs-space-12);
}

.editor-disclosure {
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.editor-disclosure > summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16) var(--vs-space-20);
  color: var(--vs-color-text-primary);
  font-weight: var(--vs-type-weight-semibold);
  cursor: pointer;
  list-style: none;
}

.editor-disclosure > summary::-webkit-details-marker {
  display: none;
}

.editor-disclosure > summary > span {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-8);
}

.editor-disclosure > summary:hover {
  background: var(--vs-color-bg-subtle);
}

.editor-disclosure__chevron {
  transition: rotate var(--vs-motion-duration-fast) var(--vs-motion-easing-standard);
}

.editor-disclosure[open] .editor-disclosure__chevron {
  rotate: 180deg;
}

.editor-disclosure__description {
  margin: 0;
  padding: 0 var(--vs-space-20) var(--vs-space-20);
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.editor-disclosure__content {
  display: grid;
  gap: var(--vs-space-32);
  padding: var(--vs-space-24) var(--vs-space-20);
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

.editor-json {
  min-block-size: 14rem;
  tab-size: 2;
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
  max-inline-size: var(--vs-content-width-settings);
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
  .editor-save-bar {
    inset-inline-start: calc(var(--vs-navigation-width-collapsed) + var(--vs-space-24));
    inset-inline-end: var(--vs-space-24);
  }
}

@media (max-width: 767px) {
  .editor-grid,
  .prep-entry,
  .editor-execution-layout {
    grid-template-columns: minmax(0, 1fr);
  }

  .editor-artwork {
    inline-size: min(100%, 11rem);
  }

  .editor-section__heading--actions,
  .editor-danger,
  .editor-save-bar,
  .editor-name-control,
  .framegen-health__heading {
    align-items: stretch;
    flex-direction: column;
  }

  .editor-section__heading--actions .vs-button,
  .editor-danger .vs-button,
  .editor-name-control .vs-button,
  .framegen-health__heading .vs-button {
    align-self: flex-start;
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
