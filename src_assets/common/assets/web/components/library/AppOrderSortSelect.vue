<script setup lang="ts">
// Shared by the App order page and the Integrations page so both edit the same app_order_<provider> key.
import { useI18n } from 'vue-i18n';

import { APP_ORDER_SORTS, type AppOrderSort } from '@/services/appOrder';

const props = defineProps<{
  id?: string;
  label: string;
  value: AppOrderSort;
  disabled?: boolean;
}>();
const emit = defineEmits<{ change: [value: AppOrderSort] }>();

// Show the saved value until the parent passes the new one, so a failed save doesn't leave the new choice on screen.
function onChange(event: Event): void {
  const select = event.target as HTMLSelectElement;
  emit('change', select.value as AppOrderSort);
  select.value = props.value;
}
const { t } = useI18n();
</script>

<template>
  <select
    :id="id"
    class="vs-select app-order-sort"
    :aria-label="label"
    :value="value"
    :disabled="disabled"
    @change="onChange"
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
