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
                <template
                  v-if="
                    config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')
                  "
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
                  v-if="
                    config.gamepad === 'ds4' ||
                    (config.gamepad === 'auto' && platform === 'windows')
                  "
                >
                  <ConfigFieldRenderer
                    setting-key="ds4_back_as_touchpad_click"
                    v-model="config.ds4_back_as_touchpad_click"
                    class="mb-3"
                  />
                </template>

                <template
                  v-if="
                    config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform === 'linux')
                  "
                >
                  <ConfigFieldRenderer
                    setting-key="ds5_inputtino_randomize_mac"
                    v-model="config.ds5_inputtino_randomize_mac"
                    class="mb-3"
                  />
                </template>
              </div>
            </div>
          </div>
        </div>
      </template>
    </template>

    <div v-if="config.controller === 'enabled'" class="mb-4">
      <ConfigFieldRenderer setting-key="back_button_timeout" v-model="config.back_button_timeout" />
    </div>

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

    <hr />

    <ConfigFieldRenderer setting-key="mouse" v-model="config.mouse" class="mb-3" />

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
  </div>
</template>

<style scoped></style>
