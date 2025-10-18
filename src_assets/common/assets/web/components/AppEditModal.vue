<template>
  <n-modal :show="open" :mask-closable="true" @update:show="(v) => emit('update:modelValue', v)">
    <n-card
      :bordered="false"
      :content-style="{
        display: 'flex',
        flexDirection: 'column',
        minHeight: 0,
        overflow: 'hidden',
      }"
      class="overflow-hidden"
      style="
        max-width: 56rem;
        width: 100%;
        height: min(85dvh, calc(100dvh - 2rem));
        max-height: calc(100dvh - 2rem);
      "
    >
      <template #header>
        <div class="flex items-center justify-between gap-3">
          <div class="flex items-center gap-3">
            <div
              class="h-14 w-14 rounded-full bg-gradient-to-br from-primary/20 to-primary/10 text-primary flex items-center justify-center shadow-inner"
            >
              <i class="fas fa-window-restore text-xl" />
            </div>
            <div class="flex flex-col">
              <span class="text-xl font-semibold">{{
                form.index === -1 ? 'Add Application' : 'Edit Application'
              }}</span>
            </div>
          </div>
          <div class="shrink-0">
            <span
              v-if="isPlayniteManaged"
              class="inline-flex items-center px-2 py-0.5 rounded bg-primary/15 text-primary text-[11px] font-semibold"
            >
              Playnite
            </span>
            <span
              v-else
              class="inline-flex items-center px-2 py-0.5 rounded bg-dark/10 dark:bg-light/10 text-[11px] font-semibold"
            >
              Custom
            </span>
          </div>
        </div>
      </template>

      <div
        ref="bodyRef"
        class="relative flex-1 min-h-0 overflow-auto pr-1"
        style="padding-bottom: calc(env(safe-area-inset-bottom) + 0.5rem)"
      >
        <!-- Scroll affordance shadows: appear when more content is available -->
        <div v-if="showTopShadow" class="scroll-shadow-top" aria-hidden="true"></div>
        <div v-if="showBottomShadow" class="scroll-shadow-bottom" aria-hidden="true"></div>

        <form
          class="space-y-6 text-sm"
          @submit.prevent="save"
          @keydown.ctrl.enter.stop.prevent="save"
        >
          <AppEditBasicsSection
            v-model:form="form"
            v-model:cmd-text="cmdText"
            v-model:name-select-value="nameSelectValue"
            v-model:selected-playnite-id="selectedPlayniteId"
            :is-playnite="isPlayniteManaged"
            :show-playnite-picker="showPlaynitePicker"
            :playnite-installed="playniteInstalled"
            :name-select-options="nameSelectOptions"
            :games-loading="gamesLoading"
            :fallback-option="fallbackOption"
            :playnite-options="playniteOptions"
            :lock-playnite="lockPlaynite"
            @name-focus="onNameFocus"
            @name-search="onNameSearch"
            @name-picked="onNamePicked"
            @load-playnite-games="loadPlayniteGames"
            @pick-playnite="onPickPlaynite"
            @unlock-playnite="unlockPlaynite"
            @open-cover-finder="openCoverFinder"
          />

          <div class="grid grid-cols-2 gap-3">
            <n-checkbox v-model:checked="form.excludeGlobalPrepCmd" size="small">
              Exclude Global Prep
            </n-checkbox>
            <n-checkbox v-if="!isPlayniteManaged" v-model:checked="form.autoDetach" size="small">
              Auto Detach
            </n-checkbox>
            <n-checkbox v-if="!isPlayniteManaged" v-model:checked="form.waitAll" size="small"
              >Wait All</n-checkbox
            >
            <n-checkbox
              v-if="isWindows && !isPlayniteManaged"
              v-model:checked="form.elevated"
              size="small"
            >
              Elevated
            </n-checkbox>
            <n-checkbox
              v-if="isWindows"
              v-model:checked="form.gen1FramegenFix"
              size="small"
              class="md:col-span-2"
            >
              <div class="flex flex-col">
                <span>1st Gen Frame Generation Capture Fix</span>
                <span class="text-[11px] opacity-60"
                  >Required for DLSS3, FSR3, NVIDIA Smooth Motion (frame gen), and Lossless Scaling
                  (frame gen). Not needed if only using Lossless Scaling for upscaling. Requires
                  Windows Graphics Capture (WGC), a display capable of 240 Hz or higher (virtual
                  display driver recommended), and RTSS installed. Configure Display Device to
                  activate only that monitor during streams.</span
                >
              </div>
            </n-checkbox>
            <n-checkbox
              v-if="isWindows"
              v-model:checked="form.gen2FramegenFix"
              size="small"
              class="md:col-span-2"
            >
              <div class="flex flex-col">
                <span>2nd Gen Frame Generation Capture Fix</span>
                <span class="text-[11px] opacity-60"
                  >For DLSS 4 with 2nd generation frame generation. Forces NVIDIA Control Panel
                  frame limiter. Requires Windows Graphics Capture (WGC) and a high refresh rate
                  display.</span
                >
              </div>
            </n-checkbox>
          </div>
          <p v-if="isWindows" class="text-[11px] opacity-60">
            Frame generation capture fixes are only needed when using frame generation technologies.
            Lossless Scaling upscaling alone does not require these fixes.
          </p>

          <AppEditLosslessScalingSection
            v-if="isWindows"
            v-model:form="form"
            v-model:frame-generation-provider="frameGenerationSelection"
            v-model:lossless-performance-mode="losslessPerformanceModeModel"
            v-model:lossless-flow-scale="losslessFlowScaleModel"
            v-model:lossless-resolution-scale="losslessResolutionScaleModel"
            v-model:lossless-scaling-mode="losslessScalingModeModel"
            v-model:lossless-sharpening="losslessSharpeningModel"
            v-model:lossless-anime-size="losslessAnimeSizeModel"
            v-model:lossless-anime-vrs="losslessAnimeVrsModel"
            :is-playnite-managed="isPlayniteManaged"
            :show-lossless-resolution="showLosslessResolution"
            :show-lossless-sharpening="showLosslessSharpening"
            :show-lossless-anime-options="showLosslessAnimeOptions"
            :has-active-lossless-overrides="hasActiveLosslessOverrides"
            :on-lossless-rtss-limit-change="onLosslessRtssLimitChange"
            :reset-active-lossless-profile="resetActiveLosslessProfile"
          />

          <AppEditPrepCommandsSection
            v-model:form="form"
            :is-windows="isWindows"
            @add-prep="addPrep"
          />

          <section class="sr-only">
            <!-- hidden submit to allow Enter to save within fields -->
            <button type="submit" tabindex="-1" aria-hidden="true"></button>
          </section>
        </form>
      </div>

      <template #footer>
        <div
          class="flex items-center justify-end w-full gap-2 border-t border-dark/10 dark:border-light/10 bg-light/80 dark:bg-surface/80 backdrop-blur px-2 py-2"
        >
          <n-button type="default" strong @click="close">{{ $t('_common.cancel') }}</n-button>
          <n-button
            v-if="form.index !== -1"
            type="error"
            :disabled="saving"
            @click="showDeleteConfirm = true"
          >
            <i class="fas fa-trash" /> {{ $t('apps.delete') }}
          </n-button>
          <n-button type="primary" :loading="saving" :disabled="saving" @click="save">
            <i class="fas fa-save" /> {{ $t('_common.save') }}
          </n-button>
        </div>
      </template>

      <AppEditCoverModal
        v-model:visible="showCoverModal"
        :cover-searching="coverSearching"
        :cover-busy="coverBusy"
        :cover-candidates="coverCandidates"
        @pick="useCover"
      />

      <AppEditDeleteConfirmModal
        v-model:visible="showDeleteConfirm"
        :is-playnite-auto="isPlayniteAuto"
        :name="form.name || ''"
        @cancel="showDeleteConfirm = false"
        @confirm="del"
      />
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { computed, ref, watch, onMounted, onBeforeUnmount } from 'vue';
import { useMessage } from 'naive-ui';
import { http } from '@/http';
import { NModal, NCard, NButton, NCheckbox } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import type {
  AppForm,
  ServerApp,
  PrepCmd,
  LosslessProfileKey,
  LosslessScalingMode,
  LosslessProfileOverrides,
  Anime4kSize,
  FrameGenerationProvider,
  FrameGenerationMode,
} from './app-edit/types';
import {
  FRAME_GENERATION_PROVIDERS,
  LOSSLESS_PROFILE_DEFAULTS,
  LOSSLESS_SCALING_SHARPENING,
  clampFlow,
  clampResolution,
  clampSharpness,
  defaultRtssFromTarget,
  emptyLosslessOverrides,
  emptyLosslessProfileState,
  normalizeFrameGenerationProvider,
  parseLosslessOverrides,
  parseLosslessProfileKey,
  parseNumeric,
} from './app-edit/lossless';
import AppEditBasicsSection from './app-edit/AppEditBasicsSection.vue';
import AppEditLosslessScalingSection from './app-edit/AppEditLosslessScalingSection.vue';
import AppEditPrepCommandsSection from './app-edit/AppEditPrepCommandsSection.vue';
import AppEditCoverModal, { type CoverCandidate } from './app-edit/AppEditCoverModal.vue';
import AppEditDeleteConfirmModal from './app-edit/AppEditDeleteConfirmModal.vue';

