<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="vaapi-encoder" class="config-page">
    <ConfigFieldRenderer
      setting-key="vaapi_strict_rc_buffer"
      v-model="config.vaapi_strict_rc_buffer"
      class="mb-3"
    />
  </div>
</template>

<style scoped></style>
