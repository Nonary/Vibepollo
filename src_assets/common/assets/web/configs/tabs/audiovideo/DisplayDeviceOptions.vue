<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue';
import PlatformLayout from '@/PlatformLayout.vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { NSelect, NInput, NInputNumber, NButton, NRadioGroup, NRadio, NGrid, NGi } from 'naive-ui';
import { useI18n } from 'vue-i18n';
import { http } from '@/http';

// Props
const props = defineProps<{ section?: 'pre' | 'options' }>();
const section = computed(() => props.section ?? 'pre');

// Use centralized store for config and platform
const store = useConfigStore();
const config = store.config;
watch(
  () => (config as any)?.dd_wa_dummy_plug_hdr10,
  (value) => {
    if (value && !config.rtss_disable_vsync_ullm) {
      config.rtss_disable_vsync_ullm = true;
    }
  },
  { immediate: true },
);

const dummyPlugWikiUrl =
  'https://github.com/Nonary/documentation/wiki/DummyPlugs#enabling-10-bit-color-on-dummy-plugs-at-high-resolutions';

// ----- Types -----
type RefreshRateOnly = {
  requested_fps: string;
  final_refresh_rate: string;
};
type ResolutionOnly = {
  requested_resolution: string;
  final_resolution: string;
};
type MixedRemap = RefreshRateOnly & ResolutionOnly;
type RemapType = 'refresh_rate_only' | 'resolution_only' | 'mixed';
type DdModeRemapping = {
  refresh_rate_only: RefreshRateOnly[];
  resolution_only: ResolutionOnly[];
  mixed: MixedRemap[];
};

const REFRESH_RATE_ONLY: RemapType = 'refresh_rate_only';
const RESOLUTION_ONLY: RemapType = 'resolution_only';
const MIXED: RemapType = 'mixed';

function isObject(v: unknown): v is Record<string, unknown> {
  return !!v && typeof v === 'object';
}
function isStringRecord(v: unknown, keys: string[]): v is Record<string, string> {
  if (!isObject(v)) return false;
  return keys.every((k) => typeof (v as any)[k] === 'string');
}
function isRefreshRateOnly(v: unknown): v is RefreshRateOnly {
  return isStringRecord(v, ['requested_fps', 'final_refresh_rate']);
}
function isResolutionOnly(v: unknown): v is ResolutionOnly {
  return isStringRecord(v, ['requested_resolution', 'final_resolution']);
}
function isMixed(v: unknown): v is MixedRemap {
  return isRefreshRateOnly(v) && isResolutionOnly(v);
}
function isRemapping(obj: unknown): obj is DdModeRemapping {
  if (!isObject(obj)) return false;
  const r = obj as any;
  return (
    Array.isArray(r.refresh_rate_only) && Array.isArray(r.resolution_only) && Array.isArray(r.mixed)
  );
}

function getRemapping(): DdModeRemapping | null {
  const v = (config as any)?.dd_mode_remapping;
  return isRemapping(v) ? v : null;
}

function canBeRemapped(): boolean {
  // Always show remapper UI as long as the display device configuration isn't disabled
  return !!config && (config as any).dd_configuration_option !== 'disabled';
}

function getRemappingType(): RemapType {
  // Always expose resolution override fields regardless of selected options
  // Design requirement: remapper shows both resolution and refresh rate inputs
  // whenever display device configuration is enabled. Default to MIXED.
  return MIXED;
}

function addRemappingEntry(): void {
  const type = getRemappingType();
  const remap = getRemapping();
  if (!remap) return;

  if (type === REFRESH_RATE_ONLY) {
    const entry: RefreshRateOnly = { requested_fps: '', final_refresh_rate: '' };
    remap.refresh_rate_only.push(entry);
  } else if (type === RESOLUTION_ONLY) {
    const entry: ResolutionOnly = { requested_resolution: '', final_resolution: '' };
    remap.resolution_only.push(entry);
  } else {
    const entry: MixedRemap = {
      requested_fps: '',
      final_refresh_rate: '',
      requested_resolution: '',
      final_resolution: '',
    };
    remap.mixed.push(entry);
  }

  // reassign to trigger version bump
  store.updateOption('dd_mode_remapping', JSON.parse(JSON.stringify(remap)));
  store.markManualDirty?.('dd_mode_remapping');
}

