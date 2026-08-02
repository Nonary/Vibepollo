<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute, useRouter } from 'vue-router';

import { ApiError } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  EmptyState,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
  UiIcon,
} from '@/components/ui';
import {
  appCoverUrl,
  appName,
  appUuid,
  AppServiceError,
  deleteApp,
  fetchApps,
  type AppRecord,
} from '@/services/apps';

type ViewMode = 'grid' | 'list';
type SortMode = 'name' | 'name-desc' | 'source';

const PAGE_SIZE = 72;
const VIEW_STORAGE_KEY = 'vibepollo.library.view';
const validSortModes = new Set<SortMode>(['name', 'name-desc', 'source']);

const route = useRoute();
const router = useRouter();
const { locale, t } = useI18n();
const apps = ref<AppRecord[]>([]);
const loading = ref(true);
const error = ref('');
const search = ref(queryValue(route.query.q));
const sort = ref<SortMode>(parseSort(route.query.sort));
const viewMode = ref<ViewMode>(readStoredView());
const renderLimit = ref(PAGE_SIZE);
const selectedUuids = ref(new Set<string>());
const focusedUuid = ref('');
const selectionAnchor = ref('');
const failedCovers = ref(new Set<string>());
const collection = ref<HTMLElement | null>(null);
const loadMoreSentinel = ref<HTMLElement | null>(null);
const contextMenu = ref<HTMLElement | null>(null);
const contextUuid = ref('');
const deleteTarget = ref<AppRecord | null>(null);
const deleteOpen = ref(false);
const deleteBusy = ref(false);
const deleteError = ref('');
let queryTimer: number | undefined;
let observer: IntersectionObserver | undefined;

const collator = computed(
  () => new Intl.Collator(locale.value || undefined, { numeric: true, sensitivity: 'base' }),
);

