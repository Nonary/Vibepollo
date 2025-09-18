<script setup lang="ts">
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import { useI18n } from 'vue-i18n';
import { NSelect } from 'naive-ui';
import NvidiaNvencEncoder from '@/configs/tabs/encoders/NvidiaNvencEncoder.vue';
import IntelQuickSyncEncoder from '@/configs/tabs/encoders/IntelQuickSyncEncoder.vue';
import AmdAmfEncoder from '@/configs/tabs/encoders/AmdAmfEncoder.vue';
import VideotoolboxEncoder from '@/configs/tabs/encoders/VideotoolboxEncoder.vue';
import SoftwareEncoder from '@/configs/tabs/encoders/SoftwareEncoder.vue';
import VAAPIEncoder from '@/configs/tabs/encoders/VAAPIEncoder.vue';
import { useConfigStore } from '@/stores/config';

const props = defineProps({
  currentTab: { type: String, default: '' },
});

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const { t } = useI18n();

// Fallback: if no currentTab provided, show all stacked (modern single page mode)
const showAll = () => !props.currentTab;

const platform = computed(() =>
  (metadata.value?.platform || config.value?.platform || '').toString().toLowerCase(),
);

const gpuList = computed(() => {
  const raw = (metadata.value as any)?.gpus;
  return Array.isArray(raw) ? raw : [];
});

const hasNvidia = computed(() => {
  const metaFlag = (metadata.value as any)?.has_nvidia_gpu;
  if (typeof metaFlag === 'boolean') return metaFlag;
  if (gpuList.value.length) {
    return gpuList.value.some(
      (gpu: any) => Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0) === 0x10de,
    );
  }
  return true;
});

const hasIntel = computed(() => {
  const metaFlag = (metadata.value as any)?.has_intel_gpu;
  if (typeof metaFlag === 'boolean') return metaFlag;
  if (gpuList.value.length) {
    return gpuList.value.some(
      (gpu: any) => Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0) === 0x8086,
    );
  }
  return true;
});

const hasAmd = computed(() => {
  const metaFlag = (metadata.value as any)?.has_amd_gpu;
  if (typeof metaFlag === 'boolean') return metaFlag;
  if (gpuList.value.length) {
    return gpuList.value.some((gpu: any) => {
      const vendor = Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0);
      return vendor === 0x1002 || vendor === 0x1022;
    });
  }
  return true;
});

const captureOptions = computed(() => {
  const opts = [{ label: t('_common.autodetect'), value: '' }];
  switch (platform.value) {
    case 'windows':
      opts.push(
        { label: 'Windows Graphics Capture (variable)', value: 'wgc' },
        { label: 'Windows Graphics Capture (constant)', value: 'wgcc' },
        { label: 'Desktop Duplication API', value: 'ddx' },
      );
      break;
    case 'linux':
      opts.push(
        { label: 'NvFBC', value: 'nvfbc' },
        { label: 'wlroots', value: 'wlr' },
        { label: 'KMS', value: 'kms' },
        { label: 'X11', value: 'x11' },
      );
      break;
    default:
      break;
  }
  return opts;
});

const encoderOptions = computed(() => {
  const opts = [{ label: t('_common.autodetect'), value: '' }];
  if (platform.value === 'windows') {
    if (hasNvidia.value) opts.push({ label: 'NVIDIA NVENC', value: 'nvenc' });
    if (hasIntel.value) opts.push({ label: 'Intel QuickSync', value: 'quicksync' });
    if (hasAmd.value) opts.push({ label: 'AMD AMF/VCE', value: 'amdvce' });
  } else if (platform.value === 'linux') {
    opts.push({ label: 'NVIDIA NVENC', value: 'nvenc' }, { label: 'VA-API', value: 'vaapi' });
  } else if (platform.value === 'macos') {
    opts.push({ label: 'VideoToolbox', value: 'videotoolbox' });
  }
  opts.push({ label: t('config.encoder_software'), value: 'software' });
  return opts;
});

const shouldShowNvenc = computed(() => (showAll() || props.currentTab === 'nv') && hasNvidia.value);
const shouldShowQsv = computed(
  () => (showAll() || props.currentTab === 'qsv') && hasIntel.value && platform.value === 'windows',
);
const shouldShowAmd = computed(
  () => (showAll() || props.currentTab === 'amd') && hasAmd.value && platform.value === 'windows',
);
const shouldShowVideotoolbox = computed(
  () => (showAll() || props.currentTab === 'vt') && platform.value === 'macos',
);
const shouldShowVaapi = computed(
  () => (showAll() || props.currentTab === 'vaapi') && platform.value === 'linux',
);
const shouldShowSoftware = computed(() => showAll() || props.currentTab === 'sw');
</script>

<template>
  <div class="config-page space-y-6">
    <div class="space-y-4">
      <div>
        <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
        <n-select
          id="capture"
          v-model:value="config.capture"
          :options="captureOptions"
          :data-search-options="captureOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
        />
        <n-text depth="3" class="text-[11px] block mt-1">{{ $t('config.capture_desc') }}</n-text>
      </div>
      <div>
        <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
        <n-select
          id="encoder"
          v-model:value="config.encoder"
          :options="encoderOptions"
          :data-search-options="encoderOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
        />
        <n-text depth="3" class="text-[11px] block mt-1">{{ $t('config.encoder_desc') }}</n-text>
      </div>
    </div>

    <div v-if="shouldShowNvenc" class="encoder-outline">
      <NvidiaNvencEncoder />
    </div>

    <div v-if="shouldShowQsv" class="encoder-outline">
      <IntelQuickSyncEncoder />
    </div>

    <AmdAmfEncoder v-if="shouldShowAmd" />
    <VideotoolboxEncoder v-if="shouldShowVideotoolbox" />
    <VAAPIEncoder v-if="shouldShowVaapi" />

    <div v-if="shouldShowSoftware" class="encoder-outline">
      <SoftwareEncoder />
    </div>
  </div>
</template>

<style scoped>
.encoder-outline {
  @apply border border-dark/35 dark:border-light/25 rounded-xl p-4 bg-light/60 dark:bg-dark/40 space-y-4;
}
</style>
