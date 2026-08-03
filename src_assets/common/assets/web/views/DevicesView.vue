<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, toRaw } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiGet, apiPost } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  EmptyState,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
  UiIcon,
  type StatusTone,
} from '@/components/ui';
import ClientSettingsEditor, {
  type ClientDeviceDraft,
  type ClientSettingsMetadata,
  type DisplayDevice,
  type HdrProfileEntry,
} from '@/components/devices/ClientSettingsEditor.vue';
import { settingsDefaults } from '@/configs/settingsSchema';
import { formatRelativeTime } from '@/utils/format';

const { locale, t } = useI18n();

interface PairedDevice {
  name: string;
  uuid: string;
  connected: boolean;
  perm?: number | string;
  enable_legacy_ordering?: boolean;
  allow_client_commands?: boolean;
  do?: Array<{ cmd?: string; elevated?: boolean }>;
  undo?: Array<{ cmd?: string; elevated?: boolean }>;
  last_seen?: number | string;
  hdr_profile?: string | null;
  display_mode?: string;
  output_name_override?: string;
  virtual_display_mode?: string;
  virtual_display_layout?: string;
  always_use_virtual_display?: boolean;
  prefer_10bit_sdr?: boolean;
  config_overrides?: Record<string, unknown>;
}

interface ClientsResponse {
  named_certs?: PairedDevice[];
  status?: boolean;
  platform?: string;
}

interface MutationResponse {
  status?: boolean;
}

interface ClientCommandEntry {
  command: string;
  elevated?: boolean;
}

interface PendingAction {
  kind: 'disconnect' | 'unpair';
  device: PairedDevice;
}

const devices = ref<PairedDevice[]>([]);
const drafts = ref<Record<string, ClientDeviceDraft>>({});
const draftOrigins = ref<Record<string, ClientDeviceDraft>>({});
const commonSettings = ref<Record<string, unknown>>({});
const metadata = ref<ClientSettingsMetadata>({});
const platform = ref('');
const commonSettingsError = ref('');
const displayDevices = ref<DisplayDevice[]>([]);
const displayDevicesLoading = ref(false);
const displayDevicesLoaded = ref(false);
const displayDevicesError = ref('');
const hdrProfiles = ref<HdrProfileEntry[]>([]);
const hdrProfilesLoading = ref(false);
const hdrProfilesLoaded = ref(false);
const hdrProfilesError = ref('');
const query = ref('');
const loading = ref(true);
const refreshing = ref(false);
const error = ref('');
const notice = ref('');
const busyUuid = ref('');
const openEditors = ref<Set<string>>(new Set());
const pendingAction = ref<PendingAction | null>(null);
const confirmOpen = ref(false);
let refreshTimer: number | undefined;

const filteredDevices = computed(() => {
  const needle = query.value.trim().toLocaleLowerCase(locale.value);
  if (!needle) return devices.value;
  return devices.value.filter((device) =>
    [device.name, device.uuid].some((value) =>
      value.toLocaleLowerCase(locale.value).includes(needle),
    ),
  );
});

const PERMISSION_VIEW = 0x02000000;
const PERMISSION_LAUNCH = 0x04000000;
const PERMISSION_ALL = 0x071f1f00;

function permissionMask(device: PairedDevice): number {
  const parsed = Number(device.perm ?? 0);
  return Number.isFinite(parsed) ? parsed & PERMISSION_ALL : 0;
}

function canViewStream(device: PairedDevice): boolean {
  return Boolean(permissionMask(device) & (PERMISSION_VIEW | PERMISSION_LAUNCH));
}

const deviceCounts = computed(() => ({
  streaming: devices.value.filter((device) => device.connected && canViewStream(device)).length,
  blocked: devices.value.filter((device) => !canViewStream(device)).length,
  offline: devices.value.filter((device) => !device.connected && canViewStream(device)).length,
}));

