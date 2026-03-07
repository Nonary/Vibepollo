<script setup lang="ts">
defineOptions({ inheritAttrs: false });

import { useAttrs } from 'vue';
import { NSwitch } from 'naive-ui';
import ConfigFieldShell from './ConfigFieldShell.vue';

const model = defineModel<boolean>({ required: true });
const attrs = useAttrs();

const props = withDefaults(
  defineProps<{
    id: string;
    label: string;
    desc?: string;
    size?: 'small' | 'medium' | 'large';
  }>(),
  {
    desc: '',
    size: 'medium',
  },
);
</script>

<template>
  <ConfigFieldShell :id="props.id" :label="props.label" :desc="props.desc">
    <template #actions><slot name="actions" /></template>
    <template #control>
      <n-switch :id="props.id" v-model:value="model" :size="props.size" v-bind="attrs" />
    </template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigFieldShell>
</template>
