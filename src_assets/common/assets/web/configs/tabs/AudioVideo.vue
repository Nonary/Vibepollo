<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { $tp } from '@/platform-i18n';
import PlatformLayout from '@/PlatformLayout.vue';
import AdapterNameSelector from '@/configs/tabs/audiovideo/AdapterNameSelector.vue';
import DisplayOutputSelector from '@/configs/tabs/audiovideo/DisplayOutputSelector.vue';
import DisplayDeviceOptions from '@/configs/tabs/audiovideo/DisplayDeviceOptions.vue';
import DisplayModesSettings from '@/configs/tabs/audiovideo/DisplayModesSettings.vue';
import FrameLimiterStep from '@/configs/tabs/audiovideo/FrameLimiterStep.vue';
import { NCheckbox, NInput, NSwitch, NRadioGroup, NRadio } from 'naive-ui';
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
      if (!config.value) return;
      if (typeof store.updateOption === 'function') {
        store.updateOption(key, next as any);
      } else {
        (config.value as any)[key] = next as any;
      }
    },
  });
}

const installSteamDrivers = boolProxy('install_steam_audio_drivers', 'true');
const streamAudio = boolProxy('stream_audio', 'true');

const virtualDisplayMode = computed<'disabled' | 'per_client' | 'shared'>({
  get() {
    const mode = config.value?.['virtual_display_mode'];
    if (typeof mode === 'string') {
      if (mode === 'disabled' || mode === 'per_client' || mode === 'shared') {
        return mode;
      }
    }
    return 'per_client';
  },
  set(mode) {
    if (!config.value) return;
    store.updateOption('virtual_display_mode', mode);
  },
});
</script>

<template>
  <div id="av" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-6">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
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
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <n-input
            id="virtual_sink"
            v-model:value="config.virtual_sink"
            type="text"
            :placeholder="$t('config.virtual_sink_placeholder')"
          />
          <div class="text-[11px] opacity-60 mt-1">
            {{ $t('config.virtual_sink_desc') }}
          </div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <n-checkbox v-model:checked="installSteamDrivers" class="mb-3">
          {{ $t('config.install_steam_audio_drivers') }}
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
            <!-- Highlight driver health before picking a mode -->
            <PlatformLayout>
              <template #windows>
                <div class="mt-3">
                  <div
                    class="px-4 py-3 rounded-md"
                    :class="[vdisplay ? 'bg-warning/10 text-warning' : 'bg-success/10 text-success']"
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
            <div v-if="virtualDisplayMode === 'disabled'" class="mt-3">
              <DisplayOutputSelector />
            </div>

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

<style scoped></style>
