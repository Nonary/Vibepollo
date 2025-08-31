<script setup lang="ts">
import { computed } from 'vue';
import { useConfigStore } from '@/stores/config';

// Read platform from centralized config store metadata
const store = useConfigStore();
const platform = computed(() => (store.metadata && store.metadata.platform) || '');
</script>

<template>
  <template v-if="$slots['windows'] && platform === 'windows'">
    <slot name="windows" />
  </template>

  <template v-if="$slots['linux'] && platform === 'linux'">
    <slot name="linux" />
  </template>

  <template v-if="$slots['macos'] && platform === 'macos'">
    <slot name="macos" />
  </template>
</template>

<style scoped></style>
