<script setup lang="ts">
defineOptions({ inheritAttrs: false });

import { computed, useAttrs } from 'vue';
import { useI18n } from 'vue-i18n';
import Checkbox from '@/Checkbox.vue';
import ConfigDurationField from '@/ConfigDurationField.vue';
import ConfigInputField from '@/ConfigInputField.vue';
import ConfigNumberField from '@/ConfigNumberField.vue';
import ConfigSelectField from '@/ConfigSelectField.vue';
import ConfigSwitchField from '@/ConfigSwitchField.vue';
import { useConfigStore } from '@/stores/config';
import {
  getConfigFieldDefinition,
  prettifyConfigKey,
  type ConfigFieldSchemaContext,
  type ConfigFieldKind,
} from '@/configs/configFieldSchema';
import type { ConfigSelectOption } from '@/configs/configSelectOptions';

const model = defineModel<unknown>({ required: true });
const attrs = useAttrs();
const store = useConfigStore();
const { t } = useI18n();

const props = defineProps<{
  settingKey: string;
  label?: string;
  desc?: string;
  kind?: ConfigFieldKind;
  size?: 'small' | 'medium' | 'large';
  placeholder?: string;
  options?: ConfigSelectOption[];
  filterable?: boolean;
  clearable?: boolean;
  monospace?: boolean;
  autosize?: boolean | { minRows: number; maxRows: number };
  inputmode?: string;
  min?: number;
  max?: number;
  step?: number;
  precision?: number;
  defaultValue?: unknown;
  inverseValues?: boolean;
  localePrefix?: string;
}>();

function translateLabel(key: string): string {
  const translationKey = `config.${key}`;
  const value = t(translationKey);
  if (!value || value === translationKey) return prettifyConfigKey(key);
  return value;
}

function translateDesc(key: string): string {
  const translationKey = `config.${key}_desc`;
  const value = t(translationKey);
  if (!value || value === translationKey) return '';
  return value;
}

const platform = computed(() =>
  String(store.metadata?.platform || (store.config as any)?.platform || '').toLowerCase(),
);

const resolvedDefaultValue = computed(() => {
  if (props.defaultValue !== undefined) return props.defaultValue;
  return (store.defaults as any)?.[props.settingKey];
});

const field = computed(() => {
  const context: ConfigFieldSchemaContext = {
    t,
    platform: platform.value,
    metadata: store.metadata,
    currentValue: model.value,
  };

  if (resolvedDefaultValue.value !== undefined) {
    context.defaultValue = resolvedDefaultValue.value;
  }
  if (props.kind !== undefined) {
    context.kind = props.kind;
  }
  if (props.options !== undefined) {
    context.options = props.options;
  }

  return getConfigFieldDefinition(props.settingKey, context);
});

const resolvedLabel = computed(() =>
  props.label !== undefined ? props.label : translateLabel(props.settingKey),
);
const resolvedDesc = computed(() =>
  props.desc !== undefined ? props.desc : translateDesc(props.settingKey),
);
const resolvedSize = computed(() => props.size ?? 'medium');
const resolvedPlaceholder = computed(() => props.placeholder ?? field.value.placeholder ?? '');
const resolvedFilterable = computed(() => props.filterable ?? field.value.filterable ?? false);
const resolvedClearable = computed(() => props.clearable ?? field.value.clearable ?? false);
const resolvedMonospace = computed(() => props.monospace ?? field.value.monospace ?? false);
const resolvedAutosize = computed(() => props.autosize ?? field.value.autosize ?? false);
const resolvedInputMode = computed(() => props.inputmode ?? field.value.inputmode ?? '');
const resolvedMin = computed(() => props.min ?? field.value.min);
const resolvedMax = computed(() => props.max ?? field.value.max);
const resolvedStep = computed(() => props.step ?? field.value.step);
const resolvedPrecision = computed(() => props.precision ?? field.value.precision);
const resolvedLocalePrefix = computed(
  () => props.localePrefix ?? field.value.localePrefix ?? 'config',
);
const resolvedInverseValues = computed(
  () => props.inverseValues ?? field.value.inverseValues ?? false,
);
const resolvedOptions = computed(() => props.options ?? field.value.options ?? []);
const resolvedNumberProps = computed(() => ({
  ...(resolvedMin.value !== undefined ? { min: resolvedMin.value } : {}),
  ...(resolvedMax.value !== undefined ? { max: resolvedMax.value } : {}),
  ...(resolvedStep.value !== undefined ? { step: resolvedStep.value } : {}),
  ...(resolvedPrecision.value !== undefined ? { precision: resolvedPrecision.value } : {}),
}));
const mergedNumberAttrs = computed(() => ({
  ...resolvedNumberProps.value,
  ...attrs,
}));

