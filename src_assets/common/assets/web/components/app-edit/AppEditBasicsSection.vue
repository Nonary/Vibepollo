<template>
  <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
    <div class="space-y-1 md:col-span-2">
      <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Name</label>
      <div class="flex items-center gap-2 mb-1">
        <n-select
          v-model:value="nameSelectValue"
          :options="nameSelectOptions"
          :loading="gamesLoading"
          filterable
          clearable
          :placeholder="'Type to search or enter a custom name'"
          class="flex-1"
          :fallback-option="fallbackOption"
          @focus="emit('name-focus')"
          @search="(q) => emit('name-search', q)"
          @update:value="(val) => emit('name-picked', val)"
        />
      </div>
      <template v-if="showPlaynitePicker">
        <div class="flex items-center gap-2">
          <n-select
            v-model:value="selectedPlayniteId"
            :options="playniteOptions"
            :loading="gamesLoading"
            filterable
            :disabled="lockPlaynite || !playniteInstalled"
            :placeholder="
              playniteInstalled ? 'Select a Playnite game…' : 'Playnite plugin not detected'
            "
            class="flex-1"
            @focus="emit('load-playnite-games')"
            @update:value="(val) => emit('pick-playnite', String(val ?? ''))"
          />
          <n-button
            v-if="lockPlaynite"
            size="small"
            type="default"
            strong
            @click="emit('unlock-playnite')"
          >
            Change
          </n-button>
        </div>
      </template>
      <div class="text-[11px] opacity-60">
        {{ isPlaynite ? 'Linked to Playnite' : 'Custom application' }}
      </div>
    </div>

    <div v-if="!isPlaynite" class="md:col-span-2">
      <div class="grid grid-cols-1 gap-4 md:grid-cols-2">
        <section
          class="rounded-xl border border-dark/10 dark:border-light/10 bg-light/80 dark:bg-dark/40 p-4 space-y-3"
        >
          <div class="space-y-1">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Command</label>
            <n-input
              v-model:value="cmdText"
              type="textarea"
              class="font-mono"
              :autosize="{ minRows: 4, maxRows: 8 }"
              placeholder="Executable command line"
            />
          </div>
          <p class="text-[11px] opacity-60">
            Sunshine waits for this process. When it closes, the stream ends.
          </p>
        </section>

        <section
          class="rounded-xl border border-dark/10 dark:border-light/10 bg-light/80 dark:bg-dark/40 p-4 space-y-3"
        >
          <div class="flex items-center justify-between gap-3">
            <div>
              <h3 class="text-xs font-semibold uppercase tracking-wide opacity-70">
                Detached Commands
              </h3>
              <p class="text-[11px] opacity-60">
                Optional commands that run first and keep the stream alive when they finish.
              </p>
            </div>
            <n-button size="small" type="primary" @click="addDetached">
              <i class="fas fa-plus" /> Add
            </n-button>
          </div>

          <div v-if="form.detached.length === 0" class="rounded-lg border border-dashed border-dark/15 dark:border-light/15 px-3 py-4 text-xs text-center opacity-60">
            No detached commands yet. Use Add to set up prep scripts or launchers.
          </div>
          <ol v-else class="space-y-3">
            <li v-for="(value, index) in form.detached" :key="index">
              <div
                class="rounded-lg border border-dark/10 dark:border-light/10 bg-white/80 dark:bg-surface/60 shadow-sm"
              >
                <header
                  class="flex items-center justify-between gap-2 px-3 py-2 border-b border-dark/10 dark:border-light/10"
                >
                  <span class="text-xs font-semibold uppercase tracking-wide opacity-70">
                    Detached Command #{{ index + 1 }}
                  </span>
                  <n-button
                    size="tiny"
                    secondary
                    type="error"
                    @click="removeDetached(index)"
                  >
                    <i class="fas fa-trash" /> Delete
                  </n-button>
                </header>
                <div class="p-3 space-y-2">
                  <n-input
                    v-model:value="form.detached[index]"
                    type="textarea"
                    class="font-mono"
                    :autosize="{ minRows: 2, maxRows: 6 }"
                    placeholder="Command to execute before the stream"
                  />
                  <p class="text-[11px] opacity-60">
                    Runs before the primary command. Sunshine continues even if this command exits.
                  </p>
                </div>
              </div>
            </li>
          </ol>
        </section>
      </div>
    </div>

    <div v-if="!isPlaynite" class="space-y-1 md:col-span-1">
      <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Working Dir</label>
      <n-input v-model:value="form.workingDir" class="font-mono" placeholder="C:/Games/App" />
    </div>

    <div class="space-y-1 md:col-span-1">
      <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Exit Timeout</label>
      <div class="flex items-center gap-2">
        <n-input-number v-model:value="form.exitTimeout" :min="0" class="w-28" />
        <span class="text-xs opacity-60">seconds</span>
      </div>
    </div>

    <div v-if="!isPlaynite" class="space-y-1 md:col-span-2">
      <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Image Path</label>
      <div class="flex items-center gap-2">
        <n-input
          v-model:value="form.imagePath"
          class="font-mono flex-1"
          placeholder="/path/to/image.png"
        />
        <n-button type="default" strong :disabled="!form.name" @click="emit('open-cover-finder')">
          <i class="fas fa-image" /> Find Cover
        </n-button>
      </div>
      <p class="text-[11px] opacity-60">
        Optional; stored only and not fetched by Sunshine.
      </p>
    </div>
  </div>
</template>

<script setup lang="ts">
import { toRefs } from 'vue';
import type { AppForm } from './types';
import { NSelect, NButton, NInput, NInputNumber } from 'naive-ui';

const rawProps = defineProps<{
  isPlaynite: boolean;
  showPlaynitePicker: boolean;
  playniteInstalled: boolean;
  nameSelectOptions: Array<{ label: string; value: string; disabled?: boolean }>;
  gamesLoading: boolean;
  fallbackOption: (value: unknown) => { label: string; value: string };
  playniteOptions: Array<{ label: string; value: string }>;
  lockPlaynite: boolean;
}>();
const {
  isPlaynite,
  showPlaynitePicker,
  playniteInstalled,
  nameSelectOptions,
  gamesLoading,
  fallbackOption,
  playniteOptions,
  lockPlaynite,
} = toRefs(rawProps);

const emit = defineEmits<{
  (e: 'name-focus'): void;
  (e: 'name-search', query: string): void;
  (e: 'name-picked', value: string | null): void;
  (e: 'load-playnite-games'): void;
  (e: 'pick-playnite', id: string): void;
  (e: 'unlock-playnite'): void;
  (e: 'open-cover-finder'): void;
}>();

// Two-way bindings
const form = defineModel<AppForm>('form', { required: true });
const cmdText = defineModel<string>('cmdText', { required: true });
const nameSelectValue = defineModel<string>('nameSelectValue', { required: true });
const selectedPlayniteId = defineModel<string>('selectedPlayniteId', { required: true });

function addDetached() {
  form.value.detached.push('');
}

function removeDetached(index: number) {
  form.value.detached.splice(index, 1);
}
</script>
