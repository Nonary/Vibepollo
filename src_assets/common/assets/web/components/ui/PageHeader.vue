<script setup lang="ts">
// A dynamic native heading keeps the component reusable without flattening document structure.
import { computed, useId, useSlots } from 'vue';

const props = withDefaults(
  defineProps<{
    title: string;
    description?: string;
    eyebrow?: string;
    headingLevel?: 1 | 2 | 3 | 4 | 5 | 6;
    compact?: boolean;
  }>(),
  {
    headingLevel: 1,
    compact: false,
  },
);

const uid = useId();
const slots = useSlots();
const titleId = 'vs-page-title-' + uid;
const descriptionId = 'vs-page-description-' + uid;
const headingTag = computed(() => 'h' + props.headingLevel);
const hasDescription = computed(() => Boolean(props.description || slots.description));
</script>

<template>
  <header
    class="vs-page-header"
    :class="{ 'vs-page-header--compact': compact }"
    :aria-labelledby="titleId"
    :aria-describedby="hasDescription ? descriptionId : undefined"
  >
    <div class="vs-page-header__main">
      <div v-if="eyebrow || $slots.eyebrow" class="vs-page-header__eyebrow">
        <slot name="eyebrow">{{ eyebrow }}</slot>
      </div>
      <component :is="headingTag" :id="titleId" class="vs-page-header__title">
        <slot name="title">{{ title }}</slot>
      </component>
      <div v-if="hasDescription" :id="descriptionId" class="vs-page-header__description">
        <slot name="description">{{ description }}</slot>
      </div>
      <div v-if="$slots.meta" class="vs-page-header__meta"><slot name="meta" /></div>
    </div>
    <div v-if="$slots.actions" class="vs-page-header__actions"><slot name="actions" /></div>
  </header>
</template>
