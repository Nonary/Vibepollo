<template>
  <div class="mt-4 space-y-4 rounded-md border border-dark/10 p-3 dark:border-light/10">
    <div class="flex items-center justify-between gap-3">
      <div>
        <div class="text-xs font-semibold uppercase tracking-wide opacity-70">Lossless Scaling</div>
        <p class="text-[11px] opacity-60">
          Enable Lossless Scaling for upscaling or to prepare Lossless frame generation.
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
      This application isn't managed by Playnite. Sunshine will try to guess which game executable
      is running and apply the Lossless Scaling profile automatically, but that detection is
      best-effort and may not always succeed. Configure Playnite integration for more reliable
      results.
    </n-alert>

    <div v-if="form.losslessScalingEnabled" class="space-y-4">
      <div class="grid gap-3 md:grid-cols-2">
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Profile</label>
          <n-radio-group v-model:value="form.losslessScalingProfile">
            <n-radio value="recommended">Recommended (Lowest Latency & Frame Pacing)</n-radio>
            <n-radio value="custom">Custom: Use my Lossless Scaling default profile</n-radio>
          </n-radio-group>
          <p class="text-[11px] opacity-60">
            Recommended keeps Sunshine-tuned values for consistent latency and frame pacing. Custom
            runs the profile you maintain inside Lossless Scaling.
          </p>
        </div>
        <div class="flex items-end justify-end">
          <n-button
            size="small"
            tertiary
            :disabled="!hasActiveLosslessOverrides"
            @click="resetActiveLosslessProfile"
          >
            Reset to Profile Defaults
          </n-button>
        </div>
      </div>

      <div class="space-y-3 p-3 rounded-md border border-primary/20 bg-primary/5">
        <div class="flex items-center gap-2">
          <i class="fas fa-info-circle text-primary"></i>
          <div class="text-xs font-semibold">How Lossless Scaling Works</div>
        </div>
        <p class="text-[11px] opacity-70">
          Lossless Scaling <strong>downscales</strong> the game using the resolution scale, then
          <strong>upscales</strong> back to the original resolution using the selected filter. This
          can improve performance but may reduce visual quality.
        </p>
      </div>

      <div class="grid gap-3 md:grid-cols-2">
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
            Upscaling Filter
          </label>
          <n-select
            v-model:value="losslessScalingModeModel"
            :options="LOSSLESS_SCALING_OPTIONS"
            size="small"
            :clearable="false"
          />
          <p class="text-[11px] opacity-60">
            Filter used after downscaling. "Off" disables scaling entirely.
          </p>
        </div>

        <div v-if="showLosslessResolution" class="space-y-1">
          <div class="flex items-center justify-between">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
              Resolution Scale
            </label>
            <n-radio-group
              v-model:value="resolutionInputMode"
              size="small"
              class="text-[11px]"
              button-style="solid"
            >
              <n-radio-button value="factor">Scale Factor</n-radio-button>
              <n-radio-button value="percent">Percent</n-radio-button>
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
        <strong>Performance Note:</strong> Only use upscaling if the game lacks native FSR/DLSS
        support.
      </n-alert>

      <div v-if="showLosslessSharpening" class="space-y-1">
        <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
          Sharpening (1-10)
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
          Post-upscaling sharpness for {{ losslessScalingModeModel.toUpperCase() }} filter.
        </p>
      </div>

      <div v-if="showLosslessAnimeOptions" class="grid gap-3 md:grid-cols-2">
        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
            Anime4K Size
          </label>
          <n-select
            v-model:value="losslessAnimeSizeModel"
            :options="LOSSLESS_ANIME_SIZES"
            size="small"
            :clearable="false"
          />
        </div>
        <div
          class="flex items-center justify-between gap-3 rounded-md border border-dark/10 px-3 py-2 dark:border-light/10"
        >
          <div>
            <div class="text-xs font-semibold uppercase tracking-wide opacity-70">VRS</div>
            <p class="text-[11px] opacity-60">Enable Variable Rate Shading where supported.</p>
          </div>
          <n-switch v-model:value="losslessAnimeVrsModel" size="small" />
        </div>
      </div>
    </div>

    <div class="border-t border-dark/10 dark:border-light/10 pt-4 space-y-3">
      <div class="flex items-center justify-between gap-3">
        <div>
          <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
            Frame Generation
          </div>
          <p class="text-[11px] opacity-60">
            Choose Off, NVIDIA Smooth Motion, or Lossless Scaling frame generation.
          </p>
        </div>
      </div>
      <n-select
        v-model:value="frameGenerationProviderModel"
        :options="frameGenerationOptions"
        size="small"
        :clearable="false"
      />

      <n-alert v-if="isNvidiaFrameGen" type="info" :show-icon="true" size="small" class="text-xs">
        <div class="space-y-1">
          <p>
            <strong>Requirements:</strong> NVIDIA GeForce RTX 40xx or 50xx series GPU with driver
            version 571.86 or higher.
          </p>
          <p>
            Sunshine enables NVIDIA Smooth Motion during the stream and restores your previous
            settings afterward.
          </p>
        </div>
      </n-alert>

      <div v-if="isLosslessFrameGen" class="space-y-3">
        <div class="grid gap-3 md:grid-cols-2">
          <div class="space-y-1">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
              Target Frame Rate
            </label>
            <n-input-number
              v-model:value="form.losslessScalingTargetFps"
              :min="1"
              :max="360"
              :step="1"
              :precision="0"
              placeholder="120"
              size="small"
            />
            <p class="text-[11px] opacity-60">Target FPS for frame generation.</p>
          </div>
          <div class="space-y-1">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
              RTSS Frame Limit
            </label>
            <n-input-number
              v-model:value="form.losslessScalingRtssLimit"
              :min="1"
              :max="360"
              :step="1"
              :precision="0"
              placeholder="60"
              size="small"
              @update:value="onLosslessRtssLimitChange"
            />
            <p class="text-[11px] opacity-60">
              Defaults to 50% of target when not set. Requires RTSS installed.
            </p>
          </div>
        </div>

        <div class="space-y-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
            Flow Scale (%)
          </label>
          <n-input-number
            v-model:value="losslessFlowScaleModel"
            :min="LOSSLESS_FLOW_MIN"
            :max="LOSSLESS_FLOW_MAX"
            :step="1"
            :precision="0"
            placeholder="50"
            size="small"
          />
          <p class="text-[11px] opacity-60">Frame blending strength (0–100). Recommended: 50%.</p>
        </div>
      </div>
    </div>

    <div
      v-if="form.losslessScalingEnabled"
      class="flex items-center justify-between gap-3 rounded-md border border-dark/10 px-3 py-2 dark:border-light/10"
    >
      <div>
        <div class="text-xs font-semibold uppercase tracking-wide opacity-70">Performance Mode</div>
        <p class="text-[11px] opacity-60">Reduces GPU usage with minimal quality impact.</p>
      </div>
      <n-switch v-model:value="losslessPerformanceModeModel" size="small" />
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, toRef } from 'vue';
import type { Anime4kSize, AppForm, FrameGenerationMode, LosslessScalingMode } from './types';
import {
  FRAME_GENERATION_PROVIDERS,
  LOSSLESS_ANIME_SIZES,
  LOSSLESS_FLOW_MAX,
  LOSSLESS_FLOW_MIN,
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
const frameGenerationProviderModel = defineModel<FrameGenerationMode>('frameGenerationProvider', {
  required: true,
});
const losslessPerformanceModeModel = defineModel<boolean>('losslessPerformanceMode', {
  required: true,
});
const losslessFlowScaleModel = defineModel<number | null>('losslessFlowScale', { required: true });
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
  onLosslessRtssLimitChange: (value: number | null) => void;
  resetActiveLosslessProfile: () => void;
}>();

const isPlayniteManaged = toRef(props, 'isPlayniteManaged');
const showLosslessResolution = toRef(props, 'showLosslessResolution');
const showLosslessSharpening = toRef(props, 'showLosslessSharpening');
const showLosslessAnimeOptions = toRef(props, 'showLosslessAnimeOptions');
const hasActiveLosslessOverrides = toRef(props, 'hasActiveLosslessOverrides');
const onLosslessRtssLimitChange = props.onLosslessRtssLimitChange;
const resetActiveLosslessProfile = props.resetActiveLosslessProfile;

const frameGenerationOptions = [
  { label: 'Off', value: 'off' as const },
  ...FRAME_GENERATION_PROVIDERS,
];

const isLosslessFrameGen = computed(
  () => frameGenerationProviderModel.value === 'lossless-scaling',
);
const isNvidiaFrameGen = computed(
  () => frameGenerationProviderModel.value === 'nvidia-smooth-motion',
);

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
</script>
