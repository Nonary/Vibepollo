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
              v-if="isPlaynite"
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
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <div class="space-y-1 md:col-span-2">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Name</label>
              <!-- Unified combobox: type any name; suggestions from Playnite if available -->
              <div class="flex items-center gap-2 mb-1">
                <n-select
                  v-model:value="nameSelectValue"
                  :options="nameSelectOptions"
                  :loading="gamesLoading"
                  filterable
                  clearable
                  :placeholder="'Type to search or enter a custom name'"
                  class="flex-1"
                  :fallback-option="fallbackOption"
                  @focus="onNameFocus"
                  @search="onNameSearch"
                  @update:value="onNamePicked"
                />
              </div>
              <!-- When adding a new app on Windows, allow picking a Playnite game (disabled if plugin not installed) -->
              <template v-if="isNew && isWindows && newAppSource === 'playnite'">
                <div class="flex items-center gap-2">
                  <n-select
                    v-model:value="selectedPlayniteId"
                    :options="playniteOptions"
                    :loading="gamesLoading"
                    filterable
                    :disabled="lockPlaynite || !playniteInstalled"
                    :placeholder="
                      playniteInstalled ? 'Select a Playnite game…' : 'Playnite plugin not detected'
                    "
                    class="flex-1"
                    @focus="loadPlayniteGames"
                    @update:value="onPickPlaynite"
                  />
                  <n-button
                    v-if="lockPlaynite"
                    size="small"
                    type="default"
                    strong
                    @click="unlockPlaynite"
                  >
                    Change
                  </n-button>
                </div>
              </template>
              <div class="text-[11px] opacity-60">
                {{ isPlaynite ? 'Linked to Playnite' : 'Custom application' }}
              </div>
            </div>
            <div v-if="!isPlaynite" class="space-y-1 md:col-span-2">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Command</label
              >
              <n-input
                v-model:value="cmdText"
                type="textarea"
                :autosize="{ minRows: 4, maxRows: 8 }"
                placeholder="Executable command line"
              />
              <p class="text-[11px] opacity-60">Enter the full command line (single string).</p>
            </div>
            <div v-if="!isPlaynite" class="space-y-1 md:col-span-1">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Working Dir</label
              >
              <n-input
                v-model:value="form.workingDir"
                class="font-mono"
                placeholder="C:/Games/App"
              />
            </div>
            <div class="space-y-1 md:col-span-1">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Exit Timeout</label
              >
              <div class="flex items-center gap-2">
                <n-input-number v-model:value="form.exitTimeout" :min="0" class="w-28" />
                <span class="text-xs opacity-60">seconds</span>
              </div>
            </div>
            <div v-if="!isPlaynite" class="space-y-1 md:col-span-2">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Image Path</label
              >
              <div class="flex items-center gap-2">
                <n-input
                  v-model:value="form.imagePath"
                  class="font-mono flex-1"
                  placeholder="/path/to/image.png"
                />
                <n-button type="default" strong :disabled="!form.name" @click="openCoverFinder">
                  <i class="fas fa-image" /> Find Cover
                </n-button>
              </div>
              <p class="text-[11px] opacity-60">
                Optional; stored only and not fetched by Sunshine.
              </p>
            </div>
            <div v-if="!isMac" class="space-y-1 md:col-span-2">
              <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                >Gamepad Override</label
              >
              <n-select v-model:value="form.gamepad" :options="gamepadOptions" size="small" />
              <p class="text-[11px] opacity-60">
                Choose a specific controller profile for this application instead of the global
                default.
              </p>
            </div>
            <div v-if="isWindows" class="space-y-1 md:col-span-2">
              <div class="flex items-center justify-between">
                <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
                  >Resolution Scale Factor</label
                >
                <span class="text-xs font-semibold">{{ scaleFactorModel }}%</span>
              </div>
              <div class="flex items-center gap-3">
                <n-slider
                  v-model:value="scaleFactorModel"
                  :min="SCALE_FACTOR_MIN"
                  :max="SCALE_FACTOR_MAX"
                  :step="1"
                  class="flex-1"
                />
                <n-input-number
                  v-model:value="scaleFactorModel"
                  :min="SCALE_FACTOR_MIN"
                  :max="SCALE_FACTOR_MAX"
                  :step="1"
                  size="small"
                  class="w-20"
                />
              </div>
              <p class="text-[11px] opacity-60">
                Adjust per-app display scaling (20% to 200%). Values above 100% render at a higher
                resolution.
              </p>
            </div>
          </div>

          <div class="grid grid-cols-2 gap-3">
            <n-checkbox v-if="!isPlaynite" v-model:checked="form.autoDetach" size="small">
              Auto Detach
            </n-checkbox>
            <n-checkbox v-if="!isPlaynite" v-model:checked="form.waitAll" size="small"
              >Wait All</n-checkbox
            >
            <n-checkbox v-model:checked="form.terminateOnPause" size="small">
              Terminate On Pause
            </n-checkbox>
            <n-checkbox v-model:checked="form.virtualDisplay" size="small">
              Use Virtual Display
            </n-checkbox>
            <n-checkbox
              v-model:checked="form.allowClientCommands"
              size="small"
              class="md:col-span-2"
            >
              Allow Client Commands
            </n-checkbox>
            <n-checkbox v-model:checked="form.useAppIdentity" size="small">
              Use App Identity
            </n-checkbox>
            <n-checkbox
              v-if="form.useAppIdentity"
              v-model:checked="form.perClientAppIdentity"
              size="small"
              class="md:col-span-2"
            >
              Per-client App Identity
            </n-checkbox>
            <n-checkbox
              v-if="isWindows && !isPlaynite"
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
                  >For DLSS3, FSR3, NVIDIA Smooth Motion, and Lossless Scaling. Requires Windows
                  Graphics Capture (WGC), a display capable of 240 Hz or higher (virtual display
                  driver recommended), and RTSS installed. Configure Display Device to activate only
                  that monitor during streams.</span
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
            Frame generation capture fixes configure limiters to resolve issues with games being
            stuck at lower frame rates when using frame generation.
          </p>

          <div
            v-if="isWindows"
            class="mt-4 space-y-3 rounded-md border border-dark/10 p-3 dark:border-light/10"
          >
            <div class="flex items-center justify-between gap-3">
              <div>
                <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
                  Frame Generation
                </div>
                <p class="text-[11px] opacity-60">
                  Enable per-app frame generation helpers. Choose NVIDIA Smooth Motion to toggle the
                  driver feature automatically, or Lossless Scaling to launch the external helper
                  with customized profiles.
                </p>
              </div>
              <n-switch v-model:value="form.losslessScalingEnabled" size="small" />
            </div>

            <div class="grid gap-3 md:grid-cols-2">
              <div class="space-y-1">
                <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
                  Provider
                </label>
                <n-select
                  v-model:value="form.frameGenerationProvider"
                  :options="FRAME_GENERATION_PROVIDERS"
                  :disabled="!form.losslessScalingEnabled"
                  size="small"
                />
                <div class="space-y-2">
                  <p class="text-[11px] opacity-60">
                    <strong>NVIDIA Smooth Motion:</strong> Recommended for RTX 40xx/50xx cards with
                    driver 571.86 or higher. Offers better performance and lower latency than
                    Lossless Scaling.
                  </p>
                  <p class="text-[11px] opacity-60">
                    <strong>Lossless Scaling:</strong> Use this if you don't have an RTX 40xx+ card,
                    or if you prefer more customization options.
                  </p>
                </div>
              </div>
            </div>

            <n-alert
              v-if="form.losslessScalingEnabled && usingLosslessProvider && !playniteInstalled"
              type="warning"
              :show-icon="true"
              size="small"
              class="text-xs"
            >
              Playnite integration is not installed. Install the Playnite plugin from the Settings →
              Playnite tab to use Lossless Scaling integration.
            </n-alert>

            <n-alert
              v-if="form.losslessScalingEnabled && usingSmoothMotionProvider"
              type="info"
              :show-icon="true"
              size="small"
              class="text-xs"
            >
              <div class="space-y-1">
                <p>
                  <strong>Requirements:</strong> NVIDIA GeForce RTX 40xx or 50xx series GPU with
                  driver version 571.86 or higher.
                </p>
                <p>
                  NVIDIA Smooth Motion will be enabled in the global profile when the stream starts
                  and restored afterward. The 1st Gen Frame Generation Capture Fix will also be
                  applied automatically for optimal streaming performance.
                </p>
              </div>
            </n-alert>

            <div v-if="form.losslessScalingEnabled && usingLosslessProvider" class="space-y-4">
              <div class="grid gap-3 md:grid-cols-2">
                <div class="space-y-1">
                  <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
                    Profile
                  </label>
                  <n-radio-group v-model:value="form.losslessScalingProfile">
                    <n-radio value="recommended">Recommended (Lowest Latency)</n-radio>
                    <n-radio value="custom">Use Lossless Scaling "Default"</n-radio>
                  </n-radio-group>
                  <p class="text-[11px] opacity-60">
                    Recommended keeps Sunshine-tuned values for the lowest latency and best frame
                    pacing, enabling WGC capture, HDR, and LSFG 3.1 adaptive defaults. Select Use
                    Lossless Scaling "Default" to run the profile you maintain inside Lossless
                    Scaling.
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

              <div class="space-y-1">
                <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
                  Scaling Type
                </label>
                <n-select
                  v-model:value="losslessScalingModeModel"
                  :options="LOSSLESS_SCALING_OPTIONS"
                  size="small"
                  :clearable="false"
                />
                <p class="text-[11px] opacity-60">
                  Choose the Lossless Scaling algorithm. Options like LS1, FSR, NIS, and SGSR expose
                  sharpening controls below. Anime4K unlocks size and VRS toggles.
                </p>
                <p class="text-[11px] opacity-60 text-warning">
                  <strong>Note:</strong> Only use scaling if your game does not natively support FSR
                  or DLSS, as it will cost additional performance.
                </p>
              </div>

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
                  Controls post-scale sharpening for the selected algorithm.
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
                    <p class="text-[11px] opacity-60">
                      Enable Variable Rate Shading where supported.
                    </p>
                  </div>
                  <n-switch v-model:value="losslessAnimeVrsModel" size="small" />
                </div>
              </div>

              <div class="grid gap-3 md:grid-cols-2">
                <div v-if="showLosslessResolution" class="space-y-1">
                  <label class="text-xs font-semibold uppercase tracking-wide opacity-70">
                    Resolution Scaling (%)
                  </label>
                  <n-input-number
                    v-model:value="losslessResolutionScaleModel"
                    :min="LOSSLESS_RESOLUTION_MIN"
                    :max="LOSSLESS_RESOLUTION_MAX"
                    :step="1"
                    :precision="0"
                    placeholder="100"
                  />
                  <p class="text-[11px] opacity-60">
                    Leave at 100% for no scaling. Overrides apply only to the selected profile.
                  </p>
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
                  />
                  <p class="text-[11px] opacity-60">
                    Tunes frame generation blending (0–100). Recommended defaults to 50%.
                  </p>
                </div>
              </div>

              <div
                class="flex items-center justify-between gap-3 rounded-md border border-dark/10 px-3 py-2 dark:border-light/10"
              >
                <div>
                  <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
                    Performance Mode
                  </div>
                  <p class="text-[11px] opacity-60">
                    Prioritizes latency over quality. Customize per profile.
                  </p>
                </div>
                <n-switch v-model:value="losslessPerformanceModeModel" size="small" />
              </div>

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
                  />
                  <p class="text-[11px] opacity-60">
                    Sets the Lossless Scaling frame generation target.
                  </p>
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
                    placeholder="72"
                    @update:value="onLosslessRtssLimitChange"
                  />
                  <p class="text-[11px] opacity-60">
                    Defaults to 60% of the target when not customized.
                  </p>
                </div>
              </div>
            </div>
          </div>

          <section class="space-y-3">
            <div class="flex items-center justify-between">
              <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">
                Prep Commands
              </h3>
              <n-button size="small" type="primary" @click="addPrep">
                <i class="fas fa-plus" /> Add
              </n-button>
            </div>
            <n-checkbox v-model:checked="form.excludeGlobalPrepCmd" size="small">
              Exclude Global Prep Commands
            </n-checkbox>
            <div v-if="form.prepCmd.length === 0" class="text-[12px] opacity-60">None</div>
            <div v-else class="space-y-2">
              <div
                v-for="(p, i) in form.prepCmd"
                :key="i"
                class="rounded-md border border-dark/10 dark:border-light/10 p-2"
              >
                <div class="flex items-center justify-between gap-2 mb-2">
                  <div class="text-xs opacity-70">Step {{ i + 1 }}</div>
                  <div class="flex items-center gap-2">
                    <n-checkbox v-if="isWindows" v-model:checked="p.elevated" size="small">
                      {{ $t('_common.elevated') }}
                    </n-checkbox>
                    <n-button size="small" type="error" strong @click="form.prepCmd.splice(i, 1)">
                      <i class="fas fa-trash" />
                    </n-button>
                  </div>
                </div>
                <div class="grid grid-cols-1 gap-2">
                  <div>
                    <label class="text-[11px] opacity-60">{{ $t('_common.do_cmd') }}</label>
                    <n-input
                      v-model:value="p.do"
                      type="textarea"
                      :autosize="{ minRows: 1, maxRows: 3 }"
                      class="font-mono"
                      placeholder="Command to run before start"
                    />
                  </div>
                  <div>
                    <label class="text-[11px] opacity-60">{{ $t('_common.undo_cmd') }}</label>
                    <n-input
                      v-model:value="p.undo"
                      type="textarea"
                      :autosize="{ minRows: 1, maxRows: 3 }"
                      class="font-mono"
                      placeholder="Command to run on stop"
                    />
                  </div>
                </div>
              </div>
            </div>
          </section>

          <section class="space-y-3">
            <div class="flex items-center justify-between">
              <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">
                State Commands
              </h3>
              <n-button size="small" type="primary" @click="addState">
                <i class="fas fa-plus" /> Add
              </n-button>
            </div>
            <n-checkbox v-model:checked="form.excludeGlobalStateCmd" size="small">
              Exclude Global State Commands
            </n-checkbox>
            <div v-if="form.stateCmd.length === 0" class="text-[12px] opacity-60">None</div>
            <div v-else class="space-y-2">
              <div
                v-for="(s, i) in form.stateCmd"
                :key="`state-${i}`"
                class="rounded-md border border-dark/10 dark:border-light/10 p-2"
              >
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
                    <label class="text-[11px] opacity-60">Do Command</label>
                    <n-input
                      v-model:value="s.do"
                      type="textarea"
                      :autosize="{ minRows: 1, maxRows: 3 }"
                      class="font-mono"
                      placeholder="Command to run when stream starts"
                    />
                  </div>
                  <div>
                    <label class="text-[11px] opacity-60">Undo Command</label>
                    <n-input
                      v-model:value="s.undo"
                      type="textarea"
                      :autosize="{ minRows: 1, maxRows: 3 }"
                      class="font-mono"
                      placeholder="Command to run when stream stops"
                    />
                  </div>
                </div>
              </div>
            </div>
          </section>

          <section v-if="!isPlaynite" class="space-y-3">
            <div class="flex items-center justify-between">
              <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">
                Detached Commands
              </h3>
              <n-button size="small" type="primary" @click="addDetached">
                <i class="fas fa-plus" /> Add
              </n-button>
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

      <n-modal
        :show="showCoverModal"
        :z-index="3300"
        :mask-style="{ backgroundColor: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }"
        @update:show="(v) => (showCoverModal = v)"
      >
        <n-card :bordered="false" style="max-width: 48rem; width: 100%">
          <template #header>
            <div class="flex items-center justify-between w-full">
              <span class="font-semibold">Covers Found</span>
              <n-button type="default" strong size="small" @click="showCoverModal = false">
                Close
              </n-button>
            </div>
          </template>
          <div class="min-h-[160px]">
            <div v-if="coverSearching" class="flex items-center justify-center py-10">
              <n-spin size="large">Loading…</n-spin>
            </div>
            <div v-else>
              <div
                class="grid grid-cols-2 sm:grid-cols-3 md:grid-cols-4 gap-3 max-h-[420px] overflow-auto pr-1"
              >
                <div
                  v-for="(cover, i) in coverCandidates"
                  :key="i"
                  class="cursor-pointer group"
                  @click="useCover(cover)"
                >
                  <div
                    class="relative rounded overflow-hidden aspect-[3/4] bg-black/5 dark:bg-white/5"
                  >
                    <img :src="cover.url" class="absolute inset-0 w-full h-full object-cover" />
                    <div
                      v-if="coverBusy"
                      class="absolute inset-0 bg-black/20 dark:bg-white/10 flex items-center justify-center"
                    >
                      <n-spin size="small" />
                    </div>
                  </div>
                  <div class="mt-1 text-xs text-center truncate" :title="cover.name">
                    {{ cover.name }}
                  </div>
                </div>
                <div
                  v-if="!coverCandidates.length"
                  class="col-span-full text-center opacity-70 py-8"
                >
                  No results. Try adjusting the app name.
                </div>
              </div>
            </div>
          </div>
        </n-card>
      </n-modal>

      <n-modal
        :show="showDeleteConfirm"
        :z-index="3300"
        :mask-style="{ backgroundColor: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }"
        @update:show="(v) => (showDeleteConfirm = v)"
      >
        <n-card
          :title="
            isPlayniteAuto
              ? 'Remove and Exclude from Auto‑Sync?'
              : ($t('apps.confirm_delete_title_named', { name: form.name || '' }) as any)
          "
          :bordered="false"
          style="max-width: 32rem; width: 100%"
        >
          <div class="text-sm text-center space-y-2">
            <template v-if="isPlayniteAuto">
              <div>
                This application is managed by Playnite. Removing it will also add it to the
                Excluded Games list so it won’t be auto‑synced back.
              </div>
              <div class="opacity-80">
                You can bring it back later by manually adding it in Applications, or by removing
                the exclusion under Settings → Playnite.
              </div>
              <div class="opacity-70">Do you want to continue?</div>
            </template>
            <template v-else>
              {{ $t('apps.confirm_delete_message_named', { name: form.name || '' }) }}
            </template>
          </div>
          <template #footer>
            <div class="w-full flex items-center justify-center gap-3">
              <n-button type="default" strong @click="showDeleteConfirm = false">{{
                $t('_common.cancel')
              }}</n-button>
              <n-button type="error" strong @click="del">{{ $t('apps.delete') }}</n-button>
            </div>
          </template>
        </n-card>
      </n-modal>
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { computed, ref, watch, onMounted, onBeforeUnmount } from 'vue';
import { useMessage } from 'naive-ui';
import { http } from '@/http';
import {
  NModal,
  NCard,
  NButton,
  NInput,
  NInputNumber,
  NCheckbox,
  NSelect,
  NSpin,
  NSwitch,
  NRadioGroup,
  NRadio,
  NAlert,
  NSlider,
} from 'naive-ui';
import { useConfigStore } from '@/stores/config';

