<script setup lang="ts">
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="vaapi-encoder" class="config-page">
    <!-- Strict RC Buffer -->
    <Checkbox
      id="vaapi_strict_rc_buffer"
      v-model="config.vaapi_strict_rc_buffer"
      class="mb-3"
      locale-prefix="config"
      default="false"
    />
  </div>
</template>

<style scoped></style>
