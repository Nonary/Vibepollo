<template>
  <div class="mt-4 space-y-4 rounded-md border border-dark/10 p-3 dark:border-light/10">
    <div class="flex items-center justify-between gap-3">
      <div>
        <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
          {{ $t('apps.framegen.lossless_title') }}
        </div>
        <p class="text-[11px] opacity-60">
          {{ $t('apps.framegen.lossless_subtitle') }}
        </p>
      </div>
      <n-switch v-model:value="form.losslessScalingEnabled" size="small" />
    </div>

    <n-alert
      v-if="form.losslessScalingEnabled && !isPlayniteManaged"
      type="warning"
      :show-icon="true"
      size="small"
      class="text-xs"
    >
      {{ $t('apps.framegen.lossless_unmanaged_warning') }}
    </n-alert>
    <n-alert
      v-if="
        form.losslessScalingEnabled &&
        losslessExecutableCheckComplete &&
        !losslessExecutableDetected
      "
      type="error"
      :show-icon="true"
      size="small"
      class="text-xs"
    >
      {{ $t('apps.framegen.lossless_not_detected') }}
    </n-alert>

    <div v-if="form.losslessScalingEnabled" class="space-y-4">
      <div class="grid gap-3 md:grid-cols-2">
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
            {{ $t('apps.framegen.profile_label') }}
          </label>
          <n-radio-group v-model:value="form.losslessScalingProfile">
            <n-radio value="recommended">{{ $t('apps.framegen.profile_recommended') }}</n-radio>
            <n-radio value="custom">{{ $t('apps.framegen.profile_custom') }}</n-radio>
          </n-radio-group>
          <p class="text-[11px] opacity-60">
            {{ $t('apps.framegen.profile_hint') }}
          </p>
        </div>
        <div class="flex items-end justify-end">
          <n-button
            size="small"
            tertiary
            :disabled="!hasActiveLosslessOverrides"
            @click="resetActiveLosslessProfile"
          >
            {{ $t('apps.framegen.reset_profile') }}
          </n-button>
        </div>
      </div>

      <div class="space-y-3 p-3 rounded-md border border-primary/20 bg-primary/5">
        <div class="flex items-center gap-2">
          <i class="fas fa-info-circle text-primary"></i>
          <div class="text-xs font-semibold">{{ $t('apps.framegen.lossless_how_it_works') }}</div>
        </div>
        <p class="text-[11px] opacity-70">
          {{ $t('apps.framegen.lossless_how_it_works_desc') }}
        </p>
      </div>

      <div class="grid gap-3 md:grid-cols-2">
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
            {{ $t('apps.framegen.upscaling_filter') }}
          </label>
          <n-select
            v-model:value="losslessScalingModeModel"
            :options="losslessScalingOptions"
            size="small"
            :clearable="false"
          />
          <p class="text-[11px] opacity-60">
            {{ $t('apps.framegen.upscaling_filter_hint') }}
          </p>
        </div>

        <div v-if="showLosslessResolution" class="space-y-1">
          <div class="flex items-center justify-between">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
              {{ $t('apps.framegen.resolution_scale') }}
            </label>
            <n-radio-group
              v-model:value="resolutionInputMode"
              size="small"
              class="text-[11px]"
              button-style="solid"
            >
              <n-radio-button value="factor">{{ $t('apps.framegen.scale_factor') }}</n-radio-button>
              <n-radio-button value="percent">{{ $t('apps.framegen.percent') }}</n-radio-button>
            </n-radio-group>
          </div>
          <div v-if="resolutionInputMode === 'factor'" class="space-y-1">
            <n-input-number
              v-model:value="resolutionFactorModel"
              :min="1"
              :max="10"
              :step="0.05"
              :precision="2"
              placeholder="1.00"
              size="small"
            />
          </div>
          <div v-else class="space-y-1">
            <n-input-number
              v-model:value="resolutionPercentModel"
              :min="LOSSLESS_RESOLUTION_MIN"
              :max="LOSSLESS_RESOLUTION_MAX"
              :step="5"
              :precision="0"
              placeholder="100"
              size="small"
            />
          </div>
          <div class="text-[11px] opacity-60">
            {{ resolutionPercentDisplay }}% • {{ resolutionFactorDisplay }}x
          </div>
        </div>
      </div>

      <n-alert
        v-if="losslessScalingModeModel !== 'off'"
        type="warning"
        :show-icon="true"
        size="small"
        class="text-xs"
      >
        <strong>{{ $t('apps.framegen.performance_note') }}</strong>
        {{ $t('apps.framegen.performance_note_desc') }}
      </n-alert>

      <div v-if="showLosslessSharpening" class="space-y-1">
        <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
          {{ $t('apps.framegen.sharpening') }}
        </label>
        <n-input-number
          v-model:value="losslessSharpeningModel"
          :min="LOSSLESS_SHARPNESS_MIN"
          :max="LOSSLESS_SHARPNESS_MAX"
          :step="1"
          :precision="0"
          size="small"
        />
        <p class="text-[11px] opacity-60">
          {{
            $t('apps.framegen.sharpening_hint', {
              filter: losslessScalingModeModel.toUpperCase(),
            })
          }}
        </p>
      </div>

      <div v-if="showLosslessAnimeOptions" class="grid gap-3 md:grid-cols-2">
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
            {{ $t('apps.framegen.anime4k_size') }}
          </label>
          <n-select
            v-model:value="losslessAnimeSizeModel"
            :options="losslessAnimeSizes"
            size="small"
            :clearable="false"
          />
        </div>
        <div
          class="flex items-center justify-between gap-3 rounded-md border border-dark/10 px-3 py-2 dark:border-light/10"
        >
          <div>
            <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
              {{ $t('apps.framegen.vrs') }}
            </div>
            <p class="text-[11px] opacity-60">{{ $t('apps.framegen.vrs_hint') }}</p>
          </div>
          <n-switch v-model:value="losslessAnimeVrsModel" size="small" />
        </div>
      </div>
    </div>

    <div
      v-if="form.losslessScalingEnabled"
      class="flex items-center justify-between gap-3 rounded-md border border-dark/10 px-3 py-2 dark:border-light/10"
    >
      <div>
        <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
          {{ $t('apps.framegen.performance_mode') }}
        </div>
        <p class="text-[11px] opacity-60">{{ $t('apps.framegen.performance_mode_hint') }}</p>
      </div>
      <n-switch v-model:value="losslessPerformanceModeModel" size="small" />
    </div>

    <div
      v-if="showLosslessLaunchSettings"
      class="space-y-3 rounded-md border border-dark/10 px-3 py-2 dark:border-light/10"
    >
      <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
        {{ $t('apps.framegen.advanced_launch') }}
      </div>
      <div class="space-y-1">
        <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
          {{ $t('apps.framegen.launch_delay_label') }}
        </label>
        <n-input-number
          v-model:value="form.losslessScalingLaunchDelay"
          :min="0"
          :max="600"
          :step="1"
          :precision="0"
          placeholder="8"
          size="small"
        />
        <p class="text-[11px] opacity-60">
          {{ $t('apps.framegen.launch_delay_hint') }}
        </p>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, toRef } from 'vue';