const confirmTitle = computed(() => {
  if (!pendingAction.value) return t('ui.devices.confirm.generic_title');
  return pendingAction.value.kind === 'unpair'
    ? t('clients.confirm_remove_title_named', { name: pendingAction.value.device.name })
    : t('ui.devices.confirm.disconnect_title', { name: pendingAction.value.device.name });
});

const confirmDescription = computed(() => {
  if (!pendingAction.value) return '';
  return pendingAction.value.kind === 'unpair'
    ? t('clients.confirm_remove_message_named', { name: pendingAction.value.device.name })
    : t('ui.devices.confirm.disconnect_description');
});

function reconcileStable(current: PairedDevice[], incoming: PairedDevice[]): PairedDevice[] {
  const byUuid = new Map(incoming.map((device) => [device.uuid, device]));
  const stable = current.flatMap((device) => {
    const replacement = byUuid.get(device.uuid);
    if (!replacement) return [];
    byUuid.delete(device.uuid);
    return [replacement];
  });
  return [...stable, ...byUuid.values()];
}

function cloneDraft(value: ClientDeviceDraft): ClientDeviceDraft {
  return {
    ...value,
    configOverrides: structuredClone(toRaw(value.configOverrides ?? {})),
    doCommands: structuredClone(toRaw(value.doCommands ?? [])),
    undoCommands: structuredClone(toRaw(value.undoCommands ?? [])),
  };
}

function normalizeCommands(value: unknown): ClientCommandEntry[] {
  if (!Array.isArray(value)) return [];
  return value.flatMap((entry) => {
    if (!entry || typeof entry !== 'object') return [];
    const command = String((entry as Record<string, unknown>).cmd ?? '').trim();
    if (!command) return [];
    return [{ command, elevated: Boolean((entry as Record<string, unknown>).elevated) }];
  });
}

function normalizeVirtualMode(value: unknown): ClientDeviceDraft['virtualDisplayMode'] {
  const mode = String(value ?? '')
    .trim()
    .toLocaleLowerCase();
  return ['global', 'per_client', 'shared', 'disabled'].includes(mode)
    ? (mode as ClientDeviceDraft['virtualDisplayMode'])
    : null;
}

function normalizeVirtualLayout(value: unknown): ClientDeviceDraft['virtualDisplayLayout'] {
  const layout = String(value ?? '')
    .trim()
    .toLocaleLowerCase();
  return [
    'exclusive',
    'extended',
    'extended_primary',
    'extended_isolated',
    'extended_primary_isolated',
  ].includes(layout)
    ? (layout as ClientDeviceDraft['virtualDisplayLayout'])
    : null;
}

function draftFromDevice(device: PairedDevice): ClientDeviceDraft {
  const virtualDisplayMode = normalizeVirtualMode(device.virtual_display_mode);
  const virtualDisplayLayout = normalizeVirtualLayout(device.virtual_display_layout);
  const physicalOutputOverride = device.output_name_override?.trim() || null;
  const displayOverrideEnabled =
    Boolean(device.always_use_virtual_display) ||
    Boolean(physicalOutputOverride) ||
    virtualDisplayMode !== null ||
    virtualDisplayLayout !== null;
  const displaySelection =
    Boolean(device.always_use_virtual_display) ||
    (virtualDisplayMode !== null && virtualDisplayMode !== 'disabled')
      ? 'virtual'
      : 'physical';

  return {
    name: device.name,
    permissions: permissionMask(device),
    enableLegacyOrdering: device.enable_legacy_ordering !== false,
    allowClientCommands: device.allow_client_commands !== false,
    doCommands: normalizeCommands(device.do),
    undoCommands: normalizeCommands(device.undo),
    displayMode: device.display_mode ?? '',
    displayOverrideEnabled,
    displaySelection,
    physicalOutputOverride,
    virtualDisplayMode,
    virtualDisplayLayout,
    hdrProfile: device.hdr_profile?.trim() ?? '',
    prefer10BitSdr: Boolean(device.prefer_10bit_sdr),
    configOverrides:
      device.config_overrides && typeof device.config_overrides === 'object'
        ? structuredClone(toRaw(device.config_overrides))
        : {},
  };
}