// Types for form and server payload
interface PrepCmd {
  do: string;
  undo: string;
  elevated?: boolean;
}

type LosslessProfileKey = 'recommended' | 'custom';
type LosslessScalingMode =
  | 'off'
  | 'ls1'
  | 'fsr'
  | 'nis'
  | 'sgsr'
  | 'bcas'
  | 'anime4k'
  | 'xbr'
  | 'sharp-bilinear'
  | 'integer'
  | 'nearest';
type Anime4kSize = 'S' | 'M' | 'L' | 'VL' | 'UL';

interface LosslessProfileOverrides {
  performanceMode: boolean | null;
  flowScale: number | null;
  resolutionScale: number | null;
  scalingMode: LosslessScalingMode | null;
  sharpening: number | null;
  anime4kSize: Anime4kSize | null;
  anime4kVrs: boolean | null;
}

interface LosslessProfileDefaults {
  performanceMode: boolean;
  flowScale: number;
  resolutionScale: number;
  scalingMode: LosslessScalingMode;
  sharpening: number;
  anime4kSize: Anime4kSize;
  anime4kVrs: boolean;
}

const LOSSLESS_FLOW_MIN = 0;
const LOSSLESS_FLOW_MAX = 100;
const LOSSLESS_RESOLUTION_MIN = 50;
const LOSSLESS_RESOLUTION_MAX = 100;
const LOSSLESS_SHARPNESS_MIN = 1;
const LOSSLESS_SHARPNESS_MAX = 10;
const SCALE_FACTOR_MIN = 20;
const SCALE_FACTOR_MAX = 200;

