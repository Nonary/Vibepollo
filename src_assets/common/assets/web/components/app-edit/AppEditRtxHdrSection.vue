<template>
  <section
    class="rounded-2xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-surface/40 p-4 space-y-4"
  >
    <div class="flex flex-col gap-3 md:flex-row md:items-start md:justify-between">
      <div class="space-y-1">
        <h3 class="text-base font-semibold text-dark dark:text-light">
          {{ $t('config.rtx_hdr') }}
        </h3>
        <p class="text-[12px] leading-relaxed opacity-70">
          {{ $t('config.rtx_hdr_intro') }}
        </p>
      </div>
      <div class="flex flex-wrap items-center gap-2">
        <n-tag v-if="rtxHdrEnabled" size="small" type="primary">{{
          $t('_common.enabled')
        }}</n-tag>
        <n-tag v-if="liveStatus !== 'idle'" size="small" :type="liveStatusTagType">
          {{ liveStatusLabel }}
        </n-tag>
      </div>
    </div>

    <n-alert v-if="liveStatus === 'error'" type="error" size="small" :bordered="false">
      {{ liveError || $t('apps.rtx_hdr_live_update_failed') }}
    </n-alert>

    <label :class="cardClass(rtxHdrEnabled)">
      <n-switch v-model:value="rtxHdrEnabled" />
      <div class="min-w-0 space-y-1">
        <div class="text-sm font-semibold leading-snug">{{ $t('config.rtx_hdr') }}</div>
        <p class="text-[12px] leading-relaxed opacity-70">
          {{ $t('config.rtx_hdr_desc') }}
        </p>
      </div>
    </label>

    <div v-if="rtxHdrEnabled" class="space-y-4">
      <label :class="cardClass(form.rtxHdrValuesOverride)">
        <n-switch v-model:value="form.rtxHdrValuesOverride" />
        <div class="min-w-0 space-y-1">
          <div class="text-sm font-semibold leading-snug">
            {{ $t('apps.overrides.title') }}
          </div>
          <p class="text-[12px] leading-relaxed opacity-70">
            {{ $t('config.rtx_hdr_intro') }}
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
import { NAlert, NSwitch, NTag } from 'naive-ui';
import type { AppForm } from './types';

const form = defineModel<AppForm>('form', { required: true });
const props = withDefaults(
  defineProps<{
    liveStatus?: 'idle' | 'queued' | 'applying' | 'applied' | 'error';
    liveError?: string;
  }>(),
  {
    liveStatus: 'idle',
    liveError: '',
  },
);
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

const liveStatusLabel = computed(() => {
  switch (props.liveStatus) {
    case 'queued':
      return t('_common.loading');
    case 'applying':
      return t('_common.loading');
    case 'applied':
      return t('_common.success');
    case 'error':
      return t('_common.error');
    default:
      return '';
  }
});

const liveStatusTagType = computed(() => {
  switch (props.liveStatus) {
    case 'error':
      return 'error';
    case 'applied':
      return 'success';
    case 'queued':
    case 'applying':
      return 'info';
    default:
      return 'default';
  }
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
