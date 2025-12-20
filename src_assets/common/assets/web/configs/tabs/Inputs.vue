<script setup lang="ts">
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import PlatformLayout from '@/PlatformLayout.vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { NSelect, NInputNumber } from 'naive-ui';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);

const platform = computed(() =>
  (metadata.value?.platform || config.value?.platform || '').toLowerCase(),
);

const labelMap: Record<string, string> = {
  auto: '_common.auto',
  ds4: 'config.gamepad_ds4',
  ds5: 'config.gamepad_ds5',
  switch: 'config.gamepad_switch',
  x360: 'config.gamepad_x360',
  xone: 'config.gamepad_xone',
};

const prioritizedByPlatform: Record<string, string[]> = {
  linux: ['ds5', 'xone', 'switch', 'x360'],
  windows: ['x360', 'ds4'],
};

const fallbackOrder = ['x360', 'ds5', 'ds4'];

const gamepadOptions = computed(() => {
  const opts = [{ label: '_common.auto', value: 'auto' }];
  const seen = new Set<string>(opts.map((o) => o.value));

  const addOption = (value: string | undefined) => {
    if (!value || seen.has(value)) return;
    const label = labelMap[value] || `config.gamepad_${value}`;
    opts.push({ label, value });
    seen.add(value);
  };

  const plat = platform.value;
  const platformOrder = prioritizedByPlatform[plat] ?? fallbackOrder;
  platformOrder.forEach(addOption);

  const current = config.value?.gamepad;
  if (current && current !== 'auto') addOption(current);

  return opts;
});

//
</script>