const LOSSLESS_SCALING_OPTIONS: { label: string; value: LosslessScalingMode }[] = [
  { label: 'Off', value: 'off' },
  { label: 'LS1', value: 'ls1' },
  { label: 'FSR', value: 'fsr' },
  { label: 'NIS', value: 'nis' },
  { label: 'SGSR', value: 'sgsr' },
  { label: 'Bicubic CAS', value: 'bcas' },
  { label: 'Anime4K', value: 'anime4k' },
  { label: 'xBR', value: 'xbr' },
  { label: 'Sharp Bilinear', value: 'sharp-bilinear' },
  { label: 'Integer', value: 'integer' },
  { label: 'Nearest Neighbor', value: 'nearest' },
];

const LOSSLESS_SCALING_SHARPENING = new Set<LosslessScalingMode>(['ls1', 'fsr', 'nis', 'sgsr']);
const LOSSLESS_ANIME_SIZES: { label: string; value: Anime4kSize }[] = [
  { label: 'Small', value: 'S' },
  { label: 'Medium', value: 'M' },
  { label: 'Large', value: 'L' },
  { label: 'Very Large', value: 'VL' },
  { label: 'Ultra Large', value: 'UL' },
];

const LOSSLESS_PROFILE_DEFAULTS: Record<LosslessProfileKey, LosslessProfileDefaults> = {
  recommended: {
    performanceMode: true,
    flowScale: 50,
    resolutionScale: 100,
    scalingMode: 'off',
    sharpening: 5,
    anime4kSize: 'S',
    anime4kVrs: false,
  },
  custom: {
    performanceMode: false,
    flowScale: 50,
    resolutionScale: 100,
    scalingMode: 'off',
    sharpening: 5,
    anime4kSize: 'S',
    anime4kVrs: false,
  },
};