interface AppEditModalProps {
  modelValue: boolean;
  app?: ServerApp | null;
  index?: number;
}

const props = defineProps<AppEditModalProps>();
const emit = defineEmits<{
  (e: 'update:modelValue', v: boolean): void;
  (e: 'saved'): void;
  (e: 'deleted'): void;
}>();
const open = computed<boolean>(() => !!props.modelValue);
function fresh(): AppForm {
  return {
    index: -1,
    name: '',
    cmd: '',
    workingDir: '',
    imagePath: '',
    excludeGlobalPrepCmd: false,
    elevated: false,
    autoDetach: true,
    waitAll: true,
    frameGenLimiterFix: false,
    exitTimeout: 5,
    prepCmd: [],
    detached: [],
    gen1FramegenFix: false,
    gen2FramegenFix: false,
    output: '',
    frameGenerationProvider: 'lossless-scaling',
    losslessScalingEnabled: false,
    losslessScalingTargetFps: null,
    losslessScalingRtssLimit: null,
    losslessScalingRtssTouched: false,
    losslessScalingProfile: 'recommended',
    losslessScalingProfiles: emptyLosslessProfileState(),
  };
}
const form = ref<AppForm>(fresh());

watch(
  () => form.value.playniteId,
  () => {
    const et = form.value.exitTimeout as any;
    if (form.value.playniteId && (typeof et !== 'number' || et === 5)) {
      form.value.exitTimeout = 10;
    }
  },
);

function fromServerApp(src?: ServerApp | null, idx: number = -1): AppForm {
  const base = fresh();
  if (!src) return { ...base, index: idx };
  const cmdStr = Array.isArray(src.cmd) ? src.cmd.join(' ') : (src.cmd ?? '');
  const prep = Array.isArray(src['prep-cmd'])
    ? src['prep-cmd'].map((p) => ({
        do: String(p?.do ?? ''),
        undo: String(p?.undo ?? ''),
        elevated: !!p?.elevated,
      }))
    : [];
  const isPlayniteLinked = !!src['playnite-id'];
  const derivedExitTimeout =
    typeof src['exit-timeout'] === 'number'
      ? src['exit-timeout']
      : isPlayniteLinked
        ? 10
        : base.exitTimeout;
  const lsEnabled = !!src['lossless-scaling-framegen'];
  const lsTarget = parseNumeric(src['lossless-scaling-target-fps']);
  const lsLimit = parseNumeric(src['lossless-scaling-rtss-limit']);
  const profileKey = parseLosslessProfileKey(src['lossless-scaling-profile']);
  const losslessProfiles = emptyLosslessProfileState();
  losslessProfiles.recommended = parseLosslessOverrides(src['lossless-scaling-recommended']);
  losslessProfiles.custom = parseLosslessOverrides(src['lossless-scaling-custom']);
  return {
    index: idx,
    name: String(src.name ?? ''),
    output: String(src.output ?? ''),
    cmd: String(cmdStr ?? ''),
    workingDir: String(src['working-dir'] ?? ''),
    imagePath: String(src['image-path'] ?? ''),
    excludeGlobalPrepCmd: !!src['exclude-global-prep-cmd'],
    elevated: !!src.elevated,
    autoDetach: src['auto-detach'] !== undefined ? !!src['auto-detach'] : base.autoDetach,
    waitAll: src['wait-all'] !== undefined ? !!src['wait-all'] : base.waitAll,
    frameGenLimiterFix:
      src['frame-gen-limiter-fix'] !== undefined
        ? !!src['frame-gen-limiter-fix']
        : base.frameGenLimiterFix,
    exitTimeout: derivedExitTimeout,
    prepCmd: prep,
    detached: Array.isArray(src.detached) ? src.detached.map((s) => String(s)) : [],
    gen1FramegenFix: !!(src['gen1-framegen-fix'] ?? src['dlss-framegen-capture-fix']),
    gen2FramegenFix: !!src['gen2-framegen-fix'],
    playniteId: src['playnite-id'] || undefined,
    playniteManaged: src['playnite-managed'] || undefined,
    frameGenerationProvider: normalizeFrameGenerationProvider(src['frame-generation-provider']),
    losslessScalingEnabled: lsEnabled,
    losslessScalingTargetFps: lsTarget,
    losslessScalingRtssLimit: lsLimit,
    losslessScalingRtssTouched: lsLimit !== null,
    losslessScalingProfile: profileKey,
    losslessScalingProfiles: losslessProfiles,
  };
}

