<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="videotoolbox-encoder" class="config-page">
    <ConfigFieldRenderer setting-key="vt_coder" v-model="config.vt_coder" class="mb-4" />

    <ConfigFieldRenderer setting-key="vt_software" v-model="config.vt_software" class="mb-4" />

    <ConfigFieldRenderer setting-key="vt_realtime" v-model="config.vt_realtime" class="mb-4" />
  </div>
</template>

<style scoped></style>
