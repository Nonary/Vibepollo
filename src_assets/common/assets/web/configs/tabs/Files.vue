<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="files" class="config-page">
    <ConfigFieldRenderer
      setting-key="file_apps"
      v-model="config.file_apps"
      class="mb-6"
      placeholder="apps.json"
    />

    <ConfigFieldRenderer
      setting-key="credentials_file"
      v-model="config.credentials_file"
      class="mb-6"
      placeholder="sunshine_state.json"
    />

    <ConfigFieldRenderer
      setting-key="log_path"
      v-model="config.log_path"
      class="mb-6"
      placeholder="sunshine.log"
    />

    <ConfigFieldRenderer
      setting-key="pkey"
      v-model="config.pkey"
      class="mb-6"
      placeholder="/dir/pkey.pem"
    />

    <ConfigFieldRenderer
      setting-key="cert"
      v-model="config.cert"
      class="mb-6"
      placeholder="/dir/cert.pem"
    />

    <ConfigFieldRenderer
      setting-key="file_state"
      v-model="config.file_state"
      class="mb-6"
      placeholder="sunshine_state.json"
    />
  </div>
</template>

<style scoped></style>