function removeRemappingEntry(idx: number): void {
  const type = getRemappingType();
  const remap = getRemapping();
  if (!remap) return;
  if (type === REFRESH_RATE_ONLY) {
    remap.refresh_rate_only.splice(idx, 1);
  } else if (type === RESOLUTION_ONLY) {
    remap.resolution_only.splice(idx, 1);
  } else {
    remap.mixed.splice(idx, 1);
  }
  store.updateOption('dd_mode_remapping', JSON.parse(JSON.stringify(remap)));
  store.markManualDirty?.('dd_mode_remapping');
}

// Safe accessor for the currently selected remapping list
const remappingArray = computed(() => {
  const type = getRemappingType();
  const dd = (config as any)?.dd_mode_remapping as any;
  const arr = dd?.[type];
  return Array.isArray(arr) ? arr : [];
});

// ----- i18n helpers -----
const { t } = useI18n();

// ----- Golden Restore (Windows) -----
const goldenBusy = ref(false);
const exportStatus = ref<null | boolean>(null);
const deleteStatus = ref<null | boolean>(null);
const goldenExists = ref<null | boolean>(null);

async function loadGoldenStatus(): Promise<void> {
  try {
    const r = await http.get('/api/display/golden_status', { validateStatus: () => true });
    goldenExists.value = r?.data?.exists === true;
  } catch {
    goldenExists.value = false;
  }
}

const createOrRecreateLabel = computed(() =>
  goldenExists.value
    ? t('troubleshooting.dd_golden_recreate')
    : t('troubleshooting.dd_golden_create'),
);

async function exportGolden(): Promise<void> {
  goldenBusy.value = true;
  exportStatus.value = null;
  try {
    const r = await http.post('/api/display/export_golden', {}, { validateStatus: () => true });
    exportStatus.value = r?.data?.status === true;
    await loadGoldenStatus();
  } catch {
    exportStatus.value = false;
  } finally {
    setTimeout(() => (goldenBusy.value = false), 600);
  }
}

async function deleteGolden(): Promise<void> {
  goldenBusy.value = true;
  deleteStatus.value = null;
  try {
    const r = await http.delete('/api/display/golden', { validateStatus: () => true });
    deleteStatus.value = r?.data?.deleted === true;
    await loadGoldenStatus();
  } catch {
    deleteStatus.value = false;
  } finally {
    setTimeout(() => (goldenBusy.value = false), 600);
  }
}

onMounted(() => {
  loadGoldenStatus();
});

// Build translated option lists as computeds so they react to locale changes
const ddConfigurationOptions = computed(() => [
  { label: t('_common.disabled_def') as string, value: 'disabled' },
  { label: t('config.dd_config_verify_only') as string, value: 'verify_only' },
  { label: t('config.dd_config_ensure_active') as string, value: 'ensure_active' },
  { label: t('config.dd_config_ensure_primary') as string, value: 'ensure_primary' },
  { label: t('config.dd_config_ensure_only_display') as string, value: 'ensure_only_display' },
]);

const ddResolutionOptions = computed(() => [
  { label: t('config.dd_resolution_option_disabled') as string, value: 'disabled' },
  { label: t('config.dd_resolution_option_auto') as string, value: 'auto' },
  { label: t('config.dd_resolution_option_manual') as string, value: 'manual' },
]);

const ddRefreshRateOptions = computed(() => [
  { label: t('config.dd_refresh_rate_option_disabled') as string, value: 'disabled' },
  { label: t('config.dd_refresh_rate_option_auto') as string, value: 'auto' },
  { label: t('config.dd_refresh_rate_option_manual') as string, value: 'manual' },
]);