function queryValue(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function parseSort(value: unknown): SortMode {
  return typeof value === 'string' && validSortModes.has(value as SortMode)
    ? (value as SortMode)
    : 'name';
}

function readStoredView(): ViewMode {
  try {
    return window.localStorage.getItem(VIEW_STORAGE_KEY) === 'list' ? 'list' : 'grid';
  } catch {
    return 'grid';
  }
}

function commandSummary(app: AppRecord): string {
  if (Array.isArray(app.cmd)) return app.cmd.filter((part) => typeof part === 'string').join(' ');
  if (typeof app.cmd === 'string' && app.cmd.trim()) return app.cmd;
  if (typeof app.output === 'string' && app.output.trim()) return app.output;
  return '';
}

function displayName(app: AppRecord): string {
  return appName(app) || t('ui.library.unnamed');
}

function serviceError(cause: unknown, fallbackKey: string): string {
  if (cause instanceof AppServiceError && cause.code === 'missing-app-uuid') {
    return t('ui.library.errors.missingUuidGeneric');
  }
  if (cause instanceof ApiError) return t(fallbackKey);
  return cause instanceof Error ? cause.message : t(fallbackKey);
}

const filteredApps = computed(() => {
  const query = search.value.trim().toLocaleLowerCase(locale.value);
  const candidates = apps.value
    .map((app, sourceIndex) => ({ app, sourceIndex }))
    .filter(({ app }) => {
      if (!query) return true;
      return `${displayName(app)} ${commandSummary(app)}`
        .toLocaleLowerCase(locale.value)
        .includes(query);
    });

  if (sort.value === 'source') return candidates.map(({ app }) => app);

  candidates.sort((left, right) => {
    const result = collator.value.compare(displayName(left.app), displayName(right.app));
    return (sort.value === 'name-desc' ? -result : result) || left.sourceIndex - right.sourceIndex;
  });
  return candidates.map(({ app }) => app);
});

const visibleApps = computed(() => filteredApps.value.slice(0, renderLimit.value));
const hasMore = computed(() => visibleApps.value.length < filteredApps.value.length);
const resultLabel = computed(() => {
  const count = filteredApps.value.length;
  return t(count === 1 ? 'ui.library.result.one' : 'ui.library.result.many', { count });
});

function setView(mode: ViewMode): void {
  viewMode.value = mode;
  try {
    window.localStorage.setItem(VIEW_STORAGE_KEY, mode);
  } catch {
    // Storage can be unavailable in locked-down browser profiles; the in-memory choice still works.
  }
}

function syncQuery(): void {
  const query = { ...route.query };
  const normalizedSearch = search.value.trim();
  if (normalizedSearch) query.q = normalizedSearch;
  else delete query.q;
  if (sort.value !== 'name') query.sort = sort.value;
  else delete query.sort;
  void router.replace({ query });
}

async function load(): Promise<void> {
  loading.value = true;
  error.value = '';
  try {
    apps.value = await fetchApps();
    failedCovers.value = new Set();
    const liveIds = new Set(apps.value.map(appUuid).filter(Boolean));
    selectedUuids.value = new Set([...selectedUuids.value].filter((uuid) => liveIds.has(uuid)));
    if (!liveIds.has(focusedUuid.value))
      focusedUuid.value = appUuid(apps.value[0] ?? ({} as AppRecord));
  } catch (cause) {
    error.value = serviceError(cause, 'ui.library.errors.load');
  } finally {
    loading.value = false;
  }
}

function openApp(app: AppRecord): void {
  const uuid = appUuid(app);
  if (!uuid) {
    error.value = t('ui.library.errors.missingUuid', { name: displayName(app) });
    return;
  }
  void router.push({ name: 'application', params: { id: uuid } });
}

function addApp(): void {
  void router.push({ name: 'application-new' });
}

function markCoverFailed(app: AppRecord): void {
  const uuid = appUuid(app);
  if (!uuid) return;
  const next = new Set(failedCovers.value);
  next.add(uuid);
  failedCovers.value = next;
}

function hasCoverFailed(app: AppRecord): boolean {
  const uuid = appUuid(app);
  return !uuid || failedCovers.value.has(uuid);
}

function toggleSelection(uuid: string): void {
  if (!uuid) return;
  const next = new Set(selectedUuids.value);
  if (next.has(uuid)) next.delete(uuid);
  else next.add(uuid);
  selectedUuids.value = next;
  selectionAnchor.value = uuid;
}

function clearSelection(): void {
  selectedUuids.value = new Set();
  selectionAnchor.value = '';
}

function onCollectionEscape(event: KeyboardEvent): void {
  if (contextUuid.value) {
    event.preventDefault();
    closeContextMenu();
  } else if (selectedUuids.value.size) {
    event.preventDefault();
    clearSelection();
  }
}

function itemButtons(): HTMLButtonElement[] {
  return collection.value
    ? Array.from(collection.value.querySelectorAll<HTMLButtonElement>('[data-library-item]'))
    : [];
}

function focusItem(uuid: string): void {
  focusedUuid.value = uuid;
  nextTick(() =>
    itemButtons()
      .find((button) => button.dataset.uuid === uuid)
      ?.focus(),
  );
}

function nextDirectionalButton(
  current: HTMLButtonElement,
  key: 'ArrowLeft' | 'ArrowRight' | 'ArrowUp' | 'ArrowDown',
): HTMLButtonElement | undefined {
  const buttons = itemButtons();
  const index = buttons.indexOf(current);
  if (index < 0) return undefined;
  if (key === 'ArrowLeft') return buttons[Math.max(0, index - 1)];
  if (key === 'ArrowRight') return buttons[Math.min(buttons.length - 1, index + 1)];

  const currentBox = current.getBoundingClientRect();
  const currentX = currentBox.left + currentBox.width / 2;
  const currentY = currentBox.top + currentBox.height / 2;
  return buttons
    .filter((candidate) => candidate !== current)
    .map((candidate) => {
      const box = candidate.getBoundingClientRect();
      const x = box.left + box.width / 2;
      const y = box.top + box.height / 2;
      const verticalDistance = key === 'ArrowUp' ? currentY - y : y - currentY;
      return { candidate, verticalDistance, horizontalDistance: Math.abs(currentX - x) };
    })
    .filter(({ verticalDistance }) => verticalDistance > 1)
    .sort(
      (left, right) =>
        left.verticalDistance - right.verticalDistance ||
        left.horizontalDistance - right.horizontalDistance,
    )[0]?.candidate;
}

function extendSelection(fromUuid: string, toUuid: string): void {
  const ids = visibleApps.value.map(appUuid);
  const start = ids.indexOf(selectionAnchor.value || fromUuid);
  const end = ids.indexOf(toUuid);
  if (start < 0 || end < 0) return;
  const next = new Set(selectedUuids.value);
  for (let index = Math.min(start, end); index <= Math.max(start, end); index += 1) {
    if (ids[index]) next.add(ids[index]);
  }
  selectedUuids.value = next;
  if (!selectionAnchor.value) selectionAnchor.value = fromUuid;
}

function onItemKeydown(event: KeyboardEvent, app: AppRecord): void {
  const uuid = appUuid(app);
  if (event.key === 'Enter') {
    event.preventDefault();
    openApp(app);
    return;
  }
  if (event.key === ' ') {
    event.preventDefault();
    toggleSelection(uuid);
    return;
  }
  if (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')) {
    event.preventDefault();
    openContextMenu(app);
    return;
  }
  if (event.key === 'Escape') {
    event.preventDefault();
    if (contextUuid.value) closeContextMenu();
    else if (selectedUuids.value.size) clearSelection();
    else if (search.value) search.value = '';
    return;
  }
  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(event.key)) return;

  event.preventDefault();
  const next = nextDirectionalButton(
    event.currentTarget as HTMLButtonElement,
    event.key as 'ArrowLeft' | 'ArrowRight' | 'ArrowUp' | 'ArrowDown',
  );
  const nextUuid = next?.dataset.uuid ?? '';
  if (!nextUuid) return;
  if (event.shiftKey) extendSelection(uuid, nextUuid);
  focusItem(nextUuid);
}

