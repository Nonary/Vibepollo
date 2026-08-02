<template>
  <div class="flex items-center gap-2">
    <label v-if="label" class="text-[11px] font-medium uppercase tracking-wider opacity-60">{{
      label
    }}</label>
    <n-select v-model:value="model" :options="selectOptions" size="small" />
  </div>
</template>
<script setup>
import { ref, onMounted, watch, computed } from 'vue';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';
import { NSelect } from 'naive-ui';
const props = defineProps({
  modelValue: { type: String, default: '__default' },
  label: { type: String, default: '' },
});
const emit = defineEmits(['update:modelValue']);
const model = ref(props.modelValue);
const clients = ref([]);
const configStore = useConfigStore();
const selectOptions = computed(() => [
  { label: 'Default', value: '__default' },
  ...clients.value.map((c) => ({ label: c.name, value: c.id })),
]);

onMounted(async () => {
  const auth = useAuthStore();
  // Only call APIs after authentication is ready
  await auth.waitForAuthentication();
  try {
    // ensure config available (some environments expect platform to be present)
    if (!configStore.config.value)
      await configStore.fetchConfig().catch(() => {
        /* ignore */
      });
    const r = await http
      .get('./api/clients/list', { validateStatus: () => true })
      .then((r) => r.data || {});
    if (r && Array.isArray(r.named_certs)) {
      clients.value = r.named_certs.map((c) => ({
        id: c.uuid,
        name: c.name || c.uuid.substring(0, 8),
      }));
    }
  } catch (e) {
    console.warn('ProfileSelector: failed to load clients', e);
  }
});

watch(model, (v) => emit('update:modelValue', v));
watch(
  () => props.modelValue,
  (v) => {
    if (v !== model.value) model.value = v;
  },
);
</script>
<style scoped></style>
