<template>
  <n-modal
    :show="open"
    :mask-closable="true"
    :trap-focus="!overridesPickerOpen"
    @update:show="handleModalShowUpdate"
  >
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
              class="app-modal-icon"
              :class="{
                'app-modal-icon--playnite': hasHeaderArtwork,
              }"
            >
              <img
                v-if="hasHeaderArtwork"
                class="app-modal-icon__image"
                :src="headerArtworkUrl"
                :alt="form.name || t('apps.application')"
                @error="headerArtworkFailed = true"
              />
              <i v-else class="fas fa-window-restore text-xl" />
            </div>
            <div class="flex flex-col">
              <span class="text-xl font-semibold">{{
                isNew ? t('apps.add_application') : t('apps.edit_application')
              }}</span>
            </div>
          </div>
          <div class="shrink-0">
            <span
              v-if="isPlayniteManaged"
              class="inline-flex items-center px-2 py-0.5 rounded bg-primary/15 text-primary text-[11px] font-semibold"
            >
              {{ t('apps.playnite_badge') }}
            </span>
            <span
              v-else
              class="inline-flex items-center px-2 py-0.5 rounded bg-dark/10 dark:bg-light/10 text-[11px] font-semibold"
            >
              {{ t('apps.source_custom') }}
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
              {{ t('apps.exclude_global_prep') }}
            </n-checkbox>
            <n-checkbox v-if="!isPlayniteManaged" v-model:checked="form.autoDetach" size="small">
              {{ t('apps.auto_detach') }}
            </n-checkbox>
            <n-checkbox v-if="!isPlayniteManaged" v-model:checked="form.waitAll" size="small"
              >{{ t('apps.wait_all') }}</n-checkbox
            >
            <n-checkbox
              v-if="isWindows && !isPlayniteManaged"
              v-model:checked="form.elevated"
              size="small"
            >
              {{ t('_common.elevated') }}
            </n-checkbox>
            <n-checkbox v-model:checked="form.terminateOnPause" size="small">
              {{ t('apps.terminate_on_pause') }}
            </n-checkbox>
            <n-checkbox v-model:checked="form.allowClientCommands" size="small" class="md:col-span-2">
              {{ t('apps.allow_client_commands') }}
            </n-checkbox>
            <n-checkbox v-model:checked="form.useAppIdentity" size="small">
              {{ t('apps.use_app_identity') }}
            </n-checkbox>
            <n-checkbox
              v-if="form.useAppIdentity"
              v-model:checked="form.perClientAppIdentity"
              size="small"
              class="md:col-span-2"
            >
              {{ t('apps.per_client_app_identity') }}
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
                v-model:value="appVirtualDisplayModeSelection"
                class="grid gap-3 sm:grid-cols-3"
              >
                <n-radio
                  v-for="option in appVirtualDisplayModeOptions"
                  :key="String(option.value)"
                  :value="option.value"
                  class="app-radio-card cursor-pointer"
                >
                  <span class="app-radio-card-title">{{ option.label }}</span>
                </n-radio>
              </n-radio-group>
              <div
                v-if="appVirtualDisplayModeSelection === 'global'"
                class="text-[11px] opacity-70"
              >
                {{ t('config.app_virtual_display_mode_follow_global') }}
              </div>

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

          <AppEditRtxHdrSection
            v-if="isWindows && hasNvidia"
            v-model:form="form"
            :live-status="liveRtxHdrStatus"
            :live-error="liveRtxHdrError"
          />

          <AppEditConfigOverridesSection
            v-model:overrides="form.configOverrides"
            v-model:picker-open="overridesPickerOpen"
          />

          <AppEditFrameGenSection
            v-if="isWindows"
            v-model:mode="frameGenerationSelection"
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
            :windows10="isWindows10"
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

          <section class="space-y-3">
            <div class="flex items-center justify-between">
              <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">
                {{ t('apps.cmd_state_name') }}
              </h3>
              <n-button size="small" type="primary" @click="addState">
                <i class="fas fa-plus" /> Add
              </n-button>
            </div>
            <n-checkbox v-model:checked="form.excludeGlobalStateCmd" size="small">
              {{ t('apps.exclude_global_state_cmd') }}
            </n-checkbox>
            <div v-if="form.stateCmd.length === 0" class="text-[12px] opacity-60">None</div>
            <div v-else class="space-y-2">
              <div v-for="(s, i) in form.stateCmd" :key="`state-${i}`"
                class="rounded-md border border-dark/10 dark:border-light/10 p-2">
                <div class="flex items-center justify-between gap-2 mb-2">
                  <div class="text-xs opacity-70">Step {{ i + 1 }}</div>
                  <div class="flex items-center gap-2">
                    <n-checkbox v-if="isWindows" v-model:checked="s.elevated" size="small">
                      Elevated
                    </n-checkbox>
                    <n-button size="small" type="error" strong @click="form.stateCmd.splice(i, 1)">
                      <i class="fas fa-trash" />
                    </n-button>
                  </div>
                </div>
                <div class="grid grid-cols-1 gap-2">
                  <div>
                    <label class="text-[11px] opacity-60">{{ t('_common.do_cmd') }}</label>
                    <n-input v-model:value="s.do" type="textarea" :autosize="{ minRows: 1, maxRows: 3 }"
                      class="font-mono" :placeholder="t('_common.do_cmd')" />
                  </div>
                  <div>
                    <label class="text-[11px] opacity-60">{{ t('_common.undo_cmd') }}</label>
                    <n-input v-model:value="s.undo" type="textarea" :autosize="{ minRows: 1, maxRows: 3 }"
                      class="font-mono" :placeholder="t('_common.undo_cmd')" />
                  </div>
                </div>
              </div>
            </div>
          </section>

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
import { computed, ref, watch, onMounted, onBeforeUnmount, nextTick } from 'vue';
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
  RtxHdrMode,
} from './app-edit/types';
import {
  LOSSLESS_PROFILE_DEFAULTS,
  LOSSLESS_SCALING_SHARPENING,
  clampFlow,
  clampResolution,
  clampSharpness,
  defaultRtssFromTarget,
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
import AppEditRtxHdrSection from './app-edit/AppEditRtxHdrSection.vue';
import AppEditCoverModal, { type CoverCandidate } from './app-edit/AppEditCoverModal.vue';
import AppEditDeleteConfirmModal from './app-edit/AppEditDeleteConfirmModal.vue';
import {
  VIRTUAL_DISPLAY_SELECTION,
  frameGenDisplayHealthKey,
  physicalFrameGenDisplayWarningKey,
  resolvesToVirtualDisplay,
  type DisplaySelection,
} from './app-edit/frameGenDisplayPolicy';

type DisplayDevice = {
  device_id?: string;
  display_name?: string;
  friendly_name?: string;
  info?: {
    active?: boolean;
  };
};
type AppVirtualDisplayModeSelection = AppVirtualDisplayMode | 'global';

interface AppEditModalProps {
  modelValue: boolean;
  app?: ServerApp | null;
  index?: number;
}

const SCALE_FACTOR_MIN = 20;
const SCALE_FACTOR_MAX = 200;

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
    uuid: undefined,
    name: '',
    cmd: '',
    workingDir: '',
    imagePath: '',
    playniteIconPath: '',
    excludeGlobalPrepCmd: false,
    excludeGlobalStateCmd: false,
    configOverrides: {},
    elevated: false,
    autoDetach: true,
    waitAll: true,
    terminateOnPause: false,
    allowClientCommands: true,
    useAppIdentity: false,
    perClientAppIdentity: false,
    gamepad: '',
    scaleFactor: 100,
    frameGenLimiterFix: false,
    exitTimeout: 5,
    prepCmd: [],
    stateCmd: [],
    detached: [],
    virtualScreen: false,
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
    rtxHdrMode: 'inherit',
    rtxHdrValuesOverride: false,
    rtxHdrForceSdr: false,
    rtxHdrPeakBrightness: 1000,
    rtxHdrMiddleGray: 50,
    rtxHdrContrast: 0,
    rtxHdrSaturation: 0,
    virtualDisplayMode: null,
    virtualDisplayLayout: null,
    ddConfigurationOption: null,
  };
}
const form = ref<AppForm>(fresh());
let formHydratingFromServer = false;
const overridesPickerOpen = ref(false);

