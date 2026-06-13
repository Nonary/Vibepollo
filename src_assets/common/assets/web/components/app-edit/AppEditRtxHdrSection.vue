<template>
  <section
    class="rounded-2xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-surface/40 p-4 space-y-4"
  >
    <div class="flex flex-col gap-3 md:flex-row md:items-start md:justify-between">
      <div class="space-y-1">
        <h3 class="text-base font-semibold text-dark dark:text-light">RTX HDR</h3>
        <p class="text-[12px] leading-relaxed opacity-70">
          Choose how this application participates in RTX HDR streams. These app choices are saved
          with the app and applied only while it is running.
        </p>
      </div>
      <n-tag v-if="form.rtxHdrMode !== 'inherit'" size="small" type="primary">
        App-specific
      </n-tag>
    </div>

    <n-radio-group v-model:value="form.rtxHdrMode" class="grid gap-3">
      <label
        v-for="option in modeOptions"
        :key="option.value"
        :class="cardClass(form.rtxHdrMode === option.value)"
      >
        <n-radio :value="option.value" />
        <div class="min-w-0 space-y-1">
          <div class="text-sm font-semibold leading-snug">{{ option.label }}</div>
          <p class="text-[12px] leading-relaxed opacity-70">{{ option.desc }}</p>
        </div>
      </label>
    </n-radio-group>

    <div v-if="form.rtxHdrMode === 'enabled'" class="space-y-4">
      <ConfigFieldRenderer
        setting-key="rtx_hdr_force_sdr"
        v-model="form.rtxHdrForceSdr"
        size="small"
        :desc="t('config.rtx_hdr_force_sdr_desc')"
      />
      <div class="grid gap-3 md:grid-cols-2">
        <ConfigFieldRenderer
          setting-key="rtx_hdr_peak_brightness"
          v-model="form.rtxHdrPeakBrightness"
          size="small"
          :desc="t('config.rtx_hdr_peak_brightness_desc')"
        />
        <ConfigFieldRenderer
          setting-key="rtx_hdr_middle_gray"
          v-model="form.rtxHdrMiddleGray"
          size="small"
          :desc="t('config.rtx_hdr_middle_gray_desc')"
        />
        <ConfigFieldRenderer
          setting-key="rtx_hdr_contrast"
          v-model="form.rtxHdrContrast"
          size="small"
          :desc="t('config.rtx_hdr_contrast_desc')"
        />
        <ConfigFieldRenderer
          setting-key="rtx_hdr_saturation"
          v-model="form.rtxHdrSaturation"
          size="small"
          :desc="t('config.rtx_hdr_saturation_desc')"
        />
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useI18n } from 'vue-i18n';
import { NRadio, NRadioGroup, NTag } from 'naive-ui';
import type { AppForm, RtxHdrMode } from './types';

const form = defineModel<AppForm>('form', { required: true });
const { t } = useI18n();

const modeOptions: Array<{ value: RtxHdrMode; label: string; desc: string }> = [
  {
    value: 'inherit',
    label: 'Inherit global behavior',
    desc: 'Use the global Capture setting and any NVIDIA application profile values.',
  },
  {
    value: 'enabled',
    label: 'Enable RTX HDR for this app',
    desc: 'Apply app-specific RTX HDR values while this application is streamed.',
  },
  {
    value: 'disabled',
    label: 'Disable RTX HDR for this app',
    desc: 'Prevent RTX HDR conversion for this application even when global RTX HDR is enabled.',
  },
];

function cardClass(active: boolean): string[] {
  return [
    'flex cursor-pointer items-start gap-3 rounded-xl border px-3 py-3 transition-colors',
    active
      ? 'border-primary/35 bg-primary/10 text-primary'
      : 'border-dark/10 bg-white/50 hover:border-primary/25 dark:border-light/10 dark:bg-white/5',
  ];
}
</script>
