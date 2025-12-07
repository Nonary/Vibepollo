<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { NButton, NSwitch, NAlert, NTag, NSelect, NInputNumber, NRadioGroup, NRadio } from 'naive-ui';
import type {
  FrameGenHealth,
  FrameGenRequirementStatus,
  FrameGenerationMode,
  LosslessProfileKey,
} from './types';
import { FRAME_GENERATION_PROVIDERS, LOSSLESS_FLOW_MIN, LOSSLESS_FLOW_MAX } from './lossless';

const modeModel = defineModel<FrameGenerationMode>('mode', { default: 'off' });
const gen1Model = defineModel<boolean>('gen1', { default: false });
const gen2Model = defineModel<boolean>('gen2', { default: false });
const losslessProfileModel = defineModel<LosslessProfileKey>('losslessProfile', {
  default: 'recommended',
});
const losslessTargetModel = defineModel<number | null>('losslessTargetFps', { default: null });
const losslessRtssModel = defineModel<number | null>('losslessRtssLimit', { default: null });
const losslessFlowModel = defineModel<number | null>('losslessFlowScale', { default: null });

const props = defineProps<{
  health: FrameGenHealth | null;
  healthLoading: boolean;
  healthError: string | null;
  losslessActive: boolean;
  nvidiaActive: boolean;
  usingVirtualDisplay: boolean;
  hasActiveLosslessOverrides: boolean;
  onLosslessRtssLimitChange: (value: number | null) => void;
  resetActiveLosslessProfile: () => void;
}>();

const emit = defineEmits<{
  (e: 'refresh-health'): void;
  (e: 'enable-virtual-screen'): void;
}>();

const hasHealthData = computed(() => !!props.health);
const frameGenOptions = computed(() => [
  { label: 'None', value: 'off' as const },
  ...FRAME_GENERATION_PROVIDERS,
]);
const isLosslessMode = computed(() => modeModel.value === 'lossless-scaling');
const hasFrameGenSelection = computed(() => modeModel.value !== 'off');
const losslessAdvancedTargets = ref(
  losslessTargetModel.value !== null || losslessRtssModel.value !== null,
);

watch(
  () => [losslessTargetModel.value, losslessRtssModel.value],
  ([target, rtss]) => {
    if (target !== null || rtss !== null) {
      losslessAdvancedTargets.value = true;
    }
  },
);

function handleLosslessAdvancedToggle(enabled: boolean) {
  losslessAdvancedTargets.value = enabled;
  if (!enabled) {
    losslessTargetModel.value = null;
    losslessRtssModel.value = null;
    props.onLosslessRtssLimitChange(null);
  }
}

const requirementRows = computed(() => {
  if (!props.health) return [];
  return [
    {
      id: 'capture',
      icon: 'fas fa-desktop',
      label: 'Windows Graphics Capture (recommended)',
      status: props.health.capture.status,
      message: props.health.capture.message,
    },
    {
      id: 'rtss',
      icon: 'fas fa-stopwatch-20',
      label: 'RTSS installed (recommended)',
      status: props.health.rtss.status,
      message: props.health.rtss.message,
    },
    {
      id: 'display',
      icon: 'fas fa-tv',
      label: 'Display can double your stream FPS',
      status: props.health.display.status,
      message: props.health.display.message,
    },
  ];
});

function statusClasses(status: FrameGenRequirementStatus) {
  switch (status) {
    case 'pass':
      return 'bg-emerald-500/10 text-emerald-500';
    case 'warn':
      return 'bg-amber-500/10 text-amber-500';
    case 'fail':
      return 'bg-rose-500/10 text-rose-500';
    default:
      return 'bg-slate-500/10 text-slate-400';
  }
}

function statusIcon(status: FrameGenRequirementStatus) {
  switch (status) {
    case 'pass':
      return 'fas fa-check-circle';
    case 'warn':
      return 'fas fa-exclamation-triangle';
    case 'fail':
      return 'fas fa-times-circle';
    default:
      return 'fas fa-question-circle';
  }
}

