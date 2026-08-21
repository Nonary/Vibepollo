<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';

import { AppButton, SettingRow } from '@/components/ui';

interface ClientCommandDraft {
  command: string;
  elevated?: boolean;
}

type CommandList = 'do' | 'undo';

const props = withDefaults(
  defineProps<{
    allowClientCommands: boolean;
    doCommands: ClientCommandDraft[];
    undoCommands: ClientCommandDraft[];
    platform?: string;
    disabled?: boolean;
    controlIdPrefix: string;
  }>(),
  { platform: '', disabled: false },
);

const emit = defineEmits<{
  'update:allowClientCommands': [value: boolean];
  'update:doCommands': [value: ClientCommandDraft[]];
  'update:undoCommands': [value: ClientCommandDraft[]];
}>();

const { t } = useI18n();

const isWindows = computed(() => props.platform.toLocaleLowerCase().includes('windows'));
const commandSections: Array<{
  key: CommandList;
  labelKey: string;
  descriptionKey: string;
}> = [
  {
    key: 'do',
    labelKey: 'pin.client_do_cmd',
    descriptionKey: 'pin.client_do_cmd_desc',
  },
  {
    key: 'undo',
    labelKey: 'pin.client_undo_cmd',
    descriptionKey: 'pin.client_undo_cmd_desc',
  },
];

function commandsFor(key: CommandList): ClientCommandDraft[] {
  return key === 'do' ? props.doCommands : props.undoCommands;
}

function updateCommands(key: CommandList, next: ClientCommandDraft[]): void {
  emit(key === 'do' ? 'update:doCommands' : 'update:undoCommands', next);
}

function addCommand(key: CommandList): void {
  updateCommands(key, [...commandsFor(key), { command: '', elevated: false }]);
}

function removeCommand(key: CommandList, index: number): void {
  updateCommands(
    key,
    commandsFor(key).filter((_, rowIndex) => rowIndex !== index),
  );
}

function updateCommand(key: CommandList, index: number, value: string): void {
  updateCommands(
    key,
    commandsFor(key).map((row, rowIndex) =>
      rowIndex === index ? { ...row, command: value } : row,
    ),
  );
}

function updateElevated(key: CommandList, index: number, value: boolean): void {
  updateCommands(
    key,
    commandsFor(key).map((row, rowIndex) =>
      rowIndex === index ? { ...row, elevated: value } : row,
    ),
  );
}
</script>

<template>
  <section
    class="client-command-editor"
    :aria-labelledby="`${controlIdPrefix}-commands-title`"
  >
    <div class="client-command-editor__heading">
      <div>
        <h3 :id="`${controlIdPrefix}-commands-title`">
          {{ t('ui.devices.editor.sections.commands') }}
        </h3>
        <p>{{ t('ui.devices.editor.commands_description') }}</p>
      </div>
    </div>

    <SettingRow
      class="client-command-editor__allow"
      :label="t('pin.allow_client_commands')"
      :description="t('pin.allow_client_commands_desc')"
      :control-id="`${controlIdPrefix}-allow`"
    >
      <label class="vs-switch">
        <input
          :id="`${controlIdPrefix}-allow`"
          type="checkbox"
          :checked="allowClientCommands"
          :disabled="disabled"
          @change="
            emit(
              'update:allowClientCommands',
              ($event.target as HTMLInputElement).checked,
            )
          "
        />
        <span class="vs-switch__track" aria-hidden="true" />
        <span class="vs-sr-only">{{ t('pin.allow_client_commands') }}</span>
      </label>
    </SettingRow>

    <div v-if="allowClientCommands" class="client-command-editor__lists">
      <fieldset v-for="section in commandSections" :key="section.key">
        <legend>{{ t(section.labelKey) }}</legend>
        <p>{{ t(section.descriptionKey) }}</p>
        <div v-if="commandsFor(section.key).length" class="client-command-editor__list">
          <div
            v-for="(entry, index) in commandsFor(section.key)"
            :key="`${section.key}-${index}`"
            class="client-command-editor__row"
          >
            <SettingRow
              :label="t('_common.cmd')"
              :control-id="`${controlIdPrefix}-${section.key}-${index}`"
            >
              <textarea
                :id="`${controlIdPrefix}-${section.key}-${index}`"
                class="vs-textarea monospace"
                rows="2"
                :value="entry.command"
                :disabled="disabled"
                @input="
                  updateCommand(
                    section.key,
                    index,
                    ($event.target as HTMLTextAreaElement).value,
                  )
                "
              />
            </SettingRow>
            <div class="client-command-editor__row-actions">
              <label v-if="isWindows" class="vs-switch-label">
                <input
                  type="checkbox"
                  :checked="entry.elevated === true"
                  :disabled="disabled"
                  @change="
                    updateElevated(
                      section.key,
                      index,
                      ($event.target as HTMLInputElement).checked,
                    )
                  "
                />
                {{ t('_common.elevated') }}
              </label>
              <AppButton
                :label="t('_common.remove')"
                icon="trash"
                variant="tertiary"
                size="compact"
                :disabled="disabled"
                @click="removeCommand(section.key, index)"
              />
            </div>
          </div>
        </div>
        <p v-else class="client-command-editor__empty">
          {{ t('ui.devices.editor.commands_empty') }}
        </p>
        <AppButton
          :label="t('_common.add')"
          icon="plus"
          size="compact"
          :disabled="disabled"
          @click="addCommand(section.key)"
        />
      </fieldset>
    </div>
  </section>
</template>

<style scoped>
.client-command-editor {
  display: grid;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-subtle);
}

.client-command-editor__heading,
.client-command-editor__lists,
.client-command-editor__list,
.client-command-editor fieldset {
  display: grid;
  gap: var(--vs-space-12);
}

.client-command-editor__heading h3,
.client-command-editor fieldset legend {
  margin: 0;
  color: var(--vs-color-text-primary);
  font-weight: var(--vs-type-weight-semibold);
}

.client-command-editor__heading p,
.client-command-editor fieldset p,
.client-command-editor__empty {
  margin: 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.client-command-editor fieldset {
  min-inline-size: 0;
  margin: 0;
  padding: var(--vs-space-12);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
}

.client-command-editor fieldset legend {
  padding-inline: var(--vs-space-4);
}

.client-command-editor__row {
  display: grid;
  gap: var(--vs-space-8);
  padding: var(--vs-space-12);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
}

.client-command-editor__row-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.client-command-editor__allow :deep(.vs-setting-row__control) {
  min-inline-size: 0;
  justify-content: flex-end;
}

.vs-switch-label {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}
</style>
