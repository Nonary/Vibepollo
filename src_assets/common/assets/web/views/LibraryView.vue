<script setup lang="ts">
import GameLibrarySetup from '../../web-legacy/components/GameLibrarySetup.vue';
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute, useRouter } from 'vue-router';

import { useSystemStore } from '@/stores/system';
import { ApiError, apiGet, apiPatch, apiPost } from '@/api/client';
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
type AppProvider = 'playnite' | 'steam' | 'lutris';

interface ProviderInfo {
  id: AppProvider;
  name: string;
  managed: boolean;
}

const PAGE_SIZE = 72;
const VIEW_STORAGE_KEY = 'vibepollo.library.view';
const validSortModes = new Set<SortMode>(['name', 'name-desc', 'source']);

const route = useRoute();
const router = useRouter();
const { locale, t } = useI18n();
const system = useSystemStore();
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
  if (providerInfo(app)) return '';
  if (Array.isArray(app.cmd)) return app.cmd.filter((part) => typeof part === 'string').join(' ');
  if (typeof app.cmd === 'string' && app.cmd.trim()) return app.cmd;
  if (typeof app.output === 'string' && app.output.trim()) return app.output;
  return '';
}

function providerInfo(app: AppRecord): ProviderInfo | null {
  if (typeof app['playnite-id'] === 'string' && app['playnite-id'].trim()) {
    return {
      id: 'playnite',
      name: 'Playnite',
      managed: app['playnite-managed'] === 'auto',
    };
  }
  if (typeof app['steam-id'] === 'string' && app['steam-id'].trim()) {
    return { id: 'steam', name: 'Steam', managed: app['steam-managed'] === 'auto' };
  }
  if (typeof app['lutris-id'] === 'string' && app['lutris-id'].trim()) {
    return { id: 'lutris', name: 'Lutris', managed: app['lutris-managed'] === 'auto' };
  }
  return null;
}

function providerLabel(app: AppRecord): string {
  const provider = providerInfo(app);
  if (!provider) return t('ui.library.providers.custom');
  return provider.managed
    ? t('ui.library.providers.managed', { provider: provider.name })
    : provider.name;
}

function providerSortName(app: AppRecord): string {
  return providerInfo(app)?.name ?? t('ui.library.providers.custom');
}

function searchSummary(app: AppRecord): string {
  return [
    commandSummary(app),
    providerLabel(app),
    app['playnite-id'],
    app['steam-id'],
    app['lutris-id'],
    app['steam-install-dir'],
    app['lutris-directory'],
  ]
    .filter((value): value is string => typeof value === 'string' && Boolean(value.trim()))
    .join(' ');
}

function displayName(app: AppRecord): string {
  return appName(app) || t('ui.library.unnamed');
}

function appInitials(app: AppRecord): string {
  return displayName(app)
    .split(/\s+/u)
    .slice(0, 2)
    .map((part) => Array.from(part)[0])
    .join('')
    .toLocaleUpperCase(locale.value);
}

function isRemoteSessionApp(app: AppRecord): boolean {
  return app['remote-session'] === 'input' || app['remote-session'] === 'monitor';
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
      return `${displayName(app)} ${searchSummary(app)}`
        .toLocaleLowerCase(locale.value)
        .includes(query);
    });

  if (sort.value === 'source') {
    candidates.sort((left, right) => {
      const sourceResult = collator.value.compare(
        providerSortName(left.app),
        providerSortName(right.app),
      );
      return sourceResult || collator.value.compare(displayName(left.app), displayName(right.app));
    });
    return candidates.map(({ app }) => app);
  }

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

let refreshing = false;
let refreshTimer: number | undefined;
async function load(silent = false): Promise<void> {
  if (refreshing) return;
  refreshing = true;
  if (!silent) loading.value = true;
  error.value = '';
  try {
    apps.value = await fetchApps();
    failedCovers.value = new Set();
    const liveIds = new Set(apps.value.map(appUuid).filter(Boolean));
    selectedUuids.value = new Set([...selectedUuids.value].filter((uuid) => liveIds.has(uuid)));
    if (!liveIds.has(focusedUuid.value))
      focusedUuid.value = appUuid(apps.value[0] ?? ({} as AppRecord));
  } catch (cause) {
    if (!silent) error.value = serviceError(cause, 'ui.library.errors.load');
  } finally {
    loading.value = false;
    refreshing = false;
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
  refreshTimer = window.setInterval(() => {
    if (!document.hidden && !deleteBusy.value && !librarySetupOpen.value) void load(true);
  }, 5000);
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
  window.clearInterval(refreshTimer);
  window.clearTimeout(queryTimer);
  observer?.disconnect();
  document.removeEventListener('pointerdown', onDocumentPointerDown);
});
const librarySetupOpen = ref(false);
const libraryConfigured = ref(false);
function libraryRequest(
  method: 'GET' | 'POST' | 'PATCH',
  path: string,
  body?: Record<string, unknown>,
): Promise<any> {
  return method === 'GET'
    ? apiGet(path)
    : method === 'PATCH'
      ? apiPatch(path, body ?? {})
      : apiPost(path, body ?? {});
}
</script>