import { useI18n } from 'vue-i18n';
import type { Anime4kSize, AppForm, LosslessScalingMode } from './types';
import {
  LOSSLESS_ANIME_SIZES,
  LOSSLESS_RESOLUTION_MAX,
  LOSSLESS_RESOLUTION_MIN,
  LOSSLESS_SCALING_OPTIONS,
  LOSSLESS_SHARPNESS_MAX,
  LOSSLESS_SHARPNESS_MIN,
  clampResolution,
} from './lossless';
import {
  NAlert,
  NButton,
  NInputNumber,
  NRadio,
  NRadioButton,
  NRadioGroup,
  NSelect,
  NSwitch,
} from 'naive-ui';

const form = defineModel<AppForm>('form', { required: true });
const { t } = useI18n();
const localizeOptions = <T,>(options: Array<{ label?: string; labelKey?: string; value: T }>) =>
  options.map((option) => ({
    label: option.labelKey ? t(option.labelKey) : (option.label ?? String(option.value)),
    value: option.value,
  }));
const losslessScalingOptions = computed(() => localizeOptions(LOSSLESS_SCALING_OPTIONS));
const losslessAnimeSizes = computed(() => localizeOptions(LOSSLESS_ANIME_SIZES));
const losslessPerformanceModeModel = defineModel<boolean>('losslessPerformanceMode', {
  required: true,
});
const losslessResolutionScaleModel = defineModel<number | null>('losslessResolutionScale', {
  required: true,
});
const losslessScalingModeModel = defineModel<LosslessScalingMode>('losslessScalingMode', {
  required: true,
});
const losslessSharpeningModel = defineModel<number>('losslessSharpening', { required: true });
const losslessAnimeSizeModel = defineModel<Anime4kSize>('losslessAnimeSize', { required: true });
const losslessAnimeVrsModel = defineModel<boolean>('losslessAnimeVrs', { required: true });

