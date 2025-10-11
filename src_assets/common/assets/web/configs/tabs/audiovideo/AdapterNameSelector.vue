<script setup lang="ts">
import { $tp } from '@/platform-i18n';
import PlatformLayout from '@/PlatformLayout.vue';
import { NInput } from 'naive-ui';

import { useConfigStore } from '@/stores/config';
import { computed } from 'vue';
const store = useConfigStore();
const config = store.config;
const platform = computed(() => config.platform || '');
</script>

<template>
  <div v-if="platform !== 'macos'" class="mb-4">
    <label for="adapter_name" class="form-label">{{ $t('config.adapter_name') }}</label>
    <n-input
      id="adapter_name"
      v-model:value="config.adapter_name"
      type="text"
      :placeholder="$tp('config.adapter_name_placeholder', '/dev/dri/renderD128')"
    />
    <div class="text-[11px] opacity-60">
      <PlatformLayout>
        <template #windows>
          {{ $t('config.adapter_name_desc_windows') }}<br />
          <pre>tools\dxgi-info.exe</pre>
        </template>
        <template #linux>
          {{ $t('config.adapter_name_desc_linux_1') }}<br />
          <pre>ls /dev/dri/renderD*  # {{ $t('config.adapter_name_desc_linux_2') }}</pre>
          <pre>
              vainfo --display drm --device /dev/dri/renderD129 | \
                grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
            </pre
          >
          {{ $t('config.adapter_name_desc_linux_3') }}<br />
          <i>VAProfileH264High : VAEntrypointEncSlice</i>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