function serializedDraft(value: ClientDeviceDraft): string {
  return JSON.stringify({
    ...value,
    configOverrides: Object.fromEntries(
      Object.entries(value.configOverrides ?? {}).sort(([left], [right]) =>
        left.localeCompare(right),
      ),
    ),
  });
}

function sameDraft(left: ClientDeviceDraft, right: ClientDeviceDraft): boolean {
  return serializedDraft(left) === serializedDraft(right);
}

function syncDrafts(incoming: PairedDevice[]): void {
  const uuids = new Set(incoming.map((device) => device.uuid));
  for (const device of incoming) {
    const nextDraft = draftFromDevice(device);
    const currentDraft = drafts.value[device.uuid];
    const origin = draftOrigins.value[device.uuid];
    if (!currentDraft || !origin || sameDraft(currentDraft, origin)) {
      drafts.value[device.uuid] = cloneDraft(nextDraft);
      draftOrigins.value[device.uuid] = cloneDraft(nextDraft);
    }
  }

  for (const uuid of Object.keys(drafts.value)) {
    if (!uuids.has(uuid)) delete drafts.value[uuid];
  }
  for (const uuid of Object.keys(draftOrigins.value)) {
    if (!uuids.has(uuid)) delete draftOrigins.value[uuid];
  }
  openEditors.value = new Set([...openEditors.value].filter((uuid) => uuids.has(uuid)));
}

function resetDraft(device: PairedDevice): void {
  const nextDraft = draftFromDevice(device);
  drafts.value[device.uuid] = cloneDraft(nextDraft);
  draftOrigins.value[device.uuid] = cloneDraft(nextDraft);
}

async function loadCommonSettings(): Promise<void> {
  commonSettingsError.value = '';
  try {
    const [configResponse, metadataResponse] = await Promise.all([
      apiGet<Record<string, unknown>>('/api/config'),
      apiGet<ClientSettingsMetadata>('/api/metadata'),
    ]);
    const configured = Object.fromEntries(
      Object.entries(configResponse).filter(([key]) => key !== 'status'),
    );
    commonSettings.value = { ...settingsDefaults, ...configured };
    metadata.value = { ...metadataResponse, platform: metadataResponse.platform || platform.value };
  } catch {
    commonSettingsError.value = t('ui.devices.common_settings_unavailable');
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
    displayDevicesError.value = t('config.display_devices_load_failed');
  } finally {
    displayDevicesLoading.value = false;
  }
}

async function loadHdrProfiles(force = false): Promise<void> {
  if (hdrProfilesLoading.value || (hdrProfilesLoaded.value && !force)) return;
  hdrProfilesLoading.value = true;
  hdrProfilesError.value = '';
  try {
    const response = await apiGet<{ status?: boolean; profiles?: HdrProfileEntry[] }>(
      '/api/clients/hdr-profiles',
    );
    if (response.status === false) throw new Error('rejected');
    hdrProfiles.value = Array.isArray(response.profiles) ? response.profiles : [];
    hdrProfilesLoaded.value = true;
  } catch {
    hdrProfilesError.value = t('clients.hdr_profile_load_failed');
  } finally {
    hdrProfilesLoading.value = false;
  }
}

async function loadDevices(silent = false): Promise<void> {
  if (refreshing.value) return;
  refreshing.value = true;
  if (!silent) error.value = '';
  try {
    const response = await apiGet<ClientsResponse>('/api/clients/list');
    if (response.status === false) throw new Error(t('ui.devices.error.list_rejected'));
    const incoming = Array.isArray(response.named_certs) ? response.named_certs : [];
    platform.value = response.platform ?? platform.value;
    if (!metadata.value.platform && platform.value) {
      metadata.value = { ...metadata.value, platform: platform.value };
    }
    syncDrafts(incoming);
    devices.value = reconcileStable(devices.value, incoming);
    if (!silent) notice.value = '';
  } catch (cause) {
    error.value =
      cause instanceof ApiError
        ? t('ui.devices.error.load')
        : cause instanceof Error
          ? cause.message
          : t('ui.devices.error.load');
  } finally {
    refreshing.value = false;
    loading.value = false;
  }
}