<template>
  <div class="vs-page vs-page--dashboard library-page">
    <GameLibrarySetup
      v-model:open="librarySetupOpen"
      :platform="system.metadata?.platform || ''"
      :metadata="system.metadata"
      :request="libraryRequest"
      @configured="libraryConfigured = $event"
      @saved="load()"
    />
    <PageHeader :title="t('ui.library.page.title')" :description="t('ui.library.page.description')">
      <template #actions>
        <AppButton
          icon="integrations"
          :label="libraryConfigured ? 'Library manager settings' : 'Setup Game Library Integration'"
          @click="librarySetupOpen = true"
        />
        <RouterLink class="button button--secondary" to="/integrations"
          ><UiIcon name="integrations" />{{ t('ui.library.actions.sources') }}</RouterLink
        >
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
          @click="load()"
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
          <option value="source">{{ t('ui.library.sort.provider') }}</option>
        </select>
      </label>

      <div class="library-view-toggle" role="group" :aria-label="t('ui.library.view.label')">
        <AppButton
          size="default"
          icon="overview"
          icon-only
          :title="t('ui.library.view.grid')"
          :variant="viewMode === 'grid' ? 'secondary' : 'tertiary'"
          :label="t('ui.library.view.grid')"
          :aria-pressed="viewMode === 'grid'"
          @click="setView('grid')"
        />
        <AppButton
          size="default"
          icon="list"
          icon-only
          :title="t('ui.library.view.list')"
          :variant="viewMode === 'list' ? 'secondary' : 'tertiary'"
          :label="t('ui.library.view.list')"
          :aria-pressed="viewMode === 'list'"
          @click="setView('list')"
        />
      </div>
    </section>
    <div class="library-collection-heading">
      <h2>{{ t('ui.library.collection.label') }}</h2>
      <span class="library-result-count" role="status" aria-live="polite">{{ resultLabel }}</span>
    </div>

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
                :aria-label="t('ui.library.cover.fallbackLabel', { name: displayName(app) })"
              >
                <UiIcon
                  v-if="isRemoteSessionApp(app)"
                  name="devices"
                  :size="48"
                  aria-hidden="true"
                />
                <span v-else class="library-item__monogram" aria-hidden="true">{{
                  appInitials(app)
                }}</span>
                <span>{{
                  t(
                    isRemoteSessionApp(app)
                      ? 'ui.library.cover.desktop'
                      : 'ui.library.cover.application',
                  )
                }}</span>
              </span>
              <span v-if="selectedUuids.has(appUuid(app))" class="library-item__selected-mark">
                <UiIcon name="check" :size="16" aria-hidden="true" />
                <span class="vs-sr-only">{{ t('ui.library.selection.selected') }}</span>
              </span>
              <span
                v-if="providerInfo(app)"
                class="library-item__provider-mark"
                role="img"
                :aria-label="providerLabel(app)"
                :title="providerLabel(app)"
              >
                <svg
                  v-if="providerInfo(app)?.id === 'steam'"
                  viewBox="0 0 16 16"
                  aria-hidden="true"
                >
                  <path
                    d="M12.5 2a3.5 3.5 0 0 0-3.453 2.941L6.568 9.057A3 3 0 0 0 6 9a3 3 0 0 0-1.307.303L.268 6.748A10 10 0 0 0 0 9c0 .371.025.738.072 1.1l2.936 1.693L3 12a3 3 0 1 0 5.984-.283l4.377-2.83A3.5 3.5 0 1 0 12.5 2Zm0 1A2.5 2.5 0 1 1 10 5.5 2.5 2.5 0 0 1 12.5 3Zm0 1A1.5 1.5 0 1 0 14 5.5 1.5 1.5 0 0 0 12.5 4ZM6 10a2 2 0 1 1-1.959 2.389l.705.408a1.5 1.5 0 0 0 1.5-2.598l-.338-.195A2 2 0 0 1 6 10Z"
                  />
                </svg>
                <svg
                  v-else-if="providerInfo(app)?.id === 'lutris'"
                  viewBox="0 0 24 24"
                  aria-hidden="true"
                >
                  <path
                    d="M21.231 18.89c-1.293 3.243-5.218 5.232-9.446 5.105C5.3 23.993 0 18.48 0 11.906S5.276.001 11.785.001c1.793 0 3.493.406 5.015 1.13.081-.177.271-.544.451-.557.238-.017.374.137.526.309.154.172.46.429.46.429s1.393-.481 2.955.377c1.563.858 1.783 1.116 2.09 1.716.152.301.195.829.2 1.282a.796.796 0 0 0-.07-.003c-.496 0-.96.455-.96 1.08 0 .263.082.496.215.678l-.01.007a1.505 1.505 0 0 0-.132.01 18.704 18.704 0 0 0-.389-.142 2.53 2.53 0 0 1-.82-.472 1.402 1.402 0 0 0-1.196-2.112c-.383 0-.73.156-.982.41-.472-.271-1.174-.482-2.527-.565l-.407-.011c-2.282.012-3.611.279-5.979 1.301-.603.283-1.206.615-1.785 1.001-.423.3-.639.67-.709 1.137a1.326 1.326 0 0 0 1.23 1.373h.042c1.27.06 2.039 1.99 2.063 2.497.004.05.004.023.003.08-.032.727-.37 1.267-1.088 1.246a1.231 1.231 0 0 1-.976-.494c-.063-.077-.103-.172-.159-.254-.666-1.081-1.732-1.36-2.771-1.523-.438-.068-1.073-.122-1.31.25a8.28 8.28 0 0 0-.577 3.063c-.02 5.036 4.041 9.118 9.026 9.118 2.575 0 5.349-.952 6.993-2.7-1.772 1.473-4.66 1.941-6.027 1.941-4.302 0-7.818-3.232-7.818-7.578 0-1.276.288-2.396.814-3.36.495.183.947.483 1.28 1.022l.013.021c.064.092.111.197.182.284.424.524.881.658 1.342.68h.01c.43.013.768-.12 1.024-.342.347-.3.55-.79.577-1.382v-.014c.002-.085 0-.053-.004-.112-.024-.376-.333-1.318-.906-2.027-.266-.331-.587-.607-.95-.774l.12-.074c.756-.457 2.364-.977 4.592-.638 1.13.173 2.055.419 3.483.879 1.657.534 2.579 1.279 3.854 1.427.15.017.301.018.45.003.41 1.129.634 2.35.634 3.621 0 2.068-.59 3.995-1.611 5.62Zm1.947-12.274s-.115.201-.364.322c-.103.05-.282-.075-.45.1-.359.726.516 1.332.923 1.315.408-.017.73-.432.712-.793-.017-.558-.82-.944-.82-.944Zm.234-1.432c.255 0 .462.26.462.58 0 .32-.207.58-.462.58-.254 0-.46-.26-.46-.58 0-.32.206-.58.46-.58Zm-3.292-.951c.492 0 .89.403.89.9a.895.895 0 0 1-.89.898.895.895 0 0 1-.89-.899c0-.496.399-.899.89-.899Z"
                  />
                </svg>
                <svg v-else viewBox="0 0 1024 1024" aria-hidden="true">
                  <path
                    d="M966.686 623.899c-9.773-81.666-29.323-161.25-54.514-239.447-13.759-42.709-30.419-84.189-56.091-121.452-31.701-46.014-74.789-72.958-130.812-78.579-29.631-2.973-57.785 4.118-85.677 12.35-61.172 18.056-123.359 25.124-186.493 14.903-30.919-5.006-61.308-13.526-91.743-21.225-76.445-19.338-145.323 4.995-191.165 69.261-11.441 16.04-21.194 33.543-29.78 51.312-25.091 51.925-40.443 107.249-54.53 162.924-18.822 74.393-33.019 149.491-33.664 226.571 0 7.184-.342 14.386.061 21.547 1.557 27.727 4.354 55.289 16.045 80.97 15.334 33.68 45.905 46.725 79.471 31.198 18.291-8.461 36.293-19.857 50.766-33.743 24.597-23.598 46.616-49.934 69.125-75.64 17.934-20.481 39.086-35.301 66.115-40.203 15.779-2.862 31.802-6.006 47.736-6.118 87.888-.62 175.783-.602 263.673-.278 51.4.189 93.314 19.382 124.091 62.134 12.518 17.388 27.83 32.889 42.78 48.371 18.598 19.259 38.974 36.431 64.412 46.39 32.967 12.907 62.547 1.677 77.882-30.198 3.965-8.242 6.963-17.122 9.155-26.017 12.67-51.446 9.346-103.373 3.158-155.081ZM315.471 527.643c-44.289.213-80.733-36.32-80.847-81.045-.115-45.048 35.472-81.194 80.197-81.458 44.521-.263 80.718 35.897 80.884 80.801.166 44.73-35.932 81.488-80.234 81.702Zm393.386-208.342c21.859.06 39.486 17.884 39.471 39.91-.015 22.133-17.489 39.677-39.523 39.682-22.045.005-39.456-17.53-39.444-39.724.011-22.044 17.728-39.928 39.496-39.868ZM622.269 486.36c-21.542.085-39.7-18.08-39.808-39.822-.108-21.888 17.617-39.622 39.62-39.641 22.066-.018 39.759 17.552 39.718 39.442-.041 21.866-17.89 39.936-39.53 40.021Zm86.698 86.973c-21.823.096-39.537-17.668-39.611-39.721-.074-22.079 17.523-39.992 39.338-40.044 21.715-.052 39.597 17.908 39.645 39.816.047 22.093-17.456 39.853-39.372 39.949Zm86.785-86.971c-21.764.155-39.671-17.882-39.651-39.938.021-22.15 17.628-39.639 39.793-39.525 22.091.114 39.527 17.993 39.155 40.152-.363 21.682-17.833 39.158-39.297 39.311Z"
                  />
                </svg>
              </span>
            </span>
            <span class="library-item__copy">
              <span class="library-item__title-line">
                <span class="library-item__title">{{ displayName(app) }}</span>
              </span>
              <span class="library-item__source">{{ providerLabel(app) }}</span>
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
              v-if="!isRemoteSessionApp(app)"
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

