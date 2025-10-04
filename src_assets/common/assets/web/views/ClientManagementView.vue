<template>
  <div class="px-4 pb-10 space-y-10">
    <h1 class="text-2xl font-semibold my-6 flex items-center gap-3 text-brand">
      <i class="fas fa-users-cog" /> {{ $t('clients.title') }}
    </h1>

    <n-card class="space-y-6" :segmented="{ content: true }">
      <template #header>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-link" /> {{ $t('clients.pair_title') }}
        </h2>
      </template>
      <div class="space-y-6">
        <n-tabs v-model:value="pairTab" type="line">
          <n-tab-pane name="otp" :tab="$t('pin.otp_pairing')">
            <div class="grid gap-6 lg:grid-cols-2">
              <div class="space-y-4">
                <div
                  class="flex flex-col items-center gap-4 rounded-2xl border border-dark/[0.08] bg-light/[0.02] p-6 dark:border-light/[0.08]"
                >
                  <div v-if="shouldShowQr" class="flex w-full justify-center">
                    <div ref="qrRef" class="rounded-xl bg-white p-4 shadow-sm"></div>
                  </div>
                  <div class="text-4xl font-semibold tracking-[0.3em] text-center">
                    {{ otpDisplay }}
                  </div>
                  <div
                    v-if="!editingHost && hostDisplay"
                    class="flex flex-col items-center gap-2 text-center text-sm"
                  >
                    <a class="text-brand break-all" :href="deepLink" target="_blank" rel="noopener">
                      art://{{ hostDisplay }}
                    </a>
                    <n-button text size="small" @click="onEditHost">
                      <i class="fas fa-pen-to-square" /> {{ $t('apps.edit') }}
                    </n-button>
                  </div>
                  <div v-else class="w-full space-y-3">
                    <n-form label-placement="top" class="space-y-3" @submit.prevent="saveHost">
                      <n-form-item label="Host">
                        <n-input v-model:value="hostAddr" placeholder="HOST" autofocus />
                      </n-form-item>
                      <n-form-item label="Port">
                        <n-input v-model:value="hostPort" placeholder="PORT" />
                      </n-form-item>
                    </n-form>
                    <div class="flex justify-end gap-3">
                      <n-button size="small" @click="cancelHostEdit">
                        {{ $t('_common.cancel') }}
                      </n-button>
                      <n-button
                        type="primary"
                        size="small"
                        :disabled="!canSaveHost"
                        @click="saveHost"
                      >
                        {{ $t('_common.save') }}
                      </n-button>
                    </div>
                  </div>
                  <div v-if="otpMessage" class="w-full">
                    <n-alert :type="otpAlertType">
                      {{ otpMessage }}
                    </n-alert>
                  </div>
                </div>
                <n-alert type="info" class="text-sm">
                  {{ $t('pin.otp_msg') }}
                </n-alert>
              </div>
              <div class="space-y-4">
                <n-form label-placement="top" class="space-y-4" @submit.prevent="requestOtp">
                  <n-form-item :label="$t('pin.otp_passphrase')">
                    <n-input
                      v-model:value="passphrase"
                      :placeholder="$t('pin.otp_passphrase')"
                      :input-props="{
                        pattern: '[0-9a-zA-Z]{4,}',
                        required: true,
                        inputmode: 'text',
                      }"
                    />
                  </n-form-item>
                  <n-form-item :label="$t('pin.device_name')">
                    <n-input v-model:value="otpDeviceName" :placeholder="$t('pin.device_name')" />
                  </n-form-item>
                  <div class="flex justify-end gap-3">
                    <n-button
                      type="primary"
                      attr-type="submit"
                      :disabled="otpRequesting || editingHost"
                    >
                      <span v-if="!otpRequesting">{{ $t('pin.generate_pin') }}</span>
                      <span v-else>{{ $t('clients.generating') }}</span>
                    </n-button>
                  </div>
                </n-form>
              </div>
            </div>
          </n-tab-pane>
          <n-tab-pane name="pin" :tab="$t('pin.pin_pairing')">
            <div class="space-y-4">
              <p class="text-sm opacity-75">{{ $t('clients.pair_desc') }}</p>
              <n-form
                class="grid grid-cols-1 gap-4 items-end md:grid-cols-3"
                @submit.prevent="registerDevice"
              >
                <n-form-item class="flex flex-col" :label="$t('navbar.pin')" label-placement="top">
                  <n-input
                    id="pin-input"
                    v-model:value="pin"
                    :placeholder="$t('navbar.pin')"
                    :input-props="{
                      inputmode: 'numeric',
                      pattern: '^[0-9]{4}$',
                      title: 'Enter 4 digits',
                      maxlength: 4,
                      required: true,
                    }"
                  />
                </n-form-item>
                <n-form-item
                  class="flex flex-col"
                  :label="$t('pin.device_name')"
                  label-placement="top"
                >
                  <n-input
                    id="name-input"
                    v-model:value="deviceName"
                    :placeholder="$t('pin.device_name')"
                    :input-props="{ required: true }"
                  />
                </n-form-item>
                <n-form-item class="flex flex-col md:items-end">
                  <n-button
                    :disabled="pairing"
                    class="w-full md:w-auto"
                    type="primary"
                    attr-type="submit"
                  >
                    <span v-if="!pairing">{{ $t('pin.send') }}</span>
                    <span v-else>{{ $t('clients.pairing') }}</span>
                  </n-button>
                </n-form-item>
              </n-form>
              <div class="space-y-2">
                <n-alert v-if="pairStatus === true" type="success">{{
                  $t('pin.pair_success')
                }}</n-alert>
                <n-alert v-if="pairStatus === false" type="error">{{
                  $t('pin.pair_failure')
                }}</n-alert>
              </div>
            </div>
          </n-tab-pane>
        </n-tabs>
        <n-alert type="warning" :title="$t('_common.warning')" class="text-sm">
          {{ $t('pin.warning_msg') }}
        </n-alert>
      </div>
    </n-card>

    <n-card class="space-y-6" :segmented="{ content: true }">
      <template #header>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-users" /> {{ $t('clients.existing_title') }}
        </h2>
      </template>
      <div class="space-y-4">
        <div class="flex flex-col gap-3 md:flex-row md:items-center">
          <p class="text-sm opacity-75 md:flex-1">{{ $t('pin.device_management_desc') }}</p>
          <n-button
            class="md:ml-auto"
            type="error"
            strong
            :disabled="unpairAllPressed || clients.length === 0"
            @click="askConfirmUnpairAll"
          >
            <i class="fas fa-user-slash" />
            {{ $t('pin.unpair_all') }}
          </n-button>
        </div>
        <p class="text-xs opacity-60">
          {{ $t('pin.device_management_warning') }}
          <a
            class="ml-1 text-brand"
            href="https://github.com/ClassicOldSong/Apollo/wiki/Permission-System"
            target="_blank"
            rel="noopener"
          >
            {{ $t('_common.learn_more') }}
          </a>
        </p>
        <n-alert v-if="showApplyMessage" type="success" class="flex items-center gap-3">
          <div>{{ $t('pin.unpair_single_success') }}</div>
          <n-button text size="small" class="ml-auto" @click="clickedApplyBanner">
            {{ $t('_common.dismiss') }}
          </n-button>
        </n-alert>
        <n-alert v-if="unpairAllStatus === true" type="success">{{
          $t('pin.unpair_all_success')
        }}</n-alert>
        <n-alert v-if="unpairAllStatus === false" type="error">{{
          $t('pin.unpair_all_error')
        }}</n-alert>
        <div v-if="clients.length > 0" class="space-y-4">
          <div
            v-for="client in clients"
            :key="client.uuid"
            class="rounded-2xl border border-dark/[0.06] bg-light/[0.02] p-4 shadow-sm transition hover:border-brand/40 dark:border-light/[0.12]"
          >
            <div class="flex flex-wrap items-center gap-3">
              <span
                class="rounded-full px-3 py-1 text-xs font-semibold text-white"
                :class="client.perm >= highlightPermissionThreshold ? 'bg-red-500' : 'bg-brand'"
              >
                [ {{ permToStr(client.perm) }} ]
              </span>
              <span class="text-base font-medium">
                {{ client.name !== '' ? client.name : $t('pin.unpair_single_unknown') }}
              </span>
              <n-tag v-if="client.connected" type="warning" size="small">{{
                $t('clients.connected')
              }}</n-tag>
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
                  @click="saveClient(client)"
                >
                  <i class="fas fa-check" />
                </n-button>
                <n-button v-if="client.editing" size="small" quaternary @click="cancelEdit(client)">
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
            <div v-if="client.editing" class="mt-4 space-y-5">
              <n-form label-placement="top" class="space-y-4" @submit.prevent>
                <n-form-item :label="$t('pin.device_name')">
                  <n-input v-model:value="client.editName" />
                </n-form-item>
                <div class="space-y-3">
                  <div class="grid gap-4 md:grid-cols-3">
                    <div v-for="group in permissionGroups" :key="group.id" class="space-y-2">
                      <div class="text-xs font-medium uppercase tracking-wide opacity-70">
                        {{ $t(group.labelKey) }}
                      </div>
                      <div class="flex flex-wrap gap-2">
                        <n-button
                          v-for="perm in group.permissions"
                          :key="perm.key"
                          size="small"
                          :type="
                            isSuppressed(client.editPerm, perm.key, perm.suppressedBy) ||
                            checkPermission(client.editPerm, perm.key)
                              ? 'primary'
                              : 'default'
                          "
                          :ghost="!checkPermission(client.editPerm, perm.key)"
                          :disabled="isSuppressed(client.editPerm, perm.key, perm.suppressedBy)"
                          @click="togglePermission(client, perm.key)"
                        >
                          {{ $t(`permissions.${perm.key}`) }}
                        </n-button>
                      </div>
                    </div>
                  </div>
                </div>
                <Checkbox
                  id="enable_legacy_ordering"
                  class="mb-1"
                  v-model="client.editEnableLegacyOrdering"
                  label="pin.enable_legacy_ordering"
                  desc="pin.enable_legacy_ordering_desc"
                  default="true"
                />
                <Checkbox
                  v-if="platform === 'windows'"
                  id="always_use_virtual_display"
                  class="mb-1"
                  v-model="client.editAlwaysUseVirtualDisplay"
                  label="pin.always_use_virtual_display"
                  desc="pin.always_use_virtual_display_desc"
                  default="false"
                />
                <n-form-item :label="$t('pin.display_mode_override')">
                  <n-input v-model:value="client.editDisplayMode" placeholder="1920x1080x59.94" />
                  <template #feedback>
                    <span class="text-xs opacity-70">
                      {{ $t('pin.display_mode_override_desc') }}
                      <a
                        class="ml-1 text-brand"
                        href="https://github.com/ClassicOldSong/Apollo/wiki/Display-Mode-Override"
                        target="_blank"
                        rel="noopener"
                      >
                        {{ $t('_common.learn_more') }}
                      </a>
                    </span>
                  </template>
                </n-form-item>
                <Checkbox
                  id="allow_client_commands"
                  class="mb-1"
                  v-model="client.editAllowClientCommands"
                  label="pin.allow_client_commands"
                  desc="pin.allow_client_commands_desc"
                  default="true"
                />
                <div
                  v-for="cmdType in commandTypes"
                  :key="cmdType"
                  v-if="client.editAllowClientCommands"
                  class="space-y-2"
                >
                  <div class="text-sm font-medium">
                    {{ $t(`pin.client_${cmdType}_cmd`) }}
                  </div>
                  <div class="text-xs opacity-70">
                    {{ $t(`pin.client_${cmdType}_cmd_desc`) }}
                    <a
                      class="ml-1 text-brand"
                      href="https://github.com/ClassicOldSong/Apollo/wiki/Client-Commands"
                      target="_blank"
                      rel="noopener"
                    >
                      {{ $t('_common.learn_more') }}
                    </a>
                  </div>
                  <div class="space-y-2">
                    <div
                      v-for="(entry, idx) in commandList(client, cmdType)"
                      :key="`cmd-${cmdType}-${client.uuid}-${idx}`"
                      class="flex flex-wrap items-center gap-3"
                    >
                      <n-input
                        v-model:value="entry.cmd"
                        class="min-w-[12rem] flex-1"
                        placeholder="cmd.exe /c ..."
                      />
                      <n-checkbox v-if="platform === 'windows'" v-model:checked="entry.elevated">
                        {{ $t('_common.elevated') }}
                      </n-checkbox>
                      <div class="ml-auto flex gap-2">
                        <n-button
                          size="small"
                          quaternary
                          type="error"
                          @click="removeCmd(commandList(client, cmdType), idx)"
                        >
                          <i class="fas fa-trash" />
                        </n-button>
                        <n-button
                          size="small"
                          quaternary
                          type="success"
                          @click="addCmd(commandList(client, cmdType), idx)"
                        >
                          <i class="fas fa-plus" />
                        </n-button>
                      </div>
                    </div>
                    <n-button
                      size="small"
                      quaternary
                      type="success"
                      @click="addCmd(commandList(client, cmdType), -1)"
                    >
                      &plus; {{ $t('config.add') }}
                    </n-button>
                  </div>
                </div>
              </n-form>
            </div>
          </div>
        </div>
        <div v-else class="p-6 text-center text-sm italic opacity-70">
          {{ $t('pin.unpair_single_no_devices') }}
        </div>
      </div>
    </n-card>

    <TrustedDevicesCard />

    <ApiTokenManager />

    <n-modal :show="showConfirmRemove" @update:show="(v) => (showConfirmRemove = v)">
      <n-card
        :title="
          $t('clients.confirm_remove_title_named', {
            name: pendingRemoveName || $t('pin.unpair_single_unknown'),
          })
        "
        style="max-width: 32rem; width: 100%"
        :bordered="false"
      >
        <div class="text-sm text-center">
          {{
            $t('clients.confirm_remove_message_named', {
              name: pendingRemoveName || $t('pin.unpair_single_unknown'),
            })
          }}
        </div>
        <template #footer>
          <div class="flex w-full items-center justify-center gap-3">
            <n-button type="default" strong @click="showConfirmRemove = false">
              {{ $t('cancel') }}
            </n-button>
            <n-button secondary @click="confirmRemove">{{ $t('clients.remove') }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>

    <n-modal :show="showConfirmUnpairAll" @update:show="(v) => (showConfirmUnpairAll = v)">
      <n-card
        :title="$t('clients.confirm_unpair_all_title')"
        style="max-width: 32rem; width: 100%"
        :bordered="false"
      >
        <div class="text-sm text-center">
          {{ $t('clients.confirm_unpair_all_message_count', { count: clients.length }) }}
        </div>
        <template #footer>
          <div class="flex w-full items-center justify-center gap-3">
            <n-button type="default" strong @click="showConfirmUnpairAll = false">
              {{ $t('cancel') }}
            </n-button>
            <n-button secondary @click="confirmUnpairAll">
              {{ $t('troubleshooting.unpair_all') }}
            </n-button>
          </div>
        </template>
      </n-card>
    </n-modal>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import {
  NAlert,
  NButton,
  NCard,
  NCheckbox,
  NForm,
  NFormItem,
  NInput,
  NModal,
  NTabs,
  NTabPane,
  NTag,
  useMessage,
} from 'naive-ui';
import ApiTokenManager from '@/ApiTokenManager.vue';
import TrustedDevicesCard from '@/components/TrustedDevicesCard.vue';
import Checkbox from '@/Checkbox.vue';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';

type CommandType = 'do' | 'undo';

type PermissionToggleKey =
  | 'list'
  | 'view'
  | 'launch'
  | 'clipboard_set'
  | 'clipboard_read'
  | 'server_cmd'
  | 'input_controller'
  | 'input_touch'
  | 'input_pen'
  | 'input_mouse'
  | 'input_kbd';

interface ClientCommand {
  cmd: string;
  elevated: boolean;
}

interface ClientApiEntry {
  uuid?: string;
  name?: string;
  display_mode?: string;
  perm?: number | string;
  connected?: boolean;
  allow_client_commands?: boolean | string | number;
  enable_legacy_ordering?: boolean | string | number;
  always_use_virtual_display?: boolean | string | number;
  do?: Array<{ cmd?: string; elevated?: boolean | string | number }>;
  undo?: Array<{ cmd?: string; elevated?: boolean | string | number }>;
}

interface ClientsListResponse {
  status?: boolean;
  named_certs?: ClientApiEntry[];
  platform?: string;
}

interface PinResponse {
  status?: boolean | string | number;
}

interface OtpResponse {
  status?: boolean;
  message?: string;
  otp?: string;
  ip?: string;
  port?: string | number;
  name?: string;
}

interface SaveClientPayload {
  uuid: string;
  name: string;
  display_mode: string;
  allow_client_commands: boolean;
  enable_legacy_ordering: boolean;
  always_use_virtual_display: boolean;
  perm: number;
  do: ClientCommand[];
  undo: ClientCommand[];
}

interface HostInfo {
  hostAddr: string;
  hostPort: string;
}

interface ClientViewModel {
  uuid: string;
  name: string;
  displayMode: string;
  perm: number;
  connected: boolean;
  allowClientCommands: boolean;
  enableLegacyOrdering: boolean;
  alwaysUseVirtualDisplay: boolean;
  doCommands: ClientCommand[];
  undoCommands: ClientCommand[];
  editing: boolean;
  editName: string;
  editDisplayMode: string;
  editPerm: number;
  editAllowClientCommands: boolean;
  editEnableLegacyOrdering: boolean;
  editAlwaysUseVirtualDisplay: boolean;
  editDo: ClientCommand[];
  editUndo: ClientCommand[];
}

interface PermissionGroup {
  id: string;
  labelKey: string;
  permissions: Array<{ key: PermissionToggleKey; suppressedBy: PermissionToggleKey[] }>;
}

const permissionMapping = {
  input_controller: 0x00000100,
  input_touch: 0x00000200,
  input_pen: 0x00000400,
  input_mouse: 0x00000800,
  input_kbd: 0x00001000,
  _all_inputs: 0x00001f00,
  clipboard_set: 0x00010000,
  clipboard_read: 0x00020000,
  file_upload: 0x00040000,
  file_dwnload: 0x00080000,
  server_cmd: 0x00100000,
  _all_operations: 0x001f0000,
  list: 0x01000000,
  view: 0x02000000,
  launch: 0x04000000,
  _allow_view: 0x06000000,
  _all_actions: 0x07000000,
  _default: 0x03000000,
  _no: 0x00000000,
  _all: 0x071f1f00,
} as const;

const permissionGroups: PermissionGroup[] = [
  {
    id: 'actions',
    labelKey: 'permissions.group_action',
    permissions: [
      { key: 'list', suppressedBy: ['view', 'launch'] },
      { key: 'view', suppressedBy: ['launch'] },
      { key: 'launch', suppressedBy: [] },
    ],
  },
  {
    id: 'operations',
    labelKey: 'permissions.group_operation',
    permissions: [
      { key: 'clipboard_set', suppressedBy: [] },
      { key: 'clipboard_read', suppressedBy: [] },
      { key: 'server_cmd', suppressedBy: [] },
    ],
  },
  {
    id: 'inputs',
    labelKey: 'permissions.group_input',
    permissions: [
      { key: 'input_controller', suppressedBy: [] },
      { key: 'input_touch', suppressedBy: [] },
      { key: 'input_pen', suppressedBy: [] },
      { key: 'input_mouse', suppressedBy: [] },
      { key: 'input_kbd', suppressedBy: [] },
    ],
  },
];

const commandTypes: CommandType[] = ['do', 'undo'];
const MODE_OVERRIDE_PATTERN = /^\d+x\d+x\d+(?:\.\d+)?$/;
const highlightPermissionThreshold = 0x04000000;

const { t } = useI18n();
const message = useMessage();
const authStore = useAuthStore();

const initialHash = typeof window !== 'undefined' ? window.location.hash : '';
const pairTab = ref<'otp' | 'pin'>(initialHash === '#PIN' ? 'pin' : 'otp');

const pin = ref('');
const deviceName = ref('');
const pairing = ref(false);
const pairStatus = ref<boolean | null>(null);
let pairStatusReset: number | null = null;

const passphrase = ref('');
const otpDeviceName = ref('');
const otpRequesting = ref(false);
const otp = ref('');
const otpMessage = ref('');
const otpStatus = ref<'success' | 'warning' | 'danger'>('warning');
const hostName = ref('');
const editingHost = ref(false);
const hostAddr = ref('');
const hostPort = ref('');
const hostInfoCache = ref<HostInfo | null>(null);
const hostManuallySet = ref(false);
const qrRef = ref<HTMLDivElement | null>(null);
let qrContainer: HTMLDivElement | null = null;
let qrInstance: any = null;
let qrScriptPromise: Promise<any> | null = null;
let otpResetHandle: number | null = null;

if (typeof window !== 'undefined') {
  try {
    const raw = window.sessionStorage.getItem('hostInfo');
    if (raw) {
      const parsed = JSON.parse(raw) as HostInfo;
      hostInfoCache.value = {
        hostAddr: parsed.hostAddr ?? '',
        hostPort: parsed.hostPort ?? '',
      };
      hostManuallySet.value = true;
      hostAddr.value = hostInfoCache.value.hostAddr;
      hostPort.value = hostInfoCache.value.hostPort;
    }
  } catch {
    hostInfoCache.value = null;
  }
}

const clients = ref<ClientViewModel[]>([]);
const platform = ref('');
const removing = ref<Record<string, boolean>>({});
const disconnecting = ref<Record<string, boolean>>({});
const showApplyMessage = ref(false);
const unpairAllPressed = ref(false);
const unpairAllStatus = ref<boolean | null>(null);
const showConfirmRemove = ref(false);
const pendingRemoveUuid = ref('');
const pendingRemoveName = ref('');
const showConfirmUnpairAll = ref(false);
const currentEditingClient = ref<ClientViewModel | null>(null);

const shouldShowQr = computed(() => {
  return (
    editingHost.value ||
    (otp.value !== '' && hostAddr.value.trim() !== '' && hostPort.value.trim() !== '')
  );
});

const hostDisplay = computed(() => {
  const addr = hostAddr.value.trim();
  const port = hostPort.value.trim();
  return addr && port ? `${addr}:${port}` : '';
});

const deepLink = computed(() => {
  const addr = hostAddr.value.trim();
  const port = hostPort.value.trim();
  if (!addr || !port) return '';
  const params = new URLSearchParams();
  if (otp.value) params.set('pin', otp.value);
  const trimmedPassphrase = passphrase.value.trim();
  if (trimmedPassphrase) params.set('passphrase', trimmedPassphrase);
  const trimmedName = hostName.value.trim();
  if (trimmedName) params.set('name', trimmedName);
  const suffix = params.toString();
  const base = `art://${addr}:${port}`;
  return encodeURI(suffix ? `${base}?${suffix}` : base);
});

const otpDisplay = computed(() => (otp.value ? otp.value : '????'));

const otpAlertType = computed<'success' | 'warning' | 'error'>(() => {
  if (otpStatus.value === 'success') return 'success';
  if (otpStatus.value === 'danger') return 'error';
  return 'warning';
});

const canSaveHost = computed(() => {
  return hostAddr.value.trim().length > 0 && hostPort.value.trim().length > 0;
});

function toBool(value: unknown, defaultValue = false): boolean {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return value !== 0;
  if (typeof value === 'string') {
    const lower = value.toLowerCase();
    if (['true', '1', 'yes', 'on'].includes(lower)) return true;
    if (['false', '0', 'no', 'off'].includes(lower)) return false;
  }
  return defaultValue;
}

function cloneCommand(entry?: ClientCommand): ClientCommand {
  return {
    cmd: entry?.cmd ?? '',
    elevated: !!entry?.elevated,
  };
}

function normalizeCommands(entries?: any): ClientCommand[] {
  if (!Array.isArray(entries)) return [];
  return entries.map((entry: any) => ({
    cmd: typeof entry?.cmd === 'string' ? entry.cmd : '',
    elevated: toBool(entry?.elevated, false),
  }));
}

function createClientViewModel(entry: ClientApiEntry): ClientViewModel {
  const doCommands = normalizeCommands(entry.do);
  const undoCommands = normalizeCommands(entry.undo);
  const perm =
    typeof entry.perm === 'number'
      ? entry.perm
      : Number.parseInt(String(entry.perm ?? '0'), 10) || 0;
  const name = entry.name ?? '';
  const displayMode = entry.display_mode ?? '';
  const allowCommands = toBool(entry.allow_client_commands, true);
  const legacyOrdering = toBool(entry.enable_legacy_ordering, true);
  const alwaysVirtual = toBool(entry.always_use_virtual_display, false);
  return {
    uuid: entry.uuid ?? '',
    name,
    displayMode,
    perm,
    connected: !!entry.connected,
    allowClientCommands: allowCommands,
    enableLegacyOrdering: legacyOrdering,
    alwaysUseVirtualDisplay: alwaysVirtual,
    doCommands,
    undoCommands,
    editing: false,
    editName: name,
    editDisplayMode: displayMode,
    editPerm: perm,
    editAllowClientCommands: allowCommands,
    editEnableLegacyOrdering: legacyOrdering,
    editAlwaysUseVirtualDisplay: alwaysVirtual,
    editDo: doCommands.map(cloneCommand),
    editUndo: undoCommands.map(cloneCommand),
  };
}

function resetClientEdits(client: ClientViewModel) {
  client.editName = client.name;
  client.editDisplayMode = client.displayMode;
  client.editPerm = client.perm;
  client.editAllowClientCommands = client.allowClientCommands;
  client.editEnableLegacyOrdering = client.enableLegacyOrdering;
  client.editAlwaysUseVirtualDisplay = client.alwaysUseVirtualDisplay;
  client.editDo = client.doCommands.map(cloneCommand);
  client.editUndo = client.undoCommands.map(cloneCommand);
}

async function refreshClients(): Promise<void> {
  try {
    const r = await http.get<ClientsListResponse>('./api/clients/list', {
      validateStatus: () => true,
    });
    const response = r.data ?? {};
    if (response.status && Array.isArray(response.named_certs)) {
      platform.value = typeof response.platform === 'string' ? response.platform : '';
      const mapped = response.named_certs.map(createClientViewModel);
      mapped.sort((a, b) => {
        const nameA = a.name.toLowerCase();
        const nameB = b.name.toLowerCase();
        if (nameA === nameB) return a.uuid.localeCompare(b.uuid);
        if (nameA === '') return 1;
        if (nameB === '') return -1;
        return nameA.localeCompare(nameB);
      });
      clients.value = mapped;
    } else {
      clients.value = [];
    }
  } catch {
    clients.value = [];
  } finally {
    currentEditingClient.value = null;
  }
}

function commandList(client: ClientViewModel, type: CommandType): ClientCommand[] {
  return type === 'do' ? client.editDo : client.editUndo;
}

function addCmd(list: ClientCommand[], idx: number) {
  const entry = cloneCommand();
  if (idx < 0) {
    list.push(entry);
  } else {
    list.splice(idx + 1, 0, entry);
  }
}

function removeCmd(list: ClientCommand[], idx: number) {
  list.splice(idx, 1);
}

function permToStr(perm: number): string {
  const segments = [];
  segments.push((perm >> 24) & 0xff);
  segments.push((perm >> 16) & 0xff);
  segments.push((perm >> 8) & 0xff);
  return segments.map((seg) => seg.toString(16).toUpperCase().padStart(2, '0')).join(' ');
}

function checkPermission(perm: number, permission: PermissionToggleKey): boolean {
  return (perm & permissionMapping[permission]) !== 0;
}

function isSuppressed(
  perm: number,
  permission: PermissionToggleKey,
  suppressedBy: PermissionToggleKey[],
): boolean {
  return suppressedBy.some((suppressed) => checkPermission(perm, suppressed));
}

function togglePermission(client: ClientViewModel, permission: PermissionToggleKey) {
  client.editPerm ^= permissionMapping[permission];
}

function editClient(client: ClientViewModel) {
  if (client.editing) return;
  if (currentEditingClient.value && currentEditingClient.value !== client) {
    cancelEdit(currentEditingClient.value);
  }
  resetClientEdits(client);
  client.editing = true;
  currentEditingClient.value = client;
}

function cancelEdit(client: ClientViewModel) {
  resetClientEdits(client);
  client.editing = false;
  if (currentEditingClient.value === client) {
    currentEditingClient.value = null;
  }
}

function applyClientEditsToBase(client: ClientViewModel, payload: SaveClientPayload) {
  client.name = payload.name;
  client.displayMode = payload.display_mode;
  client.perm = payload.perm;
  client.allowClientCommands = payload.allow_client_commands;
  client.enableLegacyOrdering = payload.enable_legacy_ordering;
  client.alwaysUseVirtualDisplay = payload.always_use_virtual_display;
  client.doCommands = payload.do.map(cloneCommand);
  client.undoCommands = payload.undo.map(cloneCommand);
  resetClientEdits(client);
}

async function saveClient(client: ClientViewModel): Promise<void> {
  if (!client.editing) return;
  const trimmedDisplayMode = client.editDisplayMode.trim();
  if (trimmedDisplayMode && !MODE_OVERRIDE_PATTERN.test(trimmedDisplayMode)) {
    message.error(t('pin.display_mode_override_error'));
    return;
  }
  const trimmedName = client.editName.trim();
  const cleanedDo = client.editDo.reduce<ClientCommand[]>((acc, entry) => {
    const cmd = entry.cmd.trim();
    if (cmd) acc.push({ cmd, elevated: !!entry.elevated });
    return acc;
  }, []);
  const cleanedUndo = client.editUndo.reduce<ClientCommand[]>((acc, entry) => {
    const cmd = entry.cmd.trim();
    if (cmd) acc.push({ cmd, elevated: !!entry.elevated });
    return acc;
  }, []);
  const payload: SaveClientPayload = {
    uuid: client.uuid,
    name: trimmedName,
    display_mode: trimmedDisplayMode,
    allow_client_commands: !!client.editAllowClientCommands,
    enable_legacy_ordering: !!client.editEnableLegacyOrdering,
    always_use_virtual_display: !!client.editAlwaysUseVirtualDisplay,
    perm: client.editPerm & permissionMapping._all,
    do: cleanedDo,
    undo: cleanedUndo,
  };
  client.editing = false;
  currentEditingClient.value = null;
  try {
    const resp = await http.post<{ status?: boolean; message?: string }>(
      './api/clients/update',
      payload,
      { validateStatus: () => true },
    );
    if (!resp.data?.status) {
      throw new Error(resp.data?.message ?? '');
    }
    applyClientEditsToBase(client, payload);
    message.success(t('_common.success'));
    await refreshClients();
  } catch (err) {
    const errMessage = err instanceof Error ? err.message : '';
    message.error(`${t('pin.save_client_error')}${errMessage}`.trim());
  } finally {
    setTimeout(() => {
      refreshClients();
    }, 1000);
  }
}

async function disconnectClient(client: ClientViewModel): Promise<void> {
  if (disconnecting.value[client.uuid]) return;
  disconnecting.value = { ...disconnecting.value, [client.uuid]: true };
  try {
    await http.post(
      './api/clients/disconnect',
      { uuid: client.uuid },
      { validateStatus: () => true },
    );
  } catch {
  } finally {
    const map = { ...disconnecting.value };
    delete map[client.uuid];
    disconnecting.value = map;
    setTimeout(() => {
      refreshClients();
    }, 1000);
  }
}

function clickedApplyBanner() {
  showApplyMessage.value = false;
}

function askConfirmUnpair(client: ClientViewModel) {
  pendingRemoveUuid.value = client.uuid;
  pendingRemoveName.value = client.name;
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
  if (!uuid) return;
  removing.value = { ...removing.value, [uuid]: true };
  try {
    await http.post('./api/clients/unpair', { uuid }, { validateStatus: () => true });
    showApplyMessage.value = true;
  } catch {
  } finally {
    const map = { ...removing.value };
    delete map[uuid];
    removing.value = map;
    setTimeout(() => {
      refreshClients();
    }, 1000);
  }
}

function askConfirmUnpairAll() {
  showConfirmUnpairAll.value = true;
}

async function confirmUnpairAll(): Promise<void> {
  showConfirmUnpairAll.value = false;
  await unpairAll();
}

async function unpairAll(): Promise<void> {
  if (unpairAllPressed.value) return;
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

function setHostFields(info: HostInfo) {
  hostAddr.value = info.hostAddr;
  hostPort.value = info.hostPort;
}

function saveHostCache(info: HostInfo, manual: boolean) {
  hostInfoCache.value = { hostAddr: info.hostAddr, hostPort: info.hostPort };
  if (manual && typeof window !== 'undefined') {
    window.sessionStorage.setItem('hostInfo', JSON.stringify(hostInfoCache.value));
    hostManuallySet.value = true;
  }
}

async function saveHost(): Promise<void> {
  if (!canSaveHost.value) return;
  const info = {
    hostAddr: hostAddr.value.trim(),
    hostPort: hostPort.value.trim(),
  };
  saveHostCache(info, true);
  editingHost.value = false;
  if (deepLink.value) {
    await renderQr(deepLink.value);
  }
}

function cancelHostEdit() {
  editingHost.value = false;
  if (hostInfoCache.value) {
    setHostFields(hostInfoCache.value);
  }
}

function onEditHost() {
  editingHost.value = true;
  if (hostInfoCache.value) {
    setHostFields(hostInfoCache.value);
  }
}

function clearQr() {
  if (qrInstance && typeof qrInstance.clear === 'function') {
    qrInstance.clear();
  }
  if (qrRef.value) {
    qrRef.value.innerHTML = '';
  }
}

async function ensureQrCodeLib(): Promise<any> {
  if (typeof window === 'undefined') return null;
  if ((window as any).QRCode) return (window as any).QRCode;
  if (!qrScriptPromise) {
    qrScriptPromise = new Promise((resolve, reject) => {
      const script = document.createElement('script');
      script.src = '/assets/js/qrcode.min.js';
      script.async = true;
      script.onload = () => resolve((window as any).QRCode);
      script.onerror = reject;
      document.head.appendChild(script);
    });
  }
  try {
    return await qrScriptPromise;
  } catch (err) {
    console.error('Failed to load QR code library', err);
    return null;
  }
}

async function renderQr(url: string): Promise<void> {
  if (!url) {
    clearQr();
    return;
  }
  const QRCode = await ensureQrCodeLib();
  if (!QRCode) return;
  if (!qrContainer) {
    qrContainer = document.createElement('div');
    qrContainer.className = 'rounded-xl bg-white p-4 shadow-sm';
  }
  if (!qrInstance) {
    qrInstance = new QRCode(qrContainer);
  } else if (typeof qrInstance.clear === 'function') {
    qrInstance.clear();
  }
  if (typeof qrInstance.makeCode === 'function') {
    qrInstance.makeCode(url);
  }
  if (qrRef.value && qrContainer) {
    qrRef.value.innerHTML = '';
    qrRef.value.appendChild(qrContainer);
  }
}

function clearOtpResetTimer() {
  if (otpResetHandle !== null && typeof window !== 'undefined') {
    window.clearTimeout(otpResetHandle);
    otpResetHandle = null;
  }
}

function scheduleOtpReset() {
  clearOtpResetTimer();
  if (typeof window === 'undefined') return;
  otpResetHandle = window.setTimeout(
    () => {
      otp.value = t('pin.otp_expired');
      otpMessage.value = t('pin.otp_expired_msg');
      otpStatus.value = 'warning';
    },
    3 * 60 * 1000,
  );
}

function resetPairingForms() {
  pin.value = '';
  deviceName.value = '';
  pairStatus.value = null;
  passphrase.value = '';
  otpDeviceName.value = '';
  otp.value = '';
  otpMessage.value = '';
  otpStatus.value = 'warning';
  hostName.value = '';
  clearOtpResetTimer();
  if (!editingHost.value) {
    clearQr();
  }
  if (pairStatusReset !== null && typeof window !== 'undefined') {
    window.clearTimeout(pairStatusReset);
    pairStatusReset = null;
  }
}

function schedulePairStatusClear() {
  if (typeof window === 'undefined') return;
  if (pairStatusReset !== null) {
    window.clearTimeout(pairStatusReset);
  }
  pairStatusReset = window.setTimeout(() => {
    pairStatus.value = null;
    pairStatusReset = null;
  }, 5000);
}

async function registerDevice(): Promise<void> {
  if (pairing.value) return;
  pairStatus.value = null;
  pairing.value = true;
  try {
    const trimmedPin = pin.value.trim();
    const trimmedName = deviceName.value.trim();
    const body = { pin: trimmedPin, name: trimmedName };
    const r = await http.post<PinResponse>('./api/pin', body, {
      validateStatus: () => true,
    });
    const ok =
      r &&
      r.status >= 200 &&
      r.status < 300 &&
      (r.data?.status === true || r.data?.status === 'true' || r.data?.status === 1);
    pairStatus.value = !!ok;
    if (ok) {
      message.success(t('pin.pair_success'));
      message.info(t('pin.pair_success_check_perm'));
      const previousCount = clients.value.length;
      await refreshClients();
      const deadline = Date.now() + 5000;
      const target = trimmedName.toLowerCase();
      while (Date.now() < deadline) {
        const found = clients.value.some((c) => c.name.toLowerCase() === target);
        if (found || clients.value.length > previousCount) break;
        await new Promise((resolve) => setTimeout(resolve, 400));
        await refreshClients();
      }
      pin.value = '';
      deviceName.value = '';
    }
  } catch {
    pairStatus.value = false;
    message.error(t('pin.pair_failure'));
  } finally {
    pairing.value = false;
    schedulePairStatusClear();
  }
}

async function requestOtp(): Promise<void> {
  if (editingHost.value || otpRequesting.value) return;
  otpRequesting.value = true;
  otpMessage.value = '';
  otpStatus.value = 'warning';
  try {
    const body = {
      passphrase: passphrase.value.trim(),
      deviceName: otpDeviceName.value.trim(),
    };
    const resp = await http.post<OtpResponse>('./api/otp', body, { validateStatus: () => true });
    const data = resp.data ?? {};
    if (!data.status) {
      otpStatus.value = 'danger';
      otpMessage.value = data.message ?? '';
      clearQr();
      return;
    }
    otp.value = data.otp ?? '';
    hostName.value = data.name ?? '';
    otpStatus.value = 'success';
    otpMessage.value = t('pin.otp_success');

    if (hostManuallySet.value && hostInfoCache.value) {
      setHostFields(hostInfoCache.value);
    } else {
      if (typeof data.ip === 'string' && data.ip) {
        hostAddr.value = data.ip;
      } else if (typeof window !== 'undefined') {
        hostAddr.value = window.location.hostname;
      }
      let portValue: string | null = null;
      if (typeof data.port !== 'undefined') {
        const numeric = Number(data.port);
        if (!Number.isNaN(numeric) && numeric > 0) {
          portValue = String(Math.floor(numeric));
        }
      }
      if (!portValue && typeof window !== 'undefined') {
        const locPort = Number.parseInt(window.location.port || '', 10);
        if (!Number.isNaN(locPort) && locPort > 0) {
          const candidate = locPort - 1;
          if (candidate > 0) portValue = String(candidate);
        }
      }
      hostPort.value = portValue ?? hostPort.value;
      saveHostCache(
        {
          hostAddr: hostAddr.value,
          hostPort: hostPort.value,
        },
        false,
      );
    }

    if (deepLink.value) {
      await renderQr(deepLink.value);
    }
    scheduleOtpReset();

    const isLocalHost =
      typeof window !== 'undefined' &&
      ['localhost', '127.0.0.1', '[::1]'].includes(window.location.hostname);
    if (!isLocalHost && typeof window !== 'undefined') {
      setTimeout(() => {
        if (window.confirm(t('pin.otp_pair_now'))) {
          window.open(deepLink.value, '_blank', 'noopener');
        }
      }, 0);
    }
  } catch (err) {
    otpStatus.value = 'danger';
    const errMessage = err instanceof Error ? err.message : '';
    otpMessage.value = errMessage || t('pin.otp_expired_msg');
    clearQr();
  } finally {
    otpRequesting.value = false;
  }
}

watch(pairTab, (tab) => {
  if (typeof window !== 'undefined') {
    window.location.hash = tab === 'pin' ? '#PIN' : '#OTP';
  }
  resetPairingForms();
  if (tab === 'otp' && shouldShowQr.value && deepLink.value) {
    nextTick(() => {
      renderQr(deepLink.value);
    });
  }
});

watch([deepLink, shouldShowQr], async ([link, show]) => {
  if (!show || !link) {
    if (!editingHost.value) {
      clearQr();
    }
    return;
  }
  await nextTick();
  await renderQr(link);
});

onMounted(async () => {
  await authStore.waitForAuthentication();
  await refreshClients();
  if (pairTab.value === 'otp' && shouldShowQr.value && deepLink.value) {
    await nextTick();
    await renderQr(deepLink.value);
  }
});

onBeforeUnmount(() => {
  clearOtpResetTimer();
  if (pairStatusReset !== null && typeof window !== 'undefined') {
    window.clearTimeout(pairStatusReset);
    pairStatusReset = null;
  }
});
</script>

<style scoped></style>