function statusFor(device: PairedDevice): { label: string; tone: StatusTone } {
  if (!canViewStream(device)) return { label: t('ui.devices.status.blocked'), tone: 'danger' };
  if (device.connected) return { label: t('ui.devices.status.streaming'), tone: 'success' };
  return { label: t('clients.offline'), tone: 'neutral' };
}

function lastSeen(device: PairedDevice): string {
  if (device.connected) return t('ui.devices.last_seen.active_now');
  if (device.last_seen === undefined || device.last_seen === null) {
    return t('ui.devices.last_seen.never');
  }
  const value = Number(device.last_seen);
  if (!Number.isFinite(value)) return t('_common.unknown');
  return formatRelativeTime(
    value < 1_000_000_000_000 ? value * 1000 : value,
    locale.value,
    t('_common.unknown'),
  );
}

function hasCustomSettings(device: PairedDevice): boolean {
  return Boolean(
    device.display_mode ||
      device.output_name_override ||
      device.virtual_display_mode ||
      device.virtual_display_layout ||
      device.always_use_virtual_display ||
      device.hdr_profile ||
      device.prefer_10bit_sdr ||
      Object.keys(device.config_overrides ?? {}).length,
  );
}

function editorIsOpen(uuid: string): boolean {
  return openEditors.value.has(uuid);
}

function toggleEditor(uuid: string): void {
  const next = new Set(openEditors.value);
  if (next.has(uuid)) {
    next.delete(uuid);
  } else {
    next.clear();
    next.add(uuid);
    void loadDisplayDevices();
    void loadHdrProfiles();
  }
  openEditors.value = next;
}

function cleanOverrides(overrides: Record<string, unknown>): Record<string, unknown> {
  return Object.fromEntries(
    Object.entries(overrides).filter(
      ([key, value]) => key.trim() && value !== undefined && value !== null,
    ),
  );
}

function updatePayload(device: PairedDevice, draft: ClientDeviceDraft): Record<string, unknown> {
  const useVirtualDisplay = draft.displayOverrideEnabled && draft.displaySelection === 'virtual';
  const outputName =
    draft.displayOverrideEnabled && draft.displaySelection === 'physical'
      ? (draft.physicalOutputOverride?.trim() ?? '')
      : '';
  const payload: Record<string, unknown> = {
    uuid: device.uuid,
    name: draft.name.trim(),
    perm: draft.permissions & PERMISSION_ALL,
    enable_legacy_ordering: draft.enableLegacyOrdering,
    allow_client_commands: draft.allowClientCommands,
    do: draft.doCommands.map((entry) => ({ cmd: entry.command.trim(), elevated: entry.elevated })),
    undo: draft.undoCommands.map((entry) => ({ cmd: entry.command.trim(), elevated: entry.elevated })),
    display_mode: draft.displayMode.trim(),
    output_name_override: outputName,
    always_use_virtual_display:
      useVirtualDisplay &&
      draft.virtualDisplayMode !== 'global' &&
      draft.virtualDisplayMode !== null,
    virtual_display_mode: outputName
      ? 'disabled'
      : useVirtualDisplay
        ? draft.virtualDisplayMode === 'global' || draft.virtualDisplayMode === null
          ? 'global'
          : draft.virtualDisplayMode
        : '',
    virtual_display_layout: useVirtualDisplay ? (draft.virtualDisplayLayout ?? '') : '',
    prefer_10bit_sdr: draft.prefer10BitSdr,
    hdr_profile: draft.hdrProfile.trim(),
    config_overrides: cleanOverrides(draft.configOverrides),
  };
  return payload;
}

