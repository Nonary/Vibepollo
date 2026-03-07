<script setup lang="ts">
defineOptions({ inheritAttrs: false });

import { computed, useAttrs } from 'vue';
import { NInput } from 'naive-ui';
import ConfigFieldShell from './ConfigFieldShell.vue';

const model = defineModel<string>({ required: true });
const attrs = useAttrs();

const props = withDefaults(
  defineProps<{
    id: string;
    label: string;
    desc?: string;
    placeholder?: string;
    type?: 'text' | 'textarea' | 'password';
    size?: 'small' | 'medium' | 'large';
    clearable?: boolean;
    monospace?: boolean;
    autosize?: boolean | { minRows: number; maxRows: number };
    inputmode?: string;
  }>(),
  {
    desc: '',
    placeholder: '',
    type: 'text',
    size: 'medium',
    clearable: false,
    monospace: false,
    autosize: false,
    inputmode: '',
  },
);

const inputClass = computed(() => (props.monospace ? 'font-mono' : ''));
const inputProps = computed(() => ({
  ...(props.type === 'textarea' && props.autosize ? { autosize: props.autosize } : {}),
  ...(props.inputmode ? { inputmode: props.inputmode } : {}),
}));
const mergedInputProps = computed(() => ({
  ...inputProps.value,
  ...attrs,
}));
</script>

<template>
  <ConfigFieldShell :id="props.id" :label="props.label" :desc="props.desc">
    <template #actions><slot name="actions" /></template>
    <template #control>
      <n-input
        :id="props.id"
        v-model:value="model"
        :type="props.type"
        :size="props.size"
        :placeholder="props.placeholder"
        :clearable="props.clearable"
        :class="inputClass"
        v-bind="mergedInputProps"
      />
    </template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigFieldShell>
</template>
