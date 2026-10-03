<script setup lang="ts">
// Shared by the App order page and the Integrations page so both edit the same app_order_<provider> key.
import { useI18n } from 'vue-i18n';

import { APP_ORDER_SORTS, type AppOrderSort } from '@/services/appOrder';

defineProps<{ id?: string; label: string; value: AppOrderSort; disabled?: boolean }>();
const emit = defineEmits<{ change: [value: AppOrderSort] }>();
const { t } = useI18n();
</script>

<template>
  <select
    :id="id"
    class="vs-select app-order-sort"
    :aria-label="label"
    :value="value"
    :disabled="disabled"
    @change="emit('change', ($event.target as HTMLSelectElement).value as AppOrderSort)"
  >
    <option v-for="mode in APP_ORDER_SORTS" :key="mode" :value="mode">
      {{ t(`ui.appOrder.sorts.${mode}`) }}
    </option>
  </select>
</template>

<style scoped>
.app-order-sort {
  inline-size: auto;
}
</style>