const draftScalarKeys = [
  'name',
  'permissions',
  'enableLegacyOrdering',
  'allowClientCommands',
  'displayMode',
  'displayOverrideEnabled',
  'displaySelection',
  'physicalOutputOverride',
  'virtualDisplayMode',
  'virtualDisplayLayout',
  'hdrProfile',
  'prefer10BitSdr',
] as const satisfies ReadonlyArray<Exclude<keyof ClientDeviceDraft, 'configOverrides'>>;

function serializedValue(value: unknown): string {
  return JSON.stringify(value);
}

function mergeLatestDraft(
  origin: ClientDeviceDraft,
  local: ClientDeviceDraft,
  latest: ClientDeviceDraft,
): { draft?: ClientDeviceDraft; conflict: boolean } {
  const merged = cloneDraft(latest);
  for (const key of draftScalarKeys) {
    const localChanged = serializedValue(local[key]) !== serializedValue(origin[key]);
    const latestChanged = serializedValue(latest[key]) !== serializedValue(origin[key]);
    if (
      localChanged &&
      latestChanged &&
      serializedValue(local[key]) !== serializedValue(latest[key])
    ) {
      return { conflict: true };
    }
    if (localChanged) Object.assign(merged, { [key]: local[key] });
  }

  const overrideKeys = new Set([
    ...Object.keys(origin.configOverrides),
    ...Object.keys(local.configOverrides),
    ...Object.keys(latest.configOverrides),
  ]);
  for (const key of overrideKeys) {
    const originValue = origin.configOverrides[key];
    const localValue = local.configOverrides[key];
    const latestValue = latest.configOverrides[key];
    const localChanged = serializedValue(localValue) !== serializedValue(originValue);
    const latestChanged = serializedValue(latestValue) !== serializedValue(originValue);
    if (
      localChanged &&
      latestChanged &&
      serializedValue(localValue) !== serializedValue(latestValue)
    ) {
      return { conflict: true };
    }
    if (!localChanged) continue;
    if (localValue === undefined) delete merged.configOverrides[key];
    else merged.configOverrides[key] = structuredClone(toRaw(localValue));
  }
  return { draft: merged, conflict: false };
}

async function saveDevice(device: PairedDevice): Promise<void> {
  const draft = drafts.value[device.uuid];
  if (!draft) return;
  error.value = '';
  notice.value = '';
  if (!draft.name.trim()) {
    error.value = t('ui.devices.error.name_required');
    return;
  }
  if (draft.displayMode.trim() && !/^\d{2,5}x\d{2,5}x\d{1,4}$/.test(draft.displayMode.trim())) {
    error.value = t('ui.devices.editor.display_mode_invalid');
    return;
  }

  busyUuid.value = device.uuid;
  try {
    let saveDraft = cloneDraft(draft);
    const origin = draftOrigins.value[device.uuid];
    const latestResponse = await apiGet<ClientsResponse>('/api/clients/list');
    if (latestResponse.status === false) throw new Error(t('ui.devices.error.list_rejected'));
    const latestDevice = latestResponse.named_certs?.find((item) => item.uuid === device.uuid);
    if (origin && latestDevice) {
      const merged = mergeLatestDraft(origin, draft, draftFromDevice(latestDevice));
      if (merged.conflict || !merged.draft) {
        error.value = t('ui.devices.error.conflict');
        return;
      }
      saveDraft = merged.draft;
    }
    const response = await apiPost<MutationResponse>(
      '/api/clients/update',
      updatePayload(device, saveDraft),
    );
    if (response.status !== true) {
      throw new Error(t('ui.devices.error.partial_update'));
    }
    drafts.value[device.uuid] = cloneDraft(saveDraft);
    draftOrigins.value[device.uuid] = cloneDraft(saveDraft);
    notice.value = t('ui.devices.notice.updated', { name: saveDraft.name.trim() });
    await loadDevices(true);
  } catch (cause) {
    error.value =
      cause instanceof ApiError
        ? t('clients.update_failed')
        : cause instanceof Error
          ? cause.message
          : t('clients.update_failed');
  } finally {
    busyUuid.value = '';
  }
}

