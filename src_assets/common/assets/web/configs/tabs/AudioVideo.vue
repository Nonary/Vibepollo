<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { $tp } from '@/platform-i18n';
import Checkbox from '@/Checkbox.vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import PlatformLayout from '@/PlatformLayout.vue';
import AdapterNameSelector from '@/configs/tabs/audiovideo/AdapterNameSelector.vue';
import DisplayOutputSelector from '@/configs/tabs/audiovideo/DisplayOutputSelector.vue';
import DisplayDeviceOptions from '@/configs/tabs/audiovideo/DisplayDeviceOptions.vue';
import DisplayModesSettings from '@/configs/tabs/audiovideo/DisplayModesSettings.vue';
import FrameLimiterStep from '@/configs/tabs/audiovideo/FrameLimiterStep.vue';
import { NSwitch, NRadioGroup, NRadio } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';

const { t } = useI18n();
const store = useConfigStore();
const { config } = storeToRefs(store);
const platform = computed(() => (config.value as any)?.platform || '');
const ddConfigDisabled = computed(
  () => (config.value as any)?.dd_configuration_option === 'disabled',
);
const frameLimiterStepLabel = computed(() =>
  ddConfigDisabled.value ? t('config.dd_step_3') : t('config.dd_step_4'),
);

// SudoVDA status mapping (Apollo-specific)
const sudovdaStatus = computed(() => ({
  '1': t('config.sudovda_status_unknown'),
  '0': t('config.sudovda_status_ready'),
  '-1': t('config.sudovda_status_uninitialized'),
  '-2': t('config.sudovda_status_version_incompatible'),
  '-3': t('config.sudovda_status_watchdog_failed'),
}));
const vdisplay = computed(() => (config as any)?.vdisplay || 0);
const currentDriverStatus = computed(
  () =>
    sudovdaStatus.value[String(vdisplay.value) as keyof typeof sudovdaStatus.value] ||
    t('config.sudovda_status_unknown'),
);

const lastAutomationOption = ref('verify_only');
watch(
  () => config.value?.dd_configuration_option,
  (next) => {
    if (typeof next === 'string' && next !== 'disabled') {
      lastAutomationOption.value = next;
    }
  },
  { immediate: true },
);

watch(
  () => config.value?.virtual_display_mode,
  (next, prev) => {
    if (typeof next === 'string' && next !== 'disabled' && prev === 'disabled') {
      const currentLayout = config.value?.['virtual_display_layout'];
      if (!currentLayout || currentLayout === 'disabled') {
        store.updateOption('virtual_display_layout', 'exclusive');
      }
    }
  },
);

const displayAutomationEnabled = computed<boolean>({
  get() {
    return config.value?.dd_configuration_option !== 'disabled';
  },
  set(enabled) {
    if (!config.value) return;
    if (!enabled) {
      const next = 'disabled';
      if (typeof store.updateOption === 'function') {
        store.updateOption('dd_configuration_option', next as any);
      } else {
        (config.value as any).dd_configuration_option = next as any;
      }
      return;
    }

    if (config.value.dd_configuration_option === 'disabled') {
      const fallback = lastAutomationOption.value || 'verify_only';
      const next = fallback === 'disabled' ? 'verify_only' : fallback;
      if (typeof store.updateOption === 'function') {
        store.updateOption('dd_configuration_option', next as any);
      } else {
        (config.value as any).dd_configuration_option = next as any;
      }
    }
  },
});

const virtualDisplayMode = computed<'disabled' | 'per_client' | 'shared'>({
  get() {
    const mode = config.value?.['virtual_display_mode'];
    if (typeof mode === 'string') {
      if (mode === 'disabled' || mode === 'per_client' || mode === 'shared') {
        return mode;
      }
    }
    return 'disabled';
  },
  set(mode) {
    if (!config.value) return;
    store.updateOption('virtual_display_mode', mode);
  },
});

const virtualDisplayLayout = computed<
  'exclusive' | 'extended' | 'extended_primary' | 'extended_isolated' | 'extended_primary_isolated'
>({
  get() {
    const layout = config.value?.['virtual_display_layout'];
    if (
      layout === 'extended' ||
      layout === 'extended_primary' ||
      layout === 'extended_isolated' ||
      layout === 'extended_primary_isolated'
    ) {
      return layout;
    }
    return 'exclusive';
  },
  set(layout) {
    if (!config.value) return;
    store.updateOption('virtual_display_layout', layout);
  },
});

