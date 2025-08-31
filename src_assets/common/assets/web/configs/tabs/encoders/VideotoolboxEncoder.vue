<script setup lang="ts">
import { ref } from 'vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { NSelect } from 'naive-ui';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="videotoolbox-encoder" class="config-page">
    <!-- Presets -->
    <div class="mb-4">
      <label for="vt_coder" class="form-label">{{ $t('config.vt_coder') }}</label>
      <n-select
        id="vt_coder"
        v-model:value="config.vt_coder"
        :options="[
          { label: $t('config.ffmpeg_auto'), value: 'auto' },
          { label: $t('config.coder_cabac'), value: 'cabac' },
          { label: $t('config.coder_cavlc'), value: 'cavlc' },
        ]"
        :data-search-options="
          [
            { label: $t('config.ffmpeg_auto'), value: 'auto' },
            { label: $t('config.coder_cabac'), value: 'cabac' },
            { label: $t('config.coder_cavlc'), value: 'cavlc' },
          ]
            .map((o) => `${o.label}::${o.value}`)
            .join('|')
        "
      />
    </div>
    <div class="mb-4">
      <label for="vt_software" class="form-label">{{ $t('config.vt_software') }}</label>
      <n-select
        id="vt_software"
        v-model:value="config.vt_software"
        :options="[
          { label: $t('_common.auto'), value: 'auto' },
          { label: $t('_common.disabled'), value: 'disabled' },
          { label: $t('config.vt_software_allowed'), value: 'allowed' },
          { label: $t('config.vt_software_forced'), value: 'forced' },
        ]"
        :data-search-options="
          [
            { label: $t('_common.auto'), value: 'auto' },
            { label: $t('_common.disabled'), value: 'disabled' },
            { label: $t('config.vt_software_allowed'), value: 'allowed' },
            { label: $t('config.vt_software_forced'), value: 'forced' },
          ]
            .map((o) => `${o.label}::${o.value}`)
            .join('|')
        "
      />
    </div>
    <Checkbox
      id="vt_realtime"
      v-model="config.vt_realtime"
      class="mb-4"
      desc=""
      locale-prefix="config"
      default="true"
    />
  </div>
</template>

<style scoped></style>
