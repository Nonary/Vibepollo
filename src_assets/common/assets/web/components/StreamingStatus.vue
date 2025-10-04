<template>
  <div class="flex items-center gap-2 text-xs">
    <span
      :class="[
        'h-2.5 w-2.5 rounded-full animate-pulse',
        streaming ? 'bg-success ring-2 ring-success/30' : 'bg-dark/30',
      ]"
    />
    <span class="font-medium" v-text="streaming ? 'Live' : 'Idle'" />
  </div>
</template>
<script setup>
import { ref, onMounted, onBeforeUnmount } from 'vue';
import { useAuthStore } from '@/stores/auth';
// Placeholder polling logic; backend endpoint not yet implemented
const streaming = ref(false);
let iv;
onMounted(() => {
  const auth = useAuthStore();
  // TODO replace with real endpoint; guard polling by auth
  iv = setInterval(() => {
    if (!auth.isAuthenticated) return;
    // placeholder polling: no state change
    void streaming.value;
  }, 10000);
});
onBeforeUnmount(() => {
  if (iv) clearInterval(iv);
});
</script>
<style scoped></style>
