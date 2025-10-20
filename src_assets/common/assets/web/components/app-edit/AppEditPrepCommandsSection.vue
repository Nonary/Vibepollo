<template>
  <section class="space-y-3">
    <div class="flex items-center justify-between">
      <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">Prep Commands</h3>
      <n-button size="small" type="primary" @click="emit('add-prep')">
        <i class="fas fa-plus" /> Add
      </n-button>
    </div>
    <div v-if="form.prepCmd.length === 0" class="text-[12px] opacity-60">None</div>
    <div v-else class="space-y-2">
      <div
        v-for="(p, i) in form.prepCmd"
        :key="i"
        class="rounded-md border border-dark/10 dark:border-light/10 p-2"
      >
        <div class="flex items-center justify-between gap-2 mb-2">
          <div class="text-xs opacity-70">Step {{ i + 1 }}</div>
          <div class="flex items-center gap-2">
            <n-checkbox v-if="isWindows" v-model:checked="p.elevated" size="small">
              {{ $t('_common.elevated') }}
            </n-checkbox>
            <n-button size="small" type="error" strong @click="remove(i)">
              <i class="fas fa-trash" />
            </n-button>
          </div>
        </div>
        <div class="grid grid-cols-1 gap-2">
          <div>
            <label class="text-[11px] opacity-60">{{ $t('_common.do_cmd') }}</label>
            <n-input
              v-model:value="p.do"
              type="textarea"
              :autosize="{ minRows: 1, maxRows: 3 }"
              class="font-mono"
              placeholder="Command to run before start"
            />
          </div>
          <div>
            <label class="text-[11px] opacity-60">{{ $t('_common.undo_cmd') }}</label>
            <n-input
              v-model:value="p.undo"
              type="textarea"
              :autosize="{ minRows: 1, maxRows: 3 }"
              class="font-mono"
              placeholder="Command to run on stop"
            />
          </div>
        </div>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { NButton, NCheckbox, NInput } from 'naive-ui';
import type { AppForm } from './types';

const form = defineModel<AppForm>('form', { required: true });

const props = defineProps<{
  isWindows: boolean;
}>();

const emit = defineEmits<{
  (e: 'add-prep'): void;
}>();

function remove(index: number) {
  form.value.prepCmd.splice(index, 1);
}

const isWindows = props.isWindows;
</script>
