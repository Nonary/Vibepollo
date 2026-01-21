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
              v-model:checked="displayOverrideEnabled"
              size="small"
              class="md:col-span-2"
            >
              <div class="flex flex-col">
                <span>{{ t('config.virtual_display_toggle_label') }}</span>
                <span class="text-[11px] opacity-60">
                  {{ t('config.virtual_display_toggle_hint') }}
                </span>
              </div>
            </n-checkbox>
          </div>

          <div
            v-if="isWindows && displayOverrideEnabled"
            class="space-y-5 rounded-xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-dark/40 p-4"
          >
            <div class="space-y-2">
              <div class="flex items-center justify-between gap-3">
                <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                  {{ t('config.app_display_override_label') }}
                </span>
              </div>
              <p class="text-[11px] opacity-70">{{ t('config.app_display_override_hint') }}</p>
            </div>
            <div class="space-y-2">
              <n-radio-group v-model:value="displaySelection" class="grid gap-3 sm:grid-cols-2">
                <n-radio value="virtual" class="app-radio-card cursor-pointer">
                  <span class="app-radio-card-title">{{
                    t('config.app_display_override_virtual')
                  }}</span>
                </n-radio>
                <n-radio value="physical" class="app-radio-card cursor-pointer">
                  <span class="app-radio-card-title">{{
                    t('config.app_display_override_physical')
                  }}</span>
                </n-radio>
              </n-radio-group>
            </div>

            <div v-if="displaySelection === 'physical'" class="space-y-2">
              <div class="flex items-center justify-between gap-3">
                <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                  {{ t('config.app_display_physical_label') }}
                </span>
                <n-button
                  size="tiny"
                  tertiary
                  :loading="displayDevicesLoading"
                  @click="loadDisplayDevices"
                >
                  {{ t('_common.refresh') }}
                </n-button>
              </div>
              <p class="text-[11px] opacity-70">{{ t('config.app_display_physical_hint') }}</p>
              <n-select
                v-model:value="physicalOutputModel"
                :options="displayDeviceOptions"
                :loading="displayDevicesLoading"
                :placeholder="t('config.app_display_physical_placeholder')"
                filterable
                clearable
                @focus="onDisplaySelectFocus"
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
              </n-select>
              <div class="text-[11px] opacity-70">
                <span v-if="displayDevicesError" class="text-red-500">{{
                  displayDevicesError
                }}</span>
                <span v-else>{{ t('config.app_display_physical_status_hint') }}</span>
              </div>
            </div>

            <div v-if="displaySelection === 'physical'" class="space-y-3">
              <div class="flex items-center justify-between gap-3">
                <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                  {{ t('config.dd_config_label') }}
                </span>
                <n-button
                  v-if="form.ddConfigurationOption"
                  size="tiny"
                  tertiary
                  @click="form.ddConfigurationOption = null"
                >
                  {{ t('config.app_virtual_display_mode_reset') }}
                </n-button>
              </div>
              <p class="text-[11px] opacity-70">{{ t('config.dd_config_hint') }}</p>
              <n-radio-group v-model:value="form.ddConfigurationOption" class="grid gap-2">
                <n-radio
                  v-for="opt in appDdConfigurationOptions"
                  :key="opt.value"
                  :value="opt.value"
                  :label="opt.label"
                />
              </n-radio-group>
            </div>

            <div
              v-if="displaySelection === 'virtual'"
              class="space-y-5 rounded-xl bg-light/40 dark:bg-dark/40 p-3 md:p-4"
            >
              <div class="space-y-2">
                <div class="flex items-center justify-between gap-3">
                  <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                    {{ t('config.app_virtual_display_mode_label') }}
                  </span>
                  <n-button
                    v-if="form.virtualDisplayMode !== null"
                    size="tiny"
                    tertiary
                    @click="form.virtualDisplayMode = null"
                  >
                    {{ t('config.app_virtual_display_mode_reset') }}
                  </n-button>
                </div>
                <p class="text-[11px] opacity-70">
                  {{ t('config.app_virtual_display_mode_hint') }}
                </p>
              </div>
              <n-radio-group
                :value="resolvedVirtualDisplayMode"
                @update:value="
                  (v) => (form.virtualDisplayMode = v === globalVirtualDisplayMode ? null : v)
                "
                class="grid gap-3 sm:grid-cols-3"
              >
                <n-radio
                  v-for="option in appVirtualDisplayModeOptions"
                  :key="option.value"
                  :value="option.value"
                  class="app-radio-card cursor-pointer"
                >
                  <span class="app-radio-card-title">{{ option.label }}</span>
                </n-radio>
              </n-radio-group>

              <div class="space-y-2">
                <div class="flex items-center justify-between gap-3">
                  <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                    {{ t('config.virtual_display_layout_label') }}
                  </span>
                  <n-button
                    v-if="form.virtualDisplayLayout !== null"
                    size="tiny"
                    tertiary
                    @click="form.virtualDisplayLayout = null"
                  >
                    {{ t('config.app_virtual_display_layout_reset') }}
                  </n-button>
                </div>
                <p class="text-[11px] opacity-70">{{ t('config.virtual_display_layout_hint') }}</p>
              </div>
              <n-radio-group
                :value="resolvedVirtualDisplayLayout"
                @update:value="
                  (v) => (form.virtualDisplayLayout = v === globalVirtualDisplayLayout ? null : v)
                "
                class="space-y-4"
              >
                <div
                  v-for="option in appVirtualDisplayLayoutOptions"
                  :key="option.value"
                  class="flex flex-col cursor-pointer py-2 px-2 rounded-md hover:bg-surface/10"
                  @click="selectVirtualDisplayLayout(option.value)"
                  @keydown.enter.prevent="selectVirtualDisplayLayout(option.value)"
                  @keydown.space.prevent="selectVirtualDisplayLayout(option.value)"
                  tabindex="0"
                >
                  <div class="flex items-center gap-3">
                    <n-radio :value="option.value" />
                    <span class="text-sm font-semibold">{{ option.label }}</span>
                  </div>
                  <span class="text-[11px] opacity-70 leading-snug ml-6">{{
                    option.description
                  }}</span>
                </div>
              </n-radio-group>
            </div>
          </div>

          <AppEditConfigOverridesSection v-model:overrides="form.configOverrides" />

          <AppEditFrameGenSection
            v-if="isWindows"
            v-model:mode="frameGenerationSelection"
            v-model:gen1="form.gen1FramegenFix"
            v-model:gen2="form.gen2FramegenFix"
            v-model:lossless-profile="form.losslessScalingProfile"
            v-model:lossless-target-fps="form.losslessScalingTargetFps"
            v-model:lossless-rtss-limit="form.losslessScalingRtssLimit"
            v-model:lossless-flow-scale="losslessFlowScaleModel"
            v-model:lossless-launch-delay="form.losslessScalingLaunchDelay"
            :health="frameGenHealth"
            :health-loading="frameGenHealthLoading"
            :health-error="frameGenHealthError"
            :lossless-active="losslessFrameGenEnabled"
            :nvidia-active="nvidiaFrameGenEnabled"
            :using-virtual-display="usingVirtualDisplay"
            :has-active-lossless-overrides="hasActiveLosslessOverrides"
            :on-lossless-rtss-limit-change="onLosslessRtssLimitChange"
            :reset-active-lossless-profile="resetActiveLosslessProfile"
            @refresh-health="handleFrameGenHealthRequest"
            @enable-virtual-screen="handleEnableVirtualScreen"
          />

          <AppEditLosslessScalingSection
            v-if="isWindows"
            v-model:form="form"
            v-model:lossless-performance-mode="losslessPerformanceModeModel"
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
            :lossless-executable-detected="losslessExecutableDetected"
            :lossless-executable-check-complete="losslessExecutableCheckComplete"
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
import { NModal, NCard, NButton, NCheckbox, NRadioGroup, NRadio, NSelect } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { useI18n } from 'vue-i18n';
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
  FrameGenHealth,
  AppVirtualDisplayMode,
  AppVirtualDisplayLayout,
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
  parseFrameGenerationMode,
  normalizeFrameGenerationProvider,
  parseLosslessOverrides,
  parseLosslessProfileKey,
  parseNumeric,
} from './app-edit/lossless';
import AppEditBasicsSection from './app-edit/AppEditBasicsSection.vue';
import AppEditConfigOverridesSection from './app-edit/AppEditConfigOverridesSection.vue';
import AppEditLosslessScalingSection from './app-edit/AppEditLosslessScalingSection.vue';
import AppEditPrepCommandsSection from './app-edit/AppEditPrepCommandsSection.vue';
import AppEditFrameGenSection from './app-edit/AppEditFrameGenSection.vue';
import AppEditCoverModal, { type CoverCandidate } from './app-edit/AppEditCoverModal.vue';
import AppEditDeleteConfirmModal from './app-edit/AppEditDeleteConfirmModal.vue';

