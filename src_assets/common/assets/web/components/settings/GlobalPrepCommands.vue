<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';

import { AppButton, SettingRow } from '@/components/ui';
import { normalizeCommandRows, serializeCommandRows, type CommandRow } from '@/utils/v2Parity';

const props = withDefaults(
  defineProps<{
    modelValue: unknown;
    platform?: string;
  }>(),
  { platform: '' },
);

const emit = defineEmits<{ 'update:modelValue': [value: CommandRow[]] }>();
const { t } = useI18n();

const rows = computed(() => normalizeCommandRows(props.modelValue, props.platform));

function update(next: CommandRow[]): void {
  emit('update:modelValue', serializeCommandRows(next, props.platform));
}

function add(): void {
  update([...rows.value, { do: '', undo: '', ...(isWindows.value ? { elevated: false } : {}) }]);
}

function remove(index: number): void {
  update(rows.value.filter((_, rowIndex) => rowIndex !== index));
}

function updateRow(index: number, key: 'do' | 'undo' | 'elevated', value: unknown): void {
  const next = rows.value.map((row, rowIndex) =>
    rowIndex === index ? { ...row, [key]: value } : row,
  );
  update(next);
}

const isWindows = computed(() => props.platform.toLocaleLowerCase().includes('windows'));
</script>

<template>
  <div class="global-prep-commands">
    <p class="settings-row__description">{{ t('config.global_prep_cmd_desc') }}</p>
    <div v-if="rows.length" class="global-prep-commands__list">
      <section v-for="(row, index) in rows" :key="index" class="global-prep-commands__row">
        <header class="global-prep-commands__header">
          <strong>{{ t('apps.prep_step', { number: index + 1 }) }}</strong>
          <div class="global-prep-commands__actions">
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
              icon="trash"
              variant="tertiary"
              size="compact"
              @click="remove(index)"
            />
          </div>
        </header>
        <div class="global-prep-commands__fields">
          <SettingRow :label="t('_common.do_cmd')" :control-id="`global-prep-do-${index}`">
            <textarea
              :id="`global-prep-do-${index}`"
              class="vs-textarea monospace"
              rows="2"
              :value="row.do"
              @input="updateRow(index, 'do', ($event.target as HTMLTextAreaElement).value)"
            />
          </SettingRow>
          <SettingRow :label="t('_common.undo_cmd')" :control-id="`global-prep-undo-${index}`">
            <textarea
              :id="`global-prep-undo-${index}`"
              class="vs-textarea monospace"
              rows="2"
              :value="row.undo"
              @input="updateRow(index, 'undo', ($event.target as HTMLTextAreaElement).value)"
            />
          </SettingRow>
        </div>
      </section>
    </div>
    <AppButton :label="t('config.add')" icon="plus" size="compact" @click="add" />
  </div>
</template>

<style scoped>
.global-prep-commands {
  display: grid;
  gap: var(--vs-space-12);
}
.global-prep-commands__list {
  display: grid;
  gap: var(--vs-space-12);
}
.global-prep-commands__row {
  display: grid;
  gap: var(--vs-space-12);
  padding: var(--vs-space-12);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
}
.global-prep-commands__header,
.global-prep-commands__actions {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-8);
}
.global-prep-commands__actions {
  justify-content: flex-end;
}
.global-prep-commands__fields {
  display: grid;
  gap: var(--vs-space-8);
}
.vs-switch-label {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}
</style>
