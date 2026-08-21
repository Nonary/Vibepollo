<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';

import { AppButton, SettingRow } from '@/components/ui';
import {
  normalizeServerCommandRows,
  serializeServerCommandRows,
  type ServerCommandRow,
} from '@/utils/v2Parity';

const props = withDefaults(
  defineProps<{
    modelValue: unknown;
    platform?: string;
  }>(),
  { platform: '' },
);

const emit = defineEmits<{ 'update:modelValue': [value: ServerCommandRow[]] }>();
const { t } = useI18n();

const rows = computed(() => normalizeServerCommandRows(props.modelValue, props.platform));
const isWindows = computed(() => props.platform.toLocaleLowerCase().includes('windows'));

function update(next: ServerCommandRow[]): void {
  emit('update:modelValue', serializeServerCommandRows(next, props.platform));
}

function add(): void {
  update([...rows.value, { name: '', cmd: '', ...(isWindows.value ? { elevated: false } : {}) }]);
}

function remove(index: number): void {
  update(rows.value.filter((_, rowIndex) => rowIndex !== index));
}

function updateRow(index: number, key: 'name' | 'cmd' | 'elevated', value: unknown): void {
  const next = rows.value.map((row, rowIndex) =>
    rowIndex === index ? { ...row, [key]: value } : row,
  );
  update(next);
}
</script>

<template>
  <div class="server-commands">
    <div v-if="rows.length" class="server-commands__list">
      <section
        v-for="(row, index) in rows"
        :key="index"
        class="server-commands__row"
        :aria-labelledby="`server-command-${index}`"
      >
        <header class="server-commands__header">
          <h4 :id="`server-command-${index}`">
            {{ t('config.server_cmd_entry', { index: index + 1 }) }}
          </h4>
          <div class="server-commands__actions">
            <label v-if="isWindows" class="vs-switch-label">
              <input
                type="checkbox"
                :checked="row.elevated === true"
                @change="updateRow(index, 'elevated', ($event.target as HTMLInputElement).checked)"
              />
              {{ t('_common.elevated') }}
            </label>
            <AppButton
              :label="t('_common.remove')"
              :aria-label="t('ui.settings.command_editor.remove', { number: index + 1 })"
              icon="trash"
              variant="tertiary"
              size="compact"
              @click="remove(index)"
            />
          </div>
        </header>
        <div class="server-commands__fields">
          <SettingRow
            :label="t('config.server_cmd_name')"
            :control-id="`server-cmd-name-${index}`"
          >
            <input
              :id="`server-cmd-name-${index}`"
              class="vs-input"
              :value="row.name"
              @input="updateRow(index, 'name', ($event.target as HTMLInputElement).value)"
            />
          </SettingRow>
          <SettingRow
            :label="t('config.server_cmd_command')"
            :control-id="`server-cmd-command-${index}`"
          >
            <textarea
              :id="`server-cmd-command-${index}`"
              class="vs-textarea monospace"
              rows="2"
              :value="row.cmd"
              @input="updateRow(index, 'cmd', ($event.target as HTMLTextAreaElement).value)"
            />
          </SettingRow>
        </div>
      </section>
    </div>
    <p v-else class="server-commands__empty">{{ t('ui.settings.command_editor.empty') }}</p>
    <AppButton :label="t('_common.add')" icon="plus" size="compact" @click="add" />
  </div>
</template>

<style scoped>
.server-commands,
.server-commands__list {
  display: grid;
  gap: var(--vs-space-12);
}

.server-commands__row {
  display: grid;
  gap: var(--vs-space-12);
  padding: var(--vs-space-12);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
}

.server-commands__header,
.server-commands__actions {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-8);
}

.server-commands__header h4 {
  margin: 0;
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
  line-height: var(--vs-type-line-height-control);
}

.server-commands__actions {
  justify-content: flex-end;
}

.server-commands__fields {
  display: grid;
  gap: var(--vs-space-8);
}

.server-commands__empty {
  margin: 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.server-commands > :deep(.vs-button) {
  justify-self: start;
}

.vs-switch-label {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}
</style>