type FrameGenerationProvider = 'lossless-scaling' | 'nvidia-smooth-motion';

const FRAME_GENERATION_PROVIDERS: Array<{ label: string; value: FrameGenerationProvider }> = [
  { label: 'NVIDIA Smooth Motion', value: 'nvidia-smooth-motion' },
  { label: 'Lossless Scaling', value: 'lossless-scaling' },
];

function emptyLosslessOverrides(): LosslessProfileOverrides {
  return {
    performanceMode: null,
    flowScale: null,
    resolutionScale: null,
    scalingMode: null,
    sharpening: null,
    anime4kSize: null,
    anime4kVrs: null,
  };
}

function emptyLosslessProfileState(): Record<LosslessProfileKey, LosslessProfileOverrides> {
  return {
    recommended: emptyLosslessOverrides(),
    custom: emptyLosslessOverrides(),
  };
}

function normalizeFrameGenerationProvider(value: unknown): FrameGenerationProvider {
  if (typeof value !== 'string') {
    return 'lossless-scaling';
  }
  const compact = value
    .toLowerCase()
    .split('')
    .filter((ch) => /[a-z0-9]/.test(ch))
    .join('');
  if (compact === 'nvidiasmoothmotion' || compact === 'smoothmotion' || compact === 'nvidia') {
    return 'nvidia-smooth-motion';
  }
  if (compact === 'losslessscaling' || compact === 'lossless') {
    return 'lossless-scaling';
  }
  return 'lossless-scaling';
}
interface AppForm {
  index: number;
  uuid?: string;
  name: string;
  output: string;
  cmd: string;
  workingDir: string;
  imagePath: string;
  excludeGlobalPrepCmd: boolean;
  excludeGlobalStateCmd: boolean;
  elevated: boolean;
  autoDetach: boolean;
  waitAll: boolean;
  terminateOnPause: boolean;
  virtualDisplay: boolean;
  allowClientCommands: boolean;
  useAppIdentity: boolean;
  perClientAppIdentity: boolean;
  gamepad: string;
  scaleFactor: number;
  frameGenLimiterFix: boolean;
  exitTimeout: number;
  prepCmd: PrepCmd[];
  stateCmd: PrepCmd[];
  detached: string[];
  gen1FramegenFix: boolean;
  gen2FramegenFix: boolean;
  frameGenerationProvider: FrameGenerationProvider;
  losslessScalingEnabled: boolean;
  losslessScalingTargetFps: number | null;
  losslessScalingRtssLimit: number | null;
  losslessScalingRtssTouched: boolean;
  losslessScalingProfile: LosslessProfileKey;
  losslessScalingProfiles: Record<LosslessProfileKey, LosslessProfileOverrides>;
  // With exactOptionalPropertyTypes, allow explicit undefined when clearing selection
  playniteId?: string | undefined;
  playniteManaged?: 'manual' | string | undefined;
}
interface ServerApp {
  name?: string;
  output?: string;
  cmd?: string | string[];
  uuid?: string;
  'working-dir'?: string;
  'image-path'?: string;
  'exclude-global-prep-cmd'?: boolean;
  'exclude-global-state-cmd'?: boolean;
  elevated?: boolean;
  'auto-detach'?: boolean;
  'wait-all'?: boolean;
  'terminate-on-pause'?: boolean;
  'virtual-display'?: boolean;
  'allow-client-commands'?: boolean;
  'use-app-identity'?: boolean;
  'per-client-app-identity'?: boolean;
  gamepad?: string;
  'scale-factor'?: number | string;
  'frame-gen-limiter-fix'?: boolean;
  'exit-timeout'?: number;
  'prep-cmd'?: Array<{ do?: string; undo?: string; elevated?: boolean }>;
  'state-cmd'?: Array<{ do?: string; undo?: string; elevated?: boolean }>;
  detached?: string[];
  'playnite-id'?: string | undefined;
  'playnite-managed'?: 'manual' | string | undefined;
  'gen1-framegen-fix'?: boolean;
  'gen2-framegen-fix'?: boolean;
  'dlss-framegen-capture-fix'?: boolean; // backward compatibility
  'frame-generation-provider'?: string;
  'lossless-scaling-framegen'?: boolean;
  'lossless-scaling-target-fps'?: number | string | null;
  'lossless-scaling-rtss-limit'?: number | string | null;
  'lossless-scaling-profile'?: string;
  'lossless-scaling-recommended'?: Record<string, unknown>;
  'lossless-scaling-custom'?: Record<string, unknown>;
}

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
    uuid: undefined,
    name: '',
    cmd: '',
    workingDir: '',
    imagePath: '',
    excludeGlobalPrepCmd: false,
    excludeGlobalStateCmd: false,
    elevated: false,
    autoDetach: true,
    waitAll: true,
    terminateOnPause: false,
    virtualDisplay: false,
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
    gen1FramegenFix: false,
    gen2FramegenFix: false,
    output: '',
    frameGenerationProvider: 'lossless-scaling',
    losslessScalingEnabled: false,
    losslessScalingTargetFps: null,
    losslessScalingRtssLimit: null,
    losslessScalingRtssTouched: false,
    losslessScalingProfile: 'custom',
    losslessScalingProfiles: emptyLosslessProfileState(),
  };
}
const form = ref<AppForm>(fresh());

