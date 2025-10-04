<template>
  <n-button
    v-if="visible"
    type="default"
    strong
    size="small"
    class="flex items-center gap-2 text-xs select-none n-button--linkish"
    :class="{ 'cursor-pointer': canSave }"
    :title="tooltip"
    @click="onClick"
  >
    <i :class="iconClass" />
    <span class="opacity-80">{{ label }}</span>
  </n-button>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { NButton, useMessage } from 'naive-ui';
import { http } from '@/http';

const route = useRoute();
const store = useConfigStore();
const { savingState, manualDirty, validationError } = storeToRefs(store);
const message = useMessage();
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
    return `Auto-save in ${countdown.value}s (Tap to Save Now)`;
  }
  switch (savingState.value) {
    case 'saving':
      return 'Save Status: Saving…';
    case 'dirty':
      return manualDirty.value
        ? 'Save Status: Unsaved Changes (Click to Save)'
        : 'Save Status: Unsaved Changes';
    case 'saved':
      return restartRequired.value
        ? 'Save Status: Saved; Restart Required (Tap to Apply)'
        : 'Save Status: Saved';
    case 'error':
      return 'Save Status: Error (Tap to Retry)';
    default:
      return 'Save Status: Waiting for Changes';
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
  if (savingState.value === 'error' && validationError.value) return validationError.value;
  if (hasPending.value)
    return `Auto-save flushes every ${Math.round(intervalMs.value / 1000)}s. Tap to save now.`;
  if (restartRequired.value)
    return 'Saved; Restart required to apply runtime changes. Tap to apply now.';
  return 'This page auto-saves most changes as you edit. Some fields may require clicking Save.';
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
          message.error(validationError.value || 'Save failed. Check fields for errors.', {
            duration: 5000,
          });
        } catch {}
      }
      return;
    }
    const ok = await store.save();
    if (!ok) {
      try {
        message.error(validationError.value || 'Save failed. Check fields for errors.', {
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