.library-collection-heading {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-8);
  margin-top: var(--vs-space-8);
}
.library-collection-heading h2 {
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
}
.library-page :deep(.vs-empty-state) {
  width: 100%;
  max-inline-size: none;
  min-block-size: 22rem;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}
.library-view-toggle :deep([aria-pressed='true']) {
  background: var(--vs-color-bg-raised);
  border-color: transparent;
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
  background: var(--vs-color-bg-surface);
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
  background: color-mix(in srgb, var(--vs-color-accent-default) 5%, var(--vs-color-bg-surface));
  font-size: var(--vs-type-size-helper);
  text-align: center;
}
.library-item__monogram {
  color: color-mix(in srgb, var(--vs-color-accent-default) 70%, var(--vs-color-text-muted));
  font-size: 56px;
  line-height: 1.2;
  font-weight: var(--vs-type-weight-medium);
  letter-spacing: -0.06em;
}
.library-collection--list .library-item__monogram {
  font-size: 24px;
}
.library-collection--list .library-item__artwork-fallback > span:last-child {
  display: none;
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

.library-item__provider-mark {
  position: absolute;
  inset-block-end: var(--vs-space-8);
  inset-inline-start: var(--vs-space-8);
  display: grid;
  inline-size: 2rem;
  block-size: 2rem;
  place-items: center;
  border: var(--vs-border-width) solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  background: color-mix(in srgb, var(--vs-color-bg-canvas) 82%, transparent);
  box-shadow: var(--vs-shadow-raised);
  color: var(--vs-color-text-primary);
  backdrop-filter: blur(8px);
}

.library-item__provider-mark svg {
  inline-size: 1.25rem;
  block-size: 1.25rem;
  fill: currentColor;
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

.library-item__title-line {
  display: flex;
  min-inline-size: 0;
  align-items: center;
  gap: var(--vs-space-8);
}

.library-item__title-line .library-item__title {
  min-inline-size: 0;
  flex: 1 1 auto;
}

.library-item__source {
  font-size: var(--vs-type-size-helper);
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
