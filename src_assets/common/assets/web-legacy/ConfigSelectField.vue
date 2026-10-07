<script setup lang="ts">
defineOptions({ inheritAttrs: false });

import { computed, useAttrs } from 'vue';
import { NSelect } from 'naive-ui';
import ConfigFieldShell from './ConfigFieldShell.vue';

type SelectOption = {
  label: string;
  value: string | number;
  disabled?: boolean;
};

const model = defineModel<string | number | null>({ required: true });
const attrs = useAttrs();

const props = withDefaults(
  defineProps<{
    id: string;
    label: string;
    desc?: string;
    options: SelectOption[];
    placeholder?: string;
    filterable?: boolean;
    clearable?: boolean;
    size?: 'small' | 'medium' | 'large';
  }>(),
  {
    desc: '',
    placeholder: '',
    filterable: false,
    clearable: false,
    size: 'medium',
  },
);

const searchOptions = computed(() =>
  props.options.map((option) => `${option.label ?? ''}::${option.value ?? ''}`).join('|'),
);
</script>

<template>
  <ConfigFieldShell :id="props.id" :label="props.label" :desc="props.desc">
    <template #actions><slot name="actions" /></template>
    <template #control>
      <n-select
        :id="props.id"
        v-model:value="model"
        :size="props.size"
        :options="props.options"
        :placeholder="props.placeholder"
        :filterable="props.filterable"
        :clearable="props.clearable"
        :data-search-options="searchOptions"
        v-bind="attrs"
      />
    </template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigFieldShell>
</template>
