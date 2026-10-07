<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="vulkan-encoder" class="config-page">
    <ConfigFieldRenderer setting-key="vk_tune" v-model="config.vk_tune" class="mb-4" />
    <ConfigFieldRenderer setting-key="vk_rc_mode" v-model="config.vk_rc_mode" class="mb-3" />
  </div>
</template>
