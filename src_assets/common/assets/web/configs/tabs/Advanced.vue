<script setup lang="ts">
import { useConfigStore } from '@/stores/config';
import { useI18n } from 'vue-i18n';
import { NInputNumber, NSelect } from 'naive-ui';

const store = useConfigStore();
const config = store.config;
const { t } = useI18n();

const hevcModeOptions = [0, 1, 2, 3].map((v) => ({ labelKey: `config.hevc_mode_${v}`, value: v }));
const av1ModeOptions = [0, 1, 2, 3].map((v) => ({ labelKey: `config.av1_mode_${v}`, value: v }));
</script>

<template>
  <div class="config-page">
    <!-- FEC Percentage -->
    <div class="mb-6">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <n-input-number
        id="fec_percentage"
        v-model:value="config.fec_percentage"
        :min="1"
        :max="255"
        :step="1"
        placeholder="20"
      />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Quantization Parameter -->
    <div class="mb-6">
      <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
      <n-input-number id="qp" v-model:value="config.qp" :min="0" :step="1" placeholder="28" />
      <div class="form-text">{{ $t('config.qp_desc') }}</div>
    </div>

    <!-- Min Threads -->
    <div class="mb-6">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <n-input-number
        id="min_threads"
        v-model:value="config.min_threads"
        placeholder="2"
        :min="1"
      />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- Limit Framerate -->
    <Checkbox
      class="mb-3"
      id="limit_framerate"
      locale-prefix="config"
      v-model="config.limit_framerate"
      default="true"
    ></Checkbox>

    <!-- ENVVAR compatibility mode -->
    <Checkbox
      class="mb-3"
      id="envvar_compatibility_mode"
      locale-prefix="config"
      v-model="config.envvar_compatibility_mode"
      default="false"
    ></Checkbox>

    <!-- Legacy ordering -->
    <Checkbox
      class="mb-3"
      id="legacy_ordering"
      locale-prefix="config"
      v-model="config.legacy_ordering"
      default="false"
    ></Checkbox>

    <!-- Ignore Encoder Probe Failure -->
    <Checkbox
      class="mb-3"
      id="ignore_encoder_probe_failure"
      locale-prefix="config"
      v-model="config.ignore_encoder_probe_failure"
      default="false"
    ></Checkbox>

    <!-- HEVC Support -->
    <div class="mb-6">
      <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
      <n-select
        id="hevc_mode"
        v-model:value="config.hevc_mode"
        :options="hevcModeOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
        :data-search-options="hevcModeOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')"
      />
      <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
    </div>

    <!-- AV1 Support -->
    <div class="mb-6">
      <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
      <n-select
        id="av1_mode"
        v-model:value="config.av1_mode"
        :options="av1ModeOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
        :data-search-options="av1ModeOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')"
      />
      <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
    </div>
  </div>
</template>

<style scoped></style>