function toServerPayload(f: AppForm): Record<string, any> {
  const payload: Record<string, any> = {
    // Index is required by the backend to determine add (-1) vs update (>= 0)
    index: typeof f.index === 'number' ? f.index : -1,
    name: f.name,
    output: f.output,
    cmd: f.cmd,
    'working-dir': f.workingDir,
    'image-path': String(f.imagePath || '').replace(/\"/g, ''),
    'exclude-global-prep-cmd': !!f.excludeGlobalPrepCmd,
    elevated: !!f.elevated,
    'auto-detach': !!f.autoDetach,
    'wait-all': !!f.waitAll,
    'gen1-framegen-fix': !!f.gen1FramegenFix,
    'gen2-framegen-fix': !!f.gen2FramegenFix,
    'exit-timeout': Number.isFinite(f.exitTimeout) ? f.exitTimeout : 5,
    'prep-cmd': f.prepCmd.map((p) => ({
      do: p.do,
      undo: p.undo,
      ...(isWindows.value ? { elevated: !!p.elevated } : {}),
    })),
    detached: Array.isArray(f.detached) ? f.detached : [],
  };
  if (f.playniteId) payload['playnite-id'] = f.playniteId;
  if (f.playniteManaged) payload['playnite-managed'] = f.playniteManaged;
  const provider = normalizeFrameGenerationProvider(f.frameGenerationProvider);
  payload['frame-generation-provider'] = provider;
  const payloadLosslessTarget = parseNumeric(f.losslessScalingTargetFps);
  const payloadLosslessLimit = parseNumeric(f.losslessScalingRtssLimit);
  payload['lossless-scaling-framegen'] = !!f.losslessScalingEnabled;
  payload['lossless-scaling-target-fps'] = f.losslessScalingEnabled ? payloadLosslessTarget : null;
  payload['lossless-scaling-rtss-limit'] = f.losslessScalingEnabled ? payloadLosslessLimit : null;
  payload['lossless-scaling-profile'] =
    f.losslessScalingProfile === 'recommended' ? 'recommended' : 'custom';
  const buildLosslessProfilePayload = (profile: LosslessProfileOverrides) => {
    const profilePayload: Record<string, any> = {};
    if (profile.performanceMode !== null) {
      profilePayload['performance-mode'] = profile.performanceMode;
    }
    if (profile.flowScale !== null) {
      profilePayload['flow-scale'] = profile.flowScale;
    }
    if (profile.resolutionScale !== null) {
      profilePayload['resolution-scale'] = profile.resolutionScale;
    }
    if (profile.scalingMode !== null) {
      profilePayload['scaling-type'] = profile.scalingMode;
    }
    if (profile.sharpening !== null) {
      profilePayload['sharpening'] = profile.sharpening;
    }
    if (profile.anime4kSize !== null) {
      profilePayload['anime4k-size'] = profile.anime4kSize;
    }
    if (profile.anime4kVrs !== null) {
      profilePayload['anime4k-vrs'] = profile.anime4kVrs;
    }
    return profilePayload;
  };
  const recommendedPayload = buildLosslessProfilePayload(f.losslessScalingProfiles.recommended);
  const customPayload = buildLosslessProfilePayload(f.losslessScalingProfiles.custom);
  if (Object.keys(recommendedPayload).length > 0) {
    payload['lossless-scaling-recommended'] = recommendedPayload;
  }
  if (Object.keys(customPayload).length > 0) {
    payload['lossless-scaling-custom'] = customPayload;
  }
  return payload;
}
// Normalize cmd to single string; rehydrate typed form when props.app changes while open
watch(
  () => props.app,
  (val) => {
    if (!open.value) return;
    form.value = fromServerApp(val as ServerApp | undefined, props.index ?? -1);
  },
  { immediate: true },
);
const cmdText = computed<string>({
  get: () => form.value.cmd || '',
  set: (v: string) => {
    form.value.cmd = v;
  },
});
const isPlayniteManaged = computed<boolean>(() => !!form.value.playniteId);
const isPlayniteAuto = computed<boolean>(
  () => isPlayniteManaged.value && form.value.playniteManaged !== 'manual',
);

const frameGenerationSelection = computed<FrameGenerationMode>({
  get: () => {
    if (form.value.frameGenerationProvider === 'nvidia-smooth-motion') {
      return 'nvidia-smooth-motion';
    }
    const hasLosslessFrameGen =
      form.value.losslessScalingTargetFps !== null || form.value.losslessScalingRtssLimit !== null;
    return hasLosslessFrameGen ? 'lossless-scaling' : 'off';
  },
  set: (mode) => {
    if (mode === 'nvidia-smooth-motion') {
      form.value.frameGenerationProvider = 'nvidia-smooth-motion';
    } else if (mode === 'lossless-scaling') {
      form.value.frameGenerationProvider = 'lossless-scaling';
      if (!form.value.losslessScalingEnabled) {
        form.value.losslessScalingEnabled = true;
      }
      if (!form.value.losslessScalingTargetFps) {
        form.value.losslessScalingTargetFps = 120;
      }
      if (!form.value.losslessScalingRtssLimit && !form.value.losslessScalingRtssTouched) {
        form.value.losslessScalingRtssLimit = defaultRtssFromTarget(
          parseNumeric(form.value.losslessScalingTargetFps),
        );
      }
    } else {
      form.value.losslessScalingTargetFps = null;
      form.value.losslessScalingRtssLimit = null;
      form.value.losslessScalingRtssTouched = false;
      if (form.value.frameGenerationProvider !== 'lossless-scaling') {
        form.value.frameGenerationProvider = 'lossless-scaling';
      }
    }
  },
});

const nvidiaFrameGenEnabled = computed<boolean>({
  get: () => frameGenerationSelection.value === 'nvidia-smooth-motion',
  set: (enabled: boolean) => {
    if (enabled) {
      frameGenerationSelection.value = 'nvidia-smooth-motion';
    } else if (frameGenerationSelection.value === 'nvidia-smooth-motion') {
      frameGenerationSelection.value = 'off';
    }
  },
});

const losslessFrameGenEnabled = computed<boolean>({
  get: () => frameGenerationSelection.value === 'lossless-scaling',
  set: (enabled: boolean) => {
    if (enabled) {
      frameGenerationSelection.value = 'lossless-scaling';
    } else if (frameGenerationSelection.value === 'lossless-scaling') {
      frameGenerationSelection.value = 'off';
    }
  },
});
watch(
  () => form.value.frameGenerationProvider,
  (provider) => {
    const normalized = normalizeFrameGenerationProvider(provider);
    if (provider !== normalized) {
      form.value.frameGenerationProvider = normalized;
      return;
    }
    // Update FPS/RTSS if using lossless and frame gen is enabled
    if (
      normalized === 'lossless-scaling' &&
      losslessFrameGenEnabled.value &&
      !form.value.losslessScalingRtssTouched
    ) {
      form.value.losslessScalingRtssLimit = defaultRtssFromTarget(
        parseNumeric(form.value.losslessScalingTargetFps),
      );
    }
  },
);

watch(
  () => form.value.losslessScalingTargetFps,
  (value) => {
    const normalized = parseNumeric(value);
    if (normalized !== value) {
      form.value.losslessScalingTargetFps = normalized;
      return;
    }
    // Only auto-update RTSS if frame gen is enabled and user hasn't manually set it
    if (losslessFrameGenEnabled.value && !form.value.losslessScalingRtssTouched) {
      form.value.losslessScalingRtssLimit = defaultRtssFromTarget(normalized);
    }
  },
);

function onLosslessRtssLimitChange(value: number | null) {
  form.value.losslessScalingRtssTouched = true;
  const normalized = parseNumeric(value);
  form.value.losslessScalingRtssLimit =
    normalized === null ? null : Math.min(360, Math.max(1, Math.round(normalized)));
}

const activeLosslessProfile = computed<LosslessProfileKey>(() =>
  form.value.losslessScalingProfile === 'recommended' ? 'recommended' : 'custom',
);

function getEffectivePerformanceMode(profile: LosslessProfileKey): boolean {
  const overrides = form.value.losslessScalingProfiles[profile];
  return overrides.performanceMode ?? LOSSLESS_PROFILE_DEFAULTS[profile].performanceMode;
}

function setPerformanceMode(profile: LosslessProfileKey, value: boolean): void {
  const defaults = LOSSLESS_PROFILE_DEFAULTS[profile];
  form.value.losslessScalingProfiles[profile].performanceMode =
    value === defaults.performanceMode ? null : value;
}

function getEffectiveFlowScale(profile: LosslessProfileKey): number {
  const overrides = form.value.losslessScalingProfiles[profile];
  return overrides.flowScale ?? LOSSLESS_PROFILE_DEFAULTS[profile].flowScale;
}

function setFlowScale(profile: LosslessProfileKey, value: number | null): void {
  const defaults = LOSSLESS_PROFILE_DEFAULTS[profile];
  const clamped = clampFlow(value);
  form.value.losslessScalingProfiles[profile].flowScale =
    clamped === null || clamped === defaults.flowScale ? null : clamped;
}

function getEffectiveResolutionScale(profile: LosslessProfileKey): number {
  const overrides = form.value.losslessScalingProfiles[profile];
  return overrides.resolutionScale ?? LOSSLESS_PROFILE_DEFAULTS[profile].resolutionScale;
}

function setResolutionScale(profile: LosslessProfileKey, value: number | null): void {
  const defaults = LOSSLESS_PROFILE_DEFAULTS[profile];
  const clamped = clampResolution(value);
  form.value.losslessScalingProfiles[profile].resolutionScale =
    clamped === null || clamped === defaults.resolutionScale ? null : clamped;
}

function getEffectiveScalingMode(profile: LosslessProfileKey): LosslessScalingMode {
  const overrides = form.value.losslessScalingProfiles[profile];
  return overrides.scalingMode ?? LOSSLESS_PROFILE_DEFAULTS[profile].scalingMode;
}

function setScalingMode(profile: LosslessProfileKey, value: LosslessScalingMode): void {
  const defaults = LOSSLESS_PROFILE_DEFAULTS[profile];
  const overrides = form.value.losslessScalingProfiles[profile];
  overrides.scalingMode = value === defaults.scalingMode ? null : value;
  if (!LOSSLESS_SCALING_SHARPENING.has(value)) {
    overrides.sharpening = null;
  }
  if (value !== 'anime4k') {
    overrides.anime4kSize = null;
    overrides.anime4kVrs = null;
  }
  // When scaling is set to 'off', reset resolution scaling to default (100%)
  if (value === 'off') {
    overrides.resolutionScale = null;
  }
}

function getEffectiveSharpening(profile: LosslessProfileKey): number {
  const overrides = form.value.losslessScalingProfiles[profile];
  const defaults = LOSSLESS_PROFILE_DEFAULTS[profile];
  return overrides.sharpening ?? defaults.sharpening;
}

function setSharpening(profile: LosslessProfileKey, value: number | null): void {
  const defaults = LOSSLESS_PROFILE_DEFAULTS[profile];
  const clamped = clampSharpness(value);
  form.value.losslessScalingProfiles[profile].sharpening =
    clamped === null || clamped === defaults.sharpening ? null : clamped;
}

function getEffectiveAnimeSize(profile: LosslessProfileKey): Anime4kSize {
  const overrides = form.value.losslessScalingProfiles[profile];
  return overrides.anime4kSize ?? LOSSLESS_PROFILE_DEFAULTS[profile].anime4kSize;
}

function setAnimeSize(profile: LosslessProfileKey, value: Anime4kSize | null): void {
  const defaults = LOSSLESS_PROFILE_DEFAULTS[profile];
  const resolved = value ?? defaults.anime4kSize;
  form.value.losslessScalingProfiles[profile].anime4kSize =
    resolved === defaults.anime4kSize ? null : resolved;
}

function getEffectiveAnimeVrs(profile: LosslessProfileKey): boolean {
  const overrides = form.value.losslessScalingProfiles[profile];
  return overrides.anime4kVrs ?? LOSSLESS_PROFILE_DEFAULTS[profile].anime4kVrs;
}

function setAnimeVrs(profile: LosslessProfileKey, value: boolean): void {
  const defaults = LOSSLESS_PROFILE_DEFAULTS[profile];
  form.value.losslessScalingProfiles[profile].anime4kVrs =
    value === defaults.anime4kVrs ? null : value;
}

const losslessPerformanceModeModel = computed<boolean>({
  get: () => getEffectivePerformanceMode(activeLosslessProfile.value),
  set: (value: boolean) => {
    setPerformanceMode(activeLosslessProfile.value, !!value);
  },
});

const losslessFlowScaleModel = computed<number | null>({
  get: () => getEffectiveFlowScale(activeLosslessProfile.value),
  set: (value) => {
    setFlowScale(activeLosslessProfile.value, value ?? null);
  },
});

const losslessResolutionScaleModel = computed<number | null>({
  get: () => getEffectiveResolutionScale(activeLosslessProfile.value),
  set: (value) => {
    setResolutionScale(activeLosslessProfile.value, value ?? null);
  },
});

const losslessScalingModeModel = computed<LosslessScalingMode>({
  get: () => getEffectiveScalingMode(activeLosslessProfile.value),
  set: (value: LosslessScalingMode) => {
    setScalingMode(activeLosslessProfile.value, value);
  },
});

const losslessSharpeningModel = computed<number>({
  get: () => getEffectiveSharpening(activeLosslessProfile.value),
  set: (value: number | null) => {
    setSharpening(activeLosslessProfile.value, value ?? null);
  },
});

const losslessAnimeSizeModel = computed<Anime4kSize>({
  get: () => getEffectiveAnimeSize(activeLosslessProfile.value),
  set: (value: Anime4kSize | null) => {
    setAnimeSize(activeLosslessProfile.value, value);
  },
});

const losslessAnimeVrsModel = computed<boolean>({
  get: () => getEffectiveAnimeVrs(activeLosslessProfile.value),
  set: (value: boolean) => {
    setAnimeVrs(activeLosslessProfile.value, !!value);
  },
});

const showLosslessSharpening = computed(() =>
  LOSSLESS_SCALING_SHARPENING.has(losslessScalingModeModel.value),
);
const showLosslessResolution = computed(() => {
  const mode = losslessScalingModeModel.value;
  return mode !== null && mode !== 'off';
});
const showLosslessAnimeOptions = computed(() => losslessScalingModeModel.value === 'anime4k');

// Watch for scaling mode changes to manage lossless enabled state
watch(
  () => losslessScalingModeModel.value,
  (mode) => {
    // If user sets scaling to something other than 'off', ensure lossless is enabled
    if (mode !== 'off' && !form.value.losslessScalingEnabled) {
      form.value.losslessScalingEnabled = true;
    }
    // If user sets scaling to 'off' and frame gen is also off, disable lossless entirely
    if (mode === 'off' && !losslessFrameGenEnabled.value) {
      form.value.losslessScalingEnabled = false;
    }
  },
);

const hasActiveLosslessOverrides = computed<boolean>(() => {
  const overrides = form.value.losslessScalingProfiles[activeLosslessProfile.value];
  return (
    overrides.performanceMode !== null ||
    overrides.flowScale !== null ||
    overrides.resolutionScale !== null ||
    overrides.scalingMode !== null ||
    overrides.sharpening !== null ||
    overrides.anime4kSize !== null ||
    overrides.anime4kVrs !== null
  );
});

function resetActiveLosslessProfile(): void {
  const overrides = form.value.losslessScalingProfiles[activeLosslessProfile.value];
  overrides.performanceMode = null;
  overrides.flowScale = null;
  overrides.resolutionScale = null;
  overrides.scalingMode = null;
  overrides.sharpening = null;
  overrides.anime4kSize = null;
  overrides.anime4kVrs = null;
}
// Unified name combobox state (supports Playnite suggestions + free-form)
const nameSelectValue = ref<string>('');
const nameOptions = ref<{ label: string; value: string }[]>([]);
const fallbackOption = (value: unknown) => {
  const v = String(value ?? '');
  const label = String(form.value.name || '').trim() || v;
  return { label, value: v };
};
const nameSearchQuery = ref('');
const nameSelectOptions = computed(() => {
  // Prefer dynamically built options (from search)
  if (nameOptions.value.length) return nameOptions.value;
  const list: { label: string; value: string }[] = [];
  const cur = String(form.value.name || '').trim();
  if (cur) list.push({ label: `Custom: "${cur}"`, value: `__custom__:${cur}` });
  if (playniteOptions.value.length) {
    list.push(...playniteOptions.value.slice(0, 20));
  }
  return list;
});

// Populate suggestions immediately on focus so dropdown isn't empty
async function onNameFocus() {
  // Show a friendly placeholder immediately to avoid "No Data"
  if (!playniteOptions.value.length) {
    nameOptions.value = [
      { label: 'Loading Playnite games…', value: '__loading__', disabled: true } as any,
    ];
  }
  // Kick off loading (don’t block the UI), then refresh list
  loadPlayniteGames()
    .catch(() => {})
    .finally(() => {
      onNameSearch(nameSearchQuery.value);
    });
}

function ensureNameSelectionFromForm() {
  const currentName = String(form.value.name || '').trim();
  const opts: { label: string; value: string }[] = [];
  if (currentName) {
    opts.push({ label: `Custom: "${currentName}"`, value: `__custom__:${currentName}` });
  }
  const pid = form.value.playniteId;
  if (pid) {
    const found = playniteOptions.value.find((o) => o.value === String(pid));
    if (found) opts.push(found);
    else if (currentName) opts.push({ label: currentName, value: String(pid) });
  }
  nameOptions.value = opts;
  nameSelectValue.value = pid ? String(pid) : currentName ? `__custom__:${currentName}` : '';
}
watch(open, (o) => {
  if (o) {
    form.value = fromServerApp(props.app ?? undefined, props.index ?? -1);
    // reset playnite picker state when opening
    selectedPlayniteId.value = '';
    lockPlaynite.value = false;
    newAppSource.value = 'custom';
    // refresh Playnite status early so the picker can enable itself
    refreshPlayniteStatus().then(() => {
      if (playniteInstalled.value) void loadPlayniteGames();
    });
    // Update scroll shadows after content paints
    requestAnimationFrame(() => updateShadows());
    // Initialize unified name combobox selection
    ensureNameSelectionFromForm();
  }
});
function close() {
  emit('update:modelValue', false);
}
function addPrep() {
  form.value.prepCmd.push({
    do: '',
    undo: '',
    ...(isWindows.value ? { elevated: false } : {}),
  });
  requestAnimationFrame(() => updateShadows());
}
const saving = ref(false);
const showDeleteConfirm = ref(false);

// Cover finder state (disabled for Playnite-managed apps)
const showCoverModal = ref(false);
const coverSearching = ref(false);
const coverBusy = ref(false);
const coverCandidates = ref<CoverCandidate[]>([]);

function getSearchBucket(name: string) {
  const prefix = (name || '')
    .substring(0, Math.min((name || '').length, 2))
    .toLowerCase()
    .replace(/[^a-z\d]/g, '');
  return prefix || '@';
}

async function searchCovers(name: string): Promise<CoverCandidate[]> {
  if (!name) return [];
  const searchName = name.replace(/\s+/g, '.').toLowerCase();
  // Use raw.githubusercontent.com to avoid CORS issues
  const dbUrl = 'https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages';
  const bucket = getSearchBucket(name);
  const res = await fetch(`${dbUrl}/buckets/${bucket}.json`);
  if (!res.ok) return [];
  const maps = await res.json();
  const ids = Object.keys(maps || {});
  const promises = ids.map(async (id) => {
    const item = maps[id];
    if (!item?.name) return null;
    if (String(item.name).replace(/\s+/g, '.').toLowerCase().startsWith(searchName)) {
      try {
        const r = await fetch(`${dbUrl}/games/${id}.json`);
        return await r.json();
      } catch {
        return null;
      }
    }
    return null;
  });
  const results = (await Promise.all(promises)).filter(Boolean);
  return results
    .filter((item) => item && item.cover && item.cover.url)
    .map((game) => {
      const thumb: string = game.cover.url;
      const dotIndex = thumb.lastIndexOf('.');
      const slashIndex = thumb.lastIndexOf('/');
      if (dotIndex < 0 || slashIndex < 0) return null as any;
      const slug = thumb.substring(slashIndex + 1, dotIndex);
      return {
        name: game.name,
        key: `igdb_${game.id}`,
        url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${slug}.jpg`,
        saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${slug}.png`,
      } as CoverCandidate;
    })
    .filter(Boolean);
}

async function openCoverFinder() {
  if (isPlayniteManaged.value) return;
  coverCandidates.value = [];
  showCoverModal.value = true;
  coverSearching.value = true;
  try {
    coverCandidates.value = await searchCovers(String(form.value.name || ''));
  } finally {
    coverSearching.value = false;
  }
}

async function useCover(cover: CoverCandidate) {
  if (!cover || coverBusy.value) return;
  coverBusy.value = true;
  try {
    const r = await http.post(
      './api/covers/upload',
      { key: cover.key, url: cover.saveUrl },
      { headers: { 'Content-Type': 'application/json' }, validateStatus: () => true },
    );
    if (r.status >= 200 && r.status < 300 && r.data && r.data.path) {
      form.value.imagePath = String(r.data.path || '');
      showCoverModal.value = false;
    }
  } finally {
    coverBusy.value = false;
  }
}

// Platform + Playnite detection
const configStore = useConfigStore();
const isWindows = computed(
  () => (configStore.metadata?.platform || '').toLowerCase() === 'windows',
);
const ddConfigOption = computed(
  () => (configStore.config as any)?.dd_configuration_option ?? 'disabled',
);
const captureMethod = computed(() => (configStore.config as any)?.capture ?? '');
const playniteInstalled = ref(false);
const isNew = computed(() => form.value.index === -1);
// New app source: 'custom' or 'playnite' (Windows only)
const newAppSource = ref<'custom' | 'playnite'>('custom');
const showPlaynitePicker = computed(
  () => isNew.value && isWindows.value && newAppSource.value === 'playnite',
);

// Playnite picker state
const gamesLoading = ref(false);
const playniteOptions = ref<{ label: string; value: string }[]>([]);
const selectedPlayniteId = ref('');
const lockPlaynite = ref(false);

async function loadPlayniteGames() {
  if (!isWindows.value || gamesLoading.value || playniteOptions.value.length) return;
  // Ensure we have up-to-date install status
  await refreshPlayniteStatus();
  if (!playniteInstalled.value) return;
  gamesLoading.value = true;
  try {
    const r = await http.get('/api/playnite/games');
    const games: any[] = Array.isArray(r.data) ? r.data : [];
    playniteOptions.value = games
      .filter((g) => !!g.installed)
      .map((g) => ({ label: g.name || g.id, value: g.id }))
      .sort((a, b) => a.label.localeCompare(b.label));
  } catch (_) {}
  gamesLoading.value = false;
  // Refresh suggestions (replace placeholder with actual items)
  try {
    onNameSearch(nameSearchQuery.value);
  } catch {}
}

async function refreshPlayniteStatus() {
  try {
    const r = await http.get('/api/playnite/status', { validateStatus: () => true });
    if (r.status === 200 && r.data && typeof r.data === 'object' && r.data !== null) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      playniteInstalled.value = !!(r.data as any).installed;
    }
  } catch (_) {}
}

