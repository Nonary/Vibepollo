<script setup lang="ts">
import { computed, useSlots } from 'vue';

const slots = useSlots();

const props = withDefaults(
  defineProps<{
    id?: string;
    label: string;
    desc?: string;
  }>(),
  {
    id: '',
    desc: '',
  },
);

const hasDescription = computed(() => Boolean(props.desc) || Boolean(slots['default']));
</script>

<template>
  <div class="space-y-1">
    <div class="flex items-start justify-between gap-3">
      <label v-if="props.id" :for="props.id" class="form-label">{{ props.label }}</label>
      <div v-else class="form-label">{{ props.label }}</div>
      <div v-if="$slots['actions']" class="shrink-0 pt-0.5">
        <slot name="actions" />
      </div>
    </div>

    <slot name="control" />

    <div v-if="hasDescription" class="form-text">
      <span v-if="props.desc">{{ props.desc }}</span>
      <slot />
    </div>

    <div v-if="$slots['meta']" class="text-[11px] opacity-60 mt-1">
      <slot name="meta" />
    </div>
  </div>
</template>