function openContextMenu(app: AppRecord): void {
  const uuid = appUuid(app);
  if (!uuid) return;
  contextUuid.value = uuid;
  focusedUuid.value = uuid;
  if (!selectedUuids.value.has(uuid)) selectedUuids.value = new Set([uuid]);
  nextTick(() => contextMenu.value?.querySelector<HTMLButtonElement>('button')?.focus());
}

function toggleContextMenu(app: AppRecord): void {
  if (contextUuid.value === appUuid(app)) closeContextMenu();
  else openContextMenu(app);
}

function closeContextMenu(): void {
  const returnTo = contextUuid.value;
  contextUuid.value = '';
  if (returnTo) focusItem(returnTo);
}

function requestDelete(app: AppRecord): void {
  contextUuid.value = '';
  deleteTarget.value = app;
  deleteError.value = '';
  deleteOpen.value = true;
}

async function confirmDelete(): Promise<void> {
  const target = deleteTarget.value;
  if (!target || deleteBusy.value) return;
  deleteBusy.value = true;
  error.value = '';
  try {
    await deleteApp(appUuid(target));
    deleteOpen.value = false;
    deleteTarget.value = null;
    await load();
  } catch (cause) {
    deleteError.value = serviceError(cause, 'ui.library.errors.delete');
  } finally {
    deleteBusy.value = false;
  }
}

function loadMore(): void {
  renderLimit.value = Math.min(renderLimit.value + PAGE_SIZE, filteredApps.value.length);
}

function onDocumentPointerDown(event: PointerEvent): void {
  if (
    contextUuid.value &&
    event.target instanceof Node &&
    !contextMenu.value?.contains(event.target)
  ) {
    contextUuid.value = '';
  }
}

watch([search, sort], () => {
  renderLimit.value = PAGE_SIZE;
  window.clearTimeout(queryTimer);
  queryTimer = window.setTimeout(syncQuery, 120);
});

watch(
  () => [route.query.q, route.query.sort],
  ([query, sortQuery]) => {
    const nextSearch = queryValue(query);
    const nextSort = parseSort(sortQuery);
    if (nextSearch !== search.value) search.value = nextSearch;
    if (nextSort !== sort.value) sort.value = nextSort;
  },
);