function onPickPlaynite(id: string) {
  const opt = playniteOptions.value.find((o) => o.value === id);
  if (!opt) return;
  // Lock in selection and set fields
  form.value.name = opt.label;
  form.value.playniteId = id;
  form.value.playniteManaged = 'manual';
  // clear command by default for Playnite managed entries
  if (!form.value.cmd) form.value.cmd = '';
  lockPlaynite.value = true;
  // Reflect selection in unified combobox
  ensureNameSelectionFromForm();
}
function unlockPlaynite() {
  lockPlaynite.value = false;
}
// When switching to custom source, clear Playnite-specific markers
watch(newAppSource, (v) => {
  if (v === 'custom') {
    form.value.playniteId = undefined;
    form.value.playniteManaged = undefined;
    lockPlaynite.value = false;
    selectedPlayniteId.value = '';
  }
});
// Track if Gen1 is being auto-enabled by Lossless Scaling to prevent alert spam
let autoEnablingGen1 = false;

watch(
  () => form.value.losslessScalingEnabled,
  (enabled) => {
    if (!enabled && losslessFrameGenEnabled.value) {
      frameGenerationSelection.value = 'off';
    }
  },
);

watch(
  () => form.value.gen1FramegenFix,
  async (enabled) => {
    if (!enabled) {
      return;
    }
    // Disable Gen2 when Gen1 is enabled (mutually exclusive)
    if (form.value.gen2FramegenFix) {
      form.value.gen2FramegenFix = false;
    }
    // Skip alerts if this was triggered by lossless scaling auto-enable
    if (autoEnablingGen1) {
      return;
    }
    message?.info(
      '1st Gen Frame Generation Capture Fix requires Windows Graphics Capture (WGC), a display capable of 240 Hz or higher, and RTSS installed. A virtual display driver (such as VDD by MikeTheTech, 244 Hz by default) is recommended.',
      { duration: 8000 },
    );
    if (!ddConfigOption.value || ddConfigOption.value === 'disabled') {
      message?.warning(
        'Enable Display Device configuration and set it to "Deactivate all other displays" so the Frame Generation capture fix can take effect.',
        { duration: 8000 },
      );
    } else if (ddConfigOption.value !== 'ensure_only_display') {
      message?.warning(
        'Set Display Device to "Deactivate all other displays" so only the high-refresh monitor stays active during the stream.',
        { duration: 8000 },
      );
    }
    try {
      const rtss = await http.get('/api/rtss/status', { validateStatus: () => true });
      const data = rtss?.data as any;
      if (!data || !data.path_exists || !data.hooks_found) {
        message?.warning(
          'RTSS is required for this fix. Install RTSS to ensure the stream remains perfectly smooth and avoid microstuttering.',
          { duration: 8000 },
        );
      }
    } catch {
      message?.warning(
        'Unable to verify RTSS installation. Install RTSS to avoid microstuttering.',
        { duration: 8000 },
      );
    }
  },
);

