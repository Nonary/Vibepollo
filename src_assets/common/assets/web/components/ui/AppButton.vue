<script setup lang="ts">
// Native button semantics remain authoritative for keyboard and form behavior.
import UiIcon from './UiIcon.vue';
import type { UiIconName } from './types';
import { useI18n } from 'vue-i18n';

const { t } = useI18n();

type ButtonVariant = 'primary' | 'secondary' | 'tertiary' | 'danger';
type ButtonSize = 'compact' | 'default' | 'touch';

const props = withDefaults(
  defineProps<{
    type?: 'button' | 'submit' | 'reset';
    variant?: ButtonVariant;
    size?: ButtonSize;
    label?: string;
    ariaLabel?: string;
    icon?: UiIconName;
    iconPosition?: 'start' | 'end';
    iconOnly?: boolean;
    disabled?: boolean;
    busy?: boolean;
    busyLabel?: string;
    block?: boolean;
  }>(),
  {
    type: 'button',
    variant: 'secondary',
    size: 'default',
    iconPosition: 'start',
    iconOnly: false,
    disabled: false,
    busy: false,
    block: false,
  },
);

const emit = defineEmits<{
  click: [event: MouseEvent];
}>();

function onClick(event: MouseEvent) {
  if (props.disabled || props.busy) {
    event.preventDefault();
    event.stopPropagation();
    return;
  }
  emit('click', event);
}
</script>

<template>
  <button
    class="vs-button"
    :class="[
      'vs-button--' + variant,
      'vs-button--' + size,
      { 'vs-button--block': block, 'vs-button--icon-only': iconOnly },
    ]"
    :type="type"
    :disabled="disabled || busy"
    :aria-disabled="disabled || busy ? 'true' : undefined"
    :aria-busy="busy ? 'true' : undefined"
    :aria-label="ariaLabel || (iconOnly ? label : undefined)"
    @click="onClick"
  >
    <span v-if="busy" class="vs-button__spinner" aria-hidden="true" />
    <UiIcon
      v-else-if="icon && iconPosition === 'start'"
      class="vs-button__icon"
      :name="icon"
      aria-hidden="true"
    />
    <span :class="{ 'vs-sr-only': iconOnly }">
      <template v-if="busy">{{ busyLabel || t('ui.common.working') }}</template>
      <slot v-else>{{ label }}</slot>
    </span>
    <UiIcon
      v-if="!busy && icon && iconPosition === 'end'"
      class="vs-button__icon"
      :name="icon"
      aria-hidden="true"
    />
  </button>
</template>
