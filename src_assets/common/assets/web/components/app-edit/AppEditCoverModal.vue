<script setup lang="ts">
import { nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { AppButton, InlineAlert, LoadingSkeleton } from '@/components/ui';
import type { CoverCandidate } from './AppEditCoverModal.types';

const props = defineProps<{
  open: boolean;
  searching: boolean;
  busy: boolean;
  candidates: CoverCandidate[];
  error: string;
  query: string;
  playniteManaged: boolean;
}>();

const emit = defineEmits<{
  'update:open': [value: boolean];
  'update:query': [value: string];
  search: [];
  pick: [cover: CoverCandidate];
}>();

const { t } = useI18n();
const dialog = ref<HTMLDialogElement | null>(null);
const closeButton = ref<InstanceType<typeof AppButton> | null>(null);
let restoreFocusTo: HTMLElement | null = null;

function componentElement(component: InstanceType<typeof AppButton> | null): HTMLElement | null {
  const element = component?.$el;
  return element instanceof HTMLElement ? element : null;
}

async function showDialog(): Promise<void> {
  if (!dialog.value) return;
  if (!dialog.value.open) {
    restoreFocusTo = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    dialog.value.showModal();
  }
  await nextTick();
  componentElement(closeButton.value)?.focus();
}

function hideDialog(restoreFocus = true): void {
  if (dialog.value?.open) dialog.value.close();
  const target = restoreFocusTo;
  if (restoreFocus && target?.isConnected) void nextTick(() => target.focus());
  restoreFocusTo = null;
}

function close(): void {
  if (props.busy) return;
  emit('update:open', false);
}

function onCancel(event: Event): void {
  event.preventDefault();
  close();
}

function onBackdrop(event: MouseEvent): void {
  if (event.target === dialog.value) close();
}

watch(
  () => props.open,
  (open) => {
    if (open) void showDialog();
    else hideDialog();
  },
  { flush: 'post' },
);

onMounted(() => {
  if (props.open) void showDialog();
});
onBeforeUnmount(() => hideDialog(false));
</script>

<template>
  <Teleport to="body">
    <dialog
      ref="dialog"
      class="vs-dialog cover-picker"
      :aria-labelledby="'cover-picker-title'"
      :aria-describedby="'cover-picker-description'"
      :aria-busy="searching || busy ? 'true' : undefined"
      @cancel="onCancel"
      @click="onBackdrop"
    >
      <section class="vs-dialog__panel cover-picker__panel" tabindex="-1">
        <header class="cover-picker__header">
          <div>
            <h2 id="cover-picker-title" class="vs-dialog__title">
              {{ t('ui.application.coverPicker.title') }}
            </h2>
            <p id="cover-picker-description" class="vs-dialog__description">
              {{ t('ui.application.coverPicker.description') }}
            </p>
          </div>
          <AppButton
            ref="closeButton"
            variant="tertiary"
            icon="x"
            icon-only
            :label="t('_common.close')"
            :disabled="busy"
            @click="close"
          />
        </header>

        <form class="cover-picker__search" @submit.prevent="emit('search')">
          <label class="vs-sr-only" for="cover-picker-search">
            {{ t('ui.application.coverPicker.searchLabel') }}
          </label>
          <input
            id="cover-picker-search"
            class="vs-input"
            type="search"
            :value="query"
            :placeholder="t('ui.application.coverPicker.searchPlaceholder')"
            :disabled="busy"
            @input="emit('update:query', ($event.target as HTMLInputElement).value)"
          />
          <AppButton
            type="submit"
            variant="primary"
            icon="search"
            :label="t('ui.application.coverPicker.searchAction')"
            :busy="searching"
            :disabled="busy || !query.trim()"
          />
        </form>

        <InlineAlert
          v-if="playniteManaged"
          tone="warning"
          :title="t('ui.application.coverPicker.playniteTitle')"
        >
          {{ t('ui.application.coverPicker.playniteWarning') }}
        </InlineAlert>

        <InlineAlert
          v-if="error"
          tone="danger"
          :title="t('ui.application.coverPicker.errorTitle')"
          announce="assertive"
        >
          {{ error }}
        </InlineAlert>

        <div v-if="searching" class="cover-picker__grid" aria-hidden="true">
          <div v-for="index in 8" :key="index" class="cover-picker__skeleton">
            <LoadingSkeleton variant="block" height="13rem" />
            <LoadingSkeleton width="70%" />
          </div>
        </div>
        <div v-else-if="candidates.length" class="cover-picker__grid">
          <button
            v-for="cover in candidates"
            :key="cover.key"
            class="cover-picker__option"
            type="button"
            :disabled="busy"
            :aria-label="t('ui.application.coverPicker.select', { name: cover.name })"
            @click="emit('pick', cover)"
          >
            <span class="cover-picker__artwork">
              <img :src="cover.url" :alt="t('ui.application.cover.alt', { name: cover.name })" />
              <span v-if="busy" class="cover-picker__busy" aria-hidden="true" />
            </span>
            <span class="cover-picker__name" :title="cover.name">{{ cover.name }}</span>
          </button>
        </div>
        <p v-else-if="!error" class="cover-picker__empty">
          {{ t('ui.application.coverPicker.empty') }}
        </p>
      </section>
    </dialog>
  </Teleport>
</template>

<style scoped>
.cover-picker {
  inline-size: min(calc(100vw - (var(--vs-space-16) * 2)), 52rem);
}

.cover-picker__panel {
  display: grid;
  gap: var(--vs-space-20);
}

.cover-picker__header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.cover-picker__search {
  display: flex;
  gap: var(--vs-space-8);
}

.cover-picker__search .vs-input {
  min-inline-size: 0;
  flex: 1;
}

.cover-picker__header .vs-dialog__description {
  margin-block-start: var(--vs-space-4);
}

.cover-picker__grid {
  display: grid;
  max-block-size: min(60vh, 32rem);
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--vs-space-12);
  overflow: auto;
  padding: var(--vs-space-2);
}