watch(
  () => form.value.gen2FramegenFix,
  async (enabled) => {
    if (!enabled) {
      return;
    }
    // Disable Gen1 when Gen2 is enabled (mutually exclusive)
    if (form.value.gen1FramegenFix) {
      form.value.gen1FramegenFix = false;
    }
    message?.info(
      '2nd Gen Frame Generation Capture Fix (for DLSS 4) forces NVIDIA Control Panel frame limiter. Requires Windows Graphics Capture (WGC) and an NVIDIA GPU.',
      { duration: 8000 },
    );
    if (!ddConfigOption.value || ddConfigOption.value === 'disabled') {
      message?.warning(
        'Enable Display Device configuration and set it to "Deactivate all other displays" for best results.',
        { duration: 8000 },
      );
    } else if (ddConfigOption.value !== 'ensure_only_display') {
      message?.warning(
        'Set Display Device to "Deactivate all other displays" so only the high-refresh monitor stays active during the stream.',
        { duration: 8000 },
      );
    }
  },
);

// Automatically enable Gen1 Frame Generation fix when Frame Generation is enabled
watch(
  () => [nvidiaFrameGenEnabled.value, losslessFrameGenEnabled.value] as const,
  ([nvidia, lossless], [prevNvidia, prevLossless]) => {
    const anyFrameGenEnabled = nvidia || lossless;
    const wasFrameGenEnabled = prevNvidia || prevLossless;
    if (anyFrameGenEnabled && !form.value.gen1FramegenFix) {
      autoEnablingGen1 = true;
      form.value.gen1FramegenFix = true;
      if (nvidia) {
        message?.info(
          '1st Gen Frame Generation Capture Fix has been automatically enabled for optimal NVIDIA Smooth Motion performance.',
          { duration: 8000 },
        );
      } else if (lossless) {
        message?.info(
          '1st Gen Frame Generation Capture Fix has been automatically enabled because it is required for Lossless Scaling frame generation.',
          { duration: 8000 },
        );
      }
      setTimeout(() => {
        autoEnablingGen1 = false;
      }, 100);
    } else if (!anyFrameGenEnabled && wasFrameGenEnabled && form.value.gen1FramegenFix) {
      autoEnablingGen1 = true;
      form.value.gen1FramegenFix = false;
      setTimeout(() => {
        autoEnablingGen1 = false;
      }, 100);
    }
  },
);
// Scroll affordance logic for modal body
const bodyRef = ref<HTMLElement | null>(null);
const showTopShadow = ref(false);
const showBottomShadow = ref(false);

