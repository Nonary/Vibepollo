<script setup lang="ts">
// Announcements are opt-in to avoid repeating static page content to assistive technology.
import { computed } from 'vue';
import UiIcon from './UiIcon.vue';
import type { AlertTone, UiIconName } from './types';

const props = withDefaults(
  defineProps<{
    tone?: AlertTone;
    title?: string;
    icon?: UiIconName;
    announce?: 'off' | 'polite' | 'assertive';
    dismissLabel?: string;
  }>(),
  {
    tone: 'info',
    announce: 'off',
  },
);

const emit = defineEmits<{
  dismiss: [];
}>();

const toneIcons: Record<AlertTone, UiIconName> = {
  info: 'info',
  success: 'check-circle',
  warning: 'alert-triangle',
  danger: 'x-circle',
};

const resolvedIcon = computed(() => props.icon ?? toneIcons[props.tone]);
const live = computed(() => (props.announce === 'off' ? undefined : props.announce));
</script>

<template>
  <div
    class="vs-inline-alert"
    :class="'vs-inline-alert--' + tone"
    :role="announce === 'assertive' ? 'alert' : announce === 'polite' ? 'status' : undefined"
    :aria-live="live"
    :aria-atomic="announce === 'off' ? undefined : 'true'"
  >
    <UiIcon class="vs-inline-alert__icon" :name="resolvedIcon" :size="20" aria-hidden="true" />
    <div class="vs-inline-alert__content">
      <div v-if="title || $slots.title" class="vs-inline-alert__title">
        <slot name="title">{{ title }}</slot>
      </div>
      <div class="vs-inline-alert__body"><slot /></div>
      <div v-if="$slots.actions" class="vs-inline-alert__actions"><slot name="actions" /></div>
    </div>
    <button
      v-if="dismissLabel"
      type="button"
      class="vs-icon-button vs-inline-alert__dismiss"
      :aria-label="dismissLabel"
      @click="emit('dismiss')"
    >
      <UiIcon name="x" aria-hidden="true" />
    </button>
  </div>
</template>
