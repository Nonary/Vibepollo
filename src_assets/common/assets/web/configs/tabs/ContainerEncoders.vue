<script setup lang="ts">
import { ref } from 'vue';
import NvidiaNvencEncoder from '@/configs/tabs/encoders/NvidiaNvencEncoder.vue';
import IntelQuickSyncEncoder from '@/configs/tabs/encoders/IntelQuickSyncEncoder.vue';
import AmdAmfEncoder from '@/configs/tabs/encoders/AmdAmfEncoder.vue';
import VideotoolboxEncoder from '@/configs/tabs/encoders/VideotoolboxEncoder.vue';
import SoftwareEncoder from '@/configs/tabs/encoders/SoftwareEncoder.vue';
import VAAPIEncoder from '@/configs/tabs/encoders/VAAPIEncoder.vue';

const props = defineProps({
  currentTab: { type: String, default: '' },
});

import { useConfigStore } from '@/stores/config';
const store = useConfigStore();
const config = store.config;
// Fallback: if no currentTab provided, show all stacked (modern single page mode)
const showAll = () => !props.currentTab;
</script>

<template>
  <!-- NVIDIA NVENC Encoder Tab -->
  <NvidiaNvencEncoder v-if="showAll() || currentTab === 'nv'" />

  <!-- Intel QuickSync Encoder Tab -->
  <IntelQuickSyncEncoder v-if="showAll() || currentTab === 'qsv'" />

  <!-- AMD AMF Encoder Tab -->
  <AmdAmfEncoder v-if="showAll() || currentTab === 'amd'" />

  <!-- VideoToolbox Encoder Tab -->
  <VideotoolboxEncoder v-if="showAll() || currentTab === 'vt'" />

  <!-- VAAPI Encoder Tab -->
  <VAAPIEncoder v-if="showAll() || currentTab === 'vaapi'" />

  <!-- Software Encoder Tab -->
  <SoftwareEncoder v-if="showAll() || currentTab === 'sw'" />
</template>

<style scoped></style>
