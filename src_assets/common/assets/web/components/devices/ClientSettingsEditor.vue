<script setup lang="ts">
import { computed, reactive, toRaw, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import SettingsOverrideEditor from '@/components/settings/SettingsOverrideEditor.vue';
import { AppButton, SettingRow, StatusBadge, UiIcon } from '@/components/ui';

export type ClientVirtualDisplayMode = 'global' | 'per_client' | 'shared' | 'disabled' | null;
export type ClientDisplaySelection = 'physical' | 'virtual';
export type ClientVirtualDisplayLayout =
  | 'exclusive'
  | 'extended'
  | 'extended_primary'
  | 'extended_isolated'
  | 'extended_primary_isolated'
  | null;

export interface ClientCommandDraft {
  command: string;
  elevated?: boolean;
}

export interface ClientDeviceDraft {
  name: string;
  permissions: number;
  enableLegacyOrdering: boolean;
  allowClientCommands: boolean;
  doCommands: ClientCommandDraft[];
  undoCommands: ClientCommandDraft[];
  displayMode: string;
  displayOverrideEnabled: boolean;
  displaySelection: ClientDisplaySelection;
  physicalOutputOverride: string | null;
  virtualDisplayMode: ClientVirtualDisplayMode;
  virtualDisplayLayout: ClientVirtualDisplayLayout;
  hdrProfile: string;
  prefer10BitSdr: boolean;
  configOverrides: Record<string, unknown>;
}

export interface ClientSettingsMetadata {
  gpus?: Array<{ description?: string; pnp_id?: string }>;
  platform?: string;
}

export interface DisplayDevice {
  device_id?: string;
  display_name?: string;
  friendly_name?: string;
  info?: { active?: unknown };
}

export interface HdrProfileEntry {
  filename?: string;
  added_ms?: number;
}

const props = defineProps<{
  modelValue: ClientDeviceDraft;
  commonSettings: Record<string, unknown>;
  metadata: ClientSettingsMetadata;
  displayDevices: DisplayDevice[];
  displayDevicesLoading: boolean;
  displayDevicesLoaded: boolean;
  displayDevicesError: string;
  hdrProfiles: HdrProfileEntry[];
  hdrProfilesLoading: boolean;
  hdrProfilesLoaded: boolean;
  hdrProfilesError: string;
  busy: boolean;
  controlIdPrefix: string;
}>();

const emit = defineEmits<{
  'update:modelValue': [value: ClientDeviceDraft];
  save: [];
  cancel: [];
  'load-display-devices': [force?: boolean];
  'load-hdr-profiles': [force?: boolean];
}>();

const { t } = useI18n();

function cloneDraft(value: ClientDeviceDraft): ClientDeviceDraft {
  return {
    ...value,
    configOverrides: structuredClone(toRaw(value.configOverrides ?? {})),
    doCommands: structuredClone(toRaw(value.doCommands ?? [])),
    undoCommands: structuredClone(toRaw(value.undoCommands ?? [])),
  };
}

function serialized(value: ClientDeviceDraft): string {
  return JSON.stringify({
    ...value,
    configOverrides: Object.fromEntries(
      Object.entries(value.configOverrides ?? {}).sort(([left], [right]) =>
        left.localeCompare(right),
      ),
    ),
  });
}

const draft = reactive<ClientDeviceDraft>(cloneDraft(props.modelValue));
let lastSerialized = serialized(props.modelValue);

watch(
  () => props.modelValue,
  (value) => {
    const nextSerialized = serialized(value);
    if (nextSerialized === lastSerialized) return;
    Object.assign(draft, cloneDraft(value));
    lastSerialized = nextSerialized;
  },
  { deep: true },
);

watch(
  draft,
  (value) => {
    const nextSerialized = serialized(value);
    if (nextSerialized === lastSerialized) return;
    lastSerialized = nextSerialized;
    emit('update:modelValue', cloneDraft(value));
  },
  { deep: true },
);

const isWindows = computed(() =>
  String(props.metadata.platform ?? '')
    .toLocaleLowerCase()
    .includes('windows'),
);

const permissionOptions = [
  { key: 'list', mask: 0x01000000 },
  { key: 'view', mask: 0x02000000 },
  { key: 'launch', mask: 0x04000000 },
  { key: 'clipboard_set', mask: 0x00010000 },
  { key: 'clipboard_read', mask: 0x00020000 },
  { key: 'server_cmd', mask: 0x00100000 },
  { key: 'input_controller', mask: 0x00000100 },
  { key: 'input_touch', mask: 0x00000200 },
  { key: 'input_pen', mask: 0x00000400 },
  { key: 'input_mouse', mask: 0x00000800 },
  { key: 'input_kbd', mask: 0x00001000 },
] as const;

function hasPermission(mask: number): boolean {
  return Boolean(draft.permissions & mask);
}

function updatePermission(mask: number, event: Event): void {
  const enabled = (event.target as HTMLInputElement).checked;
  draft.permissions = enabled ? draft.permissions | mask : draft.permissions & ~mask;
}

function normalizeVirtualMode(value: unknown): ClientVirtualDisplayMode {
  const mode = String(value ?? '')
    .trim()
    .toLocaleLowerCase();
  return ['global', 'per_client', 'shared', 'disabled'].includes(mode)
    ? (mode as ClientVirtualDisplayMode)
    : null;
}

function normalizeVirtualLayout(value: unknown): ClientVirtualDisplayLayout {
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
    ? (layout as ClientVirtualDisplayLayout)
    : null;
}

const globalVirtualDisplayMode = computed(() => {
  const mode = normalizeVirtualMode(props.commonSettings.virtual_display_mode);
  return mode === 'disabled' || mode === 'shared' ? mode : 'per_client';
});

const globalVirtualDisplayLayout = computed(
  () => normalizeVirtualLayout(props.commonSettings.virtual_display_layout) ?? 'exclusive',
);

const globalVirtualDisplayModeLabel = computed(() => {
  if (globalVirtualDisplayMode.value === 'shared') return t('config.virtual_display_mode_shared');
  if (globalVirtualDisplayMode.value === 'disabled')
    return t('config.virtual_display_mode_disabled');
  return t('config.virtual_display_mode_per_client');
});

const globalVirtualDisplayLayoutLabel = computed(() =>
  t(`config.virtual_display_layout_${globalVirtualDisplayLayout.value}`),
);

const globalVirtualDisplayScale = computed(() => {
  const value = Number(props.commonSettings.dd_virtual_display_scale ?? -1);
  return Number.isFinite(value) ? value : -1;
});

const globalVirtualDisplayScaleLabel = computed(() => {
  let valueLabel: string;
  if (globalVirtualDisplayScale.value < 0)
    valueLabel = t('ui.settings.options.virtual_scale.recommended');
  else if (globalVirtualDisplayScale.value === 0)
    valueLabel = t('ui.settings.options.virtual_scale.preserve');
  else valueLabel = `${globalVirtualDisplayScale.value}%`;
  return t('ui.devices.editor.use_common_value', { value: valueLabel });
});

const displayOverrideSummary = computed(() => {
  if (!draft.displayOverrideEnabled) return t('clients.display_route_global');
  if (draft.displaySelection === 'physical') {
    return draft.physicalOutputOverride
      ? t('clients.display_route_physical', { display: draft.physicalOutputOverride })
      : t('config.app_display_override_physical');
  }
  const mode =
    draft.virtualDisplayMode === 'global' || draft.virtualDisplayMode === null
      ? t('clients.display_route_global')
      : t('clients.display_route_virtual', {
          mode:
            draft.virtualDisplayMode === 'shared'
              ? t('config.virtual_display_mode_shared')
              : t('config.virtual_display_mode_per_client'),
        });
  return mode;
});

const hdrProfileOptions = computed(() => {
  const profiles = [...props.hdrProfiles].sort(
    (left, right) => Number(right.added_ms ?? 0) - Number(left.added_ms ?? 0),
  );
  const options = [
    { label: t('clients.hdr_profile_auto'), value: '' },
    ...profiles
      .map((profile) => String(profile.filename ?? '').trim())
      .filter(Boolean)
      .map((filename) => ({ label: filename, value: filename })),
  ];
  if (draft.hdrProfile && !options.some((option) => option.value === draft.hdrProfile)) {
    options.push({
      label: t('ui.application.options.currentValue', { value: draft.hdrProfile }),
      value: draft.hdrProfile,
    });
  }
  return options;
});

const virtualDisplayScaleOptions = computed(() => [
  { label: t('ui.settings.options.virtual_scale.recommended'), value: -1 },
  { label: t('ui.settings.options.virtual_scale.preserve'), value: 0 },
  ...[100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500].map((value) => ({
    label: `${value}%`,
    value,
  })),
]);

const virtualDisplayLayoutOptions = computed(() =>
  [
    'exclusive',
    'extended',
    'extended_primary',
    'extended_isolated',
    'extended_primary_isolated',
  ].map((value) => ({ label: t(`config.virtual_display_layout_${value}`), value })),
);

const displayConfigurationOptions = computed(() => [
  { label: t('_common.disabled'), value: 'disabled' },
  { label: t('ui.settings.options.display_preparation.verify_only'), value: 'verify_only' },
  { label: t('ui.settings.options.display_preparation.ensure_active'), value: 'ensure_active' },
  { label: t('ui.settings.options.display_preparation.ensure_primary'), value: 'ensure_primary' },
  {
    label: t('ui.settings.options.display_preparation.ensure_only'),
    value: 'ensure_only_display',
  },
]);

const displayConfigurationOption = computed<string>({
  get: () => String(draft.configOverrides.dd_configuration_option ?? ''),
  set: (value) => {
    if (!value) delete draft.configOverrides.dd_configuration_option;
    else draft.configOverrides.dd_configuration_option = value;
  },
});

const commonDisplayConfigurationLabel = computed(() => {
  const value = String(props.commonSettings.dd_configuration_option ?? 'verify_only');
  return (
    displayConfigurationOptions.value.find((option) => option.value === value)?.label ??
    t('ui.application.options.currentValue', { value })
  );
});

const visibleOverrideDisplaySelection = computed<ClientDisplaySelection | undefined>(() =>
  draft.displayOverrideEnabled ? draft.displaySelection : undefined,
);

const hiddenOverrideKeys = computed(() =>
  draft.displayOverrideEnabled && draft.displaySelection === 'physical'
    ? ['dd_configuration_option']
    : [],
);

const sharedVirtualDisplay = computed(() => {
  if (!draft.displayOverrideEnabled || draft.displaySelection !== 'virtual') {
    return globalVirtualDisplayMode.value === 'shared';
  }
  return (
    draft.virtualDisplayMode === 'shared' ||
    ((draft.virtualDisplayMode === 'global' || draft.virtualDisplayMode === null) &&
      globalVirtualDisplayMode.value === 'shared')
  );
});

const peakBrightness = computed(() => {
  const value = Number(draft.configOverrides.rtx_hdr_peak_brightness ?? '');
  return Number.isFinite(value) && value >= 400 && value <= 2000 ? value : '';
});

const displayScale = computed(() => {
  const value = Number(draft.configOverrides.dd_virtual_display_scale ?? '');
  return Number.isFinite(value) && (value === -1 || value === 0 || (value >= 100 && value <= 500))
    ? value
    : '';
});

const displayModeInvalid = computed(() => {
  const value = draft.displayMode.trim();
  return Boolean(value && !/^\d{2,5}x\d{2,5}x\d{1,4}$/.test(value));
});

const displayDeviceOptions = computed(() => {
  const seen = new Set<string>();
  const options = props.displayDevices.flatMap((device) => {
    const value = String(device.device_id || device.display_name || '').trim();
    if (!value || seen.has(value)) return [];
    seen.add(value);
    const friendly = String(
      device.friendly_name || device.display_name || t('config.app_display_physical_label'),
    );
    const active =
      device.info && typeof device.info.active === 'boolean' ? device.info.active : null;
    const suffix =
      active === null
        ? ''
        : active
          ? ` (${t('config.app_display_status_active')})`
          : ` (${t('config.app_display_status_inactive')})`;
    return [{ label: `${friendly} - ${value}${suffix}`, value }];
  });
  const current = draft.physicalOutputOverride?.trim() ?? '';
  if (current && !seen.has(current)) {
    options.unshift({
      label: t('ui.application.options.currentValue', { value: current }),
      value: current,
    });
  }
  return options;
});

function setPeakBrightness(event: Event): void {
  const value = (event.target as HTMLInputElement).value;
  if (!value) delete draft.configOverrides.rtx_hdr_peak_brightness;
  else draft.configOverrides.rtx_hdr_peak_brightness = Math.min(2000, Math.max(400, Number(value)));
}

function setDisplayScale(event: Event): void {
  const value = (event.target as HTMLSelectElement).value;
  if (!value) delete draft.configOverrides.dd_virtual_display_scale;
  else draft.configOverrides.dd_virtual_display_scale = Number(value);
}

function setDisplayOverrideEnabled(enabled: boolean): void {
  draft.displayOverrideEnabled = enabled;
  if (!enabled) {
    draft.displaySelection = 'physical';
    draft.physicalOutputOverride = null;
    draft.virtualDisplayMode = null;
    draft.virtualDisplayLayout = null;
  } else {
    applyDisplaySelection(draft.displaySelection);
  }
}

function onDisplayOverrideChange(event: Event): void {
  setDisplayOverrideEnabled((event.target as HTMLInputElement).checked);
}

function applyDisplaySelection(selection: ClientDisplaySelection): void {
  draft.displaySelection = selection;
  if (selection === 'physical') {
    draft.virtualDisplayMode = 'disabled';
    draft.virtualDisplayLayout = null;
    return;
  }
  draft.physicalOutputOverride = null;
  if (draft.virtualDisplayMode === null || draft.virtualDisplayMode === 'disabled') {
    draft.virtualDisplayMode = 'global';
  }
}
</script>

<template>
  <form class="client-settings-editor" @submit.prevent="emit('save')">
    <section
      class="client-settings-section client-settings-section--identity"
      :aria-labelledby="`${controlIdPrefix}-identity`"
    >
      <div class="client-settings-section__heading">
        <div>
          <h3 :id="`${controlIdPrefix}-identity`">
            {{ t('ui.devices.editor.sections.identity') }}
          </h3>
          <p>{{ t('clients.editing_desc') }}</p>
        </div>
      </div>
      <div class="vs-settings-group">
        <SettingRow :label="t('pin.device_name')" :control-id="`${controlIdPrefix}-name`">
          <input
            :id="`${controlIdPrefix}-name`"
            v-model="draft.name"
            class="vs-input"
            maxlength="96"
            autocomplete="off"
            required
            :disabled="busy"
          />
        </SettingRow>
        <fieldset class="client-permissions">
          <legend>{{ t('ui.devices.editor.permissions_title') }}</legend>
          <p>{{ t('ui.devices.editor.permissions_description') }}</p>
          <div class="client-permissions__grid">
            <label v-for="permission in permissionOptions" :key="permission.key">
              <input
                type="checkbox"
                :checked="hasPermission(permission.mask)"
                :disabled="busy"
                @change="updatePermission(permission.mask, $event)"
              />
              <span>{{ t(`permissions.${permission.key}`) }}</span>
            </label>
          </div>
        </fieldset>
      </div>
    </section>

    <section
      class="client-settings-section client-settings-section--display"
      :aria-labelledby="`${controlIdPrefix}-display-settings`"
    >
      <div class="client-settings-section__heading">
        <div>
          <h3 :id="`${controlIdPrefix}-display-settings`">
            {{ t('ui.devices.editor.sections.display') }}
          </h3>
          <p>{{ t('ui.devices.editor.display_description') }}</p>
        </div>
        <StatusBadge :label="displayOverrideSummary" tone="neutral" compact />
      </div>
      <div class="vs-settings-group">
        <SettingRow
          :label="t('pin.display_mode_override')"
          :description="t('pin.display_mode_override_desc')"
          :control-id="`${controlIdPrefix}-display-mode`"
        >
          <input
            :id="`${controlIdPrefix}-display-mode`"
            v-model="draft.displayMode"
            class="vs-input"
            placeholder="1920x1080x60"
            :aria-invalid="displayModeInvalid ? 'true' : undefined"
            :disabled="busy"
          />
          <span v-if="displayModeInvalid" class="client-settings-error">{{
            t('ui.devices.editor.display_mode_invalid')
          }}</span>
        </SettingRow>
        <SettingRow
          class="client-setting-row--switch"
          :label="t('ui.devices.editor.display_override')"
          :control-id="`${controlIdPrefix}-display-override`"
        >
          <label class="vs-switch">
            <input
              :id="`${controlIdPrefix}-display-override`"
              type="checkbox"
              :checked="draft.displayOverrideEnabled"
              :disabled="busy"
              @change="onDisplayOverrideChange"
            />
            <span class="vs-switch__track" aria-hidden="true" />
            <span class="vs-sr-only">{{ t('ui.devices.editor.display_override') }}</span>
          </label>
        </SettingRow>
      </div>

      <div v-if="draft.displayOverrideEnabled" class="client-display-routing">
        <fieldset>
          <legend>{{ t('ui.devices.editor.route_target') }}</legend>
          <div class="client-display-routing__choices">
            <label class="client-choice-card">
              <input
                type="radio"
                value="physical"
                :checked="draft.displaySelection === 'physical'"
                :disabled="busy"
                @change="applyDisplaySelection('physical')"
              />
              <span>{{ t('config.app_display_override_physical') }}</span>
            </label>
            <label class="client-choice-card">
              <input
                type="radio"
                value="virtual"
                :checked="draft.displaySelection === 'virtual'"
                :disabled="busy"
                @change="applyDisplaySelection('virtual')"
              />
              <span>{{ t('config.app_display_override_virtual') }}</span>
            </label>
          </div>
        </fieldset>

        <SettingRow
          v-if="draft.displaySelection === 'physical'"
          :label="t('config.app_display_physical_label')"
          :description="t('ui.devices.editor.physical_display_description')"
          :control-id="`${controlIdPrefix}-physical-output`"
        >
          <div class="client-settings-control-stack">
            <select
              :id="`${controlIdPrefix}-physical-output`"
              v-model="draft.physicalOutputOverride"
              class="vs-select"
              :disabled="busy"
              @focus="emit('load-display-devices')"
            >
              <option v-if="displayDevicesLoading" disabled value="__loading">
                {{ t('_common.loading') }}
              </option>
              <option :value="null">{{ t('config.app_display_physical_placeholder') }}</option>
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
            <span v-if="displayDevicesError" class="client-settings-error">{{
              displayDevicesError
            }}</span>
            <AppButton
              variant="tertiary"
              size="compact"
              icon="refresh"
              icon-only
              :label="t('_common.refresh')"
              :busy="displayDevicesLoading"
              @click="emit('load-display-devices', true)"
            />
          </div>
        </SettingRow>

        <SettingRow
          v-if="draft.displaySelection === 'physical'"
          :label="t('config.dd_config_label')"
          :description="t('config.dd_config_hint')"
          :control-id="`${controlIdPrefix}-display-configuration`"
        >
          <select
            :id="`${controlIdPrefix}-display-configuration`"
            v-model="displayConfigurationOption"
            class="vs-select"
            :disabled="busy"
          >
            <option value="">
              {{
                t('ui.devices.editor.use_common_value', {
                  value: commonDisplayConfigurationLabel,
                })
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

        <template v-else>
          <SettingRow
            :label="t('config.virtual_display_mode_label')"
            :description="t('config.virtual_display_mode_step_hint')"
            :control-id="`${controlIdPrefix}-virtual-display-mode`"
          >
            <select
              :id="`${controlIdPrefix}-virtual-display-mode`"
              v-model="draft.virtualDisplayMode"
              class="vs-select"
              :disabled="busy"
            >
              <option :value="'global'">
                {{ t('config.app_virtual_display_mode_follow_global') }}
              </option>
              <option value="per_client">{{ t('config.virtual_display_mode_per_client') }}</option>
              <option value="shared">{{ t('config.virtual_display_mode_shared') }}</option>
            </select>
          </SettingRow>
          <SettingRow
            :label="t('config.virtual_display_layout_label')"
            :description="t('config.virtual_display_layout_hint')"
            :control-id="`${controlIdPrefix}-virtual-display-layout`"
          >
            <select
              :id="`${controlIdPrefix}-virtual-display-layout`"
              v-model="draft.virtualDisplayLayout"
              class="vs-select"
              :disabled="busy"
            >
              <option :value="null">
                {{ t('config.app_virtual_display_layout_follow_global') }} ({{
                  globalVirtualDisplayLayoutLabel
                }})
              </option>
              <option
                v-for="option in virtualDisplayLayoutOptions"
                :key="option.value"
                :value="option.value"
              >
                {{ option.label }}
              </option>
            </select>
          </SettingRow>
          <p class="client-settings-helper">
            {{
              t('ui.devices.editor.common_display_summary', {
                mode: globalVirtualDisplayModeLabel,
                layout: globalVirtualDisplayLayoutLabel,
              })
            }}
          </p>
        </template>
      </div>
    </section>

    <section
      v-if="isWindows"
      class="client-settings-section client-settings-section--hdr"
      :aria-labelledby="`${controlIdPrefix}-hdr-settings`"
    >
      <div class="client-settings-section__heading">
        <div>
          <h3 :id="`${controlIdPrefix}-hdr-settings`">{{ t('ui.devices.editor.sections.hdr') }}</h3>
          <p>{{ t('ui.devices.editor.hdr_description') }}</p>
        </div>
      </div>
      <div class="vs-settings-group">
        <SettingRow
          :label="t('clients.hdr_profile_label')"
          :description="t('ui.devices.editor.hdr_profile_description')"
          :control-id="`${controlIdPrefix}-hdr-profile`"
        >
          <div class="client-settings-control-stack">
            <select
              :id="`${controlIdPrefix}-hdr-profile`"
              v-model="draft.hdrProfile"
              class="vs-select"
              :disabled="busy"
              @focus="emit('load-hdr-profiles')"
            >
              <option v-if="hdrProfilesLoading" disabled value="__loading">
                {{ t('_common.loading') }}
              </option>
              <option v-for="option in hdrProfileOptions" :key="option.value" :value="option.value">
                {{ option.label }}
              </option>
              <option v-if="hdrProfilesLoaded && !hdrProfiles.length" disabled value="__empty">
                {{ t('ui.devices.editor.no_hdr_profiles') }}
              </option>
            </select>
            <span v-if="hdrProfilesError" class="client-settings-error">{{
              hdrProfilesError
            }}</span>
            <AppButton
              variant="tertiary"
              size="compact"
              icon="refresh"
              icon-only
              :label="t('_common.refresh')"
              :busy="hdrProfilesLoading"
              @click="emit('load-hdr-profiles', true)"
            />
          </div>
        </SettingRow>
        <SettingRow
          :label="t('clients.hdr_peak_nits_label')"
          :description="t('ui.devices.editor.hdr_peak_description')"
          :control-id="`${controlIdPrefix}-hdr-peak`"
        >
          <input
            :id="`${controlIdPrefix}-hdr-peak`"
            class="vs-input"
            type="number"
            min="400"
            max="2000"
            step="50"
            :value="peakBrightness"
            :placeholder="t('clients.hdr_peak_nits_placeholder')"
            :disabled="busy"
            @input="setPeakBrightness"
          />
        </SettingRow>
        <SettingRow
          v-if="draft.displaySelection === 'virtual'"
          :label="t('clients.virtual_display_scale_label')"
          :description="t('clients.virtual_display_scale_desc')"
          :control-id="`${controlIdPrefix}-display-scale`"
        >
          <select
            :id="`${controlIdPrefix}-display-scale`"
            class="vs-select"
            :value="displayScale"
            :disabled="busy"
            @change="setDisplayScale"
          >
            <option value="">{{ globalVirtualDisplayScaleLabel }}</option>
            <option
              v-for="option in virtualDisplayScaleOptions"
              :key="option.value"
              :value="option.value"
            >
              {{ option.label }}
            </option>
          </select>
        </SettingRow>
        <SettingRow
          class="client-setting-row--switch"
          :label="t('config.prefer_10bit_sdr')"
          :description="t('ui.devices.editor.prefer_10bit_description')"
          :control-id="`${controlIdPrefix}-prefer-10bit`"
        >
          <label class="vs-switch">
            <input
              :id="`${controlIdPrefix}-prefer-10bit`"
              v-model="draft.prefer10BitSdr"
              type="checkbox"
              :disabled="busy"
            />
            <span class="vs-switch__track" aria-hidden="true" />
            <span class="vs-sr-only">{{ t('config.prefer_10bit_sdr_label') }}</span>
          </label>
        </SettingRow>
      </div>
      <div v-if="sharedVirtualDisplay" class="client-settings-warning" role="note">
        <UiIcon name="warning" :size="16" aria-hidden="true" />
        <span>{{ t('clients.shared_display_hdr_warning') }}</span>
      </div>
    </section>

    <SettingsOverrideEditor
      v-model="draft.configOverrides"
      :base-values="commonSettings"
      :metadata="metadata"
      scope="client"
      :display-selection="visibleOverrideDisplaySelection"
      :hidden-keys="hiddenOverrideKeys"
      :control-id-prefix="`${controlIdPrefix}-override`"
    />

    <div class="client-settings-editor__footer">
      <p>{{ t('ui.devices.editor.blocking_help') }}</p>
      <div class="client-settings-editor__actions">
        <AppButton
          type="button"
          variant="tertiary"
          :label="t('_common.cancel')"
          :disabled="busy"
          @click="emit('cancel')"
        />
        <AppButton
          type="submit"
          variant="primary"
          :label="t('ui.devices.action.save')"
          :busy="busy"
          :busy-label="t('ui.devices.action.saving')"
        />
      </div>
    </div>
  </form>
</template>

<style scoped>
.client-settings-editor {
  display: grid;
  container-name: client-settings;
  container-type: inline-size;
  gap: var(--vs-space-24);
  padding: var(--vs-space-24);
  border-top: 1px solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-subtle);
}

.client-settings-section {
  display: grid;
  gap: var(--vs-space-12);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.client-settings-section__heading {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.client-settings-section__heading h3 {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
}

.client-settings-section__heading p,
.client-settings-helper,
.client-settings-editor__footer p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.client-display-routing {
  display: grid;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.client-display-routing fieldset {
  display: grid;
  gap: var(--vs-space-8);
  border: 0;
  padding: 0;
}

.client-display-routing legend {
  margin-block-end: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
}

.client-display-routing__choices {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-8);
}

.client-choice-card {
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
  min-block-size: 44px;
  padding: var(--vs-space-12);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  cursor: pointer;
}

.client-choice-card:has(input:checked) {
  border-color: var(--vs-color-accent-default);
  background: color-mix(in srgb, var(--vs-color-accent-default) 8%, transparent);
}

.client-settings-control-stack {
  display: grid;
  inline-size: min(100%, 30rem);
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-8);
  justify-items: stretch;
}

.client-settings-control-stack > .vs-select {
  grid-row: 1;
  grid-column: 1;
  inline-size: 100%;
}

.client-settings-control-stack > :deep(.vs-button) {
  grid-row: 1;
  grid-column: 2;
}

.client-settings-control-stack > .client-settings-error {
  grid-column: 1 / -1;
}

.client-settings-section :deep(.vs-setting-row__control > .vs-input),
.client-settings-section :deep(.vs-setting-row__control > .vs-select) {
  inline-size: min(100%, 30rem);
}

.client-settings-section :deep(.vs-setting-row__control) {
  min-inline-size: min(100%, 24rem);
}

.client-settings-section :deep(.vs-setting-row.client-setting-row--switch) {
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-12);
}

.client-settings-section :deep(.client-setting-row--switch .vs-setting-row__control) {
  min-inline-size: 0;
  justify-content: flex-end;
}

.client-permissions {
  display: grid;
  gap: var(--vs-space-12);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
}

.client-permissions legend {
  padding-inline: var(--vs-space-4);
  color: var(--vs-color-text-primary);
  font-weight: var(--vs-type-weight-semibold);
}

.client-permissions p {
  margin: 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.client-permissions__grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(12rem, 1fr));
  gap: var(--vs-space-8) var(--vs-space-16);
}

.client-permissions__grid label {
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
  min-block-size: 32px;
  cursor: pointer;
}

.client-settings-error {
  color: var(--vs-color-status-danger);
  font-size: var(--vs-type-size-metadata);
}

.client-settings-warning {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-8);
  padding: var(--vs-space-12);
  border: var(--vs-border-width) solid var(--vs-color-status-warning);
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.client-settings-editor__footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-16);
  padding-top: var(--vs-space-4);
}