watch(loadMoreSentinel, (current, previous) => {
  if (previous) observer?.unobserve(previous);
  if (current) observer?.observe(current);
});

onMounted(() => {
  if ('IntersectionObserver' in window) {
    observer = new IntersectionObserver(
      (entries) => {
        if (entries.some((entry) => entry.isIntersecting)) loadMore();
      },
      { rootMargin: '320px' },
    );
    if (loadMoreSentinel.value) observer.observe(loadMoreSentinel.value);
  }
  document.addEventListener('pointerdown', onDocumentPointerDown);
  void load();
});

onBeforeUnmount(() => {
  window.clearTimeout(queryTimer);
  observer?.disconnect();
  document.removeEventListener('pointerdown', onDocumentPointerDown);
});
</script>

<template>
  <div class="vs-page vs-page--dashboard library-page">
    <PageHeader :title="t('ui.library.page.title')" :description="t('ui.library.page.description')">
      <template #actions>
        <AppButton
          icon="plus"
          variant="primary"
          :label="t('ui.library.actions.add')"
          @click="addApp"
        />
      </template>
    </PageHeader>

    <InlineAlert
      v-if="error"
      tone="danger"
      :title="t('ui.library.alerts.unavailable')"
      announce="assertive"
    >
      {{ error }}
      <template #actions>
        <AppButton
          icon="refresh"
          size="compact"
          :label="t('ui.library.actions.tryAgain')"
          @click="load"
        />
      </template>
    </InlineAlert>

    <section class="library-toolbar" :aria-label="t('ui.library.controls.region')">
      <label class="library-search">
        <span class="vs-sr-only">{{ t('ui.library.search.label') }}</span>
        <UiIcon name="search" :size="16" aria-hidden="true" />
        <input
          v-model="search"
          class="vs-input"
          type="search"
          :placeholder="t('ui.library.search.placeholder')"
          @keydown.escape.prevent="search = ''"
        />
      </label>

      <label class="library-sort">
        <span class="vs-sr-only">{{ t('ui.library.sort.label') }}</span>
        <select v-model="sort" class="vs-select" :aria-label="t('ui.library.sort.label')">
          <option value="name">{{ t('ui.library.sort.nameAsc') }}</option>
          <option value="name-desc">{{ t('ui.library.sort.nameDesc') }}</option>
          <option value="source">{{ t('ui.library.sort.configured') }}</option>
        </select>
      </label>

      <div class="library-view-toggle" role="group" :aria-label="t('ui.library.view.label')">
        <AppButton
          size="compact"
          :variant="viewMode === 'grid' ? 'secondary' : 'tertiary'"
          :label="t('ui.library.view.grid')"
          :aria-pressed="viewMode === 'grid'"
          @click="setView('grid')"
        />
        <AppButton
          size="compact"
          :variant="viewMode === 'list' ? 'secondary' : 'tertiary'"
          :label="t('ui.library.view.list')"
          :aria-pressed="viewMode === 'list'"
          @click="setView('list')"
        />
      </div>

      <span class="library-result-count" role="status" aria-live="polite">{{ resultLabel }}</span>
    </section>

    <div v-if="selectedUuids.size" class="library-selection" role="status">
      <StatusBadge tone="info">
        {{ t('ui.library.selection.count', { count: selectedUuids.size }) }}
      </StatusBadge>
      <span>{{ t('ui.library.selection.escapeHint') }}</span>
      <AppButton
        size="compact"
        variant="tertiary"
        :label="t('ui.library.selection.clear')"
        @click="clearSelection"
      />
    </div>

    <template v-if="loading">
      <span class="vs-sr-only" role="status" aria-live="polite">
        {{ t('ui.library.loading') }}
      </span>
      <div class="library-grid" aria-hidden="true">
        <div v-for="index in 12" :key="index" class="library-skeleton">
          <LoadingSkeleton variant="block" height="100%" />
          <LoadingSkeleton width="76%" />
        </div>
      </div>
    </template>

    <EmptyState
      v-else-if="!apps.length && !error"
      :title="t('ui.library.empty.title')"
      :description="t('ui.library.empty.description')"
      icon="library"
    >
      <template #actions>
        <AppButton
          icon="plus"
          variant="primary"
          :label="t('ui.library.actions.add')"
          @click="addApp"
        />
      </template>
    </EmptyState>

    <EmptyState
      v-else-if="!filteredApps.length && !error"
      :title="t('ui.library.empty.noMatchTitle')"
      :description="t('ui.library.empty.noMatchDescription', { search })"
      icon="search"
      compact
    >
      <template #actions>
        <AppButton variant="secondary" :label="t('ui.library.search.clear')" @click="search = ''" />
      </template>
    </EmptyState>

    <template v-else-if="!error">
      <div
        ref="collection"
        class="library-collection"
        :class="`library-collection--${viewMode}`"
        role="listbox"
        :aria-label="t('ui.library.collection.label')"
        aria-multiselectable="true"
        @keydown.escape="onCollectionEscape"
      >
        <article
          v-for="app in visibleApps"
          :key="appUuid(app)"
          class="library-item"
          :class="{
            'library-item--selected': selectedUuids.has(appUuid(app)),
            'library-item--menu-open': contextUuid === appUuid(app),
          }"
          @contextmenu.prevent="openContextMenu(app)"
        >
          <button
            class="library-item__open"
            type="button"
            role="option"
            data-library-item
            :data-uuid="appUuid(app)"
            :tabindex="
              focusedUuid === appUuid(app) || (!focusedUuid && app === visibleApps[0]) ? 0 : -1
            "
            :aria-label="displayName(app)"
            :aria-selected="selectedUuids.has(appUuid(app))"
            @focus="focusedUuid = appUuid(app)"
            @click="openApp(app)"
            @keydown="onItemKeydown($event, app)"
          >
            <span class="library-item__artwork">
              <img
                v-if="!hasCoverFailed(app)"
                :src="appCoverUrl(app)"
                :alt="t('ui.library.cover.alt', { name: displayName(app) })"
                loading="lazy"
                @error="markCoverFailed(app)"
              />
              <span
                v-else
                class="library-item__artwork-fallback"
                role="img"
                :aria-label="t('ui.library.cover.unavailableLabel', { name: displayName(app) })"
              >
                <UiIcon name="gamepad" :size="32" aria-hidden="true" />
                <span>{{ t('ui.library.cover.unavailable') }}</span>
              </span>
              <span v-if="selectedUuids.has(appUuid(app))" class="library-item__selected-mark">
                <UiIcon name="check" :size="16" aria-hidden="true" />
                <span class="vs-sr-only">{{ t('ui.library.selection.selected') }}</span>
              </span>
            </span>
            <span class="library-item__copy">
              <span class="library-item__title">{{ displayName(app) }}</span>
              <span
                v-if="commandSummary(app)"
                class="library-item__command vs-monospace"
                :title="commandSummary(app)"
              >
                {{ commandSummary(app) }}
              </span>
            </span>
          </button>

          <div class="library-item__actions">
            <AppButton
              size="compact"
              variant="tertiary"
              icon="edit"
              icon-only
              :label="t('ui.library.actions.editLabel', { name: displayName(app) })"
              @click.stop="openApp(app)"
            />
            <AppButton
              size="compact"
              variant="tertiary"
              icon="more"
              icon-only
              :label="t('ui.library.actions.moreLabel', { name: displayName(app) })"
              aria-haspopup="menu"
              :aria-expanded="contextUuid === appUuid(app)"
              @click="toggleContextMenu(app)"
            />
          </div>

          <div
            v-if="contextUuid === appUuid(app)"
            ref="contextMenu"
            class="library-context-menu"
            role="menu"
            :aria-label="t('ui.library.actions.menuLabel', { name: displayName(app) })"
            @keydown.escape.stop.prevent="closeContextMenu"
          >
            <AppButton
              role="menuitem"
              size="compact"
              variant="tertiary"
              icon="edit"
              :label="t('_common.edit')"
              @click="openApp(app)"
            />
            <AppButton
              role="menuitem"
              size="compact"
              variant="tertiary"
              icon="trash"
              :label="t('_common.delete')"
              @click="requestDelete(app)"
            />
          </div>
        </article>
      </div>

      <div v-if="hasMore" ref="loadMoreSentinel" class="library-load-more">
        <span>
          {{
            t('ui.library.progress.shown', {
              visible: visibleApps.length,
              total: filteredApps.length,
            })
          }}
        </span>
        <AppButton
          variant="secondary"
          :label="t('ui.library.actions.showMore')"
          @click="loadMore"
        />
      </div>
    </template>

    <ConfirmDialog
      v-model:open="deleteOpen"
      :title="
        t('ui.library.delete.title', {
          name: deleteTarget ? displayName(deleteTarget) : t('ui.library.delete.fallbackName'),
        })
      "
      :description="t('ui.library.delete.description')"
      :confirm-label="t('ui.library.delete.confirm')"
      :cancel-label="t('_common.cancel')"
      :busy-label="t('ui.library.delete.busy')"
      tone="danger"
      :busy="deleteBusy"
      :close-on-confirm="false"
      @confirm="confirmDelete"
    >
      <InlineAlert
        v-if="deleteError"
        tone="danger"
        :title="t('ui.library.delete.errorTitle')"
        announce="assertive"
      >
        {{ deleteError }}
      </InlineAlert>
    </ConfirmDialog>
  </div>
