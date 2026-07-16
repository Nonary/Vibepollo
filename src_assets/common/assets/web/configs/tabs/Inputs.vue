<script setup lang="ts">
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);

const platform = computed(() =>
  (metadata.value?.platform || config.value?.platform || '').toLowerCase(),
);
</script>

<template>
  <div id="input" class="config-page">
    <ConfigFieldRenderer setting-key="controller" v-model="config.controller" class="mb-3" />

    <div v-if="config.controller === 'enabled' && platform !== 'macos'" class="mb-6">
      <ConfigFieldRenderer setting-key="gamepad" v-model="config.gamepad" />
    </div>

    <template v-if="config.controller === 'enabled'">
      <template
        v-if="
          config.gamepad === 'ds4' ||
          config.gamepad === 'ds5' ||
          (config.gamepad === 'auto' && platform !== 'macos')
        "
      >
        <details
          open
          class="group mb-3 overflow-hidden rounded-md border border-dark/10 bg-light/60 dark:border-light/10 dark:bg-surface/50"
        >
          <summary
            class="flex cursor-pointer list-none items-center justify-between gap-3 px-4 py-3 text-sm font-medium [&::-webkit-details-marker]:hidden"
          >
            <span>
              {{
                $t(
                  config.gamepad === 'ds4'
                    ? 'config.gamepad_ds4_manual'
                    : config.gamepad === 'ds5'
                      ? 'config.gamepad_ds5_manual'
                      : 'config.gamepad_auto',
                )
              }}
            </span>
            <i class="fas fa-chevron-down text-xs opacity-60 transition-transform group-open:rotate-180" />
          </summary>
          <div class="border-t border-dark/10 px-4 py-4 dark:border-light/10">
            <template
              v-if="config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')"
            >
              <ConfigFieldRenderer
                setting-key="motion_as_ds4"
                v-model="config.motion_as_ds4"
                class="mb-3"
              />
              <ConfigFieldRenderer
                setting-key="touchpad_as_ds4"
                v-model="config.touchpad_as_ds4"
                class="mb-3"
              />
            </template>

            <template
              v-if="config.gamepad === 'ds4' || (config.gamepad === 'auto' && platform === 'windows')"
            >
              <ConfigFieldRenderer
                setting-key="ds4_back_as_touchpad_click"
                v-model="config.ds4_back_as_touchpad_click"
                class="mb-3"
              />
            </template>

            <template
              v-if="config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform === 'linux')"
            >
              <ConfigFieldRenderer
                setting-key="ds5_inputtino_randomize_mac"
                v-model="config.ds5_inputtino_randomize_mac"
                class="mb-3"
              />
            </template>
          </div>
        </details>
      </template>
    </template>

    <div v-if="config.controller === 'enabled'" class="mb-4">
      <ConfigFieldRenderer setting-key="back_button_timeout" v-model="config.back_button_timeout" />
    </div>

    <ConfigFieldRenderer
      v-if="config.controller === 'enabled'"
      setting-key="forward_rumble"
      v-model="config.forward_rumble"
      class="mb-3"
    />

    <hr />

    <ConfigFieldRenderer setting-key="keyboard" v-model="config.keyboard" class="mb-3" />

    <div v-if="config.keyboard === 'enabled' && platform === 'windows'" class="mb-4">
      <ConfigFieldRenderer setting-key="key_repeat_delay" v-model="config.key_repeat_delay" />
    </div>

    <div v-if="config.keyboard === 'enabled' && platform === 'windows'" class="mb-4">
      <ConfigFieldRenderer
        setting-key="key_repeat_frequency"
        v-model="config.key_repeat_frequency"
      />
    </div>

    <ConfigFieldRenderer
      v-if="config.keyboard === 'enabled' && platform === 'windows'"
      setting-key="always_send_scancodes"
      v-model="config.always_send_scancodes"
      class="mb-3"
    />

    <ConfigFieldRenderer
      v-if="config.keyboard === 'enabled'"
      setting-key="key_rightalt_to_key_win"
      v-model="config.key_rightalt_to_key_win"
      class="mb-3"
    />

    <ConfigFieldRenderer setting-key="mouse" v-model="config.mouse" class="mt-5 mb-3" />

    <ConfigFieldRenderer
      v-if="config.mouse === 'enabled'"
      setting-key="high_resolution_scrolling"
      v-model="config.high_resolution_scrolling"
      class="mb-3"
    />

    <ConfigFieldRenderer
      v-if="config.mouse === 'enabled'"
      setting-key="native_pen_touch"
      v-model="config.native_pen_touch"
      class="mb-3"
    />

    <hr />

    <ConfigFieldRenderer
      setting-key="enable_input_only_mode"
      v-model="config.enable_input_only_mode"
      class="mb-3"
    />
  </div>
</template>

<style scoped></style>
