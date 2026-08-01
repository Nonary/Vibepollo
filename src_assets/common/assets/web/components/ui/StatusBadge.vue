<script setup lang="ts">
// Every tone pairs its color with a distinct icon and a required text label.
import { computed } from 'vue';
import UiIcon from './UiIcon.vue';
import type { StatusTone, UiIconName } from './types';

const props = withDefaults(
  defineProps<{
    label?: string;
    tone?: StatusTone;
    icon?: UiIconName;
    compact?: boolean;
    announce?: 'off' | 'polite' | 'assertive';
  }>(),
  {
    tone: 'neutral',
    compact: false,
    announce: 'off',
  },
);

const toneIcons: Record<StatusTone, UiIconName> = {
  neutral: 'minus',
  info: 'info',
  success: 'check-circle',
  warning: 'alert-triangle',
  danger: 'x-circle',
};

const resolvedIcon = computed(() => props.icon ?? toneIcons[props.tone]);
const live = computed(() => (props.announce === 'off' ? undefined : props.announce));
</script>

<template>
  <span
    class="vs-status-badge"
    :class="['vs-status-badge--' + tone, { 'vs-status-badge--compact': compact }]"
    :role="announce === 'assertive' ? 'alert' : announce === 'polite' ? 'status' : undefined"
    :aria-live="live"
    :aria-atomic="announce === 'off' ? undefined : 'true'"
  >
    <UiIcon class="vs-status-badge__icon" :name="resolvedIcon" aria-hidden="true" />
    <span><slot>{{ label }}</slot></span>
  </span>
</template>
