<script setup lang="ts">
import { ref, computed } from 'vue';
import PlatformLayout from '@/PlatformLayout.vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { NSelect, NInput, NInputNumber, NButton } from 'naive-ui';
import { useI18n } from 'vue-i18n';

// Use centralized store for config and platform
const store = useConfigStore();
const config = store.config;

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
  const v = config.value?.dd_mode_remapping;
  return isRemapping(v) ? v : null;
}

function canBeRemapped(): boolean {
  // Always show remapper UI as long as the display device configuration isn't disabled
  return !!config.value && config.value.dd_configuration_option !== 'disabled';
}

function getRemappingType(): RemapType {
  // If config isn't ready assume mixed (safe default)
  if (!config.value) return MIXED;
  // Assuming here that at least one setting is set to "auto" if other is not
  if (config.value.dd_resolution_option !== 'auto') {
    return REFRESH_RATE_ONLY;
  }
  if (config.value.dd_refresh_rate_option !== 'auto') {
    return RESOLUTION_ONLY;
  }
  return MIXED;
}

function addRemappingEntry(): void {
  const type = getRemappingType();
  if (!config.value) return;
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
}

function removeRemappingEntry(idx: number): void {
  const type = getRemappingType();
  if (!config.value) return;
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
}

// ----- i18n helpers -----
const { t } = useI18n();

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
</script>