const ddHdrOptions = computed(() => [
  { label: t('config.dd_hdr_option_disabled') as string, value: 'disabled' },
  { label: t('config.dd_hdr_option_auto') as string, value: 'auto' },
]);

// ----- Manual Resolution Validation -----
// Validate formats like 1920x1080 (optionally allowing spaces around x)
const manualResolutionPattern = /^(\s*\d{2,5}\s*[xX]\s*\d{2,5}\s*)$/;
const manualResolutionValid = computed(() => {
  if (!config || (config as any).dd_resolution_option !== 'manual') return true;
  const v = String((config as any).dd_manual_resolution || '');
  return manualResolutionPattern.test(v);
});

function isResolutionFieldValid(v: string | undefined | null): boolean {
  if (!v) return true; // allow empty to support refresh-rate-only mappings
  return manualResolutionPattern.test(String(v));
}

// ----- Refresh Rate Validation -----
// Allow integers or decimals, must be > 0
function isPositiveNumber(value: any): boolean {
  if (value === undefined || value === null || String(value).trim() === '') return false;
  const n = Number(value);
  return Number.isFinite(n) && n > 0;
}
function isRefreshFieldValid(v: string | undefined | null): boolean {
  if (!v) return true; // allow empty when not required
  const s = String(v).trim();
  if (s === '') return true; // empty allowed in some contexts
  return /^\d+(?:\.\d+)?$/.test(s) && isPositiveNumber(s);
}
</script>

