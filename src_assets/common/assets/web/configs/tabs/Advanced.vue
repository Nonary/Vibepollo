<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div class="config-page">
    <ConfigFieldRenderer
      setting-key="fec_percentage"
      v-model="config.fec_percentage"
      class="mb-6"
    />

    <ConfigFieldRenderer setting-key="qp" v-model="config.qp" class="mb-6" />

    <ConfigFieldRenderer setting-key="min_threads" v-model="config.min_threads" class="mb-6" />

    <ConfigFieldRenderer
      setting-key="limit_framerate"
      v-model="config.limit_framerate"
      class="mb-3"
    />

    <ConfigFieldRenderer
      setting-key="envvar_compatibility_mode"
      v-model="config.envvar_compatibility_mode"
      class="mb-3"
    />

    <ConfigFieldRenderer
      setting-key="legacy_ordering"
      v-model="config.legacy_ordering"
      class="mb-3"
    />

    <ConfigFieldRenderer
      setting-key="ignore_encoder_probe_failure"
      v-model="config.ignore_encoder_probe_failure"
      class="mb-6"
    />

    <ConfigFieldRenderer setting-key="hevc_mode" v-model="config.hevc_mode" class="mb-6" />

    <ConfigFieldRenderer setting-key="av1_mode" v-model="config.av1_mode" class="mb-6" />
  </div>
</template>

<style scoped></style>
