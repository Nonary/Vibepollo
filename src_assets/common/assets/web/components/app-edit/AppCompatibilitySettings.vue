<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { AppButton, SettingRow } from '@/components/ui';
import { gamepadOptionsForPlatform } from '@/configs/settingsSchema';
import {
  appBooleanChoice,
  setAppOption,
  setAppStateCommand,
  type AppExtras,
} from '@/utils/appCompatibility';

const props = defineProps<{ modelValue: AppExtras; platform: string }>();
const emit = defineEmits<{ 'update:modelValue': [value: AppExtras] }>();
const { t } = useI18n();
const scaleEdited = ref(false);
const scaleText = ref(String(props.modelValue['scale-factor'] ?? ''));
watch(
  () => props.modelValue['scale-factor'],
  (value) => {
    scaleText.value = String(value ?? '');
  },
);
const windows = computed(() => props.platform.toLowerCase().includes('windows'));
const flags = computed(() => [
  { key: 'terminate-on-pause', label: 'apps.terminate_on_pause' },
  { key: 'allow-client-commands', label: 'apps.allow_client_commands' },
  { key: 'exclude-global-state-cmd', label: 'apps.exclude_global_state_cmd' },
  ...(windows.value
    ? [
        { key: 'use-app-identity', label: 'apps.use_app_identity' },
        { key: 'per-client-app-identity', label: 'apps.per_client_app_identity' },
      ]
    : []),
]);
const gamepads = computed(() => gamepadOptionsForPlatform(props.platform));
const gamepad = computed(() => String(props.modelValue.gamepad ?? ''));
const customGamepad = computed(
  () =>
    gamepad.value &&
    gamepad.value !== 'disabled' &&
    !gamepads.value.some((option) => option.value === gamepad.value),
);
const commands = computed<unknown[]>(() =>
  Array.isArray(props.modelValue['state-cmd']) ? props.modelValue['state-cmd'] : [],
);
function set(key: string, value: unknown): void {
  emit('update:modelValue', setAppOption(props.modelValue, key, value));
}
function selectFlag(key: string, event: Event): void {
  const value = (event.target as HTMLSelectElement).value;
  set(key, value === '' ? undefined : value === 'true' ? true : value === 'false' ? false : value);
}
function commandField(value: unknown, key: string): unknown {
  return value && typeof value === 'object' ? (value as AppExtras)[key] : undefined;
}
function updateCommand(index: number, key: string, value: unknown): void {
  emit('update:modelValue', setAppStateCommand(props.modelValue, index, key, value));
}
function scaleChanged(event: Event): void {
  scaleEdited.value = true;
  const input = event.target as HTMLInputElement;
  scaleText.value = input.value;
  if (input.value === '') set('scale-factor', undefined);
  else if (input.validity.valid) set('scale-factor', Number(input.value));
}
</script>

<template>
  <section class="app-compatibility" aria-labelledby="app-behavior-heading">
    <h2 id="app-behavior-heading">{{ t('ui.application.compatibility.title') }}</h2>
    <p>{{ t('ui.application.compatibility.description') }}</p>
    <SettingRow
      v-for="flag in flags"
      :key="flag.key"
      :label="t(flag.label)"
      :control-id="`app-${flag.key}`"
    >
      <select
        :id="`app-${flag.key}`"
        class="vs-select"
        :value="appBooleanChoice(modelValue[flag.key])"
        @change="selectFlag(flag.key, $event)"
      >
        <option value="">{{ t('ui.application.compatibility.default') }}</option>
        <option value="true">{{ t('_common.enabled') }}</option>
        <option value="false">{{ t('_common.disabled') }}</option>
        <option
          v-if="!['', 'true', 'false'].includes(appBooleanChoice(modelValue[flag.key]))"
          :value="appBooleanChoice(modelValue[flag.key])"
        >
          {{ modelValue[flag.key] }}
        </option>
      </select>
    </SettingRow>
    <SettingRow v-if="gamepads.length" :label="t('config.gamepad')" control-id="app-gamepad">
      <select
        id="app-gamepad"
        class="vs-select"
        :value="gamepad"
        @change="set('gamepad', ($event.target as HTMLSelectElement).value || undefined)"
      >
        <option value="">{{ t('_common.default_global') }}</option>
        <option value="disabled">{{ t('_common.disabled') }}</option>
        <option v-for="option in gamepads" :key="option.value" :value="option.value">
          {{ option.labelKey ? t(option.labelKey) : option.value }}
        </option>
        <option v-if="customGamepad" :value="gamepad">{{ gamepad }}</option>
      </select>
    </SettingRow>
    <SettingRow
      v-if="windows"
      :label="t('apps.resolution_scale_factor')"
      :description="t('apps.resolution_scale_factor_desc')"
      control-id="app-scale-factor"
    >
      <input
        id="app-scale-factor"
        class="vs-input"
        type="number"
        min="1"
        max="2147483647"
        step="1"
        placeholder="100"
        :data-edited="scaleEdited"
        :value="scaleText"
        @input="scaleChanged"
      />
    </SettingRow>
    <div class="state-heading">
      <div>
        <h3>{{ t('apps.cmd_state_name') }}</h3>
        <p>{{ t('apps.cmd_state_desc') }}</p>
      </div>
      <AppButton
        icon="plus"
        :label="t('ui.application.compatibility.addState')"
        @click="set('state-cmd', [...commands, { do: '', undo: '', elevated: false }])"
      />
    </div>
    <fieldset v-for="(command, index) in commands" :key="index" class="state-command">
      <legend>{{ t('ui.application.prep.legend', { number: index + 1 }) }}</legend>
      <label class="vs-field" :for="`app-state-do-${index}`">
        <span>{{ t('ui.application.compatibility.resume') }}</span>
        <input
          :id="`app-state-do-${index}`"
          class="vs-input vs-monospace"
          :value="commandField(command, 'do')"
          @input="updateCommand(index, 'do', ($event.target as HTMLInputElement).value)"
        />
      </label>
      <label class="vs-field" :for="`app-state-undo-${index}`">
        <span>{{ t('ui.application.compatibility.pause') }}</span>
        <input
          :id="`app-state-undo-${index}`"
          class="vs-input vs-monospace"
          :value="commandField(command, 'undo')"
          @input="updateCommand(index, 'undo', ($event.target as HTMLInputElement).value)"
        />
      </label>
      <label v-if="windows" class="vs-checkbox">
        <input
          type="checkbox"
          :checked="appBooleanChoice(commandField(command, 'elevated')) === 'true'"
          @change="updateCommand(index, 'elevated', ($event.target as HTMLInputElement).checked)"
        />
        <span>{{ t('ui.application.prep.elevated') }}</span>
      </label>
      <AppButton
        icon="trash"
        variant="tertiary"
        :label="t('ui.application.prep.remove', { number: index + 1 })"
        @click="
          set(
            'state-cmd',
            commands.filter((_, row) => row !== index),
          )
        "
      />
    </fieldset>
  </section>
</template>

<style scoped>
.app-compatibility {
  min-width: 0;
}
.app-compatibility > p,
.state-heading p {
  color: var(--vs-color-text-secondary);
}
.state-heading {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  flex-wrap: wrap;
  margin-top: 1.5rem;
}
.state-command {
  display: grid;
  gap: 1rem;
  min-width: 0;
  border: 0;
  margin: 1rem 0;
  padding: 0;
}
.state-command .vs-input {
  width: 100%;
  min-width: 0;
}
</style>