</template>

<style scoped>
.library-page {
  display: grid;
  gap: var(--vs-space-20);
}

.library-page :deep(.vs-page-header) {
  padding-block-end: 0;
}

.library-toolbar,
.library-selection,
.library-view-toggle,
.library-item__actions,
.library-load-more {
  display: flex;
  align-items: center;
}

.library-toolbar {
  position: sticky;
  z-index: 5;
  inset-block-start: 0;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  padding: var(--vs-space-12);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: color-mix(in srgb, var(--vs-color-bg-canvas) 92%, transparent);
  backdrop-filter: blur(10px);
}

.library-search {
  position: relative;
  flex: 1 1 20rem;
}

.library-search > svg {
  position: absolute;
  z-index: 1;
  inset-block-start: 50%;
  inset-inline-start: var(--vs-space-12);
  color: var(--vs-color-text-muted);
  pointer-events: none;
  translate: 0 -50%;
}

.library-search .vs-input {
  padding-inline-start: var(--vs-space-40);
}

.library-sort {
  flex: 0 1 12rem;
}

.library-view-toggle {
  gap: var(--vs-space-2);
  padding: var(--vs-space-2);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
}

.library-result-count {
  min-inline-size: 7.5rem;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  text-align: end;
}

.library-selection {
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
}

