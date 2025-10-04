<script setup lang="ts">
import { ref } from 'vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { NSelect } from 'naive-ui';

const store = useConfigStore();
const config = store.config;
const presetOptions = [
  { labelKey: 'config.qsv_preset_veryfast', value: 'veryfast' },
  { labelKey: 'config.qsv_preset_faster', value: 'faster' },
  { labelKey: 'config.qsv_preset_fast', value: 'fast' },
  { labelKey: 'config.qsv_preset_medium', value: 'medium' },
  { labelKey: 'config.qsv_preset_slow', value: 'slow' },
  { labelKey: 'config.qsv_preset_slower', value: 'slower' },
  { labelKey: 'config.qsv_preset_slowest', value: 'slowest' },
];
const coderOptions = [
  { labelKey: 'config.ffmpeg_auto', value: 'auto' },
  { labelKey: 'config.coder_cabac', value: 'cabac' },
  { labelKey: 'config.coder_cavlc', value: 'cavlc' },
];
</script>

<template>
  <div id="intel-quicksync-encoder" class="config-page">
    <!-- QuickSync Preset -->
    <div class="mb-4">
      <label for="qsv_preset" class="form-label">{{ $t('config.qsv_preset') }}</label>
      <n-select
        id="qsv_preset"
        v-model:value="config.qsv_preset"
        :options="presetOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
        :data-search-options="presetOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')"
      />
    </div>

    <!-- QuickSync Coder (H264) -->
    <div class="mb-4">
      <label for="qsv_coder" class="form-label">{{ $t('config.qsv_coder') }}</label>
      <n-select
        id="qsv_coder"
        v-model:value="config.qsv_coder"
        :options="coderOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
        :data-search-options="coderOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')"
      />
    </div>

    <!-- Allow Slow HEVC Encoding -->
    <Checkbox
      id="qsv_slow_hevc"
      v-model="config.qsv_slow_hevc"
      class="mb-3"
      locale-prefix="config"
      default="false"
    />
  </div>
</template>

<style scoped></style>