function statusLabel(status: FrameGenRequirementStatus) {
  switch (status) {
    case 'pass':
      return 'Ready';
    case 'warn':
      return 'Needs attention';
    case 'fail':
      return 'Fail';
    default:
      return 'Unknown';
  }
}

function targetIconClass(supported: boolean | null) {
  if (supported === true) return 'fas fa-check-circle text-emerald-500';
  if (supported === false) return 'fas fa-times-circle text-rose-500';
  return 'fas fa-question-circle text-amber-500';
}

function targetStatusLabel(supported: boolean | null) {
  if (supported === true) return 'Supported';
  if (supported === false) return 'Not supported';
  return 'Unknown';
}

function formatHz(hz: number | null) {
  if (hz === null || Number.isNaN(hz)) return 'Unknown refresh rate';
  if (hz >= 200) return `${Math.round(hz)} Hz`;
  return `${Math.round(hz * 10) / 10} Hz`;
}

const showSuggestion = computed(() => {
  const health = props.health;
  if (!health || !health.suggestion) return null;
  return health.suggestion;
});
const canEnableVirtualScreen = computed(() => !props.usingVirtualDisplay);

const displayTargets = computed(() => props.health?.display.targets || []);
</script>

<template>
  <section
    class="rounded-2xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-surface/40 p-4 space-y-4"
  >
    <div class="flex flex-col gap-3 md:flex-row md:items-start md:justify-between">
      <div class="space-y-1">
        <h3 class="text-base font-semibold text-dark dark:text-light">
          Frame Generation Configuration
        </h3>
        <p class="text-[12px] leading-relaxed opacity-70">
          Select how Sunshine coordinates frame generation and review the capture safeguards needed
          for smooth playback.
        </p>
        <div class="flex flex-wrap items-center gap-2">
          <n-tag v-if="losslessActive" size="small" type="primary">
            <i class="fas fa-bolt mr-1" /> Lossless Scaling frame generation active
          </n-tag>
          <n-tag v-if="nvidiaActive" size="small" type="info">
            <i class="fab fa-nvidia mr-1" /> NVIDIA Smooth Motion active
          </n-tag>
          <n-tag v-if="usingVirtualDisplay" size="small" type="success">
            <i class="fas fa-display mr-1" /> Sunshine virtual screen in use
          </n-tag>
        </div>
      </div>
      <div class="flex items-center gap-2">
        <n-button size="small" tertiary :loading="healthLoading" @click="emit('refresh-health')">
          <i class="fas fa-stethoscope" />
          <span class="ml-2">Run health check</span>
        </n-button>
      </div>
    </div>

    <div class="space-y-4">
      <div class="space-y-2">
        <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
          Frame Generation Kind
        </label>
        <n-select v-model:value="modeModel" :options="frameGenOptions" size="small" :clearable="false" />
        <p class="text-[12px] opacity-70 leading-relaxed">
          None keeps Sunshine out of the loop, Game Provided trusts in-game frame generation, Lossless
          Scaling wraps LSFG, and NVIDIA Smooth Motion configures the driver each launch.
        </p>
      </div>

      <div
        v-if="isLosslessMode"
        class="space-y-3 rounded-xl border border-primary/20 bg-primary/5 p-3"
      >
        <div class="flex flex-col gap-2 md:flex-row md:items-start md:justify-between">
          <div class="space-y-1">
            <div class="font-medium text-sm">Lossless Scaling Frame Generation</div>
            <p class="text-[12px] opacity-70 leading-relaxed">
              Use Sunshine&rsquo;s tuned profile or your Lossless Scaling defaults, then fine-tune the
              runtime targets.
            </p>
          </div>
          <n-button
            size="small"
            tertiary
            :disabled="!props.hasActiveLosslessOverrides"
            @click="props.resetActiveLosslessProfile()"
          >
            Reset to Profile Defaults
          </n-button>
        </div>

        <div class="space-y-2">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Profile</label>
          <n-radio-group v-model:value="losslessProfileModel" class="flex flex-col space-y-2">
            <n-radio value="recommended" class="w-full py-2 px-2 rounded-md hover:bg-surface/10">
              <div class="flex items-center gap-2 w-full">
                <span class="block text-sm">Recommended (Lowest Latency & Frame Pacing)</span>
              </div>
            </n-radio>
            <n-radio value="custom" class="w-full py-2 px-2 rounded-md hover:bg-surface/10">
              <div class="flex items-center gap-2 w-full">
                <span class="block text-sm">Custom: Use my Lossless Scaling default profile</span>
              </div>
            </n-radio>
          </n-radio-group>
          <p class="text-[12px] opacity-60 leading-relaxed">
            Recommended mirrors Sunshine&rsquo;s latency-focused template. Custom runs the profile you
            maintain inside Lossless Scaling.
          </p>
        </div>

        <div class="space-y-3">
          <div class="space-y-2">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
              Frame Targets
            </label>
            <p class="text-[12px] opacity-60 leading-relaxed">
              Sunshine inherits the FPS your streaming client requests and forwards it to Lossless
              Scaling automatically. When RTSS is available we cap it at half of that request for
              steadier pacing.
            </p>
            <div class="flex flex-wrap items-center gap-2">
              <n-switch
                size="small"
                :value="losslessAdvancedTargets"
                @update:value="handleLosslessAdvancedToggle"
              />
              <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                Advanced overrides
              </span>
              <span class="text-[11px] opacity-60">Manual FPS &amp; RTSS</span>
            </div>
          </div>
          <div v-if="losslessAdvancedTargets" class="grid gap-3 md:grid-cols-2">
            <div class="space-y-1">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
                Target Frame Rate Override
              </label>
              <n-input-number
                v-model:value="losslessTargetModel"
                :min="1"
                :max="360"
                :step="1"
                :precision="0"
                placeholder="120"
                size="small"
              />
              <p class="text-[12px] opacity-60 leading-relaxed">
                Only set this when you need to override the client&rsquo;s requested FPS for Lossless
                Scaling.
              </p>
            </div>
            <div class="space-y-1">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
                RTSS Frame Limit Override
              </label>
              <n-input-number
                v-model:value="losslessRtssModel"
                :min="1"
                :max="360"
                :step="1"
                :precision="0"
                placeholder="60"
                size="small"
                @update:value="props.onLosslessRtssLimitChange"
              />
              <p class="text-[12px] opacity-60 leading-relaxed">
                Sunshine defaults to half of the client request when left blank. Requires RTSS installed
                and running.
              </p>
            </div>
          </div>
          <div class="space-y-1">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
              Flow Scale (%)
            </label>
            <n-input-number
              v-model:value="losslessFlowModel"
              :min="LOSSLESS_FLOW_MIN"
              :max="LOSSLESS_FLOW_MAX"
              :step="1"
              :precision="0"
              placeholder="50"
              size="small"
            />
            <p class="text-[12px] opacity-60 leading-relaxed">
              Frame blending strength (0–100). Sunshine recommends 50% as a balanced default.
            </p>
          </div>
        </div>
      </div>

      <div class="grid gap-3">
        <div
          class="flex flex-wrap items-start justify-between gap-3 rounded-xl border border-dark/10 dark:border-light/10 bg-white/50 dark:bg-white/5 px-3 py-3"
        >
          <div class="space-y-1">
            <div class="font-medium text-sm">1st Gen Capture Fix</div>
            <p class="text-[12px] opacity-70 leading-relaxed">
              Use for DLSS 3, FSR 3, NVIDIA Smooth Motion, and Lossless Scaling frame generation. Not
              required for pure upscaling.
            </p>
          </div>
          <n-switch v-model:value="gen1Model" size="large" :disabled="!hasFrameGenSelection" />
        </div>
        <div
          class="flex flex-wrap items-start justify-between gap-3 rounded-xl border border-dark/10 dark:border-light/10 bg-white/50 dark:bg-white/5 px-3 py-3"
        >
          <div class="space-y-1">
            <div class="font-medium text-sm">2nd Gen Capture Fix</div>
            <p class="text-[12px] opacity-70 leading-relaxed">
              Only for DLSS 4 titles using 2nd generation frame generation. Forces the NVIDIA Control
              Panel frame limiter.
            </p>
          </div>
          <n-switch v-model:value="gen2Model" size="large" :disabled="!hasFrameGenSelection" />
        </div>
      </div>
      <div class="space-y-3">
        <n-alert v-if="healthError" type="error" size="small">
          {{ healthError }}
        </n-alert>
        <n-alert v-else-if="!hasHealthData && !healthLoading" size="small" type="info">
          Run the health check to verify capture method, RTSS, and display refresh requirements before
          streaming with frame generation.
        </n-alert>
        <n-alert v-else-if="healthLoading && !hasHealthData" type="info" size="small" :bordered="false">
          Checking requirements...
        </n-alert>
      </div>

      <div v-if="health" class="space-y-3">
        <div
          v-for="row in requirementRows"
          :key="row.id"
          class="rounded-xl border border-dark/10 dark:border-light/10 bg-white/40 dark:bg-white/5 p-3"
        >
          <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
            <div class="flex items-start gap-3">
              <div class="text-primary text-base">
                <i :class="row.icon" />
              </div>
              <div class="space-y-1">
                <div class="font-medium text-sm">{{ row.label }}</div>
                <p class="text-[12px] opacity-70 leading-relaxed">
                  {{ row.message }}
                </p>
              </div>
            </div>
            <div
              :class="[
                'inline-flex items-center gap-1 rounded-full px-2 py-1 text-[12px] font-semibold whitespace-nowrap',
                statusClasses(row.status),
              ]"
            >
              <i :class="statusIcon(row.status)" />
              <span>{{ statusLabel(row.status) }}</span>
            </div>
          </div>
        </div>

        <div
          class="rounded-xl border border-dark/10 dark:border-light/10 bg-white/40 dark:bg-white/5 p-3 space-y-3"
        >
          <div class="space-y-1">
            <div class="flex flex-col gap-1 sm:flex-row sm:items-center sm:justify-between">
              <div class="font-medium text-sm">Refresh rate coverage</div>
              <div class="text-[12px] opacity-70">
                Targeted display: {{ health.display.deviceLabel || 'Targeted display' }}
              </div>
            </div>
            <p class="text-[12px] opacity-70 leading-relaxed">
              {{ health.display.message }}
            </p>
          </div>

          <div class="grid gap-2 sm:grid-cols-2">
            <div
              v-for="target in displayTargets"
              :key="target.fps"
              class="rounded-lg border border-dark/10 dark:border-light/10 bg-white/50 dark:bg-white/10 px-3 py-2 space-y-1"
            >
              <div class="flex items-center gap-2 text-sm font-medium">
                <i :class="targetIconClass(target.supported)" />
                <span>{{ target.fps }} FPS stream</span>
              </div>
              <div class="text-[12px] opacity-70 leading-relaxed">
                Needs {{ target.requiredHz }} Hz - {{ targetStatusLabel(target.supported) }}
              </div>
            </div>
          </div>

          <n-alert
            v-if="health.display.error"
            type="warning"
            size="small"
            :show-icon="false"
            class="text-[12px]"
          >
            {{ health.display.error }}
          </n-alert>
        </div>
      </div>

      <n-alert
        v-if="showSuggestion"
        :type="showSuggestion.emphasis === 'warning' ? 'warning' : 'info'"
        size="small"
      >
        <div class="flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
          <span>{{ showSuggestion.message }}</span>
          <n-button
            v-if="canEnableVirtualScreen"
            size="small"
            type="primary"
            @click="emit('enable-virtual-screen')"
          >
            Use Virtual Screen
          </n-button>
        </div>
      </n-alert>

      <p class="text-[12px] opacity-70 leading-relaxed">
        Frame generation capture fixes are only needed when using frame generation technologies.
        Upscaling alone can stay disabled.
      </p>
    </div>
  </section>
</template>