<template>
  <PlatformLayout v-if="config">
    <template #windows>
      <div class="mb-6">
        <div class="rounded-md overflow-hidden border border-dark/10 dark:border-light/10">
          <div class="bg-surface/40 px-4 py-3">
            <h3 class="text-sm font-medium">
              {{ $t('config.dd_options_header') }}
            </h3>
          </div>
          <div class="p-4 space-y-4">
            <!-- Configuration option -->
            <div>
              <label for="dd_configuration_option" class="form-label">{{
                $t('config.dd_config_label')
              }}</label>
              <n-select
                id="dd_configuration_option"
                v-model:value="config.dd_configuration_option"
                :options="ddConfigurationOptions"
                :data-search-options="
                  ddConfigurationOptions.map((o) => `${o.label}::${o.value}`).join('|')
                "
              />
            </div>

            <!-- Resolution option -->
            <div v-if="config.dd_configuration_option !== 'disabled'">
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
              />
              <p
                v-if="
                  config.dd_resolution_option === 'auto' || config.dd_resolution_option === 'manual'
                "
                class="text-[11px] opacity-60 mt-1"
              >
                {{ $t('config.dd_resolution_option_ogs_desc') }}
              </p>

              <div v-if="config.dd_resolution_option === 'manual'" class="mt-2 pl-4">
                <p class="text-[11px] opacity-60">
                  {{ $t('config.dd_resolution_option_manual_desc') }}
                </p>
                <n-input
                  id="dd_manual_resolution"
                  v-model:value="config.dd_manual_resolution"
                  type="text"
                  class="monospace"
                  placeholder="2560x1440"
                />
              </div>
            </div>

            <!-- Refresh rate option -->
            <div v-if="config.dd_configuration_option !== 'disabled'">
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
              />

              <div v-if="config.dd_refresh_rate_option === 'manual'" class="mt-2 pl-4">
                <p class="text-[11px] opacity-60">
                  {{ $t('config.dd_refresh_rate_option_manual_desc') }}
                </p>
                <n-input
                  id="dd_manual_refresh_rate"
                  v-model:value="config.dd_manual_refresh_rate"
                  type="text"
                  class="monospace"
                  placeholder="59.9558"
                />
              </div>
            </div>

            <!-- HDR option -->
            <div v-if="config.dd_configuration_option !== 'disabled'">
              <label for="dd_hdr_option" class="form-label">{{ $t('config.dd_hdr_option') }}</label>
              <n-select
                id="dd_hdr_option"
                v-model:value="config.dd_hdr_option"
                :options="ddHdrOptions"
                :data-search-options="ddHdrOptions.map((o) => `${o.label}::${o.value}`).join('|')"
                class="mb-2"
              />

              <label for="dd_wa_hdr_toggle_delay" class="form-label">{{
                $t('config.dd_wa_hdr_toggle_delay')
              }}</label>
              <n-input-number
                id="dd_wa_hdr_toggle_delay"
                v-model:value="config.dd_wa_hdr_toggle_delay"
                placeholder="0"
                :min="0"
                :max="3000"
              />
              <p class="text-[11px] opacity-60 mt-1">
                {{ $t('config.dd_wa_hdr_toggle_delay_desc_1') }}<br />
                {{ $t('config.dd_wa_hdr_toggle_delay_desc_2') }}<br />
                {{ $t('config.dd_wa_hdr_toggle_delay_desc_3') }}
              </p>
            </div>

            <!-- Config revert delay -->
            <div v-if="config.dd_configuration_option !== 'disabled'">
              <label for="dd_config_revert_delay" class="form-label">{{
                $t('config.dd_config_revert_delay')
              }}</label>
              <n-input-number
                id="dd_config_revert_delay"
                v-model:value="config.dd_config_revert_delay"
                placeholder="3000"
                :min="0"
              />
              <p class="text-[11px] opacity-60 mt-1">
                {{ $t('config.dd_config_revert_delay_desc') }}
              </p>
            </div>

            <!-- Config revert on disconnect -->
            <div>
              <Checkbox
                id="dd_config_revert_on_disconnect"
                v-model="config.dd_config_revert_on_disconnect"
                locale-prefix="config"
                default="false"
              />
            </div>

            <!-- Display mode remapping -->
            <div v-if="canBeRemapped()">
              <label for="dd_mode_remapping" class="block text-sm font-medium mb-1 text-dark">
                {{ $t('config.dd_mode_remapping') }}
              </label>
              <p class="text-[11px] opacity-60">
                {{ $t('config.dd_mode_remapping_desc_1') }}<br />
                {{ $t('config.dd_mode_remapping_desc_2') }}<br />
                {{ $t('config.dd_mode_remapping_desc_3') }}<br />
                {{
                  $t(
                    getRemappingType() === MIXED
                      ? 'config.dd_mode_remapping_desc_4_final_values_mixed'
                      : 'config.dd_mode_remapping_desc_4_final_values_non_mixed',
                  )
                }}<br />
                <template v-if="getRemappingType() === MIXED">
                  {{ $t('config.dd_mode_remapping_desc_5_sops_mixed_only') }}<br />
                </template>
                <template v-if="getRemappingType() === RESOLUTION_ONLY">
                  {{ $t('config.dd_mode_remapping_desc_5_sops_resolution_only') }}<br />
                </template>
              </p>

              <div v-if="config.dd_mode_remapping[getRemappingType()].length > 0" class="space-y-2">
                <div
                  v-for="(value, idx) in config.dd_mode_remapping[getRemappingType()]"
                  :key="idx"
                  class="grid grid-cols-12 gap-2 items-center"
                >
                  <div v-if="getRemappingType() !== REFRESH_RATE_ONLY" class="col-span-3">
                    <n-input
                      v-model:value="value.requested_resolution"
                      type="text"
                      class="monospace"
                      :placeholder="'1920x1080'"
                    />
                  </div>
                  <div v-if="getRemappingType() !== RESOLUTION_ONLY" class="col-span-2">
                    <n-input
                      v-model:value="value.requested_fps"
                      type="text"
                      class="monospace"
                      :placeholder="'60'"
                    />
                  </div>

                  <div v-if="getRemappingType() !== REFRESH_RATE_ONLY" class="col-span-3">
                    <n-input
                      v-model:value="value.final_resolution"
                      type="text"
                      class="monospace"
                      :placeholder="'2560x1440'"
                    />
                  </div>
                  <div v-if="getRemappingType() !== RESOLUTION_ONLY" class="col-span-2">
                    <n-input
                      v-model:value="value.final_refresh_rate"
                      type="text"
                      class="monospace"
                      :placeholder="'119.95'"
                    />
                  </div>
                  <div class="col-span-2 text-right">
                    <n-button size="small" secondary @click="removeRemappingEntry(idx)">
                      <i class="fas fa-trash" />
                    </n-button>
                  </div>
                </div>
              </div>
              <div class="mt-2">
                <n-button primary size="small" @click="addRemappingEntry()">
                  &plus; {{ $t('config.dd_mode_remapping_add') }}
                </n-button>
              </div>
            </div>
          </div>
        </div>
      </div>
    </template>
    <template #linux></template>
    <template #macos></template>
  </PlatformLayout>
</template>