const stringModel = computed<string>({
  get() {
    if (typeof model.value === 'string') return model.value;
    if (model.value === null || model.value === undefined) return '';
    return String(model.value);
  },
  set(value) {
    model.value = value;
  },
});

const numberModel = computed<number | null>({
  get() {
    if (typeof model.value === 'number' && Number.isFinite(model.value)) return model.value;
    if (typeof model.value === 'string') {
      const parsed = Number(model.value);
      if (Number.isFinite(parsed)) return parsed;
    }
    return null;
  },
  set(value) {
    model.value = value;
  },
});

const selectModel = computed<string | number | null>({
  get() {
    if (
      typeof model.value === 'string' ||
      (typeof model.value === 'number' && Number.isFinite(model.value))
    ) {
      return model.value;
    }
    return resolvedOptions.value[0]?.value ?? null;
  },
  set(value) {
    model.value = value;
  },
});

const switchModel = computed<boolean>({
  get() {
    return Boolean(model.value);
  },
  set(value) {
    model.value = value;
  },
});
</script>

<template>
  <Checkbox
    v-if="field.kind === 'checkbox'"
    :id="props.settingKey"
    v-model="model"
    :label="resolvedLabel"
    :desc="resolvedDesc"
    :default="resolvedDefaultValue"
    :locale-prefix="resolvedLocalePrefix"
    :inverse-values="resolvedInverseValues"
    v-bind="attrs"
  >
    <template #actions><slot name="actions" /></template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </Checkbox>

  <ConfigSwitchField
    v-else-if="field.kind === 'switch'"
    :id="props.settingKey"
    v-model="switchModel"
    :label="resolvedLabel"
    :desc="resolvedDesc"
    :size="resolvedSize"
    v-bind="attrs"
  >
    <template #actions><slot name="actions" /></template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigSwitchField>

  <ConfigSelectField
    v-else-if="field.kind === 'select'"
    :id="props.settingKey"
    v-model="selectModel"
    :label="resolvedLabel"
    :desc="resolvedDesc"
    :size="resolvedSize"
    :options="resolvedOptions"
    :placeholder="resolvedPlaceholder"
    :filterable="resolvedFilterable"
    :clearable="resolvedClearable"
    v-bind="attrs"
  >
    <template #actions><slot name="actions" /></template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigSelectField>

  <ConfigDurationField
    v-else-if="field.kind === 'number' && field.durationUnit === 'seconds'"
    :id="props.settingKey"
    v-model="numberModel"
    :label="resolvedLabel"
    :desc="resolvedDesc"
    :size="resolvedSize"
    :min="resolvedMin"
    :max="resolvedMax"
    v-bind="attrs"
  >
    <template #actions><slot name="actions" /></template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigDurationField>

  <ConfigNumberField
    v-else-if="field.kind === 'number'"
    :id="props.settingKey"
    v-model="numberModel"
    :label="resolvedLabel"
    :desc="resolvedDesc"
    :size="resolvedSize"
    :placeholder="resolvedPlaceholder"
    v-bind="mergedNumberAttrs"
  >
    <template #actions><slot name="actions" /></template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigNumberField>

  <ConfigInputField
    v-else
    :id="props.settingKey"
    v-model="stringModel"
    :label="resolvedLabel"
    :desc="resolvedDesc"
    :size="resolvedSize"
    :type="field.kind === 'textarea' ? 'textarea' : 'text'"
    :placeholder="resolvedPlaceholder"
    :clearable="resolvedClearable"
    :monospace="resolvedMonospace"
    :autosize="resolvedAutosize"
    :inputmode="resolvedInputMode"
    v-bind="attrs"
  >
    <template #actions><slot name="actions" /></template>
    <template #meta><slot name="meta" /></template>
    <slot />
  </ConfigInputField>
</template>
