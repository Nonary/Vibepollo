<script setup lang="ts">
// Empty states keep primary recovery actions close to their explanation.
import { computed, useId, useSlots } from 'vue';
import UiIcon from './UiIcon.vue';
import type { UiIconName } from './types';

const props = withDefaults(
  defineProps<{
    title: string;
    description?: string;
    icon?: UiIconName;
    compact?: boolean;
    announce?: 'off' | 'polite';
  }>(),
  {
    icon: 'info',
    compact: false,
    announce: 'off',
  },
);

const slots = useSlots();
const uid = useId();
const titleId = 'vs-empty-title-' + uid;
const descriptionId = 'vs-empty-description-' + uid;
const hasDescription = computed(() => Boolean(props.description || slots.description));
</script>

<template>
  <section
    class="vs-empty-state"
    :class="{ 'vs-empty-state--compact': compact }"
    :aria-labelledby="titleId"
    :aria-describedby="hasDescription ? descriptionId : undefined"
    :role="announce === 'polite' ? 'status' : undefined"
    :aria-live="announce === 'polite' ? 'polite' : undefined"
  >
    <div class="vs-empty-state__icon" aria-hidden="true">
      <UiIcon :name="icon" :size="24" />
    </div>
    <h2 :id="titleId" class="vs-empty-state__title">{{ title }}</h2>
    <div v-if="hasDescription" :id="descriptionId" class="vs-empty-state__description">
      <slot name="description">{{ description }}</slot>
    </div>
    <div v-if="$slots.actions || $slots.default" class="vs-empty-state__actions">
      <slot name="actions"><slot /></slot>
    </div>
  </section>
</template>
