<template>
  <div class="px-4 pb-10 space-y-10">
    <h1 class="text-2xl font-semibold my-6 flex items-center gap-3 text-brand">
      <i class="fas fa-users-cog" /> {{ $t('clients.title') }}
    </h1>

    <!-- Pair New Client -->
    <n-card class="mb-8" :segmented="{ content: true, footer: true }">
      <template #header>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-link" /> {{ $t('clients.pair_title') }}
        </h2>
      </template>
      <div class="space-y-4">
        <p class="text-sm opacity-75">{{ $t('clients.pair_desc') }}</p>
        <n-form
          class="grid grid-cols-1 md:grid-cols-3 gap-4 items-end"
          @submit.prevent="registerDevice"
        >
          <n-form-item class="flex flex-col" :label="$t('navbar.pin')" label-placement="top">
            <n-input
              v-model:value="pin"
              :placeholder="$t('navbar.pin')"
              :input-props="{
                inputmode: 'numeric',
                pattern: '^[0-9]{4}$',
                maxlength: 4,
                required: true,
              }"
            />
          </n-form-item>
          <n-form-item class="flex flex-col" :label="$t('pin.device_name')" label-placement="top">
            <n-input v-model:value="deviceName" :placeholder="$t('pin.device_name')" />
          </n-form-item>
          <n-form-item class="flex flex-col md:items-end">
            <n-button :disabled="pairing" class="w-full md:w-auto" type="primary" attr-type="submit">
              <span v-if="!pairing">{{ $t('pin.send') }}</span>
              <span v-else>{{ $t('clients.pairing') }}</span>
            </n-button>
          </n-form-item>
        </n-form>
        <div class="space-y-2">
          <n-alert v-if="pairStatus === true" type="success">{{ $t('pin.pair_success') }}</n-alert>
          <n-alert v-if="pairStatus === false" type="error">{{ $t('pin.pair_failure') }}</n-alert>
        </div>
        <n-alert type="warning" :title="$t('_common.warning')" class="text-sm">
          {{ $t('pin.warning_msg') }}
        </n-alert>
      </div>
    </n-card>

    <!-- Existing Clients -->
    <n-card class="mb-8" :segmented="{ content: true, footer: true }">
      <template #header>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-users" /> {{ $t('clients.existing_title') }}
        </h2>
      </template>

      <div class="flex flex-col gap-3 md:flex-row md:items-center">
        <p class="text-sm opacity-75 md:flex-1">{{ $t('troubleshooting.unpair_desc') }}</p>
        <n-button
          class="md:ml-auto"
          type="error"
          strong
          :disabled="unpairAllPressed || clients.length === 0"
          @click="askConfirmUnpairAll"
        >
          <i class="fas fa-user-slash" />
          {{ $t('troubleshooting.unpair_all') }}
        </n-button>
      </div>

      <n-alert v-if="unpairAllStatus === true" type="success" class="mt-3">{{
        $t('troubleshooting.unpair_all_success')
      }}</n-alert>
      <n-alert v-if="unpairAllStatus === false" type="error" class="mt-3">{{
        $t('troubleshooting.unpair_all_error')
      }}</n-alert>

      <div v-if="clients.length > 0" class="mt-4 space-y-4">
        <div
          v-for="client in clients"
          :key="client.uuid"
          class="rounded-2xl border border-dark/[0.06] bg-light/[0.02] p-4 shadow-sm dark:border-light/[0.12]"
        >
          <div class="flex flex-wrap items-center gap-3">
            <span class="text-base font-medium">
              {{ client.name !== '' ? client.name : $t('troubleshooting.unpair_single_unknown') }}
            </span>
            <n-tag v-if="client.connected" type="warning" size="small">{{ $t('clients.connected') }}</n-tag>
            <div class="ml-auto flex items-center gap-2">
              <n-button
                v-if="client.connected"
                size="small"
                type="warning"
                quaternary
                :disabled="disconnecting[client.uuid] === true"
                @click="disconnectClient(client)"
              >
                <i class="fas fa-link-slash" />
              </n-button>
              <n-button
                v-if="client.editing"
                size="small"
                type="success"
                quaternary
                :disabled="saving[client.uuid] === true || !isClientDisplayOverrideValid"
                @click="saveClient(client)"
              >
                <i class="fas fa-check" />
              </n-button>
              <n-button
                v-if="client.editing"
                size="small"
                quaternary
                :disabled="saving[client.uuid] === true"
                @click="cancelEdit(client)"
              >
                <i class="fas fa-times" />
              </n-button>
              <n-button
                v-if="!client.editing"
                size="small"
                quaternary
                type="primary"
                @click="editClient(client)"
              >
                <i class="fas fa-edit" />
              </n-button>
              <n-button
                size="small"
                quaternary
                type="error"
                :disabled="removing[client.uuid] === true"
                @click="askConfirmUnpair(client)"
              >
                <i class="fas fa-trash" />
              </n-button>
            </div>
          </div>

          <div v-if="client.editing" class="mt-4">
            <n-form label-placement="top" class="space-y-4" @submit.prevent>
              <n-form-item :label="$t('pin.device_name')">
                <n-input v-model:value="client.editName" />
              </n-form-item>

              <n-form-item :label="$t('pin.display_mode_override')">
                <n-input v-model:value="client.editDisplayMode" placeholder="1920x1080x60" />
                <template #feedback>
                  <span class="text-xs opacity-70">{{ $t('pin.display_mode_override_desc') }}</span>
                </template>
              </n-form-item>

              <div v-if="isWindows" class="space-y-3">
                <n-checkbox
                  v-model:checked="client.editDisplayOverrideEnabled"
                  size="small"
                  @update:checked="(v) => applyClientDisplayOverrideEnabled(client, v)"
                >
                  <div class="flex flex-col">
                    <span>{{ t('config.client_display_override_label') }}</span>
                    <span class="text-[11px] opacity-60">
                      {{ t('config.client_display_override_hint') }}
                    </span>
                  </div>
                </n-checkbox>

                <div
                  v-if="client.editDisplayOverrideEnabled"
                  class="space-y-5 rounded-xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-dark/40 p-4"
                >
                  <div class="space-y-2">
                    <div class="flex items-center justify-between gap-3">
                      <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                        {{ t('config.client_display_override_label') }}
                      </span>
                    </div>
                    <p class="text-[11px] opacity-70">{{ t('config.client_display_override_hint') }}</p>
                  </div>

                  <div class="space-y-2">
                    <n-radio-group
                      :value="client.editDisplaySelection"
                      @update:value="(v) => applyClientDisplaySelection(client, v as ClientDisplaySelection)"
                      class="grid gap-3 sm:grid-cols-2"
                    >
                      <n-radio value="virtual" class="app-radio-card cursor-pointer">
                        <span class="app-radio-card-title">{{ t('config.app_display_override_virtual') }}</span>
                      </n-radio>
                      <n-radio value="physical" class="app-radio-card cursor-pointer">
                        <span class="app-radio-card-title">{{ t('config.app_display_override_physical') }}</span>
                      </n-radio>
                    </n-radio-group>
                  </div>

                  <div v-if="client.editDisplaySelection === 'physical'" class="space-y-2">
                    <div class="flex items-center justify-between gap-3">
                      <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                        {{ t('config.app_display_physical_label') }}
                      </span>
                      <n-button size="tiny" tertiary :loading="displayDevicesLoading" @click="loadDisplayDevices">
                        {{ t('_common.refresh') }}
                      </n-button>
                    </div>
                    <p class="text-[11px] opacity-70">{{ t('config.app_display_physical_hint') }}</p>
                    <n-select
                      v-model:value="client.editPhysicalOutputOverride"
                      :options="displayDeviceOptions"
                      :loading="displayDevicesLoading"
                      :placeholder="t('config.app_display_physical_placeholder')"
                      filterable
                      clearable
                      :fallback-option="(value) => ({ label: value as string, value: value as string, displayName: value as string, id: value as string, active: null })"
                      @focus="ensureDisplayDevicesLoaded"
                    >
                      <template #option="{ option }">
                        <div class="leading-tight">
                          <div class="">{{ option?.displayName || option?.label }}</div>
                          <div class="text-[12px] opacity-60 font-mono">
                            {{ option?.id || option?.value }}
                            <span
                              v-if="option?.active === true"
                              class="ml-1 text-green-600 dark:text-green-400"
                            >
                              ({{ t('config.app_display_status_active') }})
                            </span>
                            <span v-else-if="option?.active === false" class="ml-1 opacity-70">
                              ({{ t('config.app_display_status_inactive') }})
                            </span>
                          </div>
                        </div>
                      </template>
                      <template #value="{ option }">
                        <div class="leading-tight">
                          <div class="">{{ option?.displayName || option?.label }}</div>
                          <div class="text-[12px] opacity-60 font-mono">
                            {{ option?.id || option?.value }}
                          </div>
                        </div>
                      </template>
                    </n-select>
                    <div class="text-[11px] opacity-70">
                      <span v-if="displayDevicesError" class="text-red-500">{{ displayDevicesError }}</span>
                      <span v-else>{{ t('config.app_display_physical_status_hint') }}</span>
                    </div>
                  </div>

                  <div v-else class="space-y-5">
                    <div class="space-y-2">
                      <div class="flex items-center justify-between gap-3">
                        <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                          {{ t('config.virtual_display_mode_label') }}
                        </span>
                      </div>
                      <p class="text-[11px] opacity-70">{{ t('config.virtual_display_mode_step_hint') }}</p>
                      <n-radio-group v-model:value="client.editVirtualDisplayMode" class="grid gap-3 sm:grid-cols-2">
                        <n-radio
                          v-for="option in virtualDisplayModeOptions"
                          :key="option.value"
                          :value="option.value"
                          class="app-radio-card cursor-pointer"
                        >
                          <span class="app-radio-card-title">{{ option.label }}</span>
                        </n-radio>
                      </n-radio-group>
                    </div>

                    <div class="space-y-2">
                      <div class="flex items-center justify-between gap-3">
                        <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                          {{ t('config.virtual_display_layout_label') }}
                        </span>
                        <n-button
                          v-if="client.editVirtualDisplayLayout !== null"
                          size="tiny"
                          tertiary
                          @click="client.editVirtualDisplayLayout = null"
                        >
                          {{ t('config.app_virtual_display_layout_reset') }}
                        </n-button>
                      </div>
                      <p class="text-[11px] opacity-70">{{ t('config.virtual_display_layout_hint') }}</p>
                      <n-radio-group
                        :value="client.editVirtualDisplayLayout ?? globalVirtualDisplayLayout ?? 'exclusive'"
                        @update:value="(v) => (client.editVirtualDisplayLayout = v === globalVirtualDisplayLayout ? null : (v as any))"
                        class="space-y-4"
                      >
                        <div
                          v-for="option in virtualDisplayLayoutOptions"
                          :key="option.value"
                          class="flex flex-col cursor-pointer py-2 px-2 rounded-md hover:bg-surface/10"
                          @click="client.editVirtualDisplayLayout = option.value === globalVirtualDisplayLayout ? null : option.value"
                          @keydown.enter.prevent="client.editVirtualDisplayLayout = option.value === globalVirtualDisplayLayout ? null : option.value"
                          @keydown.space.prevent="client.editVirtualDisplayLayout = option.value === globalVirtualDisplayLayout ? null : option.value"
                          tabindex="0"
                        >
                          <div class="flex items-center gap-3">
                            <n-radio :value="option.value" />
                            <span class="text-sm font-semibold">{{ option.label }}</span>
                          </div>
                          <span class="text-[11px] opacity-70 leading-snug ml-6">
                            {{ t(`config.virtual_display_layout_${option.value}_desc`) }}
                          </span>
                        </div>
                      </n-radio-group>
                      <div v-if="client.editVirtualDisplayLayout === null" class="text-[11px] opacity-70">
                        {{ t('config.app_virtual_display_layout_follow_global') }}
                      </div>
                    </div>
                  </div>
                </div>
              </div>

              <n-form-item v-if="isWindows" :label="t('clients.hdr_profile_label')">
                <n-select
                  v-model:value="client.editHdrProfile"
                  :options="hdrProfileOptions"
                  :loading="hdrProfilesLoading"
                  :placeholder="t('clients.hdr_profile_placeholder')"
                  filterable
                  clearable
                  @focus="ensureHdrProfilesLoaded"
                />
                <template #feedback>
                  <span class="text-xs opacity-70">{{ t('clients.hdr_profile_desc') }}</span>
                  <span v-if="hdrProfilesError" class="text-xs text-red-500 block">{{ hdrProfilesError }}</span>
                </template>
              </n-form-item>

              <n-form-item :label="t('config.prefer_10bit_sdr')">
                <n-select
                  v-model:value="client.editPrefer10BitSdr"
                  :options="prefer10BitSdrOptions"
                  clearable
                  :placeholder="t('config.prefer_10bit_sdr_follow_global')"
                />
                <template #feedback>
                  <span class="text-xs opacity-70">{{ t('config.prefer_10bit_sdr_desc') }}</span>
                  <span v-if="client.editPrefer10BitSdr === null" class="text-xs opacity-70 block">
                    {{ t('config.prefer_10bit_sdr_follow_global') }}
                    ({{ globalPrefer10BitSdr ? t('_common.enabled') : t('_common.disabled') }})
                  </span>
                </template>
              </n-form-item>

              <AppEditConfigOverridesSection
                v-model:overrides="client.editConfigOverrides"
                scope-label="client"
              />
            </n-form>
          </div>
        </div>
      </div>
      <div v-else class="p-4 text-center italic opacity-75">
        {{ $t('troubleshooting.unpair_single_no_devices') }}
      </div>
    </n-card>

    <TrustedDevicesCard />
    <ApiTokenManager />

    <!-- Confirm remove single client -->
    <n-modal :show="showConfirmRemove" @update:show="(v) => (showConfirmRemove = v)">
      <n-card
        :title="
          $t('clients.confirm_remove_title_named', {
            name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
          })
        "
        style="max-width: 32rem; width: 100%"
        :bordered="false"
      >
        <div class="text-sm text-center">
          {{
            $t('clients.confirm_remove_message_named', {
              name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
            })
          }}
        </div>
        <template #footer>
          <div class="flex justify-end gap-2">
            <n-button @click="showConfirmRemove = false">{{ $t('_common.cancel') }}</n-button>
            <n-button type="error" secondary @click="confirmRemove">{{ $t('clients.remove') }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>

    <!-- Confirm unpair all -->
    <n-modal :show="showConfirmUnpairAll" @update:show="(v) => (showConfirmUnpairAll = v)">
      <n-card
        :title="$t('clients.confirm_unpair_all_title')"
        style="max-width: 32rem; width: 100%"
        :bordered="false"
      >
        <div class="text-sm text-center">
          {{
            $t('clients.confirm_unpair_all_message_count', {
              count: clients.length,
            })
          }}
        </div>
        <template #footer>
          <div class="flex justify-end gap-2">
            <n-button @click="showConfirmUnpairAll = false">{{ $t('_common.cancel') }}</n-button>
            <n-button secondary @click="confirmUnpairAll">{{
              $t('troubleshooting.unpair_all')
            }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { http } from '@/http';
import {
  NAlert,
  NButton,
  NCard,
  NForm,
  NFormItem,
  NInput,
  NModal,
  NRadio,
  NRadioGroup,
  NSelect,
  NTag,
  useMessage,
} from 'naive-ui';
import ApiTokenManager from '@/ApiTokenManager.vue';
import TrustedDevicesCard from '@/components/TrustedDevicesCard.vue';
import AppEditConfigOverridesSection from '@/components/app-edit/AppEditConfigOverridesSection.vue';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';

type ClientDisplaySelection = 'physical' | 'virtual';
type ClientVirtualDisplayMode = 'disabled' | 'per_client' | 'shared' | null;
type ClientVirtualDisplayLayout =
  | 'exclusive'
  | 'extended'
  | 'extended_primary'
  | 'extended_isolated'
  | 'extended_primary_isolated'
  | null;
type ClientPrefer10BitSdrOverride = 'enabled' | 'disabled' | null;

interface ClientApiEntry {
  uuid?: string;
  name?: string;
  connected?: boolean;
  hdr_profile?: string;
  display_mode?: string;
  output_name_override?: string;
  always_use_virtual_display?: boolean | string | number;
  virtual_display_mode?: string;
  virtual_display_layout?: string;
  prefer_10bit_sdr?: boolean | string | number | null;
  config_overrides?: Record<string, unknown> | null;
}

interface ClientsListResponse {
  status: boolean;
  named_certs: ClientApiEntry[];
  platform?: string;
}

interface HdrProfileEntry {
  filename?: string;
  added_ms?: number;
}

interface HdrProfilesResponse {
  status?: boolean;
  profiles?: HdrProfileEntry[];
  error?: string;
}

interface ClientViewModel {
  uuid: string;
  name: string;
  connected: boolean;
  hdrProfile: string;
  displayMode: string;
  outputOverride: string;
  alwaysUseVirtualDisplay: boolean;
  prefer10BitSdr: ClientPrefer10BitSdrOverride;
  virtualDisplayMode: ClientVirtualDisplayMode;
  virtualDisplayLayout: ClientVirtualDisplayLayout;
  configOverrides: Record<string, unknown>;

  editing: boolean;
  editHdrProfile: string | null;
  editName: string;
  editDisplayMode: string;
  editDisplayOverrideEnabled: boolean;
  editDisplaySelection: ClientDisplaySelection;
  editPhysicalOutputOverride: string | null;
  editVirtualDisplayMode: ClientVirtualDisplayMode;
  editVirtualDisplayLayout: ClientVirtualDisplayLayout;
  editPrefer10BitSdr: ClientPrefer10BitSdrOverride;
  editConfigOverrides: Record<string, unknown>;
}

interface DisplayDevice {
  device_id?: string;
  display_name?: string;
  friendly_name?: string;
  info?: unknown;
}

const { t } = useI18n();
const message = useMessage();
const configStore = useConfigStore();
const globalPrefer10BitSdr = computed<boolean>(() =>
  toBool((configStore.config as any)?.prefer_10bit_sdr, false),
);
const prefer10BitSdrOptions = computed(() => [
  { label: t('_common.enabled'), value: 'enabled' },
  { label: t('_common.disabled'), value: 'disabled' },
]);

const clients = ref<ClientViewModel[]>([]);
const platform = ref<string>('');

const pin = ref<string>('');
const deviceName = ref<string>('');
const pairing = ref<boolean>(false);
const pairStatus = ref<boolean | null>(null);

const unpairAllPressed = ref<boolean>(false);
const unpairAllStatus = ref<boolean | null>(null);
const removing = ref<Record<string, boolean>>({});
const saving = ref<Record<string, boolean>>({});
const disconnecting = ref<Record<string, boolean>>({});

const showConfirmRemove = ref<boolean>(false);
const pendingRemoveUuid = ref<string>('');
const pendingRemoveName = ref<string>('');
const showConfirmUnpairAll = ref<boolean>(false);

const isWindows = computed(() => {
  const p = (platform.value || '').toLowerCase();
  if (p) return p.startsWith('win') || p === 'windows';
  const meta = String((configStore.metadata as any)?.platform || '').toLowerCase();
  return meta === 'windows' || meta.startsWith('win');
});

function toBool(value: unknown, fallback = false): boolean {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return value !== 0;
  if (typeof value === 'string') {
    const v = value.trim().toLowerCase();
    if (['1', 'true', 'yes', 'on', 'enabled'].includes(v)) return true;
    if (['0', 'false', 'no', 'off', 'disabled', ''].includes(v)) return false;
  }
  return fallback;
}

function parseClientVirtualDisplayMode(value: unknown): ClientVirtualDisplayMode {
  const v = String(value ?? '').trim().toLowerCase();
  if (!v) return null;
  if (v === 'disabled' || v === 'per_client' || v === 'shared') return v as ClientVirtualDisplayMode;
  return null;
}

function parseClientVirtualDisplayLayout(value: unknown): ClientVirtualDisplayLayout {
  const v = String(value ?? '').trim().toLowerCase();
  if (!v) return null;
  if (
    v === 'exclusive' ||
    v === 'extended' ||
    v === 'extended_primary' ||
    v === 'extended_isolated' ||
    v === 'extended_primary_isolated'
  )
    return v as ClientVirtualDisplayLayout;
  return null;
}

function createClientViewModel(entry: ClientApiEntry): ClientViewModel {
  const name = entry.name ?? '';
  const hdrProfile = (entry.hdr_profile ?? '').trim();
  const displayMode = entry.display_mode ?? '';
  const outputOverride = entry.output_name_override ?? '';
  const alwaysVirtual = toBool(entry.always_use_virtual_display, false);
  const hdrProfile = String(entry.hdr_profile ?? '').trim();
  const configOverrides =
    entry.config_overrides && typeof entry.config_overrides === 'object' && !Array.isArray(entry.config_overrides)
      ? JSON.parse(JSON.stringify(entry.config_overrides))
      : {};
  const prefer10: ClientPrefer10BitSdrOverride =
    entry.prefer_10bit_sdr === undefined || entry.prefer_10bit_sdr === null
      ? null
      : toBool(entry.prefer_10bit_sdr, false)
        ? 'enabled'
        : 'disabled';
  const virtualMode = parseClientVirtualDisplayMode(entry.virtual_display_mode ?? '');
  const virtualLayout = parseClientVirtualDisplayLayout(entry.virtual_display_layout ?? '');
  const overrideEnabled =
    alwaysVirtual || !!outputOverride.trim() || virtualMode !== null || virtualLayout !== null;
  const selection: ClientDisplaySelection =
    alwaysVirtual || (virtualMode !== null && virtualMode !== 'disabled') ? 'virtual' : 'physical';
  const client: ClientViewModel = {
    uuid: entry.uuid ?? '',
    name,
    connected: !!entry.connected,
    hdrProfile,
    displayMode,
    outputOverride,
    alwaysUseVirtualDisplay: alwaysVirtual,
    prefer10BitSdr: prefer10,
    virtualDisplayMode: virtualMode,
    virtualDisplayLayout: virtualLayout,
    configOverrides,
    editing: false,
    editHdrProfile: hdrProfile || null,
    editName: name,
    editDisplayMode: displayMode,
    editDisplayOverrideEnabled: overrideEnabled,
    editDisplaySelection: selection,
    editPhysicalOutputOverride: outputOverride || null,
    editVirtualDisplayMode: virtualMode,
    editVirtualDisplayLayout: virtualLayout,
    editPrefer10BitSdr: prefer10,
    editConfigOverrides: JSON.parse(JSON.stringify(configOverrides)),
  };

  if (client.editDisplayOverrideEnabled) {
    applyClientDisplaySelection(client, client.editDisplaySelection);
  }

  return client;
}

function resetClientEdits(client: ClientViewModel): void {
  client.editName = client.name;
  client.editHdrProfile = (client.hdrProfile || '').trim() || null;
  client.editDisplayMode = client.displayMode;
  client.editDisplayOverrideEnabled =
    client.alwaysUseVirtualDisplay ||
    !!(client.outputOverride || '').trim() ||
    client.virtualDisplayMode !== null ||
    client.virtualDisplayLayout !== null;
  client.editDisplaySelection =
    client.alwaysUseVirtualDisplay ||
    (client.virtualDisplayMode !== null && client.virtualDisplayMode !== 'disabled')
      ? 'virtual'
      : 'physical';
  client.editPhysicalOutputOverride = client.outputOverride || null;
  client.editVirtualDisplayMode = client.virtualDisplayMode;
  client.editVirtualDisplayLayout = client.virtualDisplayLayout;
  client.editPrefer10BitSdr = client.prefer10BitSdr;
  client.editConfigOverrides = JSON.parse(JSON.stringify(client.configOverrides || {}));

  if (client.editDisplayOverrideEnabled) {
    applyClientDisplaySelection(client, client.editDisplaySelection);
  }
}

const virtualDisplayModeOptions = computed(() => [
  { label: t('config.virtual_display_mode_per_client'), value: 'per_client' },
  { label: t('config.virtual_display_mode_shared'), value: 'shared' },
]);

const globalVirtualDisplayMode = computed<ClientVirtualDisplayMode>(() =>
  parseClientVirtualDisplayMode((configStore.config as any)?.virtual_display_mode),
);

const globalVirtualDisplayLayout = computed<ClientVirtualDisplayLayout>(() =>
  parseClientVirtualDisplayLayout((configStore.config as any)?.virtual_display_layout),
);

const virtualDisplayLayoutOptions = computed(() => {
  const values: Array<Exclude<ClientVirtualDisplayLayout, null>> = [
    'exclusive',
    'extended',
    'extended_primary',
    'extended_isolated',
    'extended_primary_isolated',
  ];
  return values.map((value) => ({ label: t(`config.virtual_display_layout_${value}`), value }));
});

const hdrProfiles = ref<HdrProfileEntry[]>([]);
const hdrProfilesLoading = ref(false);
const hdrProfilesError = ref('');

const hdrProfileOptions = computed(() => {
  const list = Array.isArray(hdrProfiles.value) ? [...hdrProfiles.value] : [];
  list.sort((a, b) => (Number(b.added_ms || 0) || 0) - (Number(a.added_ms || 0) || 0));
  const options: Array<{ label: string; value: string | null }> = [{ label: t('clients.hdr_profile_auto'), value: null }];
  for (const p of list) {
    const filename = String(p?.filename || '').trim();
    if (!filename) continue;
    options.push({ label: filename, value: filename });
  }
  return options;
});

async function loadHdrProfiles(): Promise<void> {
  if (!isWindows.value) return;
  hdrProfilesLoading.value = true;
  hdrProfilesError.value = '';
  try {
    const r = await http.get<HdrProfilesResponse>('./api/clients/hdr-profiles', { validateStatus: () => true });
    const response = r.data || ({} as HdrProfilesResponse);
    const ok = r.status >= 200 && r.status < 300 && response.status === true && Array.isArray(response.profiles);
    if (!ok) {
      hdrProfiles.value = [];
      hdrProfilesError.value = response.error || t('clients.hdr_profile_load_failed');
      return;
    }
    hdrProfiles.value = response.profiles || [];
  } catch (e: any) {
    hdrProfiles.value = [];
    hdrProfilesError.value = e?.message || t('clients.hdr_profile_load_failed');
  } finally {
    hdrProfilesLoading.value = false;
  }
}

function ensureHdrProfilesLoaded(): void {
  if (!isWindows.value) return;
  if (!hdrProfilesLoading.value && hdrProfiles.value.length === 0) {
    void loadHdrProfiles();
  }
}

function applyClientDisplayOverrideEnabled(client: ClientViewModel, enabled: boolean): void {
  client.editDisplayOverrideEnabled = enabled;
  if (!enabled) {
    client.editDisplaySelection = 'physical';
    client.editPhysicalOutputOverride = null;
    client.editVirtualDisplayMode = null;
    client.editVirtualDisplayLayout = null;
    return;
  }

  applyClientDisplaySelection(client, client.editDisplaySelection);
}

function applyClientDisplaySelection(client: ClientViewModel, selection: ClientDisplaySelection): void {
  client.editDisplaySelection = selection;
  if (selection === 'physical') {
    client.editVirtualDisplayMode = 'disabled';
    client.editVirtualDisplayLayout = null;
    return;
  }

  client.editPhysicalOutputOverride = null;
  const resolvedMode = globalVirtualDisplayMode.value;
  if (client.editVirtualDisplayMode === null || client.editVirtualDisplayMode === 'disabled') {
    client.editVirtualDisplayMode = resolvedMode && resolvedMode !== 'disabled' ? resolvedMode : 'per_client';
  }
}

const isClientDisplayOverrideValid = computed(() => {
  for (const client of clients.value) {
    if (!client.editing) continue;
    if (!client.editDisplayOverrideEnabled) continue;

    if (client.editDisplaySelection === 'virtual') {
      if (client.editVirtualDisplayMode !== 'per_client' && client.editVirtualDisplayMode !== 'shared') {
        return false;
      }
    }
  }
  return true;
});

async function refreshClients(): Promise<void> {
  const auth = useAuthStore();
  if (!auth.isAuthenticated) return;
  try {
    const r = await http.get<ClientsListResponse>('./api/clients/list', { validateStatus: () => true });
    const response = r.data || ({} as ClientsListResponse);
    if (typeof response.platform === 'string') {
      platform.value = response.platform;
    }
    if (response.status === true && Array.isArray(response.named_certs)) {
      const mapped = response.named_certs.map(createClientViewModel);
      mapped.sort((a, b) => {
        const nameA = (a.name || '').toLowerCase();
        const nameB = (b.name || '').toLowerCase();
        if (nameA === nameB) return a.uuid.localeCompare(b.uuid);
        if (nameA === '') return 1;
        if (nameB === '') return -1;
        return nameA.localeCompare(nameB);
      });
      clients.value = mapped;
      ensureDisplayDevicesLoaded();
    } else {
      clients.value = [];
    }
  } catch {
    clients.value = [];
  }
}

async function registerDevice(): Promise<void> {
  if (pairing.value) return;
  pairStatus.value = null;
  pairing.value = true;
  try {
    const trimmedName = deviceName.value.trim();
    const body = { pin: pin.value.trim(), name: trimmedName };
    const r = await http.post('./api/pin', body, { validateStatus: () => true });
    const ok =
      r &&
      r.status >= 200 &&
      r.status < 300 &&
      (r.data?.status === true || r.data?.status === 'true' || r.data?.status === 1);
    pairStatus.value = !!ok;
    if (ok) {
      const prevCount = clients.value?.length || 0;
      await refreshClients();
      const deadline = Date.now() + 5000;
      const target = trimmedName.toLowerCase();
      while (Date.now() < deadline) {
        const found = clients.value?.some((c) => (c.name || '').toLowerCase() === target);
        if (found || (clients.value?.length || 0) > prevCount) break;
        await new Promise((res) => setTimeout(res, 400));
        await refreshClients();
      }
      pin.value = '';
      deviceName.value = '';
    }
  } catch {
    pairStatus.value = false;
  } finally {
    pairing.value = false;
    setTimeout(() => {
      pairStatus.value = null;
    }, 5000);
  }
}

function askConfirmUnpair(client: ClientViewModel): void {
  pendingRemoveUuid.value = client.uuid;
  pendingRemoveName.value = client && client.name ? client.name : '';
  showConfirmRemove.value = true;
}

async function confirmRemove(): Promise<void> {
  const uuid = pendingRemoveUuid.value;
  showConfirmRemove.value = false;
  pendingRemoveUuid.value = '';
  pendingRemoveName.value = '';
  if (!uuid) return;
  await unpairSingle(uuid);
}

async function unpairSingle(uuid: string): Promise<void> {
  if (removing.value[uuid]) return;
  removing.value = { ...removing.value, [uuid]: true };
  try {
    await http.post('./api/clients/unpair', { uuid }, { validateStatus: () => true });
  } catch {
  } finally {
    delete removing.value[uuid];
    removing.value = { ...removing.value };
    refreshClients();
  }
}

function askConfirmUnpairAll(): void {
  showConfirmUnpairAll.value = true;
}

async function confirmUnpairAll(): Promise<void> {
  showConfirmUnpairAll.value = false;
  await unpairAll();
}

async function unpairAll(): Promise<void> {
  unpairAllPressed.value = true;
  try {
    const r = await http.post('./api/clients/unpair-all', {}, { validateStatus: () => true });
    unpairAllStatus.value = r.data?.status === true;
  } catch {
    unpairAllStatus.value = false;
  } finally {
    unpairAllPressed.value = false;
    setTimeout(() => {
      unpairAllStatus.value = null;
    }, 5000);
    refreshClients();
  }
}

function editClient(client: ClientViewModel): void {
  for (const c of clients.value) {
    if (c.uuid !== client.uuid && c.editing) {
      c.editing = false;
      resetClientEdits(c);
    }
  }
  resetClientEdits(client);
  client.editing = true;
  ensureDisplayDevicesLoaded();
  ensureHdrProfilesLoaded();
}

function cancelEdit(client: ClientViewModel): void {
  resetClientEdits(client);
  client.editing = false;
}

async function saveClient(client: ClientViewModel): Promise<void> {
  if (saving.value[client.uuid]) return;
  saving.value = { ...saving.value, [client.uuid]: true };
  try {
    const payload: any = {
      uuid: client.uuid,
      name: (client.editName || '').trim(),
      hdr_profile: String(client.editHdrProfile ?? '').trim(),
      display_mode: (client.editDisplayMode || '').trim(),
    };

    if (!client.editDisplayOverrideEnabled) {
      payload.output_name_override = '';
      payload.always_use_virtual_display = false;
      payload.virtual_display_mode = '';
      payload.virtual_display_layout = '';
    } else if (client.editDisplaySelection === 'physical') {
      payload.output_name_override = String(client.editPhysicalOutputOverride || '').trim();
      payload.always_use_virtual_display = false;
      payload.virtual_display_mode = 'disabled';
      payload.virtual_display_layout = '';
    } else {
      payload.output_name_override = '';
      payload.always_use_virtual_display = true;
      payload.virtual_display_mode = client.editVirtualDisplayMode ?? '';
      payload.virtual_display_layout = client.editVirtualDisplayLayout ?? '';
    }

    if (!isClientDisplayOverrideValid.value) {
      message.error(t('clients.update_failed'));
      return;
    }

    payload.config_overrides =
      client.editConfigOverrides &&
      typeof client.editConfigOverrides === 'object' &&
      !Array.isArray(client.editConfigOverrides)
        ? Object.fromEntries(
            Object.entries(client.editConfigOverrides).filter(
              ([k, v]) => typeof k === 'string' && k.length > 0 && v !== undefined && v !== null,
            ),
          )
        : {};
    if (client.editPrefer10BitSdr !== null) {
      payload.prefer_10bit_sdr = client.editPrefer10BitSdr === 'enabled';
    }
    payload.hdr_profile = String(client.editHdrProfile ?? '').trim();

    const r = await http.post('./api/clients/update', payload, { validateStatus: () => true });
    const ok = r && r.status >= 200 && r.status < 300 && r.data?.status === true;
    if (!ok) {
      message.error(t('clients.update_failed'));
      return;
    }

    client.name = payload.name;
    client.hdrProfile = payload.hdr_profile;
    client.displayMode = payload.display_mode;
    client.outputOverride = payload.output_name_override;
    client.alwaysUseVirtualDisplay = payload.always_use_virtual_display;
    client.virtualDisplayMode = parseClientVirtualDisplayMode(payload.virtual_display_mode);
    client.virtualDisplayLayout = parseClientVirtualDisplayLayout(payload.virtual_display_layout);
    client.hdrProfile = payload.hdr_profile || '';
    client.prefer10BitSdr =
      payload.prefer_10bit_sdr === undefined ? null : payload.prefer_10bit_sdr ? 'enabled' : 'disabled';
    client.configOverrides =
      payload.config_overrides && typeof payload.config_overrides === 'object' && !Array.isArray(payload.config_overrides)
        ? JSON.parse(JSON.stringify(payload.config_overrides))
        : {};

    resetClientEdits(client);
    client.editing = false;
    message.success(t('clients.update_success'));
  } catch (e: any) {
    message.error(e?.message || t('clients.update_failed'));
  } finally {
    delete saving.value[client.uuid];
    saving.value = { ...saving.value };
    refreshClients();
  }
}

async function disconnectClient(client: ClientViewModel): Promise<void> {
  if (disconnecting.value[client.uuid]) return;
  disconnecting.value = { ...disconnecting.value, [client.uuid]: true };
  try {
    const r = await http.post('./api/clients/disconnect', { uuid: client.uuid }, { validateStatus: () => true });
    const ok = r && r.status >= 200 && r.status < 300 && r.data?.status === true;
    if (!ok) {
      message.error(t('clients.disconnect_failed'));
      return;
    }
    message.success(t('clients.disconnect_success'));
  } catch (e: any) {
    message.error(e?.message || t('clients.disconnect_failed'));
  } finally {
    delete disconnecting.value[client.uuid];
    disconnecting.value = { ...disconnecting.value };
    refreshClients();
  }
}

const displayDevices = ref<DisplayDevice[]>([]);
const displayDevicesLoading = ref(false);
const displayDevicesError = ref('');

async function loadDisplayDevices(): Promise<void> {
  if (!isWindows.value) return;
  displayDevicesLoading.value = true;
  displayDevicesError.value = '';
  try {
    const res = await http.get<DisplayDevice[]>('/api/display-devices', { params: { detail: 'full' } });
    displayDevices.value = Array.isArray(res.data) ? res.data : [];
  } catch (e: any) {
    displayDevicesError.value = e?.message || 'Failed to load display devices';
    displayDevices.value = [];
  } finally {
    displayDevicesLoading.value = false;
  }
}

function ensureDisplayDevicesLoaded(): void {
  if (!isWindows.value) return;
  if (!displayDevicesLoading.value && displayDevices.value.length === 0) {
    void loadDisplayDevices();
  }
}

const displayDeviceOptions = computed(() => {
  const opts: Array<{
    label: string;
    value: string;
    displayName: string;
    id: string;
    active: boolean | null;
  }> = [];
  const seen = new Set<string>();
  for (const d of displayDevices.value) {
    const value = d.device_id || d.display_name || '';
    if (!value || seen.has(value)) continue;
    const displayName = d.friendly_name || d.display_name || 'Display';
    const info = d.info as any;
    let active: boolean | null = null;
    if (info && typeof info === 'object' && 'active' in info) {
      active = !!(info as any).active;
    } else if (info) {
      active = true;
    }
    const suffix =
      active === null
        ? ''
        : active
          ? ` (${t('config.app_display_status_active')})`
          : ` (${t('config.app_display_status_inactive')})`;
    opts.push({ label: `${displayName} - ${value}${suffix}`, value, displayName, id: value, active });
    seen.add(value);
  }
  return opts;
});

onMounted(async () => {
  const auth = useAuthStore();
  await configStore.fetchConfig().catch(() => {});
  await auth.waitForAuthentication();
  await refreshClients();
});
</script>

<style scoped></style>