const APP_VIRTUAL_DISPLAY_MODES: AppVirtualDisplayMode[] = ['disabled', 'per_client', 'shared'];
const APP_VIRTUAL_DISPLAY_LAYOUTS: AppVirtualDisplayLayout[] = [
  'exclusive',
  'extended',
  'extended_primary',
  'extended_isolated',
  'extended_primary_isolated',
];
const RTX_HDR_OVERRIDE_KEYS = [
  'rtx_hdr',
  'rtx_hdr_force_sdr',
  'rtx_hdr_peak_brightness',
  'rtx_hdr_middle_gray',
  'rtx_hdr_contrast',
  'rtx_hdr_saturation',
] as const;
const RTX_HDR_LIVE_KEYS = [
  'rtx_hdr',
  'rtx_hdr_peak_brightness',
  'rtx_hdr_middle_gray',
  'rtx_hdr_contrast',
  'rtx_hdr_saturation',
] as const;
const RTX_HDR_LIVE_DEBOUNCE_MS = 200;

function clonePlainRecord(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return {};
  }
  try {
    return JSON.parse(JSON.stringify(value));
  } catch {
    return { ...(value as Record<string, unknown>) };
  }
}

function parseBooleanOverride(value: unknown, fallback: boolean): boolean {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return value !== 0;
  const normalized = String(value ?? '')
    .toLowerCase()
    .trim();
  if (['true', '1', 'enabled', 'enable', 'yes', 'on'].includes(normalized)) return true;
  if (['false', '0', 'disabled', 'disable', 'no', 'off'].includes(normalized)) return false;
  return fallback;
}

function parseNumberOverride(value: unknown, fallback: number, min: number, max: number): number {
  const parsed = typeof value === 'number' ? value : Number(value);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.min(max, Math.max(min, Math.round(parsed)));
}

function extractRtxHdrOverrides(overrides: Record<string, unknown>) {
  const rest = { ...overrides };
  const hasRtxHdrOverride = RTX_HDR_OVERRIDE_KEYS.some((key) =>
    Object.prototype.hasOwnProperty.call(rest, key),
  );
  const hasRtxHdrValueOverride = [
    'rtx_hdr_peak_brightness',
    'rtx_hdr_middle_gray',
    'rtx_hdr_contrast',
    'rtx_hdr_saturation',
  ].some((key) => Object.prototype.hasOwnProperty.call(rest, key));
  const rawEnabled = rest['rtx_hdr'];
  let mode: RtxHdrMode = 'inherit';
  if (hasRtxHdrOverride) {
    mode = parseBooleanOverride(rawEnabled, true) ? 'enabled' : 'disabled';
  }
  for (const key of RTX_HDR_OVERRIDE_KEYS) {
    delete rest[key];
  }

  return {
    rest,
    mode,
    valuesOverride: hasRtxHdrValueOverride,
    forceSdr: parseBooleanOverride(overrides['rtx_hdr_force_sdr'], false),
    peakBrightness: parseNumberOverride(overrides['rtx_hdr_peak_brightness'], 1000, 400, 2000),
    middleGray: parseNumberOverride(overrides['rtx_hdr_middle_gray'], 50, 10, 100),
    contrast: parseNumberOverride(overrides['rtx_hdr_contrast'], 0, -100, 100),
    saturation: parseNumberOverride(overrides['rtx_hdr_saturation'], 0, -100, 100),
  };
}

function buildConfigOverridesPayload(f: AppForm): Record<string, unknown> {
  const overrides = clonePlainRecord(f.configOverrides);
  for (const key of RTX_HDR_OVERRIDE_KEYS) {
    delete overrides[key];
  }
  if (f.rtxHdrMode === 'enabled') {
    overrides['rtx_hdr'] = true;
    if (f.rtxHdrValuesOverride) {
      overrides['rtx_hdr_peak_brightness'] = f.rtxHdrPeakBrightness;
      overrides['rtx_hdr_middle_gray'] = f.rtxHdrMiddleGray;
      overrides['rtx_hdr_contrast'] = f.rtxHdrContrast;
      overrides['rtx_hdr_saturation'] = f.rtxHdrSaturation;
    }
  } else if (f.rtxHdrMode === 'disabled') {
    overrides['rtx_hdr'] = false;
  }
  return Object.fromEntries(
    Object.entries(overrides).filter(
      ([key, value]) =>
        typeof key === 'string' && key.length > 0 && value !== undefined && value !== null,
    ),
  );
}

function buildRtxHdrLiveOverridesPayload(f: AppForm): Record<string, unknown> {
  if (f.rtxHdrMode === 'inherit') {
    return {};
  }

  if (f.rtxHdrMode === 'disabled') {
    return { rtx_hdr: false };
  }

  const overrides: Record<string, unknown> = { rtx_hdr: true };
  if (f.rtxHdrValuesOverride) {
    overrides.rtx_hdr_peak_brightness = f.rtxHdrPeakBrightness;
    overrides.rtx_hdr_middle_gray = f.rtxHdrMiddleGray;
    overrides.rtx_hdr_contrast = f.rtxHdrContrast;
    overrides.rtx_hdr_saturation = f.rtxHdrSaturation;
  }
  return overrides;
}

