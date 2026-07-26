<script setup lang="ts">
import { $tp } from '@/platform-i18n';
import PlatformLayout from '@/PlatformLayout.vue';
import { NInput, NSelect } from 'naive-ui';
import { useI18n } from 'vue-i18n';

import { useConfigStore } from '@/stores/config';
import { computed } from 'vue';
const store = useConfigStore();
const config = store.config;
const platform = computed(() => config.platform || '');
const { t } = useI18n();

type GpuOption = {
  label: string;
  value: string;
  displayName: string;
  detail: string;
};

function formatVideoMemory(bytes: unknown): string {
  const value = Number(bytes ?? 0);
  if (!Number.isFinite(value) || value <= 0) return '';
  const gib = value / (1024 * 1024 * 1024);
  return `${gib >= 10 ? Math.round(gib) : gib.toFixed(1)} GiB`;
}

function vendorName(vendorId: unknown): string {
  switch (Number(vendorId ?? 0)) {
    case 0x10de:
      return 'NVIDIA';
    case 0x1002:
    case 0x1022:
      return 'AMD';
    case 0x8086:
      return 'Intel';
    default:
      return '';
  }
}

// The host already reports its DXGI adapters through /api/metadata, so the picker needs no
// extra round trip: it just turns that list into selectable options.
const gpuOptions = computed<GpuOption[]>(() => {
  const automatic = t('config.adapter_name_default');
  const options: GpuOption[] = [
    { label: automatic, value: '', displayName: automatic, detail: '' },
  ];

  const detected = store.metadata?.gpus;
  const gpus = Array.isArray(detected) ? detected : [];
  for (const gpu of gpus) {
    const description = String(gpu?.description ?? '').trim();
    if (!description) continue;
    if (options.some((option) => option.value === description)) continue;
    const detail = [vendorName(gpu?.vendor_id), formatVideoMemory(gpu?.dedicated_video_memory)]
      .filter(Boolean)
      .join(' • ');
    options.push({ label: description, value: description, displayName: description, detail });
  }

  // Keep a name that is configured but not currently enumerated (GPU removed, driver reinstall,
  // typo) selectable so opening this page never silently drops the user's choice.
  const selected = String(config.adapter_name || '').trim();
  if (selected && !options.some((option) => option.value === selected)) {
    options.push({
      label: selected,
      value: selected,
      displayName: selected,
      detail: t('config.adapter_name_not_detected'),
    });
  }

  return options;
});
</script>

<template>
  <div v-if="platform !== 'macos'" class="mb-4">
    <label for="adapter_name" class="form-label">{{ $t('config.adapter_name') }}</label>
    <PlatformLayout>
      <template #windows>
        <n-select
          id="adapter_name"
          v-model:value="config.adapter_name"
          :options="gpuOptions"
          :placeholder="$t('config.adapter_name_default')"
          clearable
          filterable
          tag
        >
          <template #option="{ option }">
            <div class="leading-tight">
              <div>{{ option?.displayName || option?.label }}</div>
              <div v-if="option?.detail" class="text-[12px] opacity-60 font-mono">
                {{ option.detail }}
              </div>
            </div>
          </template>
        </n-select>
      </template>
      <template #freebsd>
        <n-input
          id="adapter_name"
          v-model:value="config.adapter_name"
          type="text"
          :placeholder="$tp('config.adapter_name_placeholder', '/dev/dri/renderD128')"
        />
      </template>
      <template #linux>
        <n-input
          id="adapter_name"
          v-model:value="config.adapter_name"
          type="text"
          :placeholder="$tp('config.adapter_name_placeholder', '/dev/dri/renderD128')"
        />
      </template>
    </PlatformLayout>
    <div class="text-[11px] opacity-60">
      <PlatformLayout>
        <template #windows>
          {{ $t('config.adapter_name_desc_windows') }}
        </template>
        <template #freebsd>
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
