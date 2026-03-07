<script setup lang="ts">
defineOptions({ inheritAttrs: false });

import { computed, useAttrs } from 'vue';
import { NInputNumber } from 'naive-ui';
import ConfigFieldShell from './ConfigFieldShell.vue';

const model = defineModel<number | null>({ required: true });
const attrs = useAttrs();

const props = withDefaults(
  defineProps<{
    id: string;
    label: string;
    desc?: string;
    placeholder?: string;
    size?: 'small' | 'medium' | 'large';
    min?: number;
    max?: number;
    step?: number;
    precision?: number;
  }>(),
  {
    desc: '',
    placeholder: '',
    size: 'medium',
  },
);

const numberProps = computed(() => ({
  ...(props.min !== undefined ? { min: props.min } : {}),
  ...(props.max !== undefined ? { max: props.max } : {}),
  ...(props.step !== undefined ? { step: props.step } : {}),
  ...(props.precision !== undefined ? { precision: props.precision } : {}),
}));
const mergedNumberProps = computed(() => ({
  ...numberProps.value,
  ...attrs,
}));
</script>

<template>
  <ConfigFieldShell :id="props.id" :label="props.label" :desc="props.desc">
    <template #actions><slot name="actions" /></template>
    <template #control>
      <n-input-number
        :id="props.id"
        v-model:value="model"
        :size="props.size"
        :placeholder="props.placeholder"
        v-bind="mergedNumberProps"
      />
    </template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigFieldShell>
</template>