const props = defineProps<{
  isPlayniteManaged: boolean;
  showLosslessResolution: boolean;
  showLosslessSharpening: boolean;
  showLosslessAnimeOptions: boolean;
  hasActiveLosslessOverrides: boolean;
  losslessExecutableDetected: boolean;
  losslessExecutableCheckComplete: boolean;
  resetActiveLosslessProfile: () => void;
}>();

const isPlayniteManaged = toRef(props, 'isPlayniteManaged');
const showLosslessResolution = toRef(props, 'showLosslessResolution');
const showLosslessSharpening = toRef(props, 'showLosslessSharpening');
const showLosslessAnimeOptions = toRef(props, 'showLosslessAnimeOptions');
const hasActiveLosslessOverrides = toRef(props, 'hasActiveLosslessOverrides');
const losslessExecutableDetected = toRef(props, 'losslessExecutableDetected');
const losslessExecutableCheckComplete = toRef(props, 'losslessExecutableCheckComplete');
const resetActiveLosslessProfile = props.resetActiveLosslessProfile;

const resolutionInputMode = ref<'factor' | 'percent'>('factor');

const resolutionPercentModel = computed<number>({
  get: () => {
    const raw = losslessResolutionScaleModel.value;
    if (typeof raw === 'number' && Number.isFinite(raw)) {
      return raw;
    }
    return 100;
  },
  set: (value) => {
    const clamped = clampResolution(value);
    losslessResolutionScaleModel.value = clamped ?? LOSSLESS_RESOLUTION_MAX;
  },
});

const resolutionFactorModel = computed<number>({
  get: () => {
    const percent = resolutionPercentModel.value;
    if (!percent || percent <= 0) return 1;
    return Number((100 / percent).toFixed(2));
  },
  set: (factor) => {
    const normalized = Math.min(10, Math.max(1, factor || 1));
    const currentPercent = resolutionPercentModel.value;
    const currentFactor = Number((100 / currentPercent).toFixed(2));
    const basePercent = 100 / normalized;
    const clampToRange = (value: number) =>
      Math.max(LOSSLESS_RESOLUTION_MIN, Math.min(LOSSLESS_RESOLUTION_MAX, value));
    const snapDown = (value: number) => clampToRange(Math.floor(value / 5) * 5);
    const snapUp = (value: number) => clampToRange(Math.ceil(value / 5) * 5);
    const snapNearest = (value: number) => clampToRange(Math.round(value / 5) * 5);
    const EPSILON = 1e-3;

    let nextPercent: number;

    if (normalized > currentFactor + EPSILON) {
      nextPercent = snapDown(basePercent);
    } else if (normalized < currentFactor - EPSILON) {
      nextPercent = snapUp(basePercent);
    } else {
      nextPercent = snapNearest(basePercent);
    }

    if (nextPercent === currentPercent) {
      if (normalized > currentFactor + EPSILON && currentPercent > LOSSLESS_RESOLUTION_MIN) {
        nextPercent = clampToRange(currentPercent - 5);
      } else if (normalized < currentFactor - EPSILON && currentPercent < LOSSLESS_RESOLUTION_MAX) {
        nextPercent = clampToRange(currentPercent + 5);
      }
    }

    resolutionPercentModel.value = nextPercent;
  },
});

const resolutionPercentDisplay = computed(() => resolutionPercentModel.value.toFixed(0));
const resolutionFactorDisplay = computed(() => resolutionFactorModel.value.toFixed(2));
const showLosslessLaunchSettings = computed(
  () => form.value.losslessScalingEnabled || form.value.frameGenerationMode === 'lossless-scaling',
);
</script>