.library-selection .vs-button {
  margin-inline-start: auto;
}

.library-grid,
.library-collection--grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(var(--vs-game-card-min-width-desktop), 1fr));
  gap: var(--vs-space-16);
}

.library-skeleton {
  display: grid;
  aspect-ratio: 2 / 3.45;
  gap: var(--vs-space-8);
}

.library-skeleton :deep(.vs-loading-skeleton:first-child) {
  min-block-size: 0;
  aspect-ratio: 2 / 3;
}

.library-collection--list {
  display: grid;
  overflow: visible;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.library-item {
  position: relative;
  min-inline-size: 0;
}

.library-item--menu-open {
  z-index: 9;
}

.library-collection--list .library-item + .library-item {
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.library-item__open {
  display: grid;
  inline-size: 100%;
  min-inline-size: 0;
  padding: 0;
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-primary);
  text-align: start;
  cursor: pointer;
  transition:
    border-color var(--vs-motion-duration-control) var(--vs-motion-easing-standard),
    background-color var(--vs-motion-duration-control) var(--vs-motion-easing-standard);
}

.library-item__open:hover,
.library-item__open:focus-visible,
.library-item--selected .library-item__open {
  border-color: var(--vs-color-accent-default);
}

.library-item--selected .library-item__open {
  box-shadow: inset 0 0 0 var(--vs-border-width) var(--vs-color-accent-default);
}

.library-item__artwork {
  position: relative;
  display: grid;
  aspect-ratio: 2 / 3;
  overflow: hidden;
  place-items: stretch;
  background: var(--vs-color-bg-subtle);
}

.library-item__artwork img {
  inline-size: 100%;
  block-size: 100%;
  object-fit: cover;
}

.library-item__artwork-fallback {
  display: grid;
  place-content: center;
  place-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-16);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  text-align: center;
}

