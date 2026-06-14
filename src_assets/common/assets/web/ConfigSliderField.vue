<script setup lang="ts">
defineOptions({ inheritAttrs: false });

import { computed, useAttrs } from 'vue';
import { NInputNumber, NSlider } from 'naive-ui';
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
    defaultValue?: unknown;
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

const sliderProps = computed(() => ({
  ...numberProps.value,
  ...(attrs['disabled'] !== undefined ? { disabled: Boolean(attrs['disabled']) } : {}),
}));

const sliderValue = computed<number>({
  get() {
    if (typeof model.value === 'number' && Number.isFinite(model.value)) return model.value;
    if (typeof props.defaultValue === 'number' && Number.isFinite(props.defaultValue)) {
      return props.defaultValue;
    }
    return props.min ?? 0;
  },
  set(value) {
    model.value = value;
  },
});

const inputWidthClass = computed(() => (props.size === 'small' ? 'w-24' : 'w-28'));
</script>

<template>
  <ConfigFieldShell :id="props.id" :label="props.label" :desc="props.desc">
    <template #actions><slot name="actions" /></template>
    <template #control>
      <div class="flex min-h-8 items-center gap-3">
        <n-slider v-model:value="sliderValue" class="min-w-0 flex-1" v-bind="sliderProps" />
        <n-input-number
          :id="props.id"
          v-model:value="model"
          :class="['shrink-0', inputWidthClass]"
          :size="props.size"
          :placeholder="props.placeholder"
          :show-button="false"
          v-bind="mergedNumberProps"
        />
      </div>
    </template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigFieldShell>
</template>