const virtualDisplayLayoutOptions = computed(() => [
  {
    value: 'exclusive',
    label: t('config.virtual_display_layout_exclusive') + ' (default)',
    description: t('config.virtual_display_layout_exclusive_desc'),
  },
  {
    value: 'extended',
    label: t('config.virtual_display_layout_extended'),
    description: t('config.virtual_display_layout_extended_desc'),
  },
  {
    value: 'extended_primary',
    label: t('config.virtual_display_layout_extended_primary'),
    description: t('config.virtual_display_layout_extended_primary_desc'),
  },
  {
    value: 'extended_isolated',
    label: t('config.virtual_display_layout_extended_isolated'),
    description: t('config.virtual_display_layout_extended_isolated_desc'),
  },
  {
    value: 'extended_primary_isolated',
    label: t('config.virtual_display_layout_extended_primary_isolated'),
    description: t('config.virtual_display_layout_extended_primary_isolated_desc'),
  },
]);

function selectVirtualDisplayLayout(v: unknown) {
  const sv = String(v);
  const opts = virtualDisplayLayoutOptions.value.map((o) => o.value);
  if (opts.includes(sv)) {
    virtualDisplayLayout.value = sv as any;
  }
}
</script>

<template>
  <div id="av" class="config-page">
    <ConfigFieldRenderer
      setting-key="audio_sink"
      v-model="config.audio_sink"
      class="mb-6"
      :desc="$tp('config.audio_sink_desc')"
      :placeholder="
        $tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')
      "
    >
      <br />
      <PlatformLayout>
        <template #windows>
          <pre>tools\audio-info.exe</pre>
        </template>
        <template #freebsd>
          <pre>pacmd list-sinks | grep "name:"</pre>
          <pre>pactl info | grep Source</pre>
        </template>
        <template #linux>
          <pre>pacmd list-sinks | grep "name:"</pre>
          <pre>pactl info | grep Source</pre>
        </template>
        <template #macos>
          <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br />
          <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
        </template>
      </PlatformLayout>
    </ConfigFieldRenderer>

    <PlatformLayout>
      <template #windows>
        <ConfigFieldRenderer
          setting-key="virtual_sink"
          v-model="config.virtual_sink"
          class="mb-6"
          :placeholder="$t('config.virtual_sink_placeholder')"
        />

        <ConfigFieldRenderer
          setting-key="install_steam_audio_drivers"
          v-model="config.install_steam_audio_drivers"
          class="mb-3"
        />
      </template>
    </PlatformLayout>

    <ConfigFieldRenderer setting-key="stream_audio" v-model="config.stream_audio" class="mb-3" />

    <ConfigFieldRenderer
      v-if="config.stream_audio === 'enabled'"
      setting-key="keep_sink_default"
      v-model="config.keep_sink_default"
      class="mb-3"
    />

    <ConfigFieldRenderer
      v-if="config.stream_audio === 'enabled'"
      setting-key="auto_capture_sink"
      v-model="config.auto_capture_sink"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="mic_sink"
      v-model="config.mic_sink"
      class="mb-3"
    />

    <ConfigFieldRenderer
      setting-key="mic_capture_device"
      v-model="config.mic_capture_device"
      class="mb-3"
    />

    <ConfigFieldRenderer
      setting-key="mic_buffer_ms"
      v-model="config.mic_buffer_ms"
      class="mb-3"
    />

    <ConfigFieldRenderer
      setting-key="mic_buffer_packets"
      v-model="config.mic_buffer_packets"
      class="mb-6"
    />

    <AdapterNameSelector />

    <!-- Display configuration: clear, guided, pre-stream focused -->
    <section class="mb-8">
      <div class="rounded-md overflow-hidden border border-dark/10 dark:border-light/10">
        <div class="bg-surface/40 px-4 py-3">
          <h3 class="text-sm font-medium">{{ $t('config.dd_display_setup_title') }}</h3>
          <p class="text-[11px] opacity-70 mt-1">
            {{ $t('config.dd_display_setup_intro') }}
          </p>
        </div>

        <div class="p-4">
          <!-- Step 1: Which display to capture -->
          <fieldset class="mb-4 border border-dark/35 dark:border-light/25 rounded-xl p-4">
            <legend class="px-2 text-sm font-medium">
              {{ $t('config.dd_step_1') }}: {{ $t('config.dd_choose_display') }}
            </legend>
            <!-- Highlight driver health before picking a mode -->
            <PlatformLayout>
              <template #windows>
                <div class="mt-3">
                  <div
                    class="px-4 py-3 rounded-md"
                    :class="[
                      vdisplay ? 'bg-warning/10 text-warning' : 'bg-success/10 text-success',
                    ]"
                  >
                    <i class="fa-solid fa-circle-info mr-2"></i>
                    {{ t('config.virtual_display_status_label') }} {{ currentDriverStatus }}
                  </div>
                  <p v-if="vdisplay" class="text-[11px] opacity-70 mt-2 leading-snug">
                    {{ t('config.virtual_display_status_hint') }}
                  </p>
                </div>
              </template>
            </PlatformLayout>
            <p class="text-[11px] opacity-70 mt-2 leading-snug">
              {{ $t('config.virtual_display_mode_step_hint') }}
            </p>
            <n-radio-group v-model:value="virtualDisplayMode" class="grid gap-2 sm:grid-cols-3">
              <n-radio value="disabled">
                {{ $t('config.virtual_display_mode_disabled') }}
              </n-radio>
              <n-radio value="per_client">
                {{ $t('config.virtual_display_mode_per_client') }}
              </n-radio>
              <n-radio value="shared">
                {{ $t('config.virtual_display_mode_shared') }}
              </n-radio>
            </n-radio-group>
            <PlatformLayout>
              <template #windows>
                <div class="mt-4 border-l-2 border-dark/10 dark:border-light/10 pl-3">
                  <Checkbox
                    id="dd_wa_virtual_double_refresh"
                    v-model="config.dd_wa_virtual_double_refresh"
                    locale-prefix="config"
                    :default="true"
                    :disabled="virtualDisplayMode === 'disabled'"
                  />
                </div>
              </template>
            </PlatformLayout>
            <div v-if="virtualDisplayMode === 'disabled'" class="mt-3">
              <DisplayOutputSelector />
            </div>
            <div v-else class="mt-3 space-y-2">
              <div class="text-sm font-medium">
                {{ $t('config.virtual_display_layout_label') }}
              </div>
              <p class="text-[11px] opacity-70 leading-snug">
                {{ $t('config.virtual_display_layout_hint') }}
              </p>
              <n-radio-group v-model:value="virtualDisplayLayout" class="space-y-4">
                <div
                  v-for="option in virtualDisplayLayoutOptions"
                  :key="option.value"
                  class="flex flex-col cursor-pointer py-2 px-2 rounded-md hover:bg-surface/10"
                  @click.prevent="selectVirtualDisplayLayout(option.value)"
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

              <!-- Warning for extended modes without primary -->
              <transition name="fade">
                <div
                  v-if="
                    virtualDisplayLayout === 'extended' ||
                    virtualDisplayLayout === 'extended_isolated'
                  "
                  class="mt-3 rounded-lg bg-amber-50 dark:bg-amber-950/30 border border-amber-200 dark:border-amber-800 p-3"
                >
                  <p class="text-[11px] text-amber-900 dark:text-amber-100">
                    <span class="flex items-start gap-2">
                      <i
                        class="fas fa-exclamation-triangle text-amber-600 dark:text-amber-400 flex-shrink-0 mt-0.5"
                      />
                      <span class="block">{{ $t('config.dd_config_ensure_active_warning') }}</span>
                    </span>
                  </p>
                </div>
              </transition>
            </div>

            <!-- HDR Calibration Tip for per-client virtual display -->
            <transition name="fade">
              <div
                v-if="virtualDisplayMode === 'per_client'"
                class="mt-4 rounded-lg bg-blue-50 dark:bg-blue-950/30 border border-blue-200 dark:border-blue-800 p-3"
              >
                <p class="text-[11px] text-blue-900 dark:text-blue-100">
                  <span class="flex items-start gap-2">
                    <i
                      class="fas fa-lightbulb text-blue-600 dark:text-blue-400 flex-shrink-0 mt-0.5"
                    />
                    <span class="block">{{ $t('config.virtual_display_hdr_tip') }}</span>
                  </span>
                </p>
              </div>
            </transition>

            <div
              class="mt-4 flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between sm:gap-4"
            >
              <div>
                <div class="text-sm font-medium">
                  {{ $t('config.dd_automation_label') }}
                </div>
                <p class="text-[11px] opacity-70 mt-1 max-w-xl">
                  {{ $t('config.dd_automation_desc') }}
                </p>
              </div>
              <n-switch
                v-model:value="displayAutomationEnabled"
                size="medium"
                class="self-start sm:self-center"
              >
                <template #checked>{{ $t('_common.enabled') }}</template>
                <template #unchecked>{{ $t('_common.disabled') }}</template>
              </n-switch>
            </div>
          </fieldset>

          <div class="my-4 border-t border-dark/5 dark:border-light/5" />

          <!-- Step 2: What to do before the stream starts -->
          <div>
            <DisplayDeviceOptions section="pre" />
          </div>

          <div class="my-4 border-t border-dark/5 dark:border-light/5" />

          <!-- Step 3: Optional adjustments -->
          <div>
            <DisplayDeviceOptions section="options" />
          </div>

          <div class="my-4 border-t border-dark/5 dark:border-light/5" />

          <FrameLimiterStep :step-label="frameLimiterStepLabel" />
        </div>
      </div>
    </section>

    <!-- Display Modes -->
    <DisplayModesSettings />
  </div>
</template>

<style scoped>
.display-mode-option {
  @apply block w-full rounded-xl border border-dark/10 dark:border-light/10 px-4 py-3 text-sm font-semibold transition-colors;
  min-height: 56px;
}

.display-mode-option :deep(.n-radio__label) {
  width: 100%;
  @apply flex items-center justify-center gap-3;
  text-align: center;
}

.display-mode-option :deep(.n-radio__indicator) {
  @apply flex-shrink-0;
}
</style>
