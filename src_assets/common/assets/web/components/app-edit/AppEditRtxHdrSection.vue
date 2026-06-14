<template>
  <section
    class="rounded-2xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-surface/40 p-4 space-y-4"
  >
    <div class="flex flex-col gap-3 md:flex-row md:items-start md:justify-between">
      <div class="space-y-1">
        <h3 class="text-base font-semibold text-dark dark:text-light">RTX HDR</h3>
        <p class="text-[12px] leading-relaxed opacity-70">
          Enable RTX HDR conversion for this app. By default, Vibepollo uses the game's existing
          NVIDIA RTX HDR profile values when one is configured, then falls back to the global
          defaults. The host is intentionally kept in SDR so the game does not apply its own HDR on
          top of RTX HDR.
        </p>
      </div>
      <n-tag v-if="rtxHdrEnabled" size="small" type="primary"> Enabled </n-tag>
    </div>

    <label :class="cardClass(rtxHdrEnabled)">
      <n-switch v-model:value="rtxHdrEnabled" />
      <div class="min-w-0 space-y-1">
        <div class="text-sm font-semibold leading-snug">Enable RTX HDR for this app</div>
        <p class="text-[12px] leading-relaxed opacity-70">
          Converts SDR frames to HDR while this application is streamed. When enabled, Vibepollo
          forces the host/game into SDR first to prevent oversaturation from native game HDR and RTX
          HDR running at the same time.
        </p>
      </div>
    </label>

    <div v-if="rtxHdrEnabled" class="space-y-4">
      <label :class="cardClass(form.rtxHdrValuesOverride)">
        <n-switch v-model:value="form.rtxHdrValuesOverride" />
        <div class="min-w-0 space-y-1">
          <div class="text-sm font-semibold leading-snug">Override HDR values directly</div>
          <p class="text-[12px] leading-relaxed opacity-70">
            Use these sliders instead of inheriting the NVIDIA RTX HDR profile or global default
            values.
          </p>
        </div>
      </label>

      <div v-if="form.rtxHdrValuesOverride" class="grid gap-3 md:grid-cols-2">
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
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { NSwitch, NTag } from 'naive-ui';
import type { AppForm } from './types';

const form = defineModel<AppForm>('form', { required: true });
const { t } = useI18n();

const rtxHdrEnabled = computed({
  get: () => form.value.rtxHdrMode === 'enabled',
  set: (enabled: boolean) => {
    form.value.rtxHdrMode = enabled ? 'enabled' : 'disabled';
    if (!enabled) {
      form.value.rtxHdrValuesOverride = false;
    }
  },
});

function cardClass(active: boolean): string[] {
  return [
    'flex cursor-pointer items-start gap-3 rounded-xl border px-3 py-3 transition-colors',
    active
      ? 'border-primary/35 bg-primary/10 text-primary'
      : 'border-dark/10 bg-white/50 hover:border-primary/25 dark:border-light/10 dark:bg-white/5',
  ];
}
</script>
