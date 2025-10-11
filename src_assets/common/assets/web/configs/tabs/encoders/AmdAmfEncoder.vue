<script setup lang="ts">
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { NSelect } from 'naive-ui';

const store = useConfigStore();
const config = store.config;
const usageOptions = [
  { labelKey: 'config.amd_usage_transcoding', value: 'transcoding' },
  { labelKey: 'config.amd_usage_webcam', value: 'webcam' },
  { labelKey: 'config.amd_usage_lowlatency_high_quality', value: 'lowlatency_high_quality' },
  { labelKey: 'config.amd_usage_lowlatency', value: 'lowlatency' },
  { labelKey: 'config.amd_usage_ultralowlatency', value: 'ultralowlatency' },
];
const rateControlOptions = [
  { labelKey: 'config.amd_rc_cbr', value: 'cbr' },
  { labelKey: 'config.amd_rc_cqp', value: 'cqp' },
  { labelKey: 'config.amd_rc_vbr_latency', value: 'vbr_latency' },
  { labelKey: 'config.amd_rc_vbr_peak', value: 'vbr_peak' },
];
const qualityOptions = [
  { labelKey: 'config.amd_quality_speed', value: 'speed' },
  { labelKey: 'config.amd_quality_balanced', value: 'balanced' },
  { labelKey: 'config.amd_quality_quality', value: 'quality' },
];
const coderOptions = [
  { labelKey: 'config.ffmpeg_auto', value: 'auto' },
  { labelKey: 'config.coder_cabac', value: 'cabac' },
  { labelKey: 'config.coder_cavlc', value: 'cavlc' },
];
</script>

<template>
  <div id="amd-amf-encoder" class="config-page">
    <!-- AMF Usage -->
    <div class="mb-4">
      <label for="amf_quality" class="form-label">{{ $t('config.amf.quality') }}</label>
      <n-select
        id="amf_quality"
        v-model:value="config.amd_usage"
        :options="usageOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
        :data-search-options="usageOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')"
      />
      <div class="form-text">
        {{ $t('config.amd_usage_desc') }}
      </div>
    </div>

    <!-- AMD Rate Control group options -->
    <div class="mb-4 rounded-md overflow-hidden border border-dark/10 dark:border-light/10">
      <div class="bg-surface/40 dark:bg-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">
          {{ $t('config.amd_rc_group') }}
        </h3>
      </div>
      <div class="p-4">
        <!-- AMF Rate Control -->
        <div class="mb-4">
          <label for="amf_rate_control" class="form-label">{{
            $t('config.amf.rate_control')
          }}</label>
          <n-select
            id="amf_rate_control"
            v-model:value="config.amd_rc"
            :options="rateControlOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
            :data-search-options="
              rateControlOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')
            "
          />
          <p class="text-[11px] opacity-60 mt-1">
            {{ $t('config.amd_rc_desc') }}
          </p>
        </div>

        <!-- AMF HRD Enforcement -->
        <Checkbox
          id="amd_enforce_hrd"
          v-model="config.amd_enforce_hrd"
          class="mb-3"
          locale-prefix="config"
          default="false"
        />
      </div>
    </div>

    <!-- AMF Quality group options -->
    <div class="mb-4 rounded-md overflow-hidden border border-dark/10 dark:border-light/10">
      <div class="bg-surface/40 dark:bg-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">
          {{ $t('config.amd_quality_group') }}
        </h3>
      </div>
      <div class="p-4">
        <!-- AMF Quality -->
        <div class="mb-6">
          <label for="amd_quality" class="form-label">{{ $t('config.amd_quality') }}</label>
          <n-select
            id="amd_quality"
            v-model:value="config.amd_quality"
            :options="qualityOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
            :data-search-options="
              qualityOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')
            "
          />
          <p class="text-[11px] opacity-60 mt-1">
            {{ $t('config.amd_quality_desc') }}
          </p>
        </div>

        <!-- AMD Preanalysis -->
        <Checkbox
          id="amd_preanalysis"
          v-model="config.amd_preanalysis"
          class="mb-3"
          locale-prefix="config"
          default="false"
        />

        <!-- AMD VBAQ -->
        <Checkbox
          id="amd_vbaq"
          v-model="config.amd_vbaq"
          class="mb-3"
          locale-prefix="config"
          default="true"
        />

        <!-- AMF Coder (H264) -->
        <div class="mb-6">
          <label for="amd_coder" class="form-label">{{ $t('config.amd_coder') }}</label>
          <n-select
            id="amd_coder"
            v-model:value="config.amd_coder"
            :options="coderOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
            :data-search-options="
              coderOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')
            "
          />
          <p class="text-[11px] opacity-60 mt-1">
            {{ $t('config.amd_coder_desc') }}
          </p>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped></style>