function requestAction(kind: PendingAction['kind'], device: PairedDevice): void {
  pendingAction.value = { kind, device };
  confirmOpen.value = true;
}

function clearPendingAction(): void {
  if (!busyUuid.value) pendingAction.value = null;
}

async function confirmAction(): Promise<void> {
  const action = pendingAction.value;
  if (!action) return;
  error.value = '';
  notice.value = '';
  busyUuid.value = action.device.uuid;
  try {
    const path = action.kind === 'unpair' ? '/api/clients/unpair' : '/api/clients/disconnect';
    const response = await apiPost<MutationResponse>(path, { uuid: action.device.uuid });
    if (response.status !== true) {
      throw new Error(
        action.kind === 'unpair'
          ? t('ui.devices.error.unpair_rejected')
          : t('clients.disconnect_failed'),
      );
    }
    notice.value =
      action.kind === 'unpair'
        ? t('ui.devices.notice.unpaired', { name: action.device.name })
        : t('ui.devices.notice.disconnected', { name: action.device.name });
    confirmOpen.value = false;
    pendingAction.value = null;
    await loadDevices(true);
  } catch (cause) {
    error.value =
      cause instanceof ApiError
        ? t('ui.devices.error.action')
        : cause instanceof Error
          ? cause.message
          : t('ui.devices.error.action');
  } finally {
    busyUuid.value = '';
  }
}

onMounted(() => {
  void loadCommonSettings();
  void loadDisplayDevices();
  void loadHdrProfiles();
  void loadDevices();
  refreshTimer = window.setInterval(() => void loadDevices(true), 8000);
});

onBeforeUnmount(() => {
  if (refreshTimer !== undefined) window.clearInterval(refreshTimer);
});
</script>