.library-item__selected-mark {
  position: absolute;
  inset-block-start: var(--vs-space-8);
  inset-inline-start: var(--vs-space-8);
  display: grid;
  inline-size: 1.75rem;
  block-size: 1.75rem;
  place-items: center;
  border-radius: var(--vs-radius-pill);
  background: var(--vs-color-accent-default);
  color: var(--vs-color-text-on-accent);
}

.library-item__copy {
  display: grid;
  min-inline-size: 0;
  gap: var(--vs-space-4);
  padding: var(--vs-space-12);
}

.library-item__title {
  overflow: hidden;
  font-weight: var(--vs-type-weight-semibold);
  line-height: var(--vs-type-line-height-control);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.library-item__command {
  overflow: hidden;
  color: var(--vs-color-text-muted);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.library-item__actions {
  position: absolute;
  z-index: 2;
  inset-block-start: var(--vs-space-8);
  inset-inline-end: var(--vs-space-8);
  gap: var(--vs-space-2);
  padding: var(--vs-space-2);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: color-mix(in srgb, var(--vs-color-bg-raised) 94%, transparent);
  box-shadow: var(--vs-shadow-raised);
  opacity: 0;
  pointer-events: none;
  transition: opacity var(--vs-motion-duration-control) var(--vs-motion-easing-standard);
}

.library-item:hover .library-item__actions,
.library-item:focus-within .library-item__actions,
.library-item--selected .library-item__actions {
  opacity: 1;
  pointer-events: auto;
}

.library-context-menu {
  position: absolute;
  z-index: 8;
  inset-block-start: calc(var(--vs-space-8) + var(--vs-size-control-compact) + var(--vs-space-4));
  inset-inline-end: var(--vs-space-8);
  display: grid;
  min-inline-size: 10rem;
  gap: var(--vs-space-2);
  padding: var(--vs-space-4);
  border: var(--vs-border-width) solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  box-shadow: var(--vs-shadow-overlay);
}

.library-context-menu :deep(.vs-button) {
  inline-size: 100%;
  justify-content: flex-start;
}

.library-collection--list .library-item__open {
  min-block-size: var(--vs-size-row-settings);
  grid-template-columns: 4.5rem minmax(0, 1fr);
  align-items: center;
  border: 0;
  border-radius: 0;
}

.library-collection--list .library-item__artwork {
  inline-size: 4.5rem;
  block-size: 6.75rem;
}

.library-collection--list .library-item__copy {
  padding-inline-end: 6rem;
}

.library-collection--list .library-item__actions {
  inset-block-start: 50%;
  opacity: 1;
  pointer-events: auto;
  translate: 0 -50%;
}

.library-collection--list .library-context-menu {
  inset-block-start: calc(50% + var(--vs-size-control-compact));
}

.library-load-more {
  flex-direction: column;
  justify-content: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-24);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

@media (max-width: 1023px) {
  .library-grid,
  .library-collection--grid {
    grid-template-columns: repeat(auto-fill, minmax(var(--vs-game-card-min-width-tablet), 1fr));
    gap: var(--vs-space-12);
  }
}

@media (max-width: 767px) {
  .library-toolbar {
    position: static;
  }

  .library-search {
    flex-basis: 100%;
  }

  .library-sort {
    flex: 1 1 10rem;
  }

  .library-result-count {
    inline-size: 100%;
    text-align: start;
  }

  .library-grid,
  .library-collection--grid {
    grid-template-columns: repeat(auto-fill, minmax(var(--vs-game-card-min-width-mobile), 1fr));
  }

  .library-item__command {
    display: none;
  }
}

@media (hover: none), (pointer: coarse) {
  .library-item__actions {
    opacity: 1;
    pointer-events: auto;
  }
}

@media (prefers-reduced-motion: reduce) {
  .library-item__open,
  .library-item__actions {
    transition: none;
  }
}
</style>
