<script setup lang="ts">
import { computed } from 'vue';
import { NCheckbox } from 'naive-ui';

const model = defineModel({ required: true });
const slots = defineSlots();
interface Props {
  id: string;
  label?: string | null;
  desc?: string | null;
  localePrefix?: string;
  inverseValues?: boolean;
  // Default backing value used to infer mapping when model is null/undefined
  default?: any;
}
const props = withDefaults(defineProps<Props>(), {
  label: null,
  desc: null,
  localePrefix: 'missing-prefix',
  inverseValues: false,
  default: null,
});

// Always include the mandatory class on the wrapper; user-supplied class on the
// component itself will be merged by Vue onto the root element automatically.

// Map an arbitrary value into a boolean-pair representation if recognizable.
// Returns null when the provided value cannot be interpreted.
function mapToBoolRepresentation(value: any) {
  if (value === true || value === false) return { possibleValues: [true, false], value };
  if (value === 1 || value === 0) return { possibleValues: [1, 0], value };

  const stringPairs = [
    ['true', 'false'],
    ['1', '0'],
    ['enabled', 'disabled'],
    ['enable', 'disable'],
    ['yes', 'no'],
    ['on', 'off'],
  ] as const;

  if (value === undefined || value === null) return null;
  const norm = String(value).toLowerCase().trim();
  for (const pair of stringPairs) {
    if (norm === pair[0] || norm === pair[1]) return { possibleValues: pair as any, value: norm };
  }
  return null;
}

// Determine the backing truthy/falsy values this checkbox should write to the model
const checkboxValues = computed(() => {
  // Prefer explicit model mapping
  const fromModel = mapToBoolRepresentation(model.value);
  if (fromModel) {
    const truthyIndex = props.inverseValues ? 1 : 0;
    const falsyIndex = props.inverseValues ? 0 : 1;
    return {
      truthy: fromModel.possibleValues[truthyIndex],
      falsy: fromModel.possibleValues[falsyIndex],
    };
  }
  // Fall back to provided default mapping
  const fromDefault = mapToBoolRepresentation(props.default);
  if (fromDefault) {
    const truthyIndex = props.inverseValues ? 1 : 0;
    const falsyIndex = props.inverseValues ? 0 : 1;
    return {
      truthy: fromDefault.possibleValues[truthyIndex],
      falsy: fromDefault.possibleValues[falsyIndex],
    };
  }
  // Final fallback is boolean mapping
  return { truthy: !props.inverseValues, falsy: !!props.inverseValues };
});

// Expose a real boolean for the UI, while mapping to the configured backend values
const isChecked = computed<boolean>({
  get() {
    const { truthy, falsy } = checkboxValues.value;
    const cur = model.value;
    // Treat undefined/null as default if provided
    const mapped = mapToBoolRepresentation(cur);
    if (mapped)
      return mapped.value === mapped.possibleValues[0]
        ? !props.inverseValues
        : !!props.inverseValues;

    // If model is not recognizable, try default to decide visual state
    const def = mapToBoolRepresentation(props.default);
    if (def)
      return def.value === def.possibleValues[0] ? !props.inverseValues : !!props.inverseValues;

    // Fallback: only true if equals our truthy literal
    return cur === truthy;
  },
  set(checked: boolean) {
    const { truthy, falsy } = checkboxValues.value;
    model.value = checked ? truthy : falsy;
  },
});

// For helper text showing what the default resolves to (enabled/disabled)
const parsedDefaultPropValue = (() => {
  const boolValues = mapToBoolRepresentation(props.default);
  if (boolValues) return boolValues.value === boolValues.possibleValues[0];
  return null as boolean | null;
})();

const labelField = props.label ?? `${props.localePrefix}.${props.id}`;
const descField = props.desc ?? `${props.localePrefix}.${props.id}_desc`;
const showDesc = props.desc !== '' || Object.entries(slots).length > 0;
const showDefValue = parsedDefaultPropValue !== null;
const defValue = parsedDefaultPropValue ? '_common.enabled_def_cbox' : '_common.disabled_def_cbox';
</script>

<template>
  <div class="form-check" :id="props.id">
    <!-- Hidden label for search indexing -->
    <label :for="props.id" style="display: none">{{ $t(labelField) }}</label>
    <n-checkbox :id="`${props.id}_cb`" v-model:checked="isChecked">
      {{ $t(labelField) }}
    </n-checkbox>
    <div v-if="showDefValue" class="mt-0 form-text">
      {{ $t(defValue) }}
    </div>
    <div v-if="showDesc" class="form-text">
      {{ $t(descField) }}
      <slot />
    </div>
  </div>
</template>
