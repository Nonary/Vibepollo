<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div class="config-page">
    <ConfigFieldRenderer
      v-model="config.fec_percentage"
      setting-key="fec_percentage"
      class="mb-6"
    />

    <ConfigFieldRenderer v-model="config.qp" setting-key="qp" class="mb-6" />

    <ConfigFieldRenderer v-model="config.min_threads" setting-key="min_threads" class="mb-6" />

    <ConfigFieldRenderer
      v-model="config.limit_framerate"
      setting-key="limit_framerate"
      class="mb-3"
    />

    <ConfigFieldRenderer
      v-model="config.envvar_compatibility_mode"
      setting-key="envvar_compatibility_mode"
      class="mb-3"
    />

    <ConfigFieldRenderer
      v-model="config.legacy_ordering"
      setting-key="legacy_ordering"
      class="mb-3"
    />

    <ConfigFieldRenderer
      v-model="config.ignore_encoder_probe_failure"
      setting-key="ignore_encoder_probe_failure"
      class="mb-6"
    />

    <ConfigFieldRenderer v-model="config.hevc_mode" setting-key="hevc_mode" class="mb-6" />

    <ConfigFieldRenderer v-model="config.av1_mode" setting-key="av1_mode" class="mb-6" />
  </div>
</template>

<style scoped></style>