<template>
  <div id="input" class="config-page">
    <!-- Enable Gamepad Input -->
    <Checkbox
      id="controller"
      v-model="config.controller"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Emulated Gamepad Type -->
    <div v-if="config.controller === 'enabled' && platform !== 'macos'" class="mb-6">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <n-select
        id="gamepad"
        v-model:value="config.gamepad"
        :options="gamepadOptions.map((o) => ({ label: $t(o.label), value: o.value }))"
        :data-search-options="
          gamepadOptions.map((o) => `${$t(o.label)}::${o.value ?? ''}`).join('|')
        "
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.gamepad_desc') }}
      </p>
    </div>

    <!-- Additional options based on gamepad type -->
    <template v-if="config.controller === 'enabled'">
      <template
        v-if="
          config.gamepad === 'ds4' ||
          config.gamepad === 'ds5' ||
          (config.gamepad === 'auto' && platform !== 'macos')
        "
      >
        <div class="mb-3 accordion">
          <div class="accordion-item">
            <h2 class="accordion-header">
              <button
                class="accordion-button"
                type="button"
                data-bs-toggle="collapse"
                data-bs-target="#panelsStayOpen-collapseOne"
              >
                {{
                  $t(
                    config.gamepad === 'ds4'
                      ? 'config.gamepad_ds4_manual'
                      : config.gamepad === 'ds5'
                        ? 'config.gamepad_ds5_manual'
                        : 'config.gamepad_auto',
                  )
                }}
              </button>
            </h2>
            <div
              id="panelsStayOpen-collapseOne"
              class="accordion-collapse collapse show"
              aria-labelledby="panelsStayOpen-headingOne"
            >
              <div class="accordion-body">
                <!-- Automatic detection options (for Windows and Linux) -->
                <template
                  v-if="
                    config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')
                  "
                >
                  <!-- Gamepad with motion-capability as DS4(Windows)/DS5(Linux) -->
                  <Checkbox
                    id="motion_as_ds4"
                    v-model="config.motion_as_ds4"
                    class="mb-3"
                    locale-prefix="config"
                    default="true"
                  ></Checkbox>
                  <!-- Gamepad with touch-capability as DS4(Windows)/DS5(Linux) -->
                  <Checkbox
                    id="touchpad_as_ds4"
                    v-model="config.touchpad_as_ds4"
                    class="mb-3"
                    locale-prefix="config"
                    default="true"
                  ></Checkbox>
                </template>
                <!-- DS4 option: DS4 back button as touchpad click (on Automatic: Windows only) -->
                <template
                  v-if="
                    config.gamepad === 'ds4' ||
                    (config.gamepad === 'auto' && platform === 'windows')
                  "
                >
                  <Checkbox
                    id="ds4_back_as_touchpad_click"
                    v-model="config.ds4_back_as_touchpad_click"
                    class="mb-3"
                    locale-prefix="config"
                    default="true"
                  ></Checkbox>
                </template>
                <!-- DS5 Option: Controller MAC randomization (on Automatic: Linux only) -->
                <template
                  v-if="
                    config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform === 'linux')
                  "
                >
                  <Checkbox
                    id="ds5_inputtino_randomize_mac"
                    v-model="config.ds5_inputtino_randomize_mac"
                    class="mb-3"
                    locale-prefix="config"
                    default="true"
                  ></Checkbox>
                </template>
              </div>
            </div>
          </div>
        </div>
      </template>
    </template>

    <!-- Home/Guide Button Emulation Timeout -->
    <div v-if="config.controller === 'enabled'" class="mb-4">
      <label for="back_button_timeout" class="form-label">{{
        $t('config.back_button_timeout')
      }}</label>
      <n-input-number
        id="back_button_timeout"
        v-model:value="config.back_button_timeout"
        placeholder="-1"
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.back_button_timeout_desc') }}
      </p>
    </div>

    <!-- Enable Keyboard Input -->
    <hr />
    <Checkbox
      id="keyboard"
      v-model="config.keyboard"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Key Repeat Delay-->
    <div v-if="config.keyboard === 'enabled' && platform === 'windows'" class="mb-4">
      <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
      <n-input-number
        id="key_repeat_delay"
        v-model:value="config.key_repeat_delay"
        placeholder="500"
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.key_repeat_delay_desc') }}
      </p>
    </div>

    <!-- Key Repeat Frequency-->
    <div v-if="config.keyboard === 'enabled' && platform === 'windows'" class="mb-4">
      <label for="key_repeat_frequency" class="form-label">{{
        $t('config.key_repeat_frequency')
      }}</label>
      <n-input-number
        id="key_repeat_frequency"
        v-model:value="config.key_repeat_frequency"
        :step="0.1"
        placeholder="24.9"
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.key_repeat_frequency_desc') }}
      </p>
    </div>

    <!-- Always send scancodes -->
    <Checkbox
      v-if="config.keyboard === 'enabled' && platform === 'windows'"
      id="always_send_scancodes"
      v-model="config.always_send_scancodes"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Mapping Key AltRight to Key Windows -->
    <Checkbox
      v-if="config.keyboard === 'enabled'"
      id="key_rightalt_to_key_win"
      v-model="config.key_rightalt_to_key_win"
      class="mb-3"
      locale-prefix="config"
      default="false"
    />

    <!-- Enable Mouse Input -->
    <Checkbox
      id="mouse"
      v-model="config.mouse"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- High resolution scrolling support -->
    <Checkbox
      v-if="config.mouse === 'enabled'"
      id="high_resolution_scrolling"
      v-model="config.high_resolution_scrolling"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Native pen/touch support -->
    <Checkbox
      v-if="config.mouse === 'enabled'"
      class="mb-3"
      id="native_pen_touch"
      locale-prefix="config"
      v-model="config.native_pen_touch"
      default="true"
    ></Checkbox>

    <!-- Enable Input Only Mode -->

    <Checkbox
      class="mb-3"
      id="enable_input_only_mode"
      locale-prefix="config"
      v-model="config.enable_input_only_mode"
      default="false"
    ></Checkbox>

    <!-- Enable Rumble Messages to Controllers -->

    <Checkbox
      v-if="platform === 'windows'"
      class="mb-3"
      id="forward_rumble"
      locale-prefix="config"
      v-model="config.forward_rumble"
      default="true"
    ></Checkbox>

  </div>
</template>

<style scoped></style>
