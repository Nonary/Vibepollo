<template>
  <div class="flex items-center gap-2 text-xs">
    <span
      :class="[
        'h-2.5 w-2.5 rounded-full',
        streaming
          ? 'bg-success animate-pulse ring-2 ring-success/30'
          : 'bg-dark/30 dark:bg-light/30',
      ]"
    />
    <span class="font-medium" v-text="streaming ? 'Live' : 'Idle'" />
  </div>
</template>
<script setup lang="ts">
import { computed, onMounted } from 'vue';
import { useAuthStore } from '@/stores/auth';
import { useSessionsStore } from '@/stores/sessions';

const sessionsStore = useSessionsStore();
const streaming = computed(() => sessionsStore.isStreaming);

onMounted(async () => {
  const auth = useAuthStore();
  await auth.waitForAuthentication();
  sessionsStore.startPolling();
});
</script>
<style scoped></style>