function updateShadows() {
  const el = bodyRef.value;
  if (!el) return;
  const { scrollTop, scrollHeight, clientHeight } = el;
  const hasOverflow = scrollHeight > clientHeight + 1;
  showTopShadow.value = hasOverflow && scrollTop > 4;
  showBottomShadow.value = hasOverflow && scrollTop + clientHeight < scrollHeight - 4;
}

function onBodyScroll() {
  updateShadows();
}

let ro: ResizeObserver | null = null;
onMounted(() => {
  const el = bodyRef.value;
  if (el) {
    el.addEventListener('scroll', onBodyScroll, { passive: true });
  }
  // Update on size/content changes
  try {
    ro = new ResizeObserver(() => updateShadows());
    if (el) ro.observe(el);
  } catch {}
  // Initial calc after next paint
  requestAnimationFrame(() => updateShadows());
});
onBeforeUnmount(() => {
  const el = bodyRef.value;
  if (el) el.removeEventListener('scroll', onBodyScroll as any);
  try {
    ro?.disconnect();
  } catch {}
  ro = null;
});

// Update name options while user searches
function onNameSearch(q: string) {
  nameSearchQuery.value = q || '';
  const query = String(q || '')
    .trim()
    .toLowerCase();
  const list: { label: string; value: string }[] = [];
  if (query.length) {
    list.push({ label: `Custom: "${q}"`, value: `__custom__:${q}` });
  } else {
    const cur = String(form.value.name || '').trim();
    if (cur) list.push({ label: `Custom: "${cur}"`, value: `__custom__:${cur}` });
  }
  if (playniteOptions.value.length) {
    const filtered = (
      query
        ? playniteOptions.value.filter((o) => o.label.toLowerCase().includes(query))
        : playniteOptions.value.slice(0, 100)
    ).slice(0, 100);
    list.push(...filtered);
  }
  nameOptions.value = list;
}

