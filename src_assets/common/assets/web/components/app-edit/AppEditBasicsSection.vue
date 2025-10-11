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

    <div v-if="!isPlaynite" class="space-y-1 md:col-span-2">
      <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Command</label>
      <n-input
        v-model:value="cmdText"
        type="textarea"
        :autosize="{ minRows: 4, maxRows: 8 }"
        placeholder="Executable command line"
      />
      <p class="text-[11px] opacity-60">Enter the full command line (single string).</p>
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
</script>
