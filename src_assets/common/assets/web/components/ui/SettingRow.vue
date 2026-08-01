<script setup lang="ts">
// Slot IDs let controls preserve programmatic labels without constraining control type.
import { computed, useId, useSlots } from 'vue';
import { useI18n } from 'vue-i18n';
import StatusBadge from './StatusBadge.vue';

const { t } = useI18n();

const props = withDefaults(
  defineProps<{
    label: string;
    description?: string;
    controlId?: string;
    stacked?: boolean;
    disabled?: boolean;
    disabledReason?: string;
    restartRequired?: boolean;
    restartLabel?: string;
  }>(),
  {
    stacked: false,
    disabled: false,
    restartRequired: false,
  },
);

const slots = useSlots();
const uid = useId();
const labelId = 'vs-setting-label-' + uid;
const descriptionId = 'vs-setting-description-' + uid;
const disabledReasonId = 'vs-setting-disabled-' + uid;
const hasDescription = computed(() => Boolean(props.description || slots.description));
</script>

<template>
  <div
    class="vs-setting-row"
    :class="{
      'vs-setting-row--stacked': stacked,
      'vs-setting-row--disabled': disabled,
    }"
    :data-disabled="disabled ? 'true' : undefined"
  >
    <div class="vs-setting-row__copy">
      <label v-if="controlId" :id="labelId" class="vs-setting-row__label" :for="controlId">
        <slot name="label">{{ label }}</slot>
      </label>
      <div v-else :id="labelId" class="vs-setting-row__label">
        <slot name="label">{{ label }}</slot>
      </div>
      <div v-if="hasDescription" :id="descriptionId" class="vs-setting-row__description">
        <slot name="description">{{ description }}</slot>
      </div>
      <div v-if="disabledReason" :id="disabledReasonId" class="vs-setting-row__disabled-reason">
        {{ disabledReason }}
      </div>
      <div v-if="restartRequired || $slots.meta" class="vs-setting-row__meta">
        <StatusBadge
          v-if="restartRequired"
          :label="restartLabel || t('ui.settings.restart_required')"
          tone="warning"
          compact
        />
        <slot name="meta" />
      </div>
    </div>
    <div class="vs-setting-row__control" :aria-disabled="disabled ? 'true' : undefined">
      <slot
        :label-id="labelId"
        :description-id="hasDescription ? descriptionId : undefined"
        :disabled-reason-id="disabledReason ? disabledReasonId : undefined"
        :disabled="disabled"
      />
    </div>
  </div>
</template>