.cover-picker__skeleton {
  display: grid;
  gap: var(--vs-space-8);
}

.cover-picker__option {
  display: grid;
  min-inline-size: 0;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8);
  border: var(--vs-border-width) solid transparent;
  border-radius: var(--vs-radius-control);
  background: transparent;
  color: var(--vs-color-text-primary);
  cursor: pointer;
  text-align: center;
}

.cover-picker__option:hover:not(:disabled),
.cover-picker__option:focus-visible {
  border-color: var(--vs-color-accent-default);
  background: var(--vs-color-bg-subtle);
}

.cover-picker__option:focus-visible {
  outline: var(--vs-focus-width) solid var(--vs-color-focus);
  outline-offset: var(--vs-focus-offset);
}

.cover-picker__option:disabled {
  cursor: wait;
}

.cover-picker__artwork {
  position: relative;
  display: block;
  aspect-ratio: 3 / 4;
  overflow: hidden;
  border-radius: var(--vs-radius-subtle);
  background: var(--vs-color-bg-subtle);
}

.cover-picker__artwork img {
  inline-size: 100%;
  block-size: 100%;
  object-fit: cover;
}

.cover-picker__busy {
  position: absolute;
  inset: 0;
  background: color-mix(in srgb, var(--vs-color-bg-raised) 68%, transparent);
}

.cover-picker__name {
  overflow: hidden;
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-medium);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.cover-picker__empty {
  padding: var(--vs-space-40) var(--vs-space-16);
  color: var(--vs-color-text-secondary);
  text-align: center;
}

@media (max-width: 47.999rem) {
  .cover-picker__grid {
    grid-template-columns: repeat(3, minmax(0, 1fr));
  }
}

@media (max-width: 29.999rem) {
  .cover-picker__search {
    align-items: stretch;
    flex-direction: column;
  }

  .cover-picker__grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
</style>