<template>
  <div class="vs-page devices-page">
    <PageHeader :title="t('ui.devices.page.title')" :description="t('ui.devices.page.description')">
      <template #meta>
        <StatusBadge
          :label="t('ui.devices.count.streaming', { count: deviceCounts.streaming })"
          tone="success"
          compact
        />
        <StatusBadge
          :label="t('ui.devices.count.offline', { count: deviceCounts.offline })"
          tone="neutral"
          compact
        />
        <StatusBadge
          v-if="deviceCounts.blocked"
          :label="t('ui.devices.count.blocked', { count: deviceCounts.blocked })"
          tone="danger"
          compact
        />
      </template>
      <template #actions>
        <AppButton
          :label="t('_common.refresh')"
          icon="refresh"
          :busy="refreshing"
          :busy-label="t('ui.devices.action.refreshing')"
          @click="loadDevices()"
        />
        <RouterLink class="vs-button vs-button--primary" to="/pair">
          <UiIcon name="plus" aria-hidden="true" />
          <span>{{ t('ui.devices.action.pair') }}</span>
        </RouterLink>
      </template>
    </PageHeader>

    <div class="devices-stack">
      <InlineAlert
        v-if="error"
        tone="danger"
        :title="t('ui.devices.alert.action_failed')"
        announce="assertive"
      >
        {{ error }}
      </InlineAlert>
      <InlineAlert
        v-if="notice"
        tone="success"
        :title="t('ui.devices.alert.updated')"
        announce="polite"
        :dismiss-label="t('_common.dismiss')"
        @dismiss="notice = ''"
      >
        {{ notice }}
      </InlineAlert>
      <InlineAlert
        v-if="commonSettingsError"
        tone="warning"
        :title="t('ui.devices.common_settings_title')"
      >
        {{ commonSettingsError }}
      </InlineAlert>

      <section class="devices-toolbar vs-surface" :aria-label="t('ui.devices.filters.aria_label')">
        <label class="vs-field device-search" for="device-search">
          <span class="vs-field__label">{{ t('ui.devices.filters.search_label') }}</span>
          <span class="search-control">
            <UiIcon name="search" :size="16" aria-hidden="true" />
            <input
              id="device-search"
              v-model="query"
              class="vs-input"
              type="search"
              autocomplete="off"
              :placeholder="t('ui.devices.filters.search_placeholder')"
            />
          </span>
        </label>
        <p class="result-count" aria-live="polite">
          {{
            t(
              'ui.devices.filters.result_count',
              { shown: filteredDevices.length, total: devices.length },
              devices.length,
            )
          }}
        </p>
      </section>

      <div v-if="loading" class="device-loading" :aria-label="t('ui.devices.loading.aria_label')">
        <LoadingSkeleton v-for="item in 3" :key="item" variant="block" height="10rem" />
      </div>

      <EmptyState
        v-else-if="!devices.length"
        :title="t('ui.devices.empty.title')"
        :description="t('ui.devices.empty.description')"
        icon="devices"
      >
        <template #actions>
          <RouterLink class="vs-button vs-button--primary" to="/pair">
            {{ t('ui.devices.action.pair_indefinite') }}
          </RouterLink>
        </template>
      </EmptyState>

      <EmptyState
        v-else-if="!filteredDevices.length"
        :title="t('ui.devices.empty.filtered_title')"
        :description="t('ui.devices.empty.filtered_description')"
        icon="search"
        compact
      />

      <ul v-else class="device-list" :aria-label="t('ui.devices.list.aria_label')">
        <li v-for="device in filteredDevices" :key="device.uuid">
          <article class="device-row vs-surface" :aria-labelledby="`device-name-${device.uuid}`">
            <div class="device-row__summary">
              <div class="device-row__icon" aria-hidden="true">
                <UiIcon name="devices" :size="20" />
              </div>
              <div class="device-row__identity">
                <div class="device-row__title-line">
                  <h2 :id="`device-name-${device.uuid}`">{{ device.name }}</h2>
                  <StatusBadge
                    :label="statusFor(device).label"
                    :tone="statusFor(device).tone"
                    compact
                  />
                  <StatusBadge
                    v-if="hasCustomSettings(device)"
                    :label="t('ui.devices.editor.custom_settings')"
                    tone="neutral"
                    compact
                  />
                </div>
                <p>{{ lastSeen(device) }}</p>
                <code>{{ device.uuid }}</code>
              </div>
              <div class="device-row__actions">
                <AppButton
                  :label="t('clients.disconnect')"
                  icon="stop"
                  size="compact"
                  :disabled="!device.connected || busyUuid === device.uuid"
                  :aria-label="t('ui.devices.action.disconnect_named', { name: device.name })"
                  @click="requestAction('disconnect', device)"
                />
                <AppButton
                  :label="t('ui.devices.action.unpair')"
                  icon="trash"
                  variant="tertiary"
                  size="compact"
                  :disabled="busyUuid === device.uuid"
                  :aria-label="t('ui.devices.action.unpair_named', { name: device.name })"
                  @click="requestAction('unpair', device)"
                />
              </div>
            </div>

            <section class="device-editor">
              <button
                type="button"
                class="device-editor__toggle"
                :aria-expanded="editorIsOpen(device.uuid)"
                :aria-controls="`device-editor-panel-${device.uuid}`"
                @click="toggleEditor(device.uuid)"
              >
                <UiIcon name="edit" :size="16" aria-hidden="true" />
                <span>{{ t('ui.devices.editor.title') }}</span>
              </button>
              <div
                v-if="editorIsOpen(device.uuid) && drafts[device.uuid]"
                :id="`device-editor-panel-${device.uuid}`"
              >
                <ClientSettingsEditor
                  v-model="drafts[device.uuid]"
                  :common-settings="commonSettings"
                  :metadata="{ ...metadata, platform: metadata.platform || platform }"
                  :display-devices="displayDevices"
                  :display-devices-loading="displayDevicesLoading"
                  :display-devices-loaded="displayDevicesLoaded"
                  :display-devices-error="displayDevicesError"
                  :hdr-profiles="hdrProfiles"
                  :hdr-profiles-loading="hdrProfilesLoading"
                  :hdr-profiles-loaded="hdrProfilesLoaded"
                  :hdr-profiles-error="hdrProfilesError"
                  :busy="busyUuid === device.uuid"
                  :control-id-prefix="`client-${device.uuid}`"
                  @save="saveDevice(device)"
                  @cancel="resetDraft(device)"
                  @load-display-devices="loadDisplayDevices"
                  @load-hdr-profiles="loadHdrProfiles"
                />
              </div>
            </section>
          </article>
        </li>
      </ul>
    </div>

    <ConfirmDialog
      v-model:open="confirmOpen"
      :title="confirmTitle"
      :description="confirmDescription"
      :confirm-label="
        pendingAction?.kind === 'unpair'
          ? t('ui.devices.confirm.unpair_label')
          : t('ui.devices.confirm.disconnect_label')
      "
      :cancel-label="t('_common.cancel')"
      :tone="pendingAction?.kind === 'unpair' ? 'danger' : 'default'"
      :busy="Boolean(pendingAction && busyUuid === pendingAction.device.uuid)"
      :busy-label="t('ui.devices.action.working')"
      :close-on-confirm="false"
      @confirm="confirmAction"
      @cancel="clearPendingAction"
    />
  </div>
