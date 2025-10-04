<script setup lang="ts">
import { ref, computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { $tp } from '@/platform-i18n';
import PlatformLayout from '@/PlatformLayout.vue';
import AdapterNameSelector from '@/configs/tabs/audiovideo/AdapterNameSelector.vue';
import DisplayOutputSelector from '@/configs/tabs/audiovideo/DisplayOutputSelector.vue';
import DisplayDeviceOptions from '@/configs/tabs/audiovideo/DisplayDeviceOptions.vue';
import DisplayModesSettings from '@/configs/tabs/audiovideo/DisplayModesSettings.vue';
import FrameLimiterStep from '@/configs/tabs/audiovideo/FrameLimiterStep.vue';
import { NCheckbox, NInput } from 'naive-ui';
import { useConfigStore } from '@/stores/config';

const { t } = useI18n();
const store = useConfigStore();
const config = store.config;
const platform = computed(() => (config as any)?.platform || '');
const ddConfigDisabled = computed(() => (config as any)?.dd_configuration_option === 'disabled');
const frameLimiterStepLabel = computed(() =>
  ddConfigDisabled.value ? t('config.dd_step_3') : t('config.dd_step_4'),
);

// SudoVDA status mapping (Apollo-specific)
const sudovdaStatus = {
  '1': 'Unknown',
  '0': 'Ready',
  '-1': 'Uninitialized',
  '-2': 'Version Incompatible',
  '-3': 'Watchdog Failed',
};

const vdisplay = computed(() => (config as any)?.vdisplay || 0);
const currentDriverStatus = computed(
  () => sudovdaStatus[String(vdisplay.value) as keyof typeof sudovdaStatus] || 'Unknown',
);

// Fallback mode validation
const validateFallbackMode = (event: Event) => {
  const target = event.target as HTMLInputElement;
  const value = target.value;
  if (!value.match(/^\d+x\d+x\d+(\.\d+)?$/)) {
    target.setCustomValidity(t('config.fallback_mode_error'));
  } else {
    target.setCustomValidity('');
  }
  target.reportValidity();
};

// Replace custom Checkbox with Naive UI using compatibility mapping
function mapToBoolRepresentation(value: any) {
  if (value === true || value === false) return { possibleValues: [true, false], value };
  if (value === 1 || value === 0) return { possibleValues: [1, 0], value };
  const stringPairs = [
    ['true', 'false'],
    ['1', '0'],
    ['enabled', 'disabled'],
    ['enable', 'disable'],
    ['yes', 'no'],
    ['on', 'off'],
  ];
  const v = String(value ?? '')
    .toLowerCase()
    .trim();
  for (const pair of stringPairs) {
    if (v === pair[0] || v === pair[1]) return { possibleValues: pair, value: v };
  }
  return null as null | {
    possibleValues: readonly [string, string] | readonly [true, false] | readonly [1, 0];
    value: any;
  };
}

function boolProxy(key: string, defaultValue: string = 'true') {
  return computed<boolean>({
    get() {
      const raw = config.value?.[key];
      const parsed = mapToBoolRepresentation(raw);
      if (parsed) return parsed.value === parsed.possibleValues[0];
      // fallback to default
      const defParsed = mapToBoolRepresentation(defaultValue);
      return defParsed ? defParsed.value === defParsed.possibleValues[0] : !!raw;
    },
    set(v: boolean) {
      const raw = config.value?.[key];
      const parsed = mapToBoolRepresentation(raw);
      const pv = parsed ? parsed.possibleValues : ['true', 'false'];
      const next = v ? pv[0] : pv[1];
      // assign preserving original type if boolean/numeric pair
      (config.value as any)[key] = next as any;
    },
  });
}

const installSteamDrivers = boolProxy('install_steam_audio_drivers', 'true');
const streamAudio = boolProxy('stream_audio', 'true');
const keepSinkDefault = boolProxy('keep_sink_default', 'true');
const autoCaptureSink = boolProxy('auto_capture_sink', 'true');
const headlessMode = boolProxy('headless_mode', 'false');
const doubleRefreshrate = boolProxy('double_refreshrate', 'false');
const isolatedVirtualDisplay = boolProxy('isolated_virtual_display_option', 'false');
</script>

<template>
  <div id="av" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-6">
      <label for="audio_sink" class="form-label">{{ t('config.audio_sink') }}</label>
      <n-input
        id="audio_sink"
        v-model:value="config.audio_sink"
        type="text"
        :placeholder="
          $tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')
        "
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $tp('config.audio_sink_desc') }}<br />
        <PlatformLayout>
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a
            ><br />
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>

    <PlatformLayout>
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-6">
          <label for="virtual_sink" class="form-label">{{ t('config.virtual_sink') }}</label>
          <n-input
            id="virtual_sink"
            v-model:value="config.virtual_sink"
            type="text"
            :placeholder="t('config.virtual_sink_placeholder')"
          />
          <div class="text-[11px] opacity-60 mt-1">
            {{ t('config.virtual_sink_desc') }}
          </div>
        </div>
        <!-- Install Steam Audio Drivers -->
        <n-checkbox v-model:checked="installSteamDrivers" class="mb-3">
          {{ t('config.install_steam_audio_drivers') }}
        </n-checkbox>

        <n-checkbox v-model:checked="keepSinkDefault" class="mb-3">
          {{ t('config.keep_sink_default') }}
        </n-checkbox>

        <n-checkbox v-model:checked="autoCaptureSink" class="mb-3">
          {{ t('config.auto_capture_sink') }}
        </n-checkbox>
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <n-checkbox v-model:checked="streamAudio" class="mb-3">
      {{ $t('config.stream_audio') }}
    </n-checkbox>

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
            <DisplayOutputSelector />
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

    <!-- Fallback Display Mode -->
    <div class="mb-3">
      <label for="fallback_mode" class="form-label">{{ t('config.fallback_mode') }}</label>
      <n-input
        id="fallback_mode"
        v-model:value="config.fallback_mode"
        type="text"
        placeholder="1920x1080x60"
        @input="validateFallbackMode"
      />
      <div class="text-[11px] opacity-60 mt-1">{{ t('config.fallback_mode_desc') }}</div>
    </div>

    <!-- Windows-specific options -->
    <PlatformLayout>
      <template #windows>
        <!-- Headless Mode -->
        <n-checkbox v-model:checked="headlessMode" class="mb-3">
          {{ t('config.headless_mode') }}
        </n-checkbox>

        <!-- Double Refreshrate -->
        <n-checkbox v-model:checked="doubleRefreshrate" class="mb-3">
          {{ t('config.double_refreshrate') }}
        </n-checkbox>

        <!-- Isolated Virtual Display -->
        <n-checkbox v-model:checked="isolatedVirtualDisplay" class="mb-3">
          {{ t('config.isolated_virtual_display_option') }}
        </n-checkbox>

        <!-- SudoVDA Driver Status -->
        <div
          class="px-4 py-3 rounded-md mb-3"
          :class="[vdisplay ? 'bg-warning/10 text-warning' : 'bg-success/10 text-success']"
        >
          <i class="fa-solid fa-circle-info mr-2"></i> SudoVDA Driver status:
          {{ currentDriverStatus }}
        </div>
        <div class="text-[11px] opacity-60 mb-3" v-if="vdisplay">
          Please ensure SudoVDA driver is installed to the latest version and enabled properly.
        </div>
      </template>
    </PlatformLayout>
  </div>
</template>

<style scoped></style>