// Handle picking either a Playnite game or a custom name
function onNamePicked(val: string | null) {
  const v = String(val || '');
  if (!v) {
    nameSelectValue.value = '';
    form.value.name = '';
    form.value.playniteId = undefined;
    form.value.playniteManaged = undefined;
    return;
  }
  if (v.startsWith('__custom__:')) {
    const name = v.substring('__custom__:'.length).trim();
    form.value.name = name;
    form.value.playniteId = undefined;
    form.value.playniteManaged = undefined;
    return;
  }
  const opt = playniteOptions.value.find((o) => o.value === v);
  if (opt) {
    form.value.name = opt.label;
    form.value.playniteId = v;
    form.value.playniteManaged = 'manual';
  }
}

// Cover preview logic removed; Sunshine no longer fetches or proxies images
async function save() {
  saving.value = true;
  try {
    // If on Windows and name exactly matches a Playnite game, auto-link it
    try {
      if (
        isWindows.value &&
        !form.value.playniteId &&
        Array.isArray(playniteOptions.value) &&
        playniteOptions.value.length &&
        typeof form.value.name === 'string'
      ) {
        const target = String(form.value.name || '')
          .trim()
          .toLowerCase();
        const exact = playniteOptions.value.find((o) => o.label.trim().toLowerCase() === target);
        if (exact) {
          form.value.playniteId = exact.value;
          form.value.playniteManaged = 'manual';
        }
      }
    } catch (_) {}
    const payload = toServerPayload(form.value);
    const response = await http.post('./api/apps', payload, {
      headers: { 'Content-Type': 'application/json' },
      validateStatus: () => true,
    });
    const okStatus = response.status >= 200 && response.status < 300;
    const responseData = response?.data as any;
    if (!okStatus || (responseData && responseData.status === false)) {
      const errMessage =
        responseData && typeof responseData === 'object' && 'error' in responseData
          ? String(responseData.error ?? 'Failed to save application.')
          : 'Failed to save application.';
      message?.error(errMessage);
      return;
    }
    emit('saved');
    close();
  } finally {
    saving.value = false;
  }
}
const message = useMessage();