type DisplayDevice = {
  device_id?: string;
  display_name?: string;
  friendly_name?: string;
  info?: {
    active?: boolean;
  };
};
type DisplaySelection = 'global' | 'virtual' | 'physical';

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
const message = useMessage();
const { t } = useI18n();
function fresh(): AppForm {
  return {
    index: -1,
    name: '',
    cmd: '',
    workingDir: '',
    imagePath: '',
    excludeGlobalPrepCmd: false,
    configOverrides: {},
    elevated: false,
    autoDetach: true,
    waitAll: true,
    frameGenLimiterFix: false,
    exitTimeout: 5,
    prepCmd: [],
    detached: [],
    virtualScreen: false,
    gen1FramegenFix: false,
    gen2FramegenFix: false,
    output: '',
    frameGenerationProvider: 'game-provided',
    frameGenerationMode: 'off',
    losslessScalingEnabled: false,
    losslessScalingTargetFps: null,
    losslessScalingRtssLimit: null,
    losslessScalingRtssTouched: false,
    losslessScalingProfile: 'recommended',
    losslessScalingProfiles: emptyLosslessProfileState(),
    losslessScalingLaunchDelay: null,
    virtualDisplayMode: null,
    virtualDisplayLayout: null,
    ddConfigurationOption: null,
  };
}
const form = ref<AppForm>(fresh());

const APP_VIRTUAL_DISPLAY_MODES: AppVirtualDisplayMode[] = ['disabled', 'per_client', 'shared'];
const APP_VIRTUAL_DISPLAY_LAYOUTS: AppVirtualDisplayLayout[] = [
  'exclusive',
  'extended',
  'extended_primary',
  'extended_isolated',
  'extended_primary_isolated',
];

function parseAppVirtualDisplayMode(value: unknown): AppVirtualDisplayMode | null {
  if (typeof value !== 'string') {
    return null;
  }
  const normalized = value.trim().toLowerCase();
  if (APP_VIRTUAL_DISPLAY_MODES.includes(normalized as AppVirtualDisplayMode)) {
    return normalized as AppVirtualDisplayMode;
  }
  return null;
}

function parseAppVirtualDisplayLayout(value: unknown): AppVirtualDisplayLayout | null {
  if (typeof value !== 'string') {
    return null;
  }
  const normalized = value.trim().toLowerCase();
  if (APP_VIRTUAL_DISPLAY_LAYOUTS.includes(normalized as AppVirtualDisplayLayout)) {
    return normalized as AppVirtualDisplayLayout;
  }
  return null;
}

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
  const lsLaunchDelayRaw = parseNumeric(src['lossless-scaling-launch-delay']);
  const lsLaunchDelay = lsLaunchDelayRaw && lsLaunchDelayRaw > 0 ? Math.round(lsLaunchDelayRaw) : null;
  const profileKey = parseLosslessProfileKey(src['lossless-scaling-profile']);
  const losslessProfiles = emptyLosslessProfileState();
  losslessProfiles.recommended = parseLosslessOverrides(src['lossless-scaling-recommended']);
  losslessProfiles.custom = parseLosslessOverrides(src['lossless-scaling-custom']);
  const frameGenerationModeFromConfig = parseFrameGenerationMode(
    (src as any)?.['frame-generation-mode'],
  );
  const normalizedProvider = normalizeFrameGenerationProvider(src['frame-generation-provider']);
  let frameGenerationMode: FrameGenerationMode = frameGenerationModeFromConfig ?? 'off';
  if (!frameGenerationModeFromConfig) {
    if (normalizedProvider === 'nvidia-smooth-motion') {
      frameGenerationMode = 'nvidia-smooth-motion';
    } else if (normalizedProvider === 'lossless-scaling') {
      const hasLosslessFrameGen = lsEnabled || lsTarget !== null || lsLimit !== null;
      frameGenerationMode = hasLosslessFrameGen ? 'lossless-scaling' : 'off';
    } else if (normalizedProvider === 'game-provided') {
      frameGenerationMode = 'game-provided';
    }
  }
  const frameGenerationProvider =
    frameGenerationModeFromConfig && frameGenerationModeFromConfig !== 'off'
      ? (frameGenerationModeFromConfig as FrameGenerationProvider)
      : normalizedProvider;
  const rawOutput = String(src.output ?? '');
  const rawVirtualScreen = (src as any)?.['virtual-screen'];
  const virtualScreen =
    typeof rawVirtualScreen === 'boolean'
      ? rawVirtualScreen
      : rawOutput === VIRTUAL_DISPLAY_SELECTION;
  const sanitizedOutput = virtualScreen && rawOutput === VIRTUAL_DISPLAY_SELECTION ? '' : rawOutput;
  const serverVirtualDisplayMode = parseAppVirtualDisplayMode(
    (src as any)?.['virtual-display-mode'],
  );
  const serverVirtualDisplayLayout = parseAppVirtualDisplayLayout(
    (src as any)?.['virtual-display-layout'],
  );
  const ddConfigRaw = (src as any)?.['dd-configuration-option'];
  let ddConfigValue: AppForm['ddConfigurationOption'] = null;
  if (typeof ddConfigRaw === 'string') {
    const normalized = ddConfigRaw.trim().toLowerCase();
    const allowed: AppForm['ddConfigurationOption'][] = [
      'disabled',
      'verify_only',
      'ensure_active',
      'ensure_primary',
      'ensure_only_display',
    ];
    if (allowed.includes(normalized as AppForm['ddConfigurationOption'])) {
      ddConfigValue = normalized as AppForm['ddConfigurationOption'];
    }
  }
  return {
    index: idx,
    name: String(src.name ?? ''),
    output: sanitizedOutput,
    cmd: String(cmdStr ?? ''),
    workingDir: String(src['working-dir'] ?? ''),
    imagePath: String(src['image-path'] ?? ''),
    excludeGlobalPrepCmd: !!src['exclude-global-prep-cmd'],
    configOverrides:
      (src as any)?.['config-overrides'] &&
      typeof (src as any)['config-overrides'] === 'object' &&
      !Array.isArray((src as any)['config-overrides'])
        ? JSON.parse(JSON.stringify((src as any)['config-overrides']))
        : {},
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
    virtualScreen,
    gen1FramegenFix: !!(src['gen1-framegen-fix'] ?? src['dlss-framegen-capture-fix']),
    gen2FramegenFix: !!src['gen2-framegen-fix'],
    playniteId: src['playnite-id'] || undefined,
    playniteManaged: src['playnite-managed'] || undefined,
    frameGenerationProvider,
    frameGenerationMode,
    losslessScalingEnabled: lsEnabled,
    losslessScalingTargetFps: lsTarget,
    losslessScalingRtssLimit: lsLimit,
    losslessScalingRtssTouched: lsLimit !== null,
    losslessScalingProfile: profileKey,
    losslessScalingProfiles: losslessProfiles,
    losslessScalingLaunchDelay: lsLaunchDelay,
    virtualDisplayMode: serverVirtualDisplayMode,
    virtualDisplayLayout: serverVirtualDisplayLayout,
    ddConfigurationOption: ddConfigValue,
  };
}