function extractRtxHdrLiveOverrides(src?: ServerApp | null): Record<string, unknown> {
  const rawConfigOverrides = clonePlainRecord((src as any)?.['config-overrides']);
  const result: Record<string, unknown> = {};
  for (const key of RTX_HDR_LIVE_KEYS) {
    if (Object.prototype.hasOwnProperty.call(rawConfigOverrides, key)) {
      result[key] = rawConfigOverrides[key];
    }
  }
  return result;
}

function stableStringify(value: unknown): string {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return JSON.stringify(value);
  }
  const entries = Object.entries(value as Record<string, unknown>).sort(([a], [b]) =>
    a.localeCompare(b),
  );
  return JSON.stringify(Object.fromEntries(entries));
}

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

watch(
  () => form.value.useAppIdentity,
  (enabled) => {
    if (!enabled) {
      form.value.perClientAppIdentity = false;
    }
  },
);

watch(
  () => form.value.scaleFactor,
  (value) => {
    const clamped = clampScaleFactor(
      typeof value === 'number' && Number.isFinite(value) ? value : null,
    );
    if (clamped !== value) {
      form.value.scaleFactor = clamped;
    }
  },
);

function clampScaleFactor(value: number | null): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return 100;
  }
  const rounded = Math.round(value);
  return Math.min(SCALE_FACTOR_MAX, Math.max(SCALE_FACTOR_MIN, rounded));
}

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
  const state = Array.isArray(src['state-cmd'])
    ? src['state-cmd'].map((p) => ({
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
  const legacyLosslessFlag = !!src['lossless-scaling-framegen'];
  const lsTarget = parseNumeric(src['lossless-scaling-target-fps']);
  const lsLimit = parseNumeric(src['lossless-scaling-rtss-limit']);
  const lsLaunchDelayRaw = parseNumeric(src['lossless-scaling-launch-delay']);
  const lsLaunchDelay =
    lsLaunchDelayRaw && lsLaunchDelayRaw > 0 ? Math.round(lsLaunchDelayRaw) : null;
  const profileKey = parseLosslessProfileKey(src['lossless-scaling-profile']);
  const losslessProfiles = emptyLosslessProfileState();
  losslessProfiles.recommended = parseLosslessOverrides(src['lossless-scaling-recommended']);
  losslessProfiles.custom = parseLosslessOverrides(src['lossless-scaling-custom']);
  const frameGenerationModeFromConfig = parseFrameGenerationMode(
    (src as any)?.['frame-generation-mode'],
  );
  const useAppIdentity = !!src['use-app-identity'];
  const providerConfigured =
    typeof src['frame-generation-provider'] === 'string' &&
    src['frame-generation-provider'].trim().length > 0;
  const legacyLosslessFrameGenConfigured =
    legacyLosslessFlag || lsTarget !== null || lsLimit !== null;
  const normalizedProvider = providerConfigured
    ? normalizeFrameGenerationProvider(src['frame-generation-provider'])
    : legacyLosslessFrameGenConfigured
      ? 'lossless-scaling'
      : base.frameGenerationProvider;
  let frameGenerationMode: FrameGenerationMode = frameGenerationModeFromConfig ?? 'off';
  if (!frameGenerationModeFromConfig) {
    if (providerConfigured && normalizedProvider === 'nvidia-smooth-motion') {
      frameGenerationMode = 'nvidia-smooth-motion';
    } else if (normalizedProvider === 'lossless-scaling') {
      frameGenerationMode = legacyLosslessFrameGenConfigured ? 'lossless-scaling' : 'off';
    } else if (providerConfigured && normalizedProvider === 'game-provided') {
      frameGenerationMode = 'game-provided';
    }
  }
  const hasExplicitLosslessEnabled = Object.prototype.hasOwnProperty.call(
    src,
    'lossless-scaling-enabled',
  );
  const lsEnabled =
    typeof src['lossless-scaling-enabled'] === 'boolean'
      ? src['lossless-scaling-enabled']
      : !hasExplicitLosslessEnabled &&
        frameGenerationMode !== 'lossless-scaling' &&
        legacyLosslessFlag;
  const frameGenerationProvider =
    frameGenerationModeFromConfig && frameGenerationModeFromConfig !== 'off'
      ? (frameGenerationModeFromConfig as FrameGenerationProvider)
      : normalizedProvider;
  const hasDisplayOutput = Object.prototype.hasOwnProperty.call(src, 'display-output');
  const rawOutput = String(hasDisplayOutput ? ((src as any)['display-output'] ?? '') : (src.output ?? ''));
  const rawVirtualScreen = src['virtual-screen'];
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
  const rawConfigOverrides = clonePlainRecord((src as any)?.['config-overrides']);
  const rtxHdrOverrides = extractRtxHdrOverrides(rawConfigOverrides);
  return {
    index: idx,
    uuid: typeof src.uuid === 'string' ? src.uuid : undefined,
    name: String(src.name ?? ''),
    output: rawOutput,
    cmd: String(cmdStr ?? ''),
    workingDir: String(src['working-dir'] ?? ''),
    imagePath: String(src['image-path'] ?? ''),
    playniteIconPath: String(src['playnite-icon-path'] ?? ''),
    excludeGlobalPrepCmd: !!src['exclude-global-prep-cmd'],
    excludeGlobalStateCmd: !!src['exclude-global-state-cmd'],
    configOverrides: rtxHdrOverrides.rest,
    elevated: !!src.elevated,
    autoDetach: src['auto-detach'] !== undefined ? !!src['auto-detach'] : base.autoDetach,
    waitAll: src['wait-all'] !== undefined ? !!src['wait-all'] : base.waitAll,
    terminateOnPause:
      src['terminate-on-pause'] !== undefined ? !!src['terminate-on-pause'] : base.terminateOnPause,
    allowClientCommands:
      src['allow-client-commands'] !== undefined
        ? !!src['allow-client-commands']
        : base.allowClientCommands,
    useAppIdentity: useAppIdentity,
    perClientAppIdentity:
      useAppIdentity && src['per-client-app-identity'] !== undefined
        ? !!src['per-client-app-identity']
        : base.perClientAppIdentity,
    gamepad: typeof src.gamepad === 'string' ? src.gamepad : '',
    scaleFactor: clampScaleFactor(parseNumeric(src['scale-factor'])),
    frameGenLimiterFix:
      src['frame-gen-limiter-fix'] !== undefined
        ? !!src['frame-gen-limiter-fix']
        : base.frameGenLimiterFix,
    exitTimeout: derivedExitTimeout,
    prepCmd: prep,
    stateCmd: state,
    detached: Array.isArray(src.detached) ? src.detached.map((s) => String(s)) : [],
    virtualScreen,
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
    rtxHdrMode: rtxHdrOverrides.mode,
    rtxHdrValuesOverride: rtxHdrOverrides.valuesOverride,
    rtxHdrForceSdr: rtxHdrOverrides.forceSdr,
    rtxHdrPeakBrightness: rtxHdrOverrides.peakBrightness,
    rtxHdrMiddleGray: rtxHdrOverrides.middleGray,
    rtxHdrContrast: rtxHdrOverrides.contrast,
    rtxHdrSaturation: rtxHdrOverrides.saturation,
    virtualDisplayMode: serverVirtualDisplayMode ?? (hasDisplayOutput ? 'disabled' : null),
    virtualDisplayLayout: serverVirtualDisplayLayout,
    ddConfigurationOption: ddConfigValue,
  };
}

function toServerPayload(f: AppForm): Record<string, any> {
  const configOverridesPayload = buildConfigOverridesPayload(f);
  const selection = displaySelection.value;
  const payload: Record<string, any> = {
    // Index is required by the backend to determine add (-1) vs update (>= 0)
    index: typeof f.index === 'number' ? f.index : -1,
    name: f.name,
    cmd: f.cmd,
    'working-dir': f.workingDir,
    'image-path': String(f.imagePath || '').replace(/\"/g, ''),
    'exclude-global-prep-cmd': !!f.excludeGlobalPrepCmd,
    'exclude-global-state-cmd': !!f.excludeGlobalStateCmd,
    ...(Object.keys(configOverridesPayload).length
      ? { 'config-overrides': configOverridesPayload }
      : {}),
    elevated: !!f.elevated,
    'auto-detach': !!f.autoDetach,
    'wait-all': !!f.waitAll,
    'terminate-on-pause': !!f.terminateOnPause,
    'allow-client-commands': !!f.allowClientCommands,
    'use-app-identity': !!f.useAppIdentity,
    'per-client-app-identity': f.useAppIdentity ? !!f.perClientAppIdentity : false,
    gamepad: String(f.gamepad || ''),
    'scale-factor': clampScaleFactor(
      typeof f.scaleFactor === 'number' && Number.isFinite(f.scaleFactor) ? f.scaleFactor : null,
    ),
    'gen1-framegen-fix': false,
    'gen2-framegen-fix': false,
    'exit-timeout': Number.isFinite(f.exitTimeout) ? f.exitTimeout : 5,
    'prep-cmd': f.prepCmd.map((p) => ({
      do: p.do,
      undo: p.undo,
      ...(isWindows.value ? { elevated: !!p.elevated } : {}),
    })),
    'state-cmd': f.stateCmd.map((p) => ({
      do: p.do,
      undo: p.undo,
      ...(isWindows.value ? { elevated: !!p.elevated } : {}),
    })),
    detached: Array.isArray(f.detached) ? f.detached : [],
    // Leave 'virtual-screen' to be persisted only if explicitly different from the global setting.
  };
  
  // Include uuid to enable backend UUID-matching for updates
  if (f.uuid) {
    payload['uuid'] = f.uuid;
  }
  
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
  if (f.playniteIconPath) payload['playnite-icon-path'] = f.playniteIconPath;
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
    resolvedProvider = 'lossless-scaling';
  }
  payload['frame-generation-provider'] = resolvedProvider;
  payload['frame-generation-mode'] = mode;
  const payloadLosslessTarget = parseNumeric(f.losslessScalingTargetFps);
  const payloadLosslessLimit = parseNumeric(f.losslessScalingRtssLimit);
  const losslessFramegenActive = mode === 'lossless-scaling';
  const losslessRuntimeActive = !!f.losslessScalingEnabled || losslessFramegenActive;
  payload['lossless-scaling-enabled'] = !!f.losslessScalingEnabled;
  payload['lossless-scaling-framegen'] = losslessFramegenActive;
  payload['lossless-scaling-target-fps'] = losslessFramegenActive ? payloadLosslessTarget : null;
  payload['lossless-scaling-rtss-limit'] = losslessFramegenActive ? payloadLosslessLimit : null;
  const payloadLosslessDelayRaw = parseNumeric(f.losslessScalingLaunchDelay);
  const payloadLosslessDelay =
    payloadLosslessDelayRaw && payloadLosslessDelayRaw > 0
      ? Math.round(payloadLosslessDelayRaw)
      : null;
  payload['lossless-scaling-launch-delay'] = losslessRuntimeActive ? payloadLosslessDelay : null;
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
  // Physical display overrides use their own field so an empty string can explicitly mean
  // "primary display" instead of "no app override".
  if (selection === 'physical') {
    payload['display-output'] = typeof f.output === 'string' ? f.output.trim() : '';
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
let liveRtxHdrSuppress = false;
// Normalize cmd to single string; rehydrate typed form when props.app changes while open
watch(
  () => props.app,
  (val) => {
    if (!open.value) return;
    liveRtxHdrSuppress = true;
    formHydratingFromServer = true;
    form.value = fromServerApp(val as ServerApp | undefined, props.index ?? -1);
    primeLiveRtxHdrState();
    nextTick(() => {
      liveRtxHdrSuppress = false;
      formHydratingFromServer = false;
    }).catch(() => {
      liveRtxHdrSuppress = false;
      formHydratingFromServer = false;
    });
  },
  { immediate: true },
);
const cmdText = computed<string>({
  get: () => form.value.cmd || '',
  set: (v: string) => {
    form.value.cmd = v;
  },
});
const scaleFactorModel = computed<number>({
  get: () => form.value.scaleFactor,
  set: (v: number) => {
    form.value.scaleFactor = clampScaleFactor(
      typeof v === 'number' && Number.isFinite(v) ? v : null,
    );
  },
});
const isPlayniteManaged = computed<boolean>(() => !!form.value.playniteId);
const isPlayniteAuto = computed<boolean>(
  () => isPlayniteManaged.value && form.value.playniteManaged !== 'manual',
);
const headerArtworkFailed = ref(false);
const headerArtworkKey = computed(() => {
  const identity = String(
    (props.app as ServerApp | null | undefined)?.uuid ||
      form.value.playniteId ||
      form.value.name ||
      '',
  );
  const appAny = props.app as any;
  return `${identity}|${form.value.playniteIconPath || ''}|${appAny?.['playnite-icon-version'] || ''}`;
});
const headerArtworkUrl = computed(() => {
  const uuid = String((props.app as ServerApp | null | undefined)?.uuid || '');
  if (!uuid) return '';
  const appAny = props.app as any;
  const version = appAny?.['playnite-icon-version'];
  const base = `/api/apps/${encodeURIComponent(uuid)}/icon`;
  return version ? `${base}?v=${version}` : base;
});
const hasHeaderArtwork = computed(
  () =>
    isPlayniteManaged.value &&
    !!headerArtworkUrl.value &&
    !!form.value.playniteIconPath &&
    !headerArtworkFailed.value,
);
watch(headerArtworkKey, () => {
  headerArtworkFailed.value = false;
});

const originalRtxHdrLiveOverrides = ref<Record<string, unknown>>({});
type RtxHdrLiveStatus = 'idle' | 'queued' | 'applying' | 'applied' | 'error';
const liveRtxHdrStatus = ref<RtxHdrLiveStatus>('idle');
const liveRtxHdrError = ref('');
let liveRtxHdrTimer: ReturnType<typeof setTimeout> | null = null;
let liveRtxHdrLastSentKey = '';
let liveRtxHdrQueue: Promise<void> = Promise.resolve();
let liveRtxHdrProgrammaticClose = false;

function activeAppUuid(): string {
  return String((props.app as ServerApp | null | undefined)?.uuid || '');
}

function currentRtxHdrLiveKey(): string {
  return stableStringify(buildRtxHdrLiveOverridesPayload(form.value));
}

function clearLiveRtxHdrTimer() {
  if (liveRtxHdrTimer) {
    clearTimeout(liveRtxHdrTimer);
    liveRtxHdrTimer = null;
  }
}

function primeLiveRtxHdrState() {
  originalRtxHdrLiveOverrides.value = extractRtxHdrLiveOverrides(props.app ?? undefined);
  liveRtxHdrLastSentKey = currentRtxHdrLiveKey();
  liveRtxHdrStatus.value = 'idle';
  liveRtxHdrError.value = '';
  clearLiveRtxHdrTimer();
}

async function postRtxHdrLiveOverrides(
  overrides: Record<string, unknown>,
  key: string,
): Promise<void> {
  const uuid = activeAppUuid();
  if (!uuid) {
    return;
  }

  liveRtxHdrStatus.value = 'applying';
  liveRtxHdrError.value = '';
  const response = await http.post(
    './api/apps/rtx_hdr/live',
    {
      uuid,
      'config-overrides': overrides,
    },
    {
      headers: { 'Content-Type': 'application/json' },
      validateStatus: () => true,
    },
  );
  const okStatus = response.status >= 200 && response.status < 300;
  const responseData = response?.data as any;
  if (!okStatus || (responseData && responseData.status === false)) {
    const errorMessage =
      typeof responseData?.error === 'string'
        ? responseData.error
        : `Live RTX HDR update failed with HTTP ${response.status}.`;
    liveRtxHdrStatus.value = 'error';
    liveRtxHdrError.value = errorMessage;
    throw new Error(errorMessage);
  }
  liveRtxHdrLastSentKey = key;
  liveRtxHdrStatus.value = responseData?.applied === false ? 'idle' : 'applied';
}

function enqueueRtxHdrLivePost(overrides: Record<string, unknown>, key: string): Promise<void> {
  liveRtxHdrQueue = liveRtxHdrQueue
    .catch(() => {})
    .then(() => postRtxHdrLiveOverrides(overrides, key))
    .catch((error) => {
      liveRtxHdrStatus.value = 'error';
      liveRtxHdrError.value =
        error instanceof Error && error.message
          ? error.message
          : t('apps.rtx_hdr_live_update_failed');
    });
  return liveRtxHdrQueue;
}

function scheduleRtxHdrLiveUpdate() {
  if (liveRtxHdrSuppress || !open.value || form.value.index === -1 || !activeAppUuid()) {
    return;
  }

  const key = currentRtxHdrLiveKey();
  if (key === liveRtxHdrLastSentKey) {
    clearLiveRtxHdrTimer();
    if (liveRtxHdrStatus.value === 'queued') {
      liveRtxHdrStatus.value = 'idle';
    }
    return;
  }

  clearLiveRtxHdrTimer();
  liveRtxHdrStatus.value = 'queued';
  liveRtxHdrError.value = '';
  liveRtxHdrTimer = setTimeout(() => {
    liveRtxHdrTimer = null;
    const overrides = buildRtxHdrLiveOverridesPayload(form.value);
    void enqueueRtxHdrLivePost(overrides, stableStringify(overrides));
  }, RTX_HDR_LIVE_DEBOUNCE_MS);
}

async function restoreOriginalRtxHdrLiveOverrides() {
  if (form.value.index === -1 || !activeAppUuid()) {
    return;
  }

  clearLiveRtxHdrTimer();
  await liveRtxHdrQueue.catch(() => {});
  const original = clonePlainRecord(originalRtxHdrLiveOverrides.value);
  const originalKey = stableStringify(original);
  if (originalKey === liveRtxHdrLastSentKey) {
    return;
  }
  await enqueueRtxHdrLivePost(original, originalKey);
}

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
    if (formHydratingFromServer) {
      return;
    }
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
  if (cur)
    list.push({
      label: t('apps.source_custom_named', { name: cur }),
      value: `__custom__:${cur}`,
    });
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
      { label: t('apps.loading'), value: '__loading__', disabled: true } as any,
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
    opts.push({
      label: t('apps.source_custom_named', { name: currentName }),
      value: `__custom__:${currentName}`,
    });
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

async function close(options: { rollbackLiveRtxHdr?: boolean } = {}) {
  const rollbackLiveRtxHdr = options.rollbackLiveRtxHdr !== false;
  if (rollbackLiveRtxHdr) {
    await restoreOriginalRtxHdrLiveOverrides();
  } else {
    clearLiveRtxHdrTimer();
  }
  liveRtxHdrProgrammaticClose = true;
  emit('update:modelValue', false);
}

function handleModalShowUpdate(visible: boolean) {
  if (visible) {
    liveRtxHdrProgrammaticClose = false;
    emit('update:modelValue', true);
    return;
  }
  if (liveRtxHdrProgrammaticClose) {
    return;
  }
  void close();
}

function addPrep() {
  form.value.prepCmd.push({
    do: '',
    undo: '',
    ...(isWindows.value ? { elevated: false } : {}),
  });
  requestAnimationFrame(() => updateShadows());
}

function addState() {
  form.value.stateCmd.push({
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
const platformName = computed(() => (configStore.metadata?.platform || '').toLowerCase());
const isWindows = computed(() => platformName.value === 'windows');
const isLinux = computed(() => platformName.value === 'linux');
const isMac = computed(() => platformName.value === 'macos');
const gamepadOptions = computed(() => {
  const options = [
    { label: 'Default (Global)', value: '' },
    { label: 'Disabled', value: 'disabled' },
    { label: 'Auto', value: 'auto' },
  ];
  if (isLinux.value) {
    options.push(
      { label: 'DualSense (PS5)', value: 'ds5' },
      { label: 'Switch Pro', value: 'switch' },
      { label: 'Xbox One', value: 'xone' },
    );
  }
  if (isWindows.value) {
    options.push({ label: 'DualShock 4', value: 'ds4' }, { label: 'Xbox 360', value: 'x360' });
  }
  return options;
});
const gpuList = computed(() => {
  const raw = (configStore.metadata as any)?.gpus;
  return Array.isArray(raw) ? raw : [];
});
const hasNvidia = computed(() => {
  const metaFlag = (configStore.metadata as any)?.has_nvidia_gpu;
  if (typeof metaFlag === 'boolean') return metaFlag;
  if (gpuList.value.length) {
    return gpuList.value.some(
      (gpu: any) => Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0) === 0x10de,
    );
  }
  return true;
});
const captureMethod = computed(() => (configStore.config as any)?.capture ?? '');
const globalOutputName = computed(() => {
  const name = (configStore.config as any)?.output_name;
  return typeof name === 'string' ? name : '';
});
const globalVirtualDisplayMode = computed<AppVirtualDisplayMode>(() => {
  const mode = (configStore.config as any)?.virtual_display_mode;
  return parseAppVirtualDisplayMode(mode) ?? 'per_client';
});
const globalVirtualDisplayLayout = computed<AppVirtualDisplayLayout>(() => {
  const layout = (configStore.config as any)?.virtual_display_layout;
  return parseAppVirtualDisplayLayout(layout) ?? 'exclusive';
});
const resolvedVirtualDisplayLayout = computed<AppVirtualDisplayLayout>(
  () => form.value.virtualDisplayLayout ?? globalVirtualDisplayLayout.value,
);
const APP_VIRTUAL_DISPLAY_MODE_LABEL_KEYS: Record<AppVirtualDisplayMode, string> = {
  disabled: 'config.virtual_display_mode_disabled',
  per_client: 'config.virtual_display_mode_per_client',
  shared: 'config.virtual_display_mode_shared',
};
const appVirtualDisplayModeOptions = computed(() =>
  (['global', ...APP_VIRTUAL_DISPLAY_MODES.filter((value) => value !== 'disabled')] as const).map(
    (value) => ({
      value,
      label:
        value === 'global'
          ? t('config.app_virtual_display_mode_follow_global')
          : t(APP_VIRTUAL_DISPLAY_MODE_LABEL_KEYS[value]),
    }),
  ),
);
const appVirtualDisplayModeSelection = computed<AppVirtualDisplayModeSelection>({
  get: () => form.value.virtualDisplayMode ?? 'global',
  set: (value) => {
    form.value.virtualDisplayMode = value === 'global' ? null : value;
  },
});
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
      if (form.value.output === VIRTUAL_DISPLAY_SELECTION) {
        form.value.output = '';
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
// Windows 11 shipped as build 22000; anything below that on Windows is Windows 10.
const isWindows10 = computed(() => {
  if (!isWindows.value) return false;
  const build = windowsBuildNumber.value;
  return build !== null && build < 22000;
});
const usingVirtualDisplay = computed(() => {
  return resolvesToVirtualDisplay({
    displaySelection: displaySelection.value,
    appVirtualDisplayMode: form.value.virtualDisplayMode,
    globalVirtualDisplayMode: globalVirtualDisplayMode.value,
    globalOutputName: globalOutputName.value,
  });
});
const displayDevices = ref<DisplayDevice[]>([]);
const displayDevicesLoading = ref(false);
const displayDevicesError = ref('');
const displayNameCache = ref<Record<string, string>>({});
const physicalOutputModel = computed<string | null>({
  get: () => {
    const value = typeof form.value.output === 'string' ? form.value.output.trim() : '';
    return displaySelection.value === 'physical' ? value : null;
  },
  set: (value) => {
    if (value === null || value === undefined) {
      displaySelection.value = 'global';
      displayOverrideEnabled.value = false;
      return;
    }
    const normalized = typeof value === 'string' ? value.trim() : '';
    form.value.output = normalized;
    form.value.virtualScreen = false;
    if (normalized) {
      lastPhysicalOutput.value = normalized;
    }
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
    displayDevicesError.value = e?.message || t('apps.framegen.health_devices_error');
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
    active?: boolean | null;
  }> = [
    {
      label: t('config.output_name_default') as string,
      value: '',
      displayName: t('config.output_name_default') as string,
      id: '',
      active: null,
    },
  ];
  const seen = new Set<string>(['']);
  for (const d of displayDevices.value) {
    const value = d.device_id || d.display_name || '';
    if (!value || seen.has(value)) continue;
    const displayName = d.friendly_name || d.display_name || t('config.display_fallback');
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
    liveRtxHdrProgrammaticClose = false;
    liveRtxHdrSuppress = true;
    formHydratingFromServer = true;
    form.value = fromServerApp(props.app ?? undefined, props.index ?? -1);
    primeLiveRtxHdrState();
    nextTick(() => {
      liveRtxHdrSuppress = false;
      formHydratingFromServer = false;
    }).catch(() => {
      liveRtxHdrSuppress = false;
      formHydratingFromServer = false;
    });
    selectedPlayniteId.value = '';
    lockPlaynite.value = false;
    newAppSource.value = 'custom';
    refreshPlayniteStatus().then(() => {
      if (playniteInstalled.value) void loadPlayniteGames();
    });
    requestAnimationFrame(() => updateShadows());
    ensureNameSelectionFromForm();
    // Frame-gen health is only meaningful after an explicit check; start clean on open.
    frameGenHealth.value = null;
    frameGenHealthError.value = null;
    if (isWindows.value) {
      refreshLosslessExecutableStatus().catch(() => {});
      if (displaySelection.value === 'physical' && displayDevices.value.length === 0) {
        loadDisplayDevices().catch(() => {});
      }
    }
  } else {
    clearLiveRtxHdrTimer();
    overridesPickerOpen.value = false;
    frameGenHealth.value = null;
    frameGenHealthError.value = null;
  }
});

watch(
  () => [
    form.value.rtxHdrMode,
    form.value.rtxHdrValuesOverride,
    form.value.rtxHdrPeakBrightness,
    form.value.rtxHdrMiddleGray,
    form.value.rtxHdrContrast,
    form.value.rtxHdrSaturation,
  ],
  () => scheduleRtxHdrLiveUpdate(),
);

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

      const usingVirtual = usingVirtualDisplay.value;
      const captureValue = (captureMethod.value || '').toString().toLowerCase();
      let captureStatus: FrameGenHealth['capture']['status'];
      let captureMessage: string;
      const autoTreatsAsWgc = captureValue === '' && autoCaptureUsesWgc.value;
      if (captureValue === 'wgc' || captureValue === 'wgcc' || autoTreatsAsWgc) {
        captureStatus = 'pass';
        captureMessage = autoTreatsAsWgc
          ? t('apps.framegen.health_capture_wgc_auto')
          : t('apps.framegen.health_capture_wgc_active');
      } else if (captureValue === '') {
        captureStatus = 'warn';
        captureMessage = t('apps.framegen.health_capture_autodetect');
      } else {
        captureStatus = 'fail';
        captureMessage = t('apps.framegen.health_capture_switch');
      }
      let rtssInstalled = false;
      let rtssHooks = false;
      let rtssRunning = false;
      let rtssStatus: FrameGenHealth['rtss']['status'] = 'unknown';
      let rtssMessage = t('apps.framegen.health_rtss_unverified');
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
            rtssMessage = t('apps.framegen.health_rtss_hooks');
          } else if (rtssInstalled) {
            rtssStatus = 'warn';
            rtssMessage = t('apps.framegen.health_rtss_no_hooks');
          } else {
            rtssStatus = 'fail';
            rtssMessage = t('apps.framegen.health_rtss_install');
          }
        } else {
          rtssStatus = 'unknown';
          rtssMessage = t('apps.framegen.health_rtss_endpoint_error');
        }
      } else {
        rtssStatus = 'unknown';
        rtssMessage = t('apps.framegen.health_rtss_unreachable');
      }

      const physicalGameProvidedFrameGen =
        !usingVirtual && form.value.frameGenerationMode === 'game-provided';
      if (physicalGameProvidedFrameGen) {
        rtssStatus = 'warn';
        rtssMessage = rtssInstalled
          ? t('apps.framegen.health_physical_rtss_latency')
          : t('apps.framegen.health_physical_no_rtss');
        captureStatus = 'warn';
        captureMessage = t(physicalFrameGenDisplayWarningKey());
      } else if (!usingVirtual) {
        captureStatus = 'pass';
        captureMessage = t('apps.framegen.health_physical_supported');
      }

      const fpsTargets = [60, 90, 120, 144];
      const tolerance = 0.5;
      let displayStatus: FrameGenHealth['display']['status'] = 'unknown';
      let displayMessage = t('apps.framegen.health_display_unknown');
      let displayLabel = usingVirtual
        ? t('apps.framegen.health_display_virtual_label')
        : t('apps.framegen.health_display_active_label');
      let displayId = usingVirtual ? VIRTUAL_DISPLAY_SELECTION : '';
      let displayHz: number | null = null;
      let displayError: string | null = null;
      let displayTargets = fpsTargets.map((fps) => ({
        fps,
        requiredHz: usingVirtual ? fps * 4 : fps * 2,
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
                t('apps.framegen.health_display_active_label');
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
                  edidFetchError = e?.message || t('apps.framegen.health_edid_refresh_failed');
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
                displayMessage = t('apps.framegen.health_display_refresh_unreadable');
              } else if (evaluationHz >= 240 - tolerance) {
                displayStatus = 'pass';
                if (only144Fails) {
                  const baseHz = hasActive ? (activeRefresh ?? evaluationHz) : evaluationHz;
                  displayMessage = t('apps.framegen.health_display_120_only_current', {
                    hz: Math.round(baseHz),
                  });
                  if (!hasActive && highestSupported !== null) {
                    displayMessage = t('apps.framegen.health_display_120_only_supported', {
                      hz: Math.round(highestSupported),
                    });
                  } else if (deltaSupported && highestSupported !== null) {
                    displayMessage += ` ${t('apps.framegen.health_display_switch_when_stream_starts', {
                      hz: Math.round(highestSupported),
                    })}`;
                  }
                } else if (!hasActive && highestSupported !== null) {
                  displayMessage = t('apps.framegen.health_display_double_120', {
                    hz: Math.round(highestSupported),
                  });
                } else if (deltaSupported && highestSupported !== null) {
                  displayMessage = t('apps.framegen.health_display_switch_smooth', {
                    currentHz: Math.round(activeRefresh ?? evaluationHz),
                    targetHz: Math.round(highestSupported),
                  });
                } else {
                  displayMessage = t('apps.framegen.health_display_ok_120');
                }
              } else if (evaluationHz >= 180 - tolerance) {
                displayStatus = 'warn';
                if (!hasActive && highestSupported !== null) {
                  displayMessage = t('apps.framegen.health_display_enforce_or_virtual', {
                    hz: Math.round(evaluationHz),
                  });
                } else if (hasActive) {
                  if (highestFailUnder144 !== null) {
                    displayMessage = t('apps.framegen.health_display_target_needs_virtual', {
                      hz: Math.round(activeRefresh ?? evaluationHz),
                      fps: highestFailUnder144,
                    });
                  } else {
                    displayMessage = t('apps.framegen.health_display_120_stutter', {
                      hz: Math.round(activeRefresh ?? evaluationHz),
                    });
                  }
                  if (deltaSupported && highestSupported !== null) {
                    displayMessage += ` ${t('apps.framegen.health_display_switch_if_active', {
                      hz: Math.round(highestSupported),
                    })}`;
                  }
                } else {
                  displayMessage = t('apps.framegen.health_display_maybe_low');
                }
              } else {
                displayStatus = 'fail';
                if (!hasActive && highestSupported !== null) {
                  displayMessage = t('apps.framegen.health_display_tops_out', {
                    hz: Math.round(evaluationHz),
                  });
                } else if (hasActive) {
                  const mention = highestFailUnder144 ?? 120;
                  displayMessage = t('apps.framegen.health_display_target_needs_virtual', {
                    hz: Math.round(activeRefresh ?? evaluationHz),
                    fps: mention,
                  });
                  if (deltaSupported && highestSupported !== null) {
                    displayMessage += ` ${t('apps.framegen.health_display_switch_if_configured', {
                      hz: Math.round(highestSupported),
                    })}`;
                  }
                } else {
                  displayMessage = t('apps.framegen.health_display_unavailable_240');
                }
              }
            } else {
              displayStatus = 'unknown';
              displayMessage = t('apps.framegen.health_display_no_devices');
              displayError = t('apps.framegen.health_display_no_devices_error');
            }
          } else {
            displayStatus = 'unknown';
            displayMessage = t('apps.framegen.health_display_helper_silent');
            displayError = t('apps.framegen.health_display_enum_failed');
          }
        } else {
          displayStatus = 'unknown';
          displayMessage = t('apps.framegen.health_display_helper_unreachable');
          displayError = t('apps.framegen.health_display_helper_failed');
        }
      } else {
        displayStatus = 'pass';
        displayMessage = t(frameGenDisplayHealthKey(true, form.value.frameGenerationMode));
      }

      if (usingVirtual) {
        displayTargets = fpsTargets.map((fps) => ({
          fps,
          requiredHz: fps * 4,
          supported: true,
        }));
      }

      const osBuild = windowsBuildNumber.value;
      const losslessSelected = form.value.frameGenerationMode === 'lossless-scaling';
      let osStatus: FrameGenHealth['os']['status'];
      let osMessage: string;
      if (osBuild === null) {
        osStatus = 'unknown';
        osMessage = t('apps.framegen.health_os_unknown');
      } else if (osBuild >= 22000) {
        osStatus = 'pass';
        osMessage = t('apps.framegen.health_os_win11');
      } else if (losslessSelected) {
        osStatus = 'fail';
        osMessage = t('apps.framegen.health_os_win10_lossless');
      } else {
        osStatus = 'warn';
        osMessage = t('apps.framegen.health_os_win10');
      }

      const health: FrameGenHealth = {
        checkedAt: Date.now(),
        os: {
          status: osStatus,
          buildNumber: osBuild,
          message: osMessage,
        },
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
          message:
            form.value.frameGenerationMode === 'game-provided'
              ? t('apps.framegen.health_suggestion_virtual')
              : t('apps.framegen.health_suggestion_target', { fps: highestFailUnder144 }),
          emphasis: 'warning',
        };
      } else if (captureStatus === 'warn' || captureStatus === 'fail') {
        health.suggestion = {
          message:
            form.value.frameGenerationMode === 'game-provided'
              ? t('apps.framegen.health_suggestion_virtual')
              : t('apps.framegen.health_suggestion_capture'),
          emphasis: 'info',
        };
      }

      frameGenHealth.value = health;
      frameGenHealthError.value = null;
    } catch (error) {
      frameGenHealth.value = null;
      frameGenHealthError.value =
        error instanceof Error ? error.message : t('apps.framegen.health_run_error');
      if (!options.silent) {
        message?.error(t('apps.framegen.health_run_error'));
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

const playniteInstalled = ref(false);
const APP_UUID_RE = /^[A-Fa-f0-9]{8}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{12}$/;
const isNew = computed(() => !form.value.uuid && form.value.index < 0);
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

function deleteTargetForForm(f: AppForm): string {
  const uuid = String(f.uuid || '').trim();
  if (APP_UUID_RE.test(uuid)) {
    return uuid;
  }
  if (Number.isInteger(f.index) && f.index >= 0) {
    return String(f.index);
  }
  return '';
}

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
      const data = r.data as any;
      playniteInstalled.value = data.installed === true || data.active === true;
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
watch(
  () => displaySelection.value,
  (selection, prev) => {
    if (!isWindows.value) return;
    if (!frameGenHealth.value) return;
    if (selection === prev) return;
    const reason: FrameGenHealthReason =
      selection === 'virtual' || prev === 'virtual' ? 'virtual-toggle' : 'output-change';
    refreshFrameGenHealth({ reason, silent: true }).catch(() => {});
  },
);

watch(
  () => usingVirtualDisplay.value,
  (usesVirtual, previous) => {
    if (!isWindows.value) return;
    if (open.value && (previous !== usesVirtual || frameGenHealth.value)) {
      refreshFrameGenHealth({ reason: 'virtual-toggle', silent: true }).catch(() => {});
    }
  },
  { immediate: true },
);

watch(
  () => captureMethod.value,
  () => {
    if (!isWindows.value) return;
    if (!frameGenHealth.value) return;
    refreshFrameGenHealth({ reason: 'capture-change', silent: true }).catch(() => {});
  },
);

watch(
  () => autoCaptureUsesWgc.value,
  (enabled, prev) => {
    if (enabled === prev) return;
    if (!isWindows.value) return;
    if (!frameGenHealth.value) return;
    refreshFrameGenHealth({ reason: 'capture-change', silent: true }).catch(() => {});
  },
);

watch(
  () => [form.value.output, globalOutputName.value],
  () => {
    if (!isWindows.value) return;
    if (!frameGenHealth.value) return;
    refreshFrameGenHealth({ reason: 'output-change', silent: true }).catch(() => {});
  },
);

// Re-run the frame-generation health check whenever frame generation is turned on.
watch(
  () => frameGenerationSelection.value,
  (mode) => {
    if (!isWindows.value) return;
    if (formHydratingFromServer) return;
    if (mode === 'off') return;
    refreshFrameGenHealth({ reason: 'auto', silent: true }).catch(() => {});
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
    list.push({ label: t('apps.source_custom_named', { name: q }), value: `__custom__:${q}` });
  } else {
    const cur = String(form.value.name || '').trim();
    if (cur)
      list.push({ label: t('apps.source_custom_named', { name: cur }), value: `__custom__:${cur}` });
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

// Cover preview logic removed; Vibepollo no longer fetches or proxies images
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
          ? String(responseData.error ?? t('validation.save_failed'))
          : t('validation.save_failed');
      message?.error(errMessage);
      return;
    }
    emit('saved');
    await close({ rollbackLiveRtxHdr: false });
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

    const target = deleteTargetForForm(form.value);
    if (!target) {
      message?.error(t('apps.delete_invalid_target'));
      return;
    }

    const r = await http.delete(`./api/apps/${encodeURIComponent(target)}`, {
      validateStatus: () => true,
    });
    const responseData = r?.data as any;
    const ok = r.status >= 200 && r.status < 300 && responseData?.status === true;
    if (!ok) {
      const errMessage =
        responseData && typeof responseData === 'object' && 'error' in responseData
          ? String(responseData.error ?? t('apps.delete_failed'))
          : t('apps.delete_failed_http', { status: r.status });
      message?.error(errMessage);
      return;
    }
    try {
      if (responseData?.playniteFullscreenDisabled) {
        try {
          configStore.updateOption('playnite_fullscreen_entry_enabled', false);
        } catch {}
        try {
          message?.info(
            t('playnite.fullscreen_entry_removed'),
          );
        } catch {}
      }
    } catch {}
    // Best-effort force sync on Windows environments
    try {
      await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    } catch (_) {}
    emit('deleted');
    await close();
  } finally {
    saving.value = false;
  }
}
</script>
<style scoped>
.mobile-only-hidden {
  display: none;
}

.app-modal-icon {
  width: 3.5rem;
  height: 3.5rem;
  flex: 0 0 3.5rem;
  border-radius: 9999px;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  background: linear-gradient(
    135deg,
    rgb(var(--color-primary) / 0.2),
    rgb(var(--color-primary) / 0.1)
  );
  color: rgb(var(--color-primary));
  box-shadow: inset 0 0 0 1px rgb(var(--color-primary) / 0.14);
}

.app-modal-icon--playnite {
  border-radius: 0.45rem;
  background: rgb(var(--color-dark) / 0.08);
  box-shadow: inset 0 0 0 1px rgb(var(--color-dark) / 0.08);
}

.dark .app-modal-icon--playnite {
  background: rgb(var(--color-light) / 0.08);
  box-shadow: inset 0 0 0 1px rgb(var(--color-light) / 0.08);
}

.app-modal-icon__image {
  width: 100%;
  height: 100%;
  display: block;
  object-fit: contain;
  padding: 0.25rem;
  /* High-quality smooth scaling; avoid forcing a GPU layer, which can soften the image. */
  image-rendering: auto;
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
