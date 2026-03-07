<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="software-encoder" class="config-page">
    <ConfigFieldRenderer setting-key="sw_preset" v-model="config.sw_preset" class="mb-4" />

    <ConfigFieldRenderer setting-key="sw_tune" v-model="config.sw_tune" class="mb-4" />
  </div>
</template>

<style scoped></style>
