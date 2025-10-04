<script setup lang="ts">
import { ref } from 'vue';
import { useConfigStore } from '@/stores/config';
import { NInput } from 'naive-ui';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="files" class="config-page">
    <!-- Apps File -->
    <div class="mb-6">
      <label for="file_apps" class="form-label">{{ $t('config.file_apps') }}</label>
      <n-input
        id="file_apps"
        v-model:value="config.file_apps"
        type="text"
        placeholder="apps.json"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.file_apps_desc') }}
      </div>
    </div>

    <!-- Credentials File -->
    <div class="mb-6">
      <label for="credentials_file" class="form-label">{{ $t('config.credentials_file') }}</label>
      <n-input
        id="credentials_file"
        v-model:value="config.credentials_file"
        type="text"
        placeholder="sunshine_state.json"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.credentials_file_desc') }}
      </div>
    </div>

    <!-- Log Path -->
    <div class="mb-6">
      <label for="log_path" class="form-label">{{ $t('config.log_path') }}</label>
      <n-input
        id="log_path"
        v-model:value="config.log_path"
        type="text"
        placeholder="sunshine.log"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.log_path_desc') }}
      </div>
    </div>

    <!-- Private Key -->
    <div class="mb-6">
      <label for="pkey" class="form-label">{{ $t('config.pkey') }}</label>
      <n-input id="pkey" v-model:value="config.pkey" type="text" placeholder="/dir/pkey.pem" />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.pkey_desc') }}
      </div>
    </div>

    <!-- Certificate -->
    <div class="mb-6">
      <label for="cert" class="form-label">{{ $t('config.cert') }}</label>
      <n-input id="cert" v-model:value="config.cert" type="text" placeholder="/dir/cert.pem" />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.cert_desc') }}
      </div>
    </div>

    <!-- State File -->
    <div class="mb-6">
      <label for="file_state" class="form-label">{{ $t('config.file_state') }}</label>
      <n-input
        id="file_state"
        v-model:value="config.file_state"
        type="text"
        placeholder="sunshine_state.json"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.file_state_desc') }}
      </div>
    </div>
  </div>
</template>

<style scoped></style>