</template>

<style scoped>
.devices-page,
.devices-stack {
  display: grid;
  gap: var(--vs-space-24);
}

.devices-toolbar {
  display: flex;
  align-items: end;
  justify-content: space-between;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16);
}

.device-search {
  width: min(100%, 32rem);
}

.search-control {
  position: relative;
  display: flex;
  align-items: center;
}

.search-control > svg {
  position: absolute;
  left: var(--vs-space-12);
  z-index: 1;
  color: var(--vs-color-text-muted);
  pointer-events: none;
}

.search-control .vs-input {
  padding-left: var(--vs-space-40);
}

.result-count {
  flex: none;
  padding-bottom: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  font-variant-numeric: tabular-nums;
}

.device-loading,
.device-list {
  display: grid;
  gap: var(--vs-space-12);
}

.device-list {
  padding: 0;
  list-style: none;
}

.device-row {
  overflow: clip;
}

.device-row__summary {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-16);
  min-height: 88px;
  padding: var(--vs-space-16) var(--vs-space-20);
}

.device-row__icon {
  display: grid;
  width: 40px;
  height: 40px;
  place-items: center;
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-text-secondary);
}

.device-row__identity {
  min-width: 0;
}

.device-row__title-line,
.device-row__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.device-row h2 {
  overflow: hidden;
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.device-row p,
.device-row code {
  display: block;
  margin-top: var(--vs-space-2);
  overflow: hidden;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.device-row code {
  color: var(--vs-color-text-muted);
}

.device-editor {
  border-top: 1px solid var(--vs-color-border-subtle);
}

.device-editor__toggle {
  display: flex;
  width: 100%;
  min-height: 44px;
  align-items: center;
  gap: var(--vs-space-8);
  padding: 0 var(--vs-space-20);
  border: 0;
  background: transparent;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
  text-align: start;
  cursor: pointer;
}

.device-editor__toggle:hover,
.device-editor__toggle[aria-expanded='true'] {
  color: var(--vs-color-text-primary);
  background: var(--vs-color-bg-subtle);
}

@media (max-width: 767px) {
  .devices-toolbar,
  .device-row__summary {
    display: grid;
    grid-template-columns: minmax(0, 1fr);
    align-items: stretch;
  }

  .result-count {
    padding: 0;
  }

  .device-row__icon {
    display: none;
  }

  .device-row__actions {
    padding-top: var(--vs-space-8);
  }

  .device-row__actions > :deep(.vs-button) {
    flex: 1 1 10rem;
  }
}

@media (forced-colors: active) {
  .device-row__icon,
  .device-editor {
    border: 1px solid CanvasText;
  }
}
</style>
