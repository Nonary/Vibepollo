<script setup lang="ts">
import { ref, computed } from 'vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { useI18n } from 'vue-i18n';
import { NSelect, NInputNumber } from 'naive-ui';

const store = useConfigStore();
const config = store.config;
const platform = computed(() => config.value?.platform || '');
const { t } = useI18n();
const presetOptions = [
  { label: () => `P1 ${t('config.nvenc_preset_1')}`, value: 1 },
  { label: 'P2', value: 2 },
  { label: 'P3', value: 3 },
  { label: 'P4', value: 4 },
  { label: 'P5', value: 5 },
  { label: 'P6', value: 6 },
  { label: () => `P7 ${t('config.nvenc_preset_7')}`, value: 7 },
];
const twopassOptions = [
  { labelKey: 'config.nvenc_twopass_disabled', value: 'disabled' },
  { labelKey: 'config.nvenc_twopass_quarter_res', value: 'quarter_res' },
  { labelKey: 'config.nvenc_twopass_full_res', value: 'full_res' },
];
</script>

<template>
  <div id="nvidia-nvenc-encoder" class="config-page">
    <!-- Performance preset -->
    <div class="mb-4">
      <label for="nvenc_preset" class="form-label">{{ $t('config.nvenc_preset') }}</label>
      <n-select
        id="nvenc_preset"
        v-model:value="config.nvenc_preset"
        :options="
          presetOptions.map((o) => ({
            label: typeof o.label === 'function' ? o.label() : o.label,
            value: o.value,
          }))
        "
        :data-search-options="
          presetOptions
            .map((o) => `${typeof o.label === 'function' ? o.label() : o.label}::${o.value}`)
            .join('|')
        "
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.nvenc_preset_desc') }}
      </p>
    </div>

    <!-- Two-pass mode -->
    <div class="mb-4">
      <label for="nvenc_twopass" class="form-label">{{ $t('config.nvenc_twopass') }}</label>
      <n-select
        id="nvenc_twopass"
        v-model:value="config.nvenc_twopass"
        :options="twopassOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
        :data-search-options="twopassOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')"
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.nvenc_twopass_desc') }}
      </p>
    </div>

    <!-- Spatial AQ -->
    <Checkbox
      id="nvenc_spatial_aq"
      v-model="config.nvenc_spatial_aq"
      class="mb-3"
      locale-prefix="config"
      default="false"
    />

    <!-- Single-frame VBV/HRD percentage increase -->
    <div class="mb-4">
      <label for="nvenc_vbv_increase" class="form-label">{{
        $t('config.nvenc_vbv_increase')
      }}</label>
      <n-input-number
        id="nvenc_vbv_increase"
        v-model:value="config.nvenc_vbv_increase"
        :min="0"
        :max="400"
        placeholder="0"
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.nvenc_vbv_increase_desc') }}<br />
        <br />
        <a href="https://en.wikipedia.org/wiki/Video_buffering_verifier">VBV/HRD</a>
      </p>
    </div>

    <!-- Miscellaneous options -->
    <div class="mb-4 rounded-md overflow-hidden border border-dark/10 dark:border-light/10">
      <div class="bg-surface/40 dark:bg-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">
          {{ $t('config.misc') }}
        </h3>
      </div>
      <div class="p-4">
        <!-- NVENC Realtime HAGS priority -->
        <Checkbox
          v-if="platform === 'windows'"
          id="nvenc_realtime_hags"
          v-model="config.nvenc_realtime_hags"
          class="mb-3"
          locale-prefix="config"
          default="true"
        >
          <br />
          <br />
          <a href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/"
            >HAGS</a
          >
        </Checkbox>

        <!-- Prefer lower encoding latency over power savings -->
        <Checkbox
          v-if="platform === 'windows'"
          id="nvenc_latency_over_power"
          v-model="config.nvenc_latency_over_power"
          class="mb-3"
          locale-prefix="config"
          default="true"
        />

        <!-- Present OpenGL/Vulkan on top of DXGI -->
        <Checkbox
          v-if="platform === 'windows'"
          id="nvenc_opengl_vulkan_on_dxgi"
          v-model="config.nvenc_opengl_vulkan_on_dxgi"
          class="mb-3"
          locale-prefix="config"
          default="true"
        />

        <!-- NVENC H264 CAVLC -->
        <Checkbox
          id="nvenc_h264_cavlc"
          v-model="config.nvenc_h264_cavlc"
          class="mb-3"
          locale-prefix="config"
          default="false"
        />
      </div>
    </div>
  </div>
</template>

<style scoped></style>