// Keep Playnite default exit-timeout logic centralized:
// When a Playnite link is present and the timeout hasn't been customized
// (i.e., still at the default 5 or unset), use 10 seconds by default.
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

function parseNumeric(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) {
    return value;
  }
  if (typeof value === 'string') {
    const trimmed = value.trim();
    if (trimmed.length === 0) return null;
    const parsed = Number(trimmed);
    if (Number.isFinite(parsed)) {
      return parsed;
    }
  }
  return null;
}

function clampFlow(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  const rounded = Math.round(value);
  return Math.min(LOSSLESS_FLOW_MAX, Math.max(LOSSLESS_FLOW_MIN, rounded));
}

function clampResolution(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  const rounded = Math.round(value);
  return Math.min(LOSSLESS_RESOLUTION_MAX, Math.max(LOSSLESS_RESOLUTION_MIN, rounded));
}

function clampSharpness(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  const rounded = Math.round(value);
  return Math.min(LOSSLESS_SHARPNESS_MAX, Math.max(LOSSLESS_SHARPNESS_MIN, rounded));
}

function clampScaleFactor(value: number | null): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return 100;
  }
  const rounded = Math.round(value);
  return Math.min(SCALE_FACTOR_MAX, Math.max(SCALE_FACTOR_MIN, rounded));
}

