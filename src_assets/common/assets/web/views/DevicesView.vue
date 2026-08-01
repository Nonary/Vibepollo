<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
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
import { formatRelativeTime } from '@/utils/format';

const { locale, t } = useI18n();

interface PairedDevice {
  name: string;
  uuid: string;
  enabled: boolean;
  connected: boolean;
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
  enabled_updated?: boolean;
}

interface DeviceDraft {
  name: string;
  enabled: boolean;
}

interface PendingAction {
  kind: 'disconnect' | 'unpair';
  device: PairedDevice;
}

const devices = ref<PairedDevice[]>([]);
const drafts = ref<Record<string, DeviceDraft>>({});
const query = ref('');
const loading = ref(true);
const refreshing = ref(false);
const error = ref('');
const notice = ref('');
const busyUuid = ref('');
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

const deviceCounts = computed(() => ({
  streaming: devices.value.filter((device) => device.connected && device.enabled).length,
  blocked: devices.value.filter((device) => !device.enabled).length,
  offline: devices.value.filter((device) => !device.connected && device.enabled).length,
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

function syncDrafts(incoming: PairedDevice[]): void {
  const previous = new Map(devices.value.map((device) => [device.uuid, device]));
  for (const device of incoming) {
    const draft = drafts.value[device.uuid];
    const oldDevice = previous.get(device.uuid);
    const draftIsUntouched =
      !draft ||
      (oldDevice && draft.name === oldDevice.name && draft.enabled === oldDevice.enabled);
    if (draftIsUntouched) {
      drafts.value[device.uuid] = { name: device.name, enabled: device.enabled };
    }
  }

  const uuids = new Set(incoming.map((device) => device.uuid));
  for (const uuid of Object.keys(drafts.value)) {
    if (!uuids.has(uuid)) delete drafts.value[uuid];
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
  if (!device.enabled) return { label: t('ui.devices.status.blocked'), tone: 'danger' };
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

function updatePayload(device: PairedDevice, draft: DeviceDraft): Record<string, unknown> {
  const payload: Record<string, unknown> = {
    uuid: device.uuid,
    name: draft.name.trim(),
    enabled: draft.enabled,
    display_mode: device.display_mode ?? '',
    output_name_override: device.output_name_override ?? '',
    always_use_virtual_display: device.always_use_virtual_display ?? false,
    virtual_display_mode: device.virtual_display_mode ?? '',
    virtual_display_layout: device.virtual_display_layout ?? '',
    prefer_10bit_sdr: device.prefer_10bit_sdr ?? false,
  };
  if (device.config_overrides !== undefined) payload.config_overrides = device.config_overrides;
  if (device.hdr_profile !== undefined) payload.hdr_profile = device.hdr_profile;
  return payload;
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

  busyUuid.value = device.uuid;
  try {
    const response = await apiPost<MutationResponse>(
      '/api/clients/update',
      updatePayload(device, draft),
    );
    if (response.status !== true || response.enabled_updated === false) {
      throw new Error(t('ui.devices.error.partial_update'));
    }
    notice.value = t('ui.devices.notice.updated', { name: draft.name.trim() });
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
    const path =
      action.kind === 'unpair' ? '/api/clients/unpair' : '/api/clients/disconnect';
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
  void loadDevices();
  refreshTimer = window.setInterval(() => void loadDevices(true), 8000);
});

onBeforeUnmount(() => {
  if (refreshTimer !== undefined) window.clearInterval(refreshTimer);
});
</script>

<template>
  <div class="vs-page devices-page">
    <PageHeader
      :title="t('ui.devices.page.title')"
      :description="t('ui.devices.page.description')"
    >
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

      <div
        v-if="loading"
        class="device-loading"
        :aria-label="t('ui.devices.loading.aria_label')"
      >
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
              <div class="device-row__icon" aria-hidden="true"><UiIcon name="devices" :size="20" /></div>
              <div class="device-row__identity">
                <div class="device-row__title-line">
                  <h2 :id="`device-name-${device.uuid}`">{{ device.name }}</h2>
                  <StatusBadge
                    :label="statusFor(device).label"
                    :tone="statusFor(device).tone"
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

            <details class="device-editor">
              <summary>
                <UiIcon name="edit" :size="16" aria-hidden="true" />
                <span>{{ t('ui.devices.editor.title') }}</span>
              </summary>
              <form v-if="drafts[device.uuid]" class="device-editor__form" @submit.prevent="saveDevice(device)">
                <label class="vs-field" :for="`device-name-input-${device.uuid}`">
                  <span class="vs-field__label">{{ t('pin.device_name') }}</span>
                  <input
                    :id="`device-name-input-${device.uuid}`"
                    v-model="drafts[device.uuid].name"
                    class="vs-input"
                    autocomplete="off"
                    maxlength="96"
                    required
                    :disabled="busyUuid === device.uuid"
                  />
                  <span class="vs-field__helper">{{ t('ui.devices.editor.name_helper') }}</span>
                </label>

                <label class="vs-switch device-enabled">
                  <input
                    v-model="drafts[device.uuid].enabled"
                    type="checkbox"
                    :disabled="busyUuid === device.uuid"
                  />
                  <span class="vs-switch__track" aria-hidden="true" />
                  <span>{{ t('ui.devices.editor.allow_stream') }}</span>
                </label>

                <div class="device-editor__footer">
                  <p>{{ t('ui.devices.editor.blocking_help') }}</p>
                  <AppButton
                    type="submit"
                    variant="primary"
                    :label="t('ui.devices.action.save')"
                    :busy="busyUuid === device.uuid"
                    :busy-label="t('ui.devices.action.saving')"
                  />
                </div>
              </form>
            </details>
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
.device-row__actions,
.device-editor__footer {
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

.device-editor summary {
  display: flex;
  min-height: 44px;
  align-items: center;
  gap: var(--vs-space-8);
  padding: 0 var(--vs-space-20);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
  cursor: pointer;
  list-style: none;
}

.device-editor summary::-webkit-details-marker {
  display: none;
}

.device-editor summary:hover {
  color: var(--vs-color-text-primary);
  background: var(--vs-color-bg-subtle);
}

.device-editor__form {
  display: grid;
  grid-template-columns: minmax(15rem, 1fr) minmax(13rem, auto);
  align-items: end;
  gap: var(--vs-space-20);
  padding: var(--vs-space-20);
  border-top: 1px solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-subtle);
}

.device-enabled {
  align-self: center;
}

.device-editor__footer {
  grid-column: 1 / -1;
  justify-content: space-between;
  padding-top: var(--vs-space-4);
}

.device-editor__footer p {
  margin: 0;
  white-space: normal;
}

@media (max-width: 767px) {
  .devices-toolbar,
  .device-row__summary,
  .device-editor__form {
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

  .device-editor__footer {
    align-items: stretch;
    flex-direction: column;
  }
}

@media (forced-colors: active) {
  .device-row__icon,
  .device-editor__form {
    border: 1px solid CanvasText;
  }
}
</style>