<template>
  <PlatformLayout v-if="config">
    <template #windows>
      <div class="space-y-4">
        <!-- Step 2 content combined: configuration + snapshot (single card) -->
        <fieldset
          v-if="section === 'pre'"
          class="border border-dark/35 dark:border-light/25 rounded-xl p-4"
        >
          <legend class="px-2 text-sm font-medium">
            {{ $t('config.dd_step_2') }}: {{ $t('config.dd_pre_stream_setup') }}
          </legend>
          <!-- Configuration option -->
          <div class="text-sm font-medium mb-2">{{ $t('config.dd_config_label') }}</div>
          <n-radio-group v-model:value="config.dd_configuration_option">
            <div class="grid gap-2">
              <n-radio
                v-for="opt in ddConfigurationOptions"
                :key="opt.value"
                :value="opt.value"
                :label="opt.label"
              />
            </div>
          </n-radio-group>
          <div class="text-[11px] opacity-60 mt-1">
            {{ $t('config.dd_config_hint') }}
          </div>

          <div class="my-4 border-t border-dark/5 dark:border-light/5" />

          <!-- Snapshot for recovery -->
          <template v-if="config.dd_configuration_option !== 'disabled'">
            <div class="px-0 text-sm font-medium">Save a display snapshot (improves stability)</div>
            <p class="text-[11px] opacity-60 mt-1">
              {{ $t('troubleshooting.dd_golden_help') }}
              Saving a snapshot of your ideal monitor setup helps Sunshine recover when Windows
              fails to restore displays after streaming.
            </p>

            <div
              :class="[
                'golden-status mt-3 flex flex-wrap items-center gap-2 rounded px-3 py-2 text-[12px]',
                goldenExists === true
                  ? 'bg-success/10 text-success'
                  : goldenExists === false
                    ? 'bg-warning/10 text-warning'
                    : 'bg-light/80 dark:bg-dark/60 text-dark dark:text-light',
              ]"
            >
              <div class="flex items-center gap-2 golden-status-label">
                <i
                  :class="[
                    'text-sm',
                    goldenExists === true
                      ? 'fas fa-check-circle'
                      : goldenExists === false
                        ? 'fas fa-exclamation-triangle'
                        : 'fas fa-spinner animate-spin',
                  ]"
                />
                <span class="font-semibold">
                  {{
                    goldenExists === true
                      ? $t('troubleshooting.dd_golden_status_present')
                      : goldenExists === false
                        ? $t('troubleshooting.dd_golden_status_missing')
                        : 'Checking…'
                  }}
                </span>
              </div>

              <div class="golden-actions flex flex-wrap items-center gap-2 md:ml-auto">
                <n-button size="tiny" type="default" strong @click="loadGoldenStatus">
                  <i class="fas fa-sync" />
                  <span class="ml-1">{{ $t('troubleshooting.dd_golden_refresh') }}</span>
                </n-button>
                <div class="hidden sm:block h-4 w-px bg-current/25" />
                <n-button
                  size="tiny"
                  type="primary"
                  strong
                  :disabled="goldenBusy"
                  :loading="goldenBusy && exportStatus === null && deleteStatus === null"
                  @click="exportGolden"
                >
                  <span>{{ createOrRecreateLabel }}</span>
                </n-button>
                <n-button
                  size="tiny"
                  type="error"
                  strong
                  :disabled="goldenBusy || goldenExists !== true"
                  :loading="goldenBusy && deleteStatus === null"
                  @click="deleteGolden"
                >
                  {{ $t('troubleshooting.dd_golden_delete') }}
                </n-button>
              </div>
            </div>

            <transition name="fade">
              <p
                v-if="exportStatus === true"
                class="mt-2 alert alert-success rounded px-3 py-2 text-sm"
              >
                {{ $t('troubleshooting.dd_export_golden_success') }}
              </p>
            </transition>
            <transition name="fade">
              <p
                v-if="exportStatus === false"
                class="mt-2 alert alert-danger rounded px-3 py-2 text-sm"
              >
                {{ $t('troubleshooting.dd_export_golden_error') }}
              </p>
            </transition>
            <transition name="fade">
              <p
                v-if="deleteStatus === true"
                class="mt-2 alert alert-success rounded px-3 py-2 text-sm"
              >
                {{ $t('troubleshooting.dd_golden_deleted') }}
              </p>
            </transition>
            <transition name="fade">
              <p
                v-if="deleteStatus === false"
                class="mt-2 alert alert-danger rounded px-3 py-2 text-sm"
              >
                {{ $t('troubleshooting.dd_golden_delete_error') }}
              </p>
            </transition>
          </template>
        </fieldset>

        <!-- Optional adjustments (belongs to Step 3 in parent) -->
        <fieldset
          v-if="section === 'options' && config.dd_configuration_option !== 'disabled'"
          class="border border-dark/35 dark:border-light/25 rounded-xl p-4"
        >
          <legend class="px-2 text-sm font-medium">
            {{ $t('config.dd_step_3') }}: {{ $t('config.dd_optional_adjustments') }}
          </legend>
          <div class="space-y-6">
            <!-- Display overrides (formerly Display mode remapping) -->
            <section v-if="canBeRemapped()" class="space-y-3">
              <div class="space-y-2">
                <label
                  for="dd_mode_remapping"
                  class="block text-sm font-medium text-dark dark:text-light"
                >
                  {{ $t('config.dd_display_overrides') }}
                </label>
                <div class="text-[11px] opacity-60 space-y-1">
                  <p>{{ $t('config.dd_mode_remapping_desc_1') }}</p>
                  <p>{{ $t('config.dd_mode_remapping_desc_2') }}</p>
                  <p>{{ $t('config.dd_mode_remapping_desc_3') }}</p>
                  <p>
                    {{
                      $t(
                        getRemappingType() === MIXED
                          ? 'config.dd_mode_remapping_desc_4_final_values_mixed'
                          : 'config.dd_mode_remapping_desc_4_final_values_non_mixed',
                      )
                    }}
                  </p>
                  <p v-if="getRemappingType() === MIXED">
                    {{ $t('config.dd_mode_remapping_desc_5_sops_mixed_only') }}
                  </p>
                  <p v-if="getRemappingType() === RESOLUTION_ONLY">
                    {{ $t('config.dd_mode_remapping_desc_5_sops_resolution_only') }}
                  </p>
                </div>
              </div>

              <div v-if="remappingArray.length > 0" class="space-y-2">
                <div class="rounded-lg border border-dark/10 dark:border-light/10 overflow-hidden">
                  <div
                    class="max-h-[360px] overflow-y-auto p-2 w-full"
                    data-testid="dd-remap-scroll"
                  >
                    <div
                      v-for="(value, idx) in remappingArray"
                      :key="idx"
                      class="remap-row flex flex-wrap gap-2 lg:grid lg:grid-cols-12 lg:gap-2 lg:items-start"
                    >
                      <div
                        v-if="getRemappingType() !== REFRESH_RATE_ONLY"
                        class="remap-col lg:col-span-3"
                      >
                        <n-input
                          v-model:value="value.requested_resolution"
                          type="text"
                          class="font-mono w-full"
                          :placeholder="'1920x1080'"
                          @update:value="store.markManualDirty?.('dd_mode_remapping')"
                          v-bind="
                            isResolutionFieldValid(value.requested_resolution)
                              ? {}
                              : { status: 'error' }
                          "
                        />
                      </div>
                      <div
                        v-if="getRemappingType() !== RESOLUTION_ONLY"
                        class="remap-col lg:col-span-2"
                      >
                        <n-input
                          v-model:value="value.requested_fps"
                          type="text"
                          class="font-mono w-full"
                          :placeholder="'60'"
                          @update:value="store.markManualDirty?.('dd_mode_remapping')"
                          v-bind="
                            isRefreshFieldValid(value.requested_fps) ? {} : { status: 'error' }
                          "
                        />
                      </div>

                      <div
                        v-if="getRemappingType() !== REFRESH_RATE_ONLY"
                        class="remap-col lg:col-span-3"
                      >
                        <n-input
                          v-model:value="value.final_resolution"
                          type="text"
                          class="font-mono w-full"
                          :placeholder="'2560x1440'"
                          @update:value="store.markManualDirty?.('dd_mode_remapping')"
                          v-bind="
                            isResolutionFieldValid(value.final_resolution)
                              ? {}
                              : { status: 'error' }
                          "
                        />
                      </div>
                      <div
                        v-if="getRemappingType() !== RESOLUTION_ONLY"
                        class="remap-col lg:col-span-2"
                      >
                        <n-input
                          v-model:value="value.final_refresh_rate"
                          type="text"
                          class="font-mono w-full"
                          :placeholder="'119.95'"
                          @update:value="store.markManualDirty?.('dd_mode_remapping')"
                          v-bind="
                            isRefreshFieldValid(value.final_refresh_rate) ? {} : { status: 'error' }
                          "
                        />
                      </div>
                      <div
                        class="remap-actions flex w-full items-start justify-start lg:col-span-2 lg:w-auto lg:justify-end"
                      >
                        <n-button
                          size="small"
                          type="error"
                          strong
                          @click="removeRemappingEntry(idx)"
                        >
                          <i class="fas fa-trash" />
                        </n-button>
                      </div>

                      <!-- Second grid row for validation messages to preserve top alignment -->
                      <div
                        v-if="
                          getRemappingType() !== REFRESH_RATE_ONLY &&
                          !isResolutionFieldValid(value.requested_resolution)
                        "
                        class="remap-message w-full lg:col-span-3 text-[11px] text-red-500 mt-1"
                      >
                        Invalid. Use WIDTHxHEIGHT (e.g., 1920x1080) or leave blank.
                      </div>
                      <div
                        v-if="
                          getRemappingType() !== RESOLUTION_ONLY &&
                          !isRefreshFieldValid(value.requested_fps)
                        "
                        class="remap-message w-full lg:col-span-2 text-[11px] text-red-500 mt-1"
                      >
                        Invalid. Use a positive number or leave blank.
                      </div>
                      <div
                        v-if="
                          getRemappingType() !== REFRESH_RATE_ONLY &&
                          !isResolutionFieldValid(value.final_resolution)
                        "
                        class="remap-message w-full lg:col-span-3 text-[11px] text-red-500 mt-1"
                      >
                        Invalid. Use WIDTHxHEIGHT (e.g., 2560x1440) or leave blank.
                      </div>
                      <div
                        v-if="
                          getRemappingType() !== RESOLUTION_ONLY &&
                          !isRefreshFieldValid(value.final_refresh_rate)
                        "
                        class="remap-message w-full lg:col-span-2 text-[11px] text-red-500 mt-1"
                      >
                        Invalid. Use a positive number or leave blank.
                      </div>
                      <div
                        v-if="
                          getRemappingType() === MIXED &&
                          !value.final_resolution &&
                          !value.final_refresh_rate
                        "
                        class="remap-message w-full lg:col-span-12 text-[11px] text-red-500"
                      >
                        For mixed mappings, specify at least one Final field.
                      </div>
                    </div>
                  </div>
                </div>
              </div>
              <div class="flex justify-end pt-2">
                <n-button type="primary" strong size="small" @click="addRemappingEntry()">
                  &plus; {{ $t('config.dd_mode_remapping_add') }}
                </n-button>
              </div>
            </section>

            <n-grid :cols="12" x-gap="16" y-gap="16" class="optional-adjustments-grid">
              <!-- Resolution option -->
              <n-gi :span="12" :lg="6">
                <div class="space-y-3">
                  <div class="space-y-2">
                    <label for="dd_resolution_option" class="form-label">{{
                      $t('config.dd_resolution_option')
                    }}</label>
                    <n-select
                      id="dd_resolution_option"
                      v-model:value="config.dd_resolution_option"
                      :options="ddResolutionOptions"
                      :data-search-options="
                        ddResolutionOptions.map((o) => `${o.label}::${o.value}`).join('|')
                      "
                      class="w-full"
                    />
                    <p
                      v-if="
                        config.dd_resolution_option === 'auto' ||
                        config.dd_resolution_option === 'manual'
                      "
                      class="text-[11px] opacity-60"
                    >
                      {{ $t('config.dd_resolution_option_ogs_desc') }}
                    </p>
                  </div>

                  <div
                    v-if="config.dd_resolution_option === 'manual'"
                    class="optional-subsection space-y-2 border-l border-dark/10 dark:border-light/10 pl-3"
                  >
                    <p class="text-[11px] opacity-60">
                      {{ $t('config.dd_resolution_option_manual_desc') }}
                    </p>
                    <n-input
                      id="dd_manual_resolution"
                      v-model:value="config.dd_manual_resolution"
                      type="text"
                      class="font-mono w-full"
                      placeholder="2560x1440"
                      @update:value="store.markManualDirty?.('dd_manual_resolution')"
                      v-bind="manualResolutionValid ? {} : { status: 'error' }"
                    />
                    <p v-if="!manualResolutionValid" class="text-[11px] text-red-500">
                      Invalid format. Use WIDTHxHEIGHT, e.g., 2560x1440.
                    </p>
                  </div>
                </div>
              </n-gi>

              <!-- Refresh rate option -->
              <n-gi :span="12" :lg="6">
                <div class="space-y-3">
                  <div class="space-y-2">
                    <label for="dd_refresh_rate_option" class="form-label">{{
                      $t('config.dd_refresh_rate_option')
                    }}</label>
                    <n-select
                      id="dd_refresh_rate_option"
                      v-model:value="config.dd_refresh_rate_option"
                      :options="ddRefreshRateOptions"
                      :data-search-options="
                        ddRefreshRateOptions.map((o) => `${o.label}::${o.value}`).join('|')
                      "
                      class="w-full"
                    />
                  </div>

                  <div
                    v-if="config.dd_refresh_rate_option === 'manual'"
                    class="optional-subsection space-y-2 border-l border-dark/10 dark:border-light/10 pl-3"
                  >
                    <p class="text-[11px] opacity-60">
                      {{ $t('config.dd_refresh_rate_option_manual_desc') }}
                    </p>
                    <n-input
                      id="dd_manual_refresh_rate"
                      v-model:value="config.dd_manual_refresh_rate"
                      type="text"
                      class="font-mono w-full"
                      placeholder="59.9558"
                      @update:value="store.markManualDirty?.('dd_manual_refresh_rate')"
                      v-bind="
                        isRefreshFieldValid(config.dd_manual_refresh_rate)
                          ? {}
                          : { status: 'error' }
                      "
                    />
                    <p
                      v-if="!isRefreshFieldValid(config.dd_manual_refresh_rate)"
                      class="text-[11px] text-red-500"
                    >
                      Invalid refresh rate. Use a positive number, e.g., 60 or 59.94.
                    </p>
                  </div>
                </div>
              </n-gi>

              <!-- HDR option -->
              <n-gi :span="12" :lg="6">
                <div class="space-y-3">
                  <div class="space-y-2">
                    <label for="dd_hdr_option" class="form-label">{{
                      $t('config.dd_hdr_option')
                    }}</label>
                    <n-select
                      id="dd_hdr_option"
                      v-model:value="config.dd_hdr_option"
                      :options="ddHdrOptions"
                      :data-search-options="
                        ddHdrOptions.map((o) => `${o.label}::${o.value}`).join('|')
                      "
                      class="w-full"
                    />
                  </div>

                  <div
                    class="optional-subsection space-y-2 border-l border-dark/10 dark:border-light/10 pl-3"
                  >
                    <Checkbox
                      id="dd_wa_hdr_toggle"
                      v-model="config.dd_wa_hdr_toggle"
                      locale-prefix="config"
                      default="false"
                    />
                    <Checkbox
                      id="dd_wa_dummy_plug_hdr10"
                      v-model="(config as any).dd_wa_dummy_plug_hdr10"
                      locale-prefix="config"
                      :default="false"
                    >
                      <template #default>
                        <span class="block">
                          <a
                            :href="dummyPlugWikiUrl"
                            class="underline break-words"
                            rel="noopener"
                            target="_blank"
                          >
                            {{ $t('config.dd_wa_dummy_plug_hdr10_link') }}
                          </a>
                        </span>
                      </template>
                    </Checkbox>
                  </div>
                </div>
              </n-gi>

              <!-- Revert behavior -->
              <n-gi :span="12" :lg="6">
                <div class="space-y-3">
                  <div class="space-y-2">
                    <label for="dd_config_revert_delay" class="form-label">{{
                      $t('config.dd_config_revert_delay')
                    }}</label>
                    <n-input-number
                      id="dd_config_revert_delay"
                      v-model:value="config.dd_config_revert_delay"
                      placeholder="3000"
                      :min="0"
                      class="w-full"
                    />
                    <p class="text-[11px] opacity-60">
                      {{ $t('config.dd_config_revert_delay_desc') }}
                    </p>
                  </div>

                  <div
                    class="optional-subsection space-y-2 border-l border-dark/10 dark:border-light/10 pl-3"
                  >
                    <Checkbox
                      id="dd_config_revert_on_disconnect"
                      v-model="config.dd_config_revert_on_disconnect"
                      locale-prefix="config"
                      default="false"
                    />
                  </div>
                </div>
              </n-gi>
            </n-grid>
          </div>
        </fieldset>
      </div>
    </template>
    <template #linux></template>
    <template #macos></template>
  </PlatformLayout>
</template>

<style scoped>
.golden-status {
  width: 100%;
  flex-direction: column;
  align-items: stretch;
}

.golden-status-label {
  align-items: flex-start;
}

.golden-actions {
  width: 100%;
  justify-content: flex-start;
}

@media (min-width: 768px) {
  .golden-status {
    flex-direction: row;
    align-items: center;
  }

  .golden-status-label {
    align-items: center;
  }

  .golden-actions {
    width: auto;
    justify-content: flex-end;
  }
}

.remap-row {
  width: 100%;
}

.remap-row > * {
  min-width: 0;
}

.remap-col {
  flex: 1 1 220px;
}

.remap-actions {
  flex: 1 1 160px;
}

.remap-message {
  flex: 1 1 100%;
}

.remap-row input,
.remap-row .n-input,
.remap-row .n-input__input {
  max-width: 100%;
}

.remap-row,
.remap-row * {
  box-sizing: border-box;
}
</style>