function toServerPayload(f: AppForm): Record<string, any> {
  const selection = displaySelection.value;
  const payload: Record<string, any> = {
    // Index is required by the backend to determine add (-1) vs update (>= 0)
    index: typeof f.index === 'number' ? f.index : -1,
    name: f.name,
    cmd: f.cmd,
    'working-dir': f.workingDir,
    'image-path': String(f.imagePath || '').replace(/\"/g, ''),
    'exclude-global-prep-cmd': !!f.excludeGlobalPrepCmd,
    ...(f.configOverrides &&
    typeof f.configOverrides === 'object' &&
    !Array.isArray(f.configOverrides) &&
    Object.keys(f.configOverrides).length
      ? {
          'config-overrides': Object.fromEntries(
            Object.entries(f.configOverrides).filter(
              ([k, v]) => typeof k === 'string' && k.length > 0 && v !== undefined && v !== null,
            ),
          ),
        }
      : {}),
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
    // Leave 'virtual-screen' to be persisted only if explicitly different from the global setting.
  };
  // Only persist virtual display mode/layout if explicitly set and different from global defaults
  const _globalVDMode = globalVirtualDisplayMode.value;
  const _globalVDLayout = globalVirtualDisplayLayout.value;
  const _globalOutput = globalOutputName.value;
  if (f.virtualDisplayMode !== null && f.virtualDisplayMode !== _globalVDMode) {
    payload['virtual-display-mode'] = f.virtualDisplayMode;
  }
  if (f.virtualDisplayLayout !== null && f.virtualDisplayLayout !== _globalVDLayout) {
    payload['virtual-display-layout'] = f.virtualDisplayLayout;
  }
  if (f.playniteId) payload['playnite-id'] = f.playniteId;
  if (f.playniteManaged) payload['playnite-managed'] = f.playniteManaged;
  const provider = normalizeFrameGenerationProvider(f.frameGenerationProvider);
  const mode = f.frameGenerationMode ?? 'off';
  let resolvedProvider: FrameGenerationProvider = provider;
  if (mode === 'nvidia-smooth-motion') {
    resolvedProvider = 'nvidia-smooth-motion';
  } else if (mode === 'lossless-scaling') {
    resolvedProvider = 'lossless-scaling';
  } else if (mode === 'game-provided') {
    resolvedProvider = 'game-provided';
  } else {
    resolvedProvider = provider;
  }
  payload['frame-generation-provider'] = resolvedProvider;
  payload['frame-generation-mode'] = mode;
  const payloadLosslessTarget = parseNumeric(f.losslessScalingTargetFps);
  const payloadLosslessLimit = parseNumeric(f.losslessScalingRtssLimit);
  const losslessRuntimeActive = !!f.losslessScalingEnabled || mode === 'lossless-scaling';
  payload['lossless-scaling-framegen'] = losslessRuntimeActive;
  payload['lossless-scaling-target-fps'] =
    mode === 'lossless-scaling' ? payloadLosslessTarget : null;
  payload['lossless-scaling-rtss-limit'] =
    mode === 'lossless-scaling' ? payloadLosslessLimit : null;
  const payloadLosslessDelayRaw = parseNumeric(f.losslessScalingLaunchDelay);
  const payloadLosslessDelay =
    payloadLosslessDelayRaw && payloadLosslessDelayRaw > 0
      ? Math.round(payloadLosslessDelayRaw)
      : null;
  payload['lossless-scaling-launch-delay'] = losslessRuntimeActive
    ? payloadLosslessDelay
    : null;
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
  // Only persist output if it differs from global output (including virtual selection flag)
  if (typeof f.output === 'string') {
    const curOut = String(f.output || '');
    if (curOut !== '' && (curOut !== _globalOutput || selection === 'physical')) {
      payload['output'] = curOut;
    }
  }

  // Only persist virtual-screen if it differs from the global virtual output flag.
  const globalIsVirtual = _globalOutput === VIRTUAL_DISPLAY_SELECTION;
  if (!!f.virtualScreen !== globalIsVirtual) {
    payload['virtual-screen'] = !!f.virtualScreen;
  }
  if (f.ddConfigurationOption) {
    payload['dd-configuration-option'] = f.ddConfigurationOption;
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

const losslessExecutableStatus = ref<any | null>(null);
const losslessExecutableCheckComplete = ref(false);
function hasLosslessCandidates(status: any | null): boolean {
  return Array.isArray(status?.candidates) && status.candidates.length > 0;
}
const losslessExecutableDetected = computed<boolean>(() => {
  const status = losslessExecutableStatus.value;
  if (!status) {
    return false;
  }
  if (status.checked_exists || status.configured_exists || status.default_exists) {
    return true;
  }
  return hasLosslessCandidates(status);
});

async function refreshLosslessExecutableStatus() {
  if (!isWindows.value) {
    losslessExecutableStatus.value = null;
    losslessExecutableCheckComplete.value = true;
    return;
  }
  losslessExecutableCheckComplete.value = false;
  try {
    const params: Record<string, string> = {};
    const configuredPath = (configStore.config as any)?.lossless_scaling_path;
    if (configuredPath) {
      params['path'] = String(configuredPath);
    }
    const response = await http.get('/api/lossless_scaling/status', {
      params,
      validateStatus: () => true,
    });
    if (response.status >= 200 && response.status < 300) {
      losslessExecutableStatus.value = response.data ?? {};
    } else {
      losslessExecutableStatus.value = null;
    }
    losslessExecutableCheckComplete.value = true;
  } catch {
    losslessExecutableStatus.value = null;
    losslessExecutableCheckComplete.value = true;
  }
}

const frameGenerationSelection = computed<FrameGenerationMode>({
  get: () => form.value.frameGenerationMode ?? 'off',
  set: (mode) => {
    form.value.frameGenerationMode = mode;
    if (mode === 'nvidia-smooth-motion') {
      form.value.frameGenerationProvider = 'nvidia-smooth-motion';
      form.value.losslessScalingTargetFps = null;
      form.value.losslessScalingRtssLimit = null;
      form.value.losslessScalingRtssTouched = false;
    } else if (mode === 'lossless-scaling') {
      form.value.frameGenerationProvider = 'lossless-scaling';
    } else if (mode === 'game-provided') {
      form.value.frameGenerationProvider = 'game-provided';
      form.value.losslessScalingTargetFps = null;
      form.value.losslessScalingRtssLimit = null;
      form.value.losslessScalingRtssTouched = false;
    } else {
      form.value.frameGenerationProvider = 'game-provided';
      form.value.losslessScalingTargetFps = null;
      form.value.losslessScalingRtssLimit = null;
      form.value.losslessScalingRtssTouched = false;
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
    if (normalized === 'nvidia-smooth-motion') {
      if (form.value.frameGenerationMode !== 'nvidia-smooth-motion') {
        form.value.frameGenerationMode = 'nvidia-smooth-motion';
      }
    } else if (normalized === 'lossless-scaling') {
      if (form.value.frameGenerationMode !== 'lossless-scaling') {
        form.value.frameGenerationMode = 'lossless-scaling';
      }
    } else if (normalized === 'game-provided') {
      if (
        form.value.frameGenerationMode === 'lossless-scaling' ||
        form.value.frameGenerationMode === 'nvidia-smooth-motion'
      ) {
        form.value.frameGenerationMode = 'game-provided';
      }
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
  const normalized = parseNumeric(value);
  if (normalized === null) {
    form.value.losslessScalingRtssTouched = false;
    form.value.losslessScalingRtssLimit = null;
    return;
  }
  form.value.losslessScalingRtssTouched = true;
  form.value.losslessScalingRtssLimit = Math.min(360, Math.max(1, Math.round(normalized)));
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
  if (profile === activeLosslessProfile.value) {
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
const VIRTUAL_DISPLAY_SELECTION = 'sunshine:sudovda_virtual_display';
const globalOutputName = computed(() => {
  const name = (configStore.config as any)?.output_name;
  return typeof name === 'string' ? name : '';
});
const globalVirtualDisplayMode = computed<AppVirtualDisplayMode>(() => {
  const mode = (configStore.config as any)?.virtual_display_mode;
  return parseAppVirtualDisplayMode(mode) ?? 'disabled';
});
const globalVirtualDisplayLayout = computed<AppVirtualDisplayLayout>(() => {
  const layout = (configStore.config as any)?.virtual_display_layout;
  return parseAppVirtualDisplayLayout(layout) ?? 'exclusive';
});
const resolvedVirtualDisplayMode = computed<AppVirtualDisplayMode>(
  () => form.value.virtualDisplayMode ?? globalVirtualDisplayMode.value,
);
const resolvedVirtualDisplayLayout = computed<AppVirtualDisplayLayout>(
  () => form.value.virtualDisplayLayout ?? globalVirtualDisplayLayout.value,
);
const APP_VIRTUAL_DISPLAY_MODE_LABEL_KEYS: Record<AppVirtualDisplayMode, string> = {
  disabled: 'config.virtual_display_mode_disabled',
  per_client: 'config.virtual_display_mode_per_client',
  shared: 'config.virtual_display_mode_shared',
};
const appVirtualDisplayModeOptions = computed(() =>
  APP_VIRTUAL_DISPLAY_MODES.filter((value) => value !== 'disabled').map((value) => ({
    value,
    label: t(APP_VIRTUAL_DISPLAY_MODE_LABEL_KEYS[value]),
  })),
);
const appVirtualDisplayLayoutOptions = computed(() =>
  APP_VIRTUAL_DISPLAY_LAYOUTS.map((value) => ({
    value,
    label: t(`config.virtual_display_layout_${value}`),
    description: t(`config.virtual_display_layout_${value}_desc`),
  })),
);
const appDdConfigurationOptions = computed(() => [
  { label: t('_common.disabled') as string, value: 'disabled' },
  { label: t('config.dd_config_verify_only') as string, value: 'verify_only' },
  { label: t('config.dd_config_ensure_active') as string, value: 'ensure_active' },
  { label: t('config.dd_config_ensure_primary') as string, value: 'ensure_primary' },
  { label: t('config.dd_config_ensure_only_display') as string, value: 'ensure_only_display' },
]);

function selectVirtualDisplayLayout(v: unknown) {
  const sv = String(v).trim().toLowerCase();
  if (APP_VIRTUAL_DISPLAY_LAYOUTS.includes(sv as AppVirtualDisplayLayout)) {
    form.value.virtualDisplayLayout = sv as AppVirtualDisplayLayout;
  }
}
const lastPhysicalOutput = ref('');
const lastVirtualDisplayMode = ref<AppVirtualDisplayMode | null>(null);
const displaySelection = computed<DisplaySelection>({
  get: () => {
    const currentOutput = typeof form.value.output === 'string' ? form.value.output.trim() : '';
    const globalMode = globalVirtualDisplayMode.value;
    const appMode = form.value.virtualDisplayMode;
    if (form.value.virtualScreen || form.value.output === VIRTUAL_DISPLAY_SELECTION) {
      return 'virtual';
    }
    if (currentOutput) {
      return 'physical';
    }
    if (appMode === 'disabled') {
      return 'physical';
    }
    if (appMode !== null && appMode !== globalMode) {
      return 'virtual';
    }
    return 'global';
  },
  set: (selection) => {
    if (selection === 'virtual') {
      form.value.virtualScreen = true;
      if (form.value.virtualDisplayMode === 'disabled') {
        form.value.virtualDisplayMode =
          lastVirtualDisplayMode.value ?? globalVirtualDisplayMode.value ?? null;
      }
      form.value.output = '';
      form.value.ddConfigurationOption = null;
    } else if (selection === 'physical') {
      if (form.value.virtualDisplayMode && form.value.virtualDisplayMode !== 'disabled') {
        lastVirtualDisplayMode.value = form.value.virtualDisplayMode;
      }
      form.value.virtualDisplayMode = 'disabled';
      form.value.virtualScreen = false;
      const current = typeof form.value.output === 'string' ? form.value.output.trim() : '';
      if (!current || current === VIRTUAL_DISPLAY_SELECTION) {
        if (lastPhysicalOutput.value) {
          form.value.output = lastPhysicalOutput.value;
        } else if (globalOutputName.value && globalOutputName.value !== VIRTUAL_DISPLAY_SELECTION) {
          form.value.output = globalOutputName.value;
        }
      }
    } else {
      form.value.virtualScreen = false;
      form.value.virtualDisplayMode = null;
      form.value.output = '';
      form.value.ddConfigurationOption = null;
    }
  },
});
const displayOverrideEnabled = computed<boolean>({
  get: () => displaySelection.value !== 'global',
  set: (enabled) => {
    if (!enabled) {
      displaySelection.value = 'global';
    } else if (displaySelection.value === 'global') {
      displaySelection.value = 'virtual';
    }
  },
});
const windowsDisplayVersion = computed(() => {
  const v = (configStore.metadata as any)?.windows_display_version;
  return typeof v === 'string' ? v : '';
});
const windowsBuildNumber = computed<number | null>(() => {
  const raw = (configStore.metadata as any)?.windows_build_number;
  if (typeof raw === 'number' && Number.isFinite(raw)) return raw;
  if (typeof raw === 'string') {
    const parsed = Number(raw);
    if (Number.isFinite(parsed)) return parsed;
  }
  return null;
});
const autoCaptureUsesWgc = computed(() => {
  if (!isWindows.value) return false;
  const displayVersion = windowsDisplayVersion.value.toUpperCase();
  if (
    displayVersion.includes('23H2') ||
    displayVersion.includes('24H1') ||
    displayVersion.includes('24H2')
  ) {
    return true;
  }
  const build = windowsBuildNumber.value;
  if (build !== null) {
    // Windows 11 23H2 corresponds to build 22631; treat newer builds as equivalent or better
    return build >= 22631;
  }
  return false;
});
const virtualOutputName = computed(() => {
  const outputName = (configStore.config as any)?.output_name;
  return typeof outputName === 'string' ? outputName : '';
});
const usingVirtualDisplay = computed(() => {
  const selection = displaySelection.value;
  if (selection === 'virtual') return true;
  if (selection === 'physical') return false;
  const mode = resolvedVirtualDisplayMode.value;
  if (mode === 'per_client' || mode === 'shared') {
    return true;
  }
  if (mode === 'disabled') {
    return virtualOutputName.value === VIRTUAL_DISPLAY_SELECTION;
  }
  return false;
});
const skipDisplayWarnings = computed(() => usingVirtualDisplay.value);
const displayDevices = ref<DisplayDevice[]>([]);
const displayDevicesLoading = ref(false);
const displayDevicesError = ref('');
const displayNameCache = ref<Record<string, string>>({});
const physicalOutputModel = computed<string | null>({
  get: () => {
    const value = typeof form.value.output === 'string' ? form.value.output.trim() : '';
    return value || null;
  },
  set: (value) => {
    const normalized = typeof value === 'string' ? value.trim() : '';
    if (!normalized) {
      displaySelection.value = 'global';
      displayOverrideEnabled.value = false;
      return;
    }
    form.value.output = normalized;
    form.value.virtualScreen = false;
    lastPhysicalOutput.value = normalized;
    displaySelection.value = 'physical';
    displayOverrideEnabled.value = true;
  },
});

async function loadDisplayDevices(): Promise<void> {
  displayDevicesLoading.value = true;
  displayDevicesError.value = '';
  try {
    const res = await http.get<DisplayDevice[]>('/api/display-devices', {
      params: { detail: 'full' },
    });
    const devices = Array.isArray(res.data) ? res.data : [];
    displayDevices.value = devices;
    cacheDisplayNames(devices);
  } catch (e: any) {
    displayDevicesError.value = e?.message || 'Failed to load display devices';
    displayDevices.value = [];
  } finally {
    displayDevicesLoading.value = false;
  }
}

function normalizeDisplayKey(value: unknown): string {
  if (typeof value !== 'string') return '';
  return value.trim().toLowerCase();
}

function cacheDisplayNames(devices: DisplayDevice[]): void {
  if (!devices.length) return;
  const updated = { ...displayNameCache.value };
  for (const device of devices) {
    const label = device.friendly_name || device.display_name;
    if (!label) continue;
    for (const candidate of [device.device_id, device.display_name]) {
      const key = normalizeDisplayKey(candidate);
      if (!key) continue;
      updated[key] = label;
    }
  }
  displayNameCache.value = updated;
}

function getCachedDisplayLabel(value: string): string | null {
  const key = normalizeDisplayKey(value);
  if (!key) return null;
  return displayNameCache.value[key] ?? null;
}

const displayDeviceOptions = computed(() => {
  const opts: Array<{
    label: string;
    value: string;
    displayName?: string;
    id?: string;
    active?: boolean;
  }> = [];
  const seen = new Set<string>();
  for (const d of displayDevices.value) {
    const value = d.device_id || d.display_name || '';
    if (!value || seen.has(value)) continue;
    const displayName = d.friendly_name || d.display_name || 'Display';
    const guid = d.device_id || '';
    const dispName = d.display_name || '';
    const info = d.info as any;
    let active: boolean | null = null;
    if (info && typeof info === 'object' && 'active' in info) {
      active = !!(info as any).active;
    } else if (info) {
      active = true;
    }
    const parts: string[] = [displayName];
    if (guid) parts.push(guid);
    if (dispName) {
      const status = active === null ? '' : active ? ' (active)' : ' (inactive)';
      parts.push(dispName + status);
    }
    const label = parts.join(' - ');
    const idLine = guid && dispName ? `${guid} - ${dispName}` : guid || dispName;
    opts.push({ label, value, displayName, id: idLine, active });
    seen.add(value);
  }
  const current = typeof form.value.output === 'string' ? form.value.output.trim() : '';
  if (current && !seen.has(current)) {
    const label = getCachedDisplayLabel(current) ?? current;
    opts.push({ label, value: current, displayName: label, id: current, active: null });
  }
  if (
    lastPhysicalOutput.value &&
    !seen.has(lastPhysicalOutput.value) &&
    lastPhysicalOutput.value !== current
  ) {
    const id = lastPhysicalOutput.value;
    const label = getCachedDisplayLabel(id) ?? id;
    opts.push({ label, value: id, displayName: label, id, active: null });
  }
  return opts;
});

function onDisplaySelectFocus() {
  if (!displayDevicesLoading.value && displayDevices.value.length === 0) {
    void loadDisplayDevices();
  }
}

watch(
  () => form.value.output,
  (value) => {
    const normalized = typeof value === 'string' ? value.trim() : '';
    if (normalized && normalized !== VIRTUAL_DISPLAY_SELECTION) {
      lastPhysicalOutput.value = normalized;
    }
  },
  { immediate: true },
);

watch(
  () => form.value.virtualDisplayMode,
  (mode) => {
    if (mode && mode !== 'disabled') {
      lastVirtualDisplayMode.value = mode;
    }
  },
  { immediate: true },
);

const frameGenHealth = ref<FrameGenHealth | null>(null);
const frameGenHealthLoading = ref(false);
const frameGenHealthError = ref<string | null>(null);
let frameGenHealthPromise: Promise<void> | null = null;

watch(open, (o) => {
  if (o) {
    form.value = fromServerApp(props.app ?? undefined, props.index ?? -1);
    if (displaySelection.value === 'physical') {
      const currentOutput = typeof form.value.output === 'string' ? form.value.output.trim() : '';
      if (
        !currentOutput &&
        globalOutputName.value &&
        globalOutputName.value !== VIRTUAL_DISPLAY_SELECTION
      ) {
        form.value.output = globalOutputName.value;
      }
    }
    selectedPlayniteId.value = '';
    lockPlaynite.value = false;
    newAppSource.value = 'custom';
    refreshPlayniteStatus().then(() => {
      if (playniteInstalled.value) void loadPlayniteGames();
    });
    requestAnimationFrame(() => updateShadows());
    ensureNameSelectionFromForm();
    if (isWindows.value && (form.value.gen1FramegenFix || form.value.gen2FramegenFix)) {
      refreshFrameGenHealth({ reason: 'open', silent: true }).catch(() => {});
    } else {
      frameGenHealth.value = null;
      frameGenHealthError.value = null;
    }
    if (isWindows.value) {
      refreshLosslessExecutableStatus().catch(() => {});
      if (displaySelection.value === 'physical' && displayDevices.value.length === 0) {
        loadDisplayDevices().catch(() => {});
      }
    }
  } else {
    frameGenHealth.value = null;
    frameGenHealthError.value = null;
  }
});

watch(
  () => (configStore.config as any)?.lossless_scaling_path,
  () => {
    if (!open.value || !isWindows.value) return;
    refreshLosslessExecutableStatus().catch(() => {});
  },
);

watch(
  () => displaySelection.value,
  (selection) => {
    if (
      selection === 'physical' &&
      isWindows.value &&
      displayDevices.value.length === 0 &&
      !displayDevicesLoading.value
    ) {
      loadDisplayDevices().catch(() => {});
    }
    if (selection === 'physical' && !form.value.ddConfigurationOption) {
      form.value.ddConfigurationOption = 'verify_only';
    }
  },
);

type FrameGenHealthReason =
  | 'gen1'
  | 'gen2'
  | 'manual'
  | 'auto'
  | 'virtual-toggle'
  | 'capture-change'
  | 'output-change'
  | 'open';

interface FrameGenHealthOptions {
  reason?: FrameGenHealthReason;
  silent?: boolean;
}

function normalizeDeviceId(value: unknown): string {
  return typeof value === 'string' ? value.trim().toLowerCase() : '';
}

function parseRefreshHz(raw: any): number | null {
  if (raw === null || raw === undefined) return null;
  if (Array.isArray(raw)) {
    for (const item of raw) {
      const candidate = parseRefreshHz(item);
      if (candidate !== null) return candidate;
    }
    return null;
  }
  if (typeof raw === 'number') {
    return Number.isFinite(raw) ? raw : null;
  }
  if (typeof raw === 'string') {
    const trimmed = raw.trim();
    if (!trimmed) return null;
    const sanitized = trimmed.replace(/(hz|fps|frames|refresh)/gi, '').trim();
    const fractionMatch = sanitized.match(/^([-+]?\d+(?:\.\d+)?)\s*\/\s*([-+]?\d+(?:\.\d+)?)/);
    if (fractionMatch) {
      const numerator = Number(fractionMatch[1]);
      const denominator = Number(fractionMatch[2]);
      if (Number.isFinite(numerator) && Number.isFinite(denominator) && denominator !== 0) {
        return numerator / denominator;
      }
    }
    const valueMatch = sanitized.match(/[-+]?\d+(?:\.\d+)?/);
    if (valueMatch) {
      const num = Number(valueMatch[0]);
      if (Number.isFinite(num)) return num;
    }
    return null;
  }
  if (typeof raw === 'object') {
    if ('hz' in raw) {
      const hzCandidate = parseRefreshHz((raw as any).hz);
      if (hzCandidate !== null) return hzCandidate;
    }
    if ('value' in raw) {
      const valueCandidate = parseRefreshHz((raw as any).value);
      if (valueCandidate !== null) return valueCandidate;
    }
    if (typeof raw.type === 'string' && raw.value !== undefined) {
      const typed = raw as { type: string; value: unknown };
      if (typed.type === 'double') {
        return parseRefreshHz(typed.value);
      }
      if (typed.type === 'rational') {
        const val = typed.value ?? {};
        const numerator = Number(
          (val as any)?.numerator ?? (val as any)?.m_numerator ?? (val as any)?.num,
        );
        const denominator = Number(
          (val as any)?.denominator ?? (val as any)?.m_denominator ?? (val as any)?.den ?? 1,
        );
        if (Number.isFinite(numerator) && Number.isFinite(denominator) && denominator !== 0) {
          return numerator / denominator;
        }
      }
    }
    const numerator = Number(
      (raw as any)?.numerator ??
        (raw as any)?.m_numerator ??
        (raw as any)?.num ??
        (raw as any)?.n ??
        null,
    );
    const denominator = Number(
      (raw as any)?.denominator ?? (raw as any)?.m_denominator ?? (raw as any)?.den ?? 1,
    );
    if (Number.isFinite(numerator) && Number.isFinite(denominator) && denominator !== 0) {
      return numerator / denominator;
    }
  }
  return null;
}

function parseRefreshList(raw: unknown): number[] {
  const values: number[] = [];
  const collect = (entry: unknown) => {
    const hz = parseRefreshHz(entry);
    if (hz !== null && Number.isFinite(hz)) {
      values.push(hz);
    }
  };
  if (Array.isArray(raw)) {
    raw.forEach(collect);
  } else if (raw !== null && raw !== undefined) {
    collect(raw);
  }
  const seen = new Set<string>();
  const result: number[] = [];
  for (const hz of values) {
    if (hz <= 0) continue;
    const key = hz.toFixed(3);
    if (seen.has(key)) continue;
    seen.add(key);
    result.push(hz);
  }
  result.sort((a, b) => a - b);
  return result;
}

async function refreshFrameGenHealth(options: FrameGenHealthOptions = {}): Promise<void> {
  if (!isWindows.value) return;
  if (frameGenHealthPromise) return frameGenHealthPromise;
  const run = async () => {
    frameGenHealthLoading.value = true;
    frameGenHealthError.value = null;
    try {
      const [rtssResult, displayResult] = await Promise.allSettled([
        http.get('/api/rtss/status', { validateStatus: () => true }),
        http.get('/api/display-devices?detail=full', { validateStatus: () => true }),
      ]);

      const captureValue = (captureMethod.value || '').toString().toLowerCase();
      let captureStatus: FrameGenHealth['capture']['status'];
      let captureMessage: string;
      const autoTreatsAsWgc = captureValue === '' && autoCaptureUsesWgc.value;
      if (captureValue === 'wgc' || captureValue === 'wgcc' || autoTreatsAsWgc) {
        captureStatus = 'pass';
        captureMessage = autoTreatsAsWgc
          ? 'Automatic capture uses Windows Graphics Capture on this Windows build.'
          : 'Windows Graphics Capture is active for this system.';
      } else if (captureValue === '') {
        captureStatus = 'warn';
        captureMessage =
          'Autodetect may fall back to Desktop Duplication. Select Windows Graphics Capture in Settings -> Capture.';
      } else {
        captureStatus = 'fail';
        captureMessage =
          'Switch capture method to Windows Graphics Capture in Settings -> Capture to keep frame generation compatible.';
      }

      let rtssInstalled = false;
      let rtssHooks = false;
      let rtssRunning = false;
      let rtssStatus: FrameGenHealth['rtss']['status'] = 'unknown';
      let rtssMessage = 'Unable to verify RTSS.';
      if (rtssResult.status === 'fulfilled') {
        const res = rtssResult.value;
        const ok = res.status >= 200 && res.status < 300;
        if (ok) {
          const data = res.data as any;
          rtssInstalled = !!data?.path_exists;
          rtssHooks = !!data?.hooks_found;
          rtssRunning = !!data?.process_running;
          if (rtssInstalled && rtssHooks) {
            rtssStatus = 'pass';
            rtssMessage = 'RTSS hooks detected. Sunshine can control the frame limiter.';
          } else if (rtssInstalled) {
            rtssStatus = 'warn';
            rtssMessage =
              'RTSS is installed but hooks were not detected. Launch RTSS and ensure the Sunshine profile is active.';
          } else {
            rtssStatus = 'fail';
            rtssMessage = 'Install RTSS to avoid microstutter when frame generation is enabled.';
          }
        } else {
          rtssStatus = 'unknown';
          rtssMessage = 'RTSS status endpoint returned an error.';
        }
      } else {
        rtssStatus = 'unknown';
        rtssMessage = 'Unable to reach the RTSS status endpoint.';
      }

      const usingVirtual = usingVirtualDisplay.value;
      const fpsTargets = [60, 90, 120, 144];
      const tolerance = 0.5;
      let displayStatus: FrameGenHealth['display']['status'] = 'unknown';
      let displayMessage = 'Unable to determine display refresh capabilities.';
      let displayLabel = usingVirtual ? 'Sunshine Virtual Screen' : 'Active display';
      let displayId = usingVirtual ? VIRTUAL_DISPLAY_SELECTION : '';
      let displayHz: number | null = null;
      let displayError: string | null = null;
      let displayTargets = fpsTargets.map((fps) => ({
        fps,
        requiredHz: fps * 2,
        supported: usingVirtual ? true : null,
      }));
      let highestFailUnder144: number | null = null;
      let only144Fails = false;
      const edidSupport: Record<string, boolean | null> = {};
      let edidCapHz: number | null = null;
      let edidFetchError: string | null = null;

      if (!usingVirtual) {
        if (displayResult.status === 'fulfilled') {
          const res = displayResult.value;
          const ok = res.status >= 200 && res.status < 300;
          if (ok && Array.isArray(res.data)) {
            const devices = res.data as any[];
            const appOutput = form.value.output;
            const globalOutput = globalOutputName.value;
            const candidates = [
              appOutput && appOutput !== VIRTUAL_DISPLAY_SELECTION ? appOutput : '',
              globalOutput && globalOutput !== VIRTUAL_DISPLAY_SELECTION ? globalOutput : '',
            ].filter(Boolean) as string[];
            const normalizedCandidates = candidates.map((c) => normalizeDeviceId(c));
            let target = devices.find((item) => {
              const id = normalizeDeviceId(item?.device_id);
              const displayName = normalizeDeviceId(item?.display_name);
              return (
                normalizedCandidates.includes(id) || normalizedCandidates.includes(displayName)
              );
            });
            if (!target) {
              target = devices.find((item) => item && item.info) || devices[0];
            }
            if (target) {
              displayLabel =
                (typeof target.friendly_name === 'string' && target.friendly_name) ||
                (typeof target.display_name === 'string' && target.display_name) ||
                'Active display';
              displayId =
                (typeof target.device_id === 'string' && target.device_id) ||
                (typeof target.display_name === 'string' && target.display_name) ||
                '';
              const info = target.info as any;
              const refreshRaw = info?.refresh_rate ?? info?.refreshRate;
              const activeRefresh = parseRefreshHz(refreshRaw);
              const supportedRatesRaw =
                (target as any)?.supported_refresh_rates ?? (target as any)?.supportedRefreshRates;
              const supportedRates = parseRefreshList(supportedRatesRaw);
              const highestSupportedDxgi =
                supportedRates.length > 0 ? supportedRates[supportedRates.length - 1] : null;

              try {
                const deviceHint = displayId || displayLabel;
                if (deviceHint) {
                  const edidRes = await http.get('/api/framegen/edid-refresh', {
                    params: {
                      device_id: deviceHint,
                      targets: fpsTargets.map((fps) => fps * 2).join(','),
                    },
                    validateStatus: () => true,
                  });
                  if (
                    edidRes.status >= 200 &&
                    edidRes.status < 300 &&
                    edidRes.data &&
                    edidRes.data.status !== false
                  ) {
                    const data: any = edidRes.data;
                    if (!displayLabel && typeof data?.device_label === 'string') {
                      displayLabel = data.device_label;
                    }
                    const rangeHz = parseRefreshHz((data as any)?.max_vertical_hz);
                    const timingHz = parseRefreshHz((data as any)?.max_timing_hz);
                    const capCandidate =
                      rangeHz !== null && rangeHz > 0
                        ? rangeHz
                        : timingHz !== null && timingHz > 0
                          ? timingHz
                          : null;
                    if (capCandidate !== null) {
                      edidCapHz = capCandidate;
                    }
                    const targetEntries = Array.isArray((data as any)?.targets)
                      ? (data as any).targets
                      : [];
                    for (const entry of targetEntries) {
                      const hz = parseRefreshHz((entry as any)?.hz);
                      if (hz === null) continue;
                      const key = hz.toFixed(3);
                      if (typeof (entry as any)?.supported === 'boolean') {
                        edidSupport[key] = (entry as any).supported;
                      } else if (!(key in edidSupport)) {
                        edidSupport[key] = null;
                      }
                    }
                  } else if (edidRes.data && typeof (edidRes.data as any).error === 'string') {
                    edidFetchError = (edidRes.data as any).error;
                  }
                }
              } catch (e: any) {
                if (!edidFetchError) {
                  edidFetchError = e?.message || 'EDID refresh validation failed.';
                }
              }

              let highestSupported =
                edidCapHz !== null && Number.isFinite(edidCapHz) ? edidCapHz : highestSupportedDxgi;

              displayHz = activeRefresh;
              displayTargets = fpsTargets.map((fps) => {
                const required = fps * 2;
                const edidKey = required.toFixed(3);
                let supported: boolean | null;
                if (
                  Object.prototype.hasOwnProperty.call(edidSupport, edidKey) &&
                  typeof edidSupport[edidKey] === 'boolean'
                ) {
                  supported = edidSupport[edidKey] as boolean;
                } else if (supportedRates.length > 0) {
                  supported = supportedRates.some((rate) => rate >= required - tolerance);
                } else if (activeRefresh !== null) {
                  supported = activeRefresh >= required - tolerance;
                } else {
                  supported = null;
                }
                return { fps, requiredHz: required, supported };
              });

              const failingUnder144 = displayTargets.filter(
                (entry) => entry.supported === false && entry.fps < 144,
              );
              highestFailUnder144 = failingUnder144.length
                ? Math.max(...failingUnder144.map((entry) => entry.fps))
                : null;
              only144Fails =
                displayTargets.some((entry) => entry.fps === 144 && entry.supported === false) &&
                highestFailUnder144 === null;

              const evaluationHz = highestSupported ?? activeRefresh;
              const hasActive = activeRefresh !== null;
              const deltaSupported =
                highestSupported !== null &&
                hasActive &&
                Math.abs(highestSupported - activeRefresh) > tolerance;
              if (!displayError && edidFetchError) {
                displayError = edidFetchError;
              }

              if (evaluationHz === null) {
                displayStatus = 'unknown';
                displayMessage =
                  'Unable to read the refresh rate from the configured display. Double-check Display Device Step 1.';
              } else if (evaluationHz >= 240 - tolerance) {
                displayStatus = 'pass';
                if (only144Fails) {
                  const baseHz = hasActive ? (activeRefresh ?? evaluationHz) : evaluationHz;
                  displayMessage = `Current refresh is ${Math.round(baseHz)} Hz. Streams up to 120 FPS are covered. Only 144 FPS streams require the Sunshine virtual screen or a higher-refresh display.`;
                  if (!hasActive && highestSupported !== null) {
                    displayMessage = `Display supports up to ${Math.round(highestSupported)} Hz. Streams up to 120 FPS are covered. Only 144 FPS streams require the Sunshine virtual screen or a higher-refresh display.`;
                  } else if (deltaSupported && highestSupported !== null) {
                    displayMessage += ` Sunshine can switch to ${Math.round(highestSupported)} Hz when a stream starts if Display Device Step 1 keeps that monitor active.`;
                  }
                } else if (!hasActive && highestSupported !== null) {
                  displayMessage = `Display supports up to ${Math.round(highestSupported)} Hz. Sunshine can double 120 FPS streams.`;
                } else if (deltaSupported && highestSupported !== null) {
                  displayMessage = `Current refresh is ${Math.round(activeRefresh ?? evaluationHz)} Hz. Sunshine can switch to ${Math.round(highestSupported)} Hz during streams to keep frame generation smooth.`;
                } else {
                  displayMessage = 'Display refresh is high enough to double 120 FPS streams.';
                }
              } else if (evaluationHz >= 180 - tolerance) {
                displayStatus = 'warn';
                if (!hasActive && highestSupported !== null) {
                  displayMessage = `Display supports up to ${Math.round(evaluationHz)} Hz. Configure Display Device Step 1 to enforce the higher refresh or use the display override below to switch to the Sunshine virtual display.`;
                } else if (hasActive) {
                  if (highestFailUnder144 !== null) {
                    displayMessage = `Current refresh is ${Math.round(activeRefresh ?? evaluationHz)} Hz. Streams targeting up to ${highestFailUnder144} FPS need the Sunshine virtual screen or a higher-refresh display.`;
                  } else {
                    displayMessage = `Current refresh is ${Math.round(activeRefresh ?? evaluationHz)} Hz. 120 FPS frame generation may stutter without a higher refresh display. Use the display override below to switch to the Sunshine virtual display or move the stream to a higher-refresh monitor.`;
                  }
                  if (deltaSupported && highestSupported !== null) {
                    displayMessage += ` Sunshine can switch up to ${Math.round(highestSupported)} Hz if Display Device Step 1 keeps only that monitor active.`;
                  }
                } else {
                  displayMessage =
                    'Unable to read the current refresh rate, but the display may not reach the required 240 Hz. Use the display override below to switch to the Sunshine virtual display or move the stream to a higher-refresh monitor.';
                }
              } else {
                displayStatus = 'fail';
                if (!hasActive && highestSupported !== null) {
                  displayMessage = `Display tops out at ${Math.round(evaluationHz)} Hz. Use the display override below to switch to the Sunshine virtual display or switch to a 240 Hz display for frame generation.`;
                } else if (hasActive) {
                  const mention = highestFailUnder144 ?? 120;
                  displayMessage = `Current refresh is ${Math.round(activeRefresh ?? evaluationHz)} Hz. Streams targeting up to ${mention} FPS need the Sunshine virtual screen or a higher-refresh display.`;
                  if (deltaSupported && highestSupported !== null) {
                    displayMessage += ` Sunshine can switch up to ${Math.round(highestSupported)} Hz if configured in Display Device Step 1.`;
                  }
                } else {
                  displayMessage =
                    'Display refresh information was unavailable. Use the display override below to switch to the Sunshine virtual display or switch to a 240 Hz display for frame generation.';
                }
              }
            } else {
              displayStatus = 'unknown';
              displayMessage =
                'No display devices were returned by Sunshine’s helper. Frame generation may not be able to enforce refresh changes.';
              displayError = 'Display helper returned no devices.';
            }
          } else {
            displayStatus = 'unknown';
            displayMessage = 'Display helper did not respond with device information.';
            displayError = 'Display device enumeration failed.';
          }
        } else {
          displayStatus = 'unknown';
          displayMessage = 'Unable to reach the display helper.';
          displayError = 'Display helper request failed.';
        }
      } else {
        displayStatus = 'pass';
        displayMessage =
          'Sunshine virtual screen guarantees a high refresh surface for frame generation.';
      }

      if (usingVirtual) {
        displayTargets = fpsTargets.map((fps) => ({
          fps,
          requiredHz: fps * 2,
          supported: true,
        }));
      }

      const health: FrameGenHealth = {
        checkedAt: Date.now(),
        capture: {
          status: captureStatus,
          method: captureValue,
          message: captureMessage,
        },
        rtss: {
          status: rtssStatus,
          installed: rtssInstalled,
          running: rtssRunning,
          hooksDetected: rtssHooks,
          message: rtssMessage,
        },
        display: {
          status: displayStatus,
          deviceLabel: displayLabel,
          deviceId: displayId,
          currentHz: displayHz,
          targets: displayTargets,
          virtualActive: usingVirtual,
          message: displayMessage,
          error: displayError,
        },
        suggestion: undefined,
      };

      if (highestFailUnder144 !== null) {
        health.suggestion = {
          message: `Use the display override above to switch to the Sunshine virtual display or configure Display Device Step 1 to target the virtual display so ${highestFailUnder144} FPS streams stay smooth.`,
          emphasis: 'warning',
        };
      } else if (captureStatus === 'warn' || captureStatus === 'fail') {
        health.suggestion = {
          message:
            'Set Capture -> Method to Windows Graphics Capture so frame generation stays stable.',
          emphasis: 'info',
        };
      }

      frameGenHealth.value = health;
      frameGenHealthError.value = null;
    } catch (error) {
      frameGenHealth.value = null;
      frameGenHealthError.value =
        error instanceof Error ? error.message : 'Unable to run frame generation health check.';
      if (!options.silent) {
        message?.error('Unable to run frame generation health check.');
      }
    } finally {
      frameGenHealthLoading.value = false;
      frameGenHealthPromise = null;
    }
  };
  frameGenHealthPromise = run();
  return frameGenHealthPromise;
}

function handleFrameGenHealthRequest() {
  refreshFrameGenHealth({ reason: 'manual' }).catch(() => {});
}

function handleEnableVirtualScreen() {
  if (!isWindows.value) return;
  displayOverrideEnabled.value = true;
  displaySelection.value = 'virtual';
  refreshFrameGenHealth({ reason: 'virtual-toggle', silent: true }).catch(() => {});
}

function warnIfHealthIssues(reason: FrameGenHealthReason) {
  if (
    reason === 'auto' ||
    reason === 'virtual-toggle' ||
    reason === 'capture-change' ||
    reason === 'output-change' ||
    reason === 'open'
  ) {
    return;
  }
  if (!message) return;
  const health = frameGenHealth.value;
  if (!health) return;
  if (health.capture.status === 'warn' || health.capture.status === 'fail') {
    message.warning(
      'Switch capture method to Windows Graphics Capture in Settings -> Capture to keep frame generation compatible.',
      { duration: 8000 },
    );
  }
  if (health.rtss.status === 'warn' || health.rtss.status === 'fail') {
    message.warning(
      'RTSS is required for this fix. Install and launch RTSS to avoid microstutter.',
      { duration: 8000 },
    );
  }
  if (!skipDisplayWarnings.value && !health.display.virtualActive) {
    const requiresHigh = health.display.targets.some(
      (target) => target.fps < 144 && target.supported === false,
    );
    if (requiresHigh) {
      message.warning(
        'Use the display override to switch to the Sunshine virtual display or adjust Display Device Step 1 to keep only the high-refresh monitor active.',
        { duration: 8000 },
      );
    }
  }
}

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
      "1st Gen Frame Generation Capture Fix requires Windows Graphics Capture (WGC), RTSS, and a display capable of 240 Hz or higher. Sunshine's virtual screen or any display that satisfies the doubled refresh requirement will work.",
      { duration: 8000 },
    );
    if (!skipDisplayWarnings.value) {
      if (!ddConfigOption.value || ddConfigOption.value === 'disabled') {
        message?.warning(
          'Configure Step 1 for Sunshine\'s virtual screen or enable Display Device and set it to "Deactivate all other displays" so the doubled refresh requirement is met during the stream.',
          { duration: 8000 },
        );
      } else if (ddConfigOption.value !== 'ensure_only_display') {
        message?.warning(
          'Set Step 1 to use Sunshine\'s virtual screen or adjust Display Device to "Deactivate all other displays" so only the high-refresh monitor stays active.',
          { duration: 8000 },
        );
      }
    }
    await refreshFrameGenHealth({ reason: 'gen1' });
    warnIfHealthIssues('gen1');
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
      "2nd Gen Frame Generation Capture Fix (for DLSS 4) forces the NVIDIA Control Panel frame limiter and needs Windows Graphics Capture (WGC) plus an NVIDIA GPU. Sunshine's virtual screen guarantees support, but any display that satisfies the doubled refresh requirement also works.",
      { duration: 8000 },
    );
    if (!skipDisplayWarnings.value) {
      if (!ddConfigOption.value || ddConfigOption.value === 'disabled') {
        message?.warning(
          'Configure Step 1 for Sunshine\'s virtual screen or enable Display Device and set it to "Deactivate all other displays" so the doubled refresh requirement is met during the stream.',
          { duration: 8000 },
        );
      } else if (ddConfigOption.value !== 'ensure_only_display') {
        message?.warning(
          'Set Step 1 to use Sunshine\'s virtual screen or adjust Display Device to "Deactivate all other displays" so only the high-refresh monitor stays active.',
          { duration: 8000 },
        );
      }
    }
    await refreshFrameGenHealth({ reason: 'gen2' });
    warnIfHealthIssues('gen2');
  },
);

watch(
  () => displaySelection.value,
  (selection, prev) => {
    if (!isWindows.value) return;
    if (!(form.value.gen1FramegenFix || form.value.gen2FramegenFix || frameGenHealth.value)) return;
    if (selection === prev) return;
    const reason: FrameGenHealthReason =
      selection === 'virtual' || prev === 'virtual' ? 'virtual-toggle' : 'output-change';
    refreshFrameGenHealth({ reason, silent: true }).catch(() => {});
  },
);

watch(
  () => captureMethod.value,
  () => {
    if (!isWindows.value) return;
    if (!(form.value.gen1FramegenFix || form.value.gen2FramegenFix || frameGenHealth.value)) return;
    refreshFrameGenHealth({ reason: 'capture-change', silent: true }).catch(() => {});
  },
);

watch(
  () => autoCaptureUsesWgc.value,
  (enabled, prev) => {
    if (enabled === prev) return;
    if (!isWindows.value) return;
    if (!(form.value.gen1FramegenFix || form.value.gen2FramegenFix || frameGenHealth.value)) return;
    refreshFrameGenHealth({ reason: 'capture-change', silent: true }).catch(() => {});
  },
);

watch(
  () => [form.value.output, globalOutputName.value],
  () => {
    if (!isWindows.value) return;
    if (!(form.value.gen1FramegenFix || form.value.gen2FramegenFix || frameGenHealth.value)) return;
    refreshFrameGenHealth({ reason: 'output-change', silent: true }).catch(() => {});
  },
);

// Automatically enable Gen1 Frame Generation fix when Frame Generation is enabled
watch(
  () => frameGenerationSelection.value,
  (mode, prevMode) => {
    const anyFrameGenEnabled = mode !== 'off';
    const wasFrameGenEnabled = prevMode !== 'off';
    if (anyFrameGenEnabled && !form.value.gen1FramegenFix) {
      autoEnablingGen1 = true;
      form.value.gen1FramegenFix = true;
      if (mode === 'nvidia-smooth-motion') {
        message?.info(
          '1st Gen Frame Generation Capture Fix has been automatically enabled for optimal NVIDIA Smooth Motion performance.',
          { duration: 8000 },
        );
      } else if (mode === 'lossless-scaling') {
        message?.info(
          '1st Gen Frame Generation Capture Fix has been automatically enabled because it is required for Lossless Scaling frame generation.',
          { duration: 8000 },
        );
      } else if (mode === 'game-provided') {
        message?.info(
          '1st Gen Frame Generation Capture Fix has been automatically enabled to keep in-game frame generation tear-free.',
          { duration: 8000 },
        );
      }
      refreshFrameGenHealth({ reason: 'auto', silent: true }).catch(() => {});
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
            'Playnite Fullscreen entry removed. The Playnite Desktop option was turned off in Settings -> Playnite.',
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
