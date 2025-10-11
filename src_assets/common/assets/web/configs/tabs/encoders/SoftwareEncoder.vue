<script setup lang="ts">
import { useConfigStore } from '@/stores/config';
import { NSelect } from 'naive-ui';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="software-encoder" class="config-page">
    <div class="mb-4">
      <label for="sw_preset" class="form-label">{{ $t('config.sw_preset') }}</label>
      <n-select
        id="sw_preset"
        v-model:value="config.sw_preset"
        :options="[
          { label: $t('config.sw_preset_ultrafast'), value: 'ultrafast' },
          { label: $t('config.sw_preset_superfast'), value: 'superfast' },
          { label: $t('config.sw_preset_veryfast'), value: 'veryfast' },
          { label: $t('config.sw_preset_faster'), value: 'faster' },
          { label: $t('config.sw_preset_fast'), value: 'fast' },
          { label: $t('config.sw_preset_medium'), value: 'medium' },
          { label: $t('config.sw_preset_slow'), value: 'slow' },
          { label: $t('config.sw_preset_slower'), value: 'slower' },
          { label: $t('config.sw_preset_veryslow'), value: 'veryslow' },
        ]"
        :data-search-options="
          [
            { label: $t('config.sw_preset_ultrafast'), value: 'ultrafast' },
            { label: $t('config.sw_preset_superfast'), value: 'superfast' },
            { label: $t('config.sw_preset_veryfast'), value: 'veryfast' },
            { label: $t('config.sw_preset_faster'), value: 'faster' },
            { label: $t('config.sw_preset_fast'), value: 'fast' },
            { label: $t('config.sw_preset_medium'), value: 'medium' },
            { label: $t('config.sw_preset_slow'), value: 'slow' },
            { label: $t('config.sw_preset_slower'), value: 'slower' },
            { label: $t('config.sw_preset_veryslow'), value: 'veryslow' },
          ]
            .map((o) => `${o.label}::${o.value}`)
            .join('|')
        "
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.sw_preset_desc') }}
      </p>
    </div>

    <div class="mb-4">
      <label for="sw_tune" class="form-label">{{ $t('config.sw_tune') }}</label>
      <n-select
        id="sw_tune"
        v-model:value="config.sw_tune"
        :options="[
          { label: $t('config.sw_tune_film'), value: 'film' },
          { label: $t('config.sw_tune_animation'), value: 'animation' },
          { label: $t('config.sw_tune_grain'), value: 'grain' },
          { label: $t('config.sw_tune_stillimage'), value: 'stillimage' },
          { label: $t('config.sw_tune_fastdecode'), value: 'fastdecode' },
          { label: $t('config.sw_tune_zerolatency'), value: 'zerolatency' },
        ]"
        :data-search-options="
          [
            { label: $t('config.sw_tune_film'), value: 'film' },
            { label: $t('config.sw_tune_animation'), value: 'animation' },
            { label: $t('config.sw_tune_grain'), value: 'grain' },
            { label: $t('config.sw_tune_stillimage'), value: 'stillimage' },
            { label: $t('config.sw_tune_fastdecode'), value: 'fastdecode' },
            { label: $t('config.sw_tune_zerolatency'), value: 'zerolatency' },
          ]
            .map((o) => `${o.label}::${o.value}`)
            .join('|')
        "
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.sw_tune_desc') }}
      </p>
    </div>
  </div>
</template>

<style scoped></style>