async function del() {
  saving.value = true;
  try {
    // If Playnite auto-managed, add to exclusion list before removing
    const pid = form.value.playniteId;
    if (isPlayniteAuto.value && pid) {
      try {
        // Ensure config store is loaded
        try {
          // @ts-ignore optional chaining for older runtime
          if (!configStore.config) await (configStore.fetchConfig?.() || Promise.resolve());
        } catch {}
        // Start from current local store state to avoid desync
        const current: Array<{ id: string; name: string }> = Array.isArray(
          (configStore.config as any)?.playnite_exclude_games,
        )
          ? ((configStore.config as any).playnite_exclude_games as any)
          : [];
        const map = new Map(current.map((e) => [String(e.id), String(e.name || '')] as const));
        const name = playniteOptions.value.find((o) => o.value === String(pid))?.label || '';
        map.set(String(pid), name);
        const next = Array.from(map.entries()).map(([id, name]) => ({ id, name }));
        // Update local store (keeps UI in sync) and persist via store API
        configStore.updateOption('playnite_exclude_games', next);
        await configStore.save();
      } catch (_) {
        // best-effort; continue with deletion even if exclusion save fails
      }
    }

    const r = await http.delete(`./api/apps/${form.value.index}`, { validateStatus: () => true });
    try {
      if (r && (r as any).data && (r as any).data.playniteFullscreenDisabled) {
        try {
          configStore.updateOption('playnite_fullscreen_entry_enabled', false);
        } catch {}
        try {
          message?.info(
            'Playnite Fullscreen entry removed. The Playnite Desktop option was turned off in Settings → Playnite.',
          );
        } catch {}
      }
    } catch {}
    // Best-effort force sync on Windows environments
    try {
      await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    } catch (_) {}
    emit('deleted');
    close();
  } finally {
    saving.value = false;
  }
}
</script>
<style scoped>
.mobile-only-hidden {
  display: none;
}

/* Mobile-friendly modal sizing and sticky header/footer */
@media (max-width: 640px) {
  :deep(.n-modal .n-card) {
    border-radius: 0 !important;
    max-width: 100vw !important;
    width: 100vw !important;
    height: 100dvh !important;
    max-height: 100dvh !important;
  }
  :deep(.n-modal .n-card .n-card__header),
  :deep(.n-modal .n-card .n-card-header) {
    position: sticky;
    top: 0;
    z-index: 10;
    backdrop-filter: saturate(1.2) blur(8px);
    background: rgb(var(--color-light) / 0.9);
  }
  :deep(.dark .n-modal .n-card .n-card__header),
  :deep(.dark .n-modal .n-card .n-card-header) {
    background: rgb(var(--color-surface) / 0.9);
  }
  :deep(.n-modal .n-card .n-card__footer),
  :deep(.n-modal .n-card .n-card-footer) {
    position: sticky;
    bottom: 0;
    z-index: 10;
    backdrop-filter: saturate(1.2) blur(8px);
    background: rgb(var(--color-light) / 0.9);
    padding-bottom: calc(env(safe-area-inset-bottom) + 0.5rem) !important;
  }
  :deep(.dark .n-modal .n-card .n-card__footer),
  :deep(.dark .n-modal .n-card .n-card-footer) {
    background: rgb(var(--color-surface) / 0.9);
  }
}
.scroll-shadow-top {
  position: sticky;
  top: 0;
  height: 16px;
  background: linear-gradient(
    to bottom,
    rgb(var(--color-light) / 0.9),
    rgb(var(--color-light) / 0)
  );
  pointer-events: none;
  z-index: 1;
}
.dark .scroll-shadow-top {
  background: linear-gradient(
    to bottom,
    rgb(var(--color-surface) / 0.9),
    rgb(var(--color-surface) / 0)
  );
}
.scroll-shadow-bottom {
  position: sticky;
  bottom: 0;
  height: 20px;
  background: linear-gradient(to top, rgb(var(--color-light) / 0.9), rgb(var(--color-light) / 0));
  pointer-events: none;
  z-index: 1;
}
.dark .scroll-shadow-bottom {
  background: linear-gradient(
    to top,
    rgb(var(--color-surface) / 0.9),
    rgb(var(--color-surface) / 0)
  );
}
.ui-input {
  width: 100%;
  border: 1px solid rgba(0, 0, 0, 0.12);
  background: rgba(255, 255, 255, 0.75);
  padding: 8px 10px;
  border-radius: 8px;
  font-size: 13px;
  line-height: 1.2;
}
.dark .ui-input {
  background: rgba(13, 16, 28, 0.65);
  border-color: rgba(255, 255, 255, 0.14);
  color: #f5f9ff;
}
.ui-checkbox {
  width: 14px;
  height: 14px;
}
</style>