.client-settings-editor__actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

@container client-settings (max-width: 64rem) {
  .client-settings-section
    :deep(.vs-setting-row:not(.vs-setting-row--stacked, .client-setting-row--switch)) {
    grid-template-columns: minmax(0, 1fr);
    align-items: stretch;
    gap: var(--vs-space-12);
  }

  .client-settings-section :deep(.vs-setting-row__control) {
    min-inline-size: 0;
    justify-content: flex-start;
  }

  .client-settings-section :deep(.vs-setting-row__control > .vs-input),
  .client-settings-section :deep(.vs-setting-row__control > .vs-select),
  .client-settings-control-stack {
    inline-size: min(100%, 30rem);
  }

  .client-settings-section__heading,
  .client-settings-editor__footer {
    display: grid;
  }

  .client-settings-editor__actions {
    justify-items: stretch;
    justify-content: stretch;
  }
}

@container client-settings (max-width: 20rem) {
  .client-display-routing__choices {
    grid-template-columns: minmax(0, 1fr);
  }

  .client-settings-section,
  .client-display-routing {
    padding: var(--vs-space-12);
  }

  .client-settings-editor__actions > :deep(.vs-button) {
    inline-size: 100%;
  }
}

@media (max-width: 47.999rem) {
  .client-settings-editor {
    gap: var(--vs-space-16);
    padding: var(--vs-space-12);
  }
}
</style>