function defaultRtssFromTarget(target: number | null): number | null {
  if (typeof target !== 'number' || !Number.isFinite(target) || target <= 0) {
    return null;
  }
  return Math.min(360, Math.max(1, Math.round(target / 2)));
}

function parseLosslessProfileKey(value: unknown): LosslessProfileKey {
  if (typeof value === 'string' && value.toLowerCase() === 'recommended') {
    return 'recommended';
  }
  return 'custom';
}

function parseLosslessOverrides(input: unknown): LosslessProfileOverrides {
  const overrides = emptyLosslessOverrides();
  if (!input || typeof input !== 'object') {
    return overrides;
  }
  const source = input as Record<string, unknown>;
  if (typeof source['performance-mode'] === 'boolean') {
    overrides.performanceMode = source['performance-mode'] as boolean;
  }
  const rawFlow = clampFlow(parseNumeric(source['flow-scale']));
  if (rawFlow !== null) {
    overrides.flowScale = rawFlow;
  }
  const rawResolution = clampResolution(parseNumeric(source['resolution-scale']));
  if (rawResolution !== null) {
    overrides.resolutionScale = rawResolution;
  }
  const modeRaw = typeof source['scaling-type'] === 'string' ? source['scaling-type'] : null;
  if (modeRaw) {
    const normalized = modeRaw.toLowerCase() as LosslessScalingMode;
    if (LOSSLESS_SCALING_OPTIONS.some((o) => o.value === normalized)) {
      overrides.scalingMode = normalized;
    }
  }
  const rawSharpness = clampSharpness(parseNumeric(source['sharpening']));
  if (rawSharpness !== null) {
    overrides.sharpening = rawSharpness;
  }
  const animeSizeRaw =
    typeof source['anime4k-size'] === 'string' ? source['anime4k-size'].toUpperCase() : null;
  if (animeSizeRaw && LOSSLESS_ANIME_SIZES.some((o) => o.value === animeSizeRaw)) {
    overrides.anime4kSize = animeSizeRaw as Anime4kSize;
  }
  if (typeof source['anime4k-vrs'] === 'boolean') {
    overrides.anime4kVrs = source['anime4k-vrs'] as boolean;
  }
  return overrides;
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
  const lsEnabled = !!src['lossless-scaling-framegen'];
  const lsTarget = parseNumeric(src['lossless-scaling-target-fps']);
  const lsLimit = parseNumeric(src['lossless-scaling-rtss-limit']);
  const profileKey = parseLosslessProfileKey(src['lossless-scaling-profile']);
  const losslessProfiles = emptyLosslessProfileState();
  losslessProfiles.recommended = parseLosslessOverrides(src['lossless-scaling-recommended']);
  losslessProfiles.custom = parseLosslessOverrides(src['lossless-scaling-custom']);
  const useAppIdentity = !!src['use-app-identity'];
  return {
    index: idx,
    uuid: typeof src.uuid === 'string' ? src.uuid : undefined,
    name: String(src.name ?? ''),
    output: String(src.output ?? ''),
    cmd: String(cmdStr ?? ''),
    workingDir: String(src['working-dir'] ?? ''),
    imagePath: String(src['image-path'] ?? ''),
    excludeGlobalPrepCmd: !!src['exclude-global-prep-cmd'],
    excludeGlobalStateCmd: !!src['exclude-global-state-cmd'],
    elevated: !!src.elevated,
    autoDetach: src['auto-detach'] !== undefined ? !!src['auto-detach'] : base.autoDetach,
    waitAll: src['wait-all'] !== undefined ? !!src['wait-all'] : base.waitAll,
    terminateOnPause:
      src['terminate-on-pause'] !== undefined ? !!src['terminate-on-pause'] : base.terminateOnPause,
    virtualDisplay:
      src['virtual-display'] !== undefined ? !!src['virtual-display'] : base.virtualDisplay,
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
    'exclude-global-state-cmd': !!f.excludeGlobalStateCmd,
    elevated: !!f.elevated,
    'auto-detach': !!f.autoDetach,
    'wait-all': !!f.waitAll,
    'terminate-on-pause': !!f.terminateOnPause,
    'virtual-display': !!f.virtualDisplay,
    'allow-client-commands': !!f.allowClientCommands,
    'use-app-identity': !!f.useAppIdentity,
    'per-client-app-identity': f.useAppIdentity ? !!f.perClientAppIdentity : false,
    gamepad: String(f.gamepad || ''),
    'scale-factor': clampScaleFactor(
      typeof f.scaleFactor === 'number' && Number.isFinite(f.scaleFactor) ? f.scaleFactor : null,
    ),
    'gen1-framegen-fix': !!f.gen1FramegenFix,
    'gen2-framegen-fix': !!f.gen2FramegenFix,
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
  };
  const trimmedUuid = typeof f.uuid === 'string' ? f.uuid.trim() : '';
  if (trimmedUuid) {
    payload.uuid = trimmedUuid;
  }
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
const scaleFactorModel = computed<number>({
  get: () => form.value.scaleFactor,
  set: (v: number) => {
    form.value.scaleFactor = clampScaleFactor(
      typeof v === 'number' && Number.isFinite(v) ? v : null,
    );
  },
});
const isPlaynite = computed<boolean>(() => !!form.value.playniteId);
const isPlayniteAuto = computed<boolean>(
  () => isPlaynite.value && form.value.playniteManaged !== 'manual',
);
const usingLosslessProvider = computed<boolean>(
  () => form.value.frameGenerationProvider === 'lossless-scaling',
);
const usingSmoothMotionProvider = computed<boolean>(
  () => form.value.frameGenerationProvider === 'nvidia-smooth-motion',
);
watch(
  () => form.value.frameGenerationProvider,
  (provider) => {
    const normalized = normalizeFrameGenerationProvider(provider);
    if (provider !== normalized) {
      form.value.frameGenerationProvider = normalized;
      return;
    }
    if (
      normalized === 'lossless-scaling' &&
      form.value.losslessScalingEnabled &&
      !form.value.losslessScalingRtssTouched
    ) {
      form.value.losslessScalingRtssLimit = defaultRtssFromTarget(
        parseNumeric(form.value.losslessScalingTargetFps),
      );
    }
  },
);
watch(
  () => form.value.losslessScalingEnabled,
  (enabled) => {
    if (!enabled) {
      form.value.losslessScalingRtssTouched = false;
      return;
    }
    if (!usingLosslessProvider.value) {
      return;
    }
    if (!form.value.losslessScalingRtssTouched) {
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
    if (!form.value.losslessScalingEnabled || !usingLosslessProvider.value) {
      return;
    }
    if (!form.value.losslessScalingRtssTouched) {
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
type CoverCandidate = { name: string; key: string; url: string; saveUrl: string };
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
  if (isPlaynite.value) return;
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
const ddConfigOption = computed(
  () => (configStore.config as any)?.dd_configuration_option ?? 'disabled',
);
const captureMethod = computed(() => (configStore.config as any)?.capture ?? '');
const playniteInstalled = ref(false);
const isNew = computed(() => form.value.index === -1);
// New app source: 'custom' or 'playnite' (Windows only)
const newAppSource = ref<'custom' | 'playnite'>('custom');

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
  () => [form.value.losslessScalingEnabled, form.value.frameGenerationProvider] as const,
  ([enabled, provider]) => {
    if (enabled && !form.value.gen1FramegenFix) {
      autoEnablingGen1 = true;
      form.value.gen1FramegenFix = true;
      // Show appropriate message based on provider
      if (provider === 'nvidia-smooth-motion') {
        message?.info(
          '1st Gen Frame Generation Capture Fix has been automatically enabled for optimal NVIDIA Smooth Motion performance.',
          { duration: 8000 },
        );
      } else {
        message?.info(
          '1st Gen Frame Generation Capture Fix has been automatically enabled because it is required for Lossless Scaling integration.',
          { duration: 8000 },
        );
      }
      // Reset flag after Vue updates to avoid race conditions
      setTimeout(() => {
        autoEnablingGen1 = false;
      }, 100);
    }
  },
);
function addDetached() {
  form.value.detached.push('');
  requestAnimationFrame(() => updateShadows());
}

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
