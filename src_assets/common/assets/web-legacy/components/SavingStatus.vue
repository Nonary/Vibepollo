<template>
  <n-button
    v-if="visible"
    type="default"
    strong
    size="small"
    :circle="props.compact"
    class="saving-status flex items-center gap-2 text-xs select-none n-button--linkish"
    :class="{ 'cursor-pointer': canSave, 'saving-status--compact': props.compact }"
    :aria-label="label"
    :title="tooltip"
    @click="onClick"
  >
    <i :class="iconClass" />
    <span v-if="!props.compact" class="saving-status__label opacity-80">{{ label }}</span>
  </n-button>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { useI18n } from 'vue-i18n';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { NButton, useMessage } from 'naive-ui';
import { http } from '@/http';

const route = useRoute();
const props = defineProps<{
  compact?: boolean;
}>();
const store = useConfigStore();
const { savingState, manualDirty, validationError } = storeToRefs(store);
const message = useMessage();
const { t } = useI18n();
const validationMessage = computed(() =>
  validationError.value ? t(validationError.value) : t('validation.save_failed'),
);
const hasPending = computed(() => store.hasPendingPatch());
const restartRequired = computed(
  () => !!(store.lastSaveResult && store.lastSaveResult.restartRequired),
);
const intervalMs = computed(() => store.autosaveIntervalMs || 3000);
const nowMs = ref(Date.now());
const nextAt = computed(() => store.nextAutosaveAt());
const countdown = computed(() => {
  if (!hasPending.value) return 0;
  const ms = Math.max(0, nextAt.value - nowMs.value);
  return Math.ceil(ms / 1000);
});

let timer: any = null;
onMounted(() => {
  timer = setInterval(() => (nowMs.value = Date.now()), 250);
});
onUnmounted(() => {
  if (timer) clearInterval(timer);
});

const visible = computed(() => route.path === '/settings');
const canSave = computed(
  () =>
    visible.value &&
    (savingState.value === 'error' ||
      manualDirty.value === true ||
      hasPending.value === true ||
      (savingState.value === 'saved' && restartRequired.value === true)),
);

const label = computed(() => {
  if (hasPending.value) {
    return t('saving_status.autosave_in', { seconds: countdown.value });
  }
  switch (savingState.value) {
    case 'saving':
      return t('saving_status.saving');
    case 'dirty':
      return manualDirty.value
        ? t('saving_status.dirty_manual')
        : t('saving_status.dirty');
    case 'saved':
      return restartRequired.value
        ? t('saving_status.saved_restart')
        : t('saving_status.saved');
    case 'error':
      return t('saving_status.error');
    default:
      return t('saving_status.waiting');
  }
});

const iconClass = computed(() => {
  const base = 'fas text-[11px]';
  if (hasPending.value) return base + ' fa-clock text-warning';
  switch (savingState.value) {
    case 'saving':
      return base + ' fa-spinner animate-spin opacity-80';
    case 'dirty':
      return base + ' fa-circle-exclamation text-warning';
    case 'saved':
      return restartRequired.value
        ? base + ' fa-power-off text-secondary'
        : base + ' fa-check text-success';
    case 'error':
      return base + ' fa-triangle-exclamation text-danger';
    default:
      return base + ' fa-circle opacity-60 pulse-soft';
  }
});

const tooltip = computed(() => {
  if (savingState.value === 'error' && validationError.value) return validationMessage.value;
  if (hasPending.value)
    return t('saving_status.tooltip_autosave', {
      seconds: Math.round(intervalMs.value / 1000),
    });
  if (restartRequired.value)
    return t('saving_status.tooltip_restart');
  return t('saving_status.tooltip_general');
});

async function onClick() {
  if (!canSave.value) return;
  try {
    if (restartRequired.value && savingState.value === 'saved') {
      await http.post(
        '/api/restart',
        {},
        { headers: { 'Content-Type': 'application/json' }, validateStatus: () => true },
      );
      return;
    }
    if (hasPending.value) {
      const ok = await store.flushPatchQueue();
      if (!ok) {
        try {
          message.error(validationMessage.value, {
            duration: 5000,
          });
        } catch {}
      }
      return;
    }
    const ok = await store.save();
    if (!ok) {
      try {
        message.error(validationMessage.value, {
          duration: 5000,
        });
      } catch {}
    }
  } catch {}
}
</script>

<style scoped>
@keyframes pulseSoft {
  0%,
  100% {
    opacity: 0.55;
    transform: scale(1);
  }
  50% {
    opacity: 0.9;
    transform: scale(1.06);
  }
}
.pulse-soft {
  animation: pulseSoft 1.6s ease-in-out infinite;
}
</style>
