<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet, apiPatch } from '@/api/client';
import AppOrderSortSelect from '@/components/library/AppOrderSortSelect.vue';
import { AppButton, InlineAlert, PageHeader, UiIcon } from '@/components/ui';
import {
  APP_ORDER_GROUPS,
  appOrderGroup,
  parseAppOrderGroups,
  parseAppOrderSort,
  type AppOrderGroup,
  type AppOrderProvider,
  type AppOrderSort,
} from '@/services/appOrder';
import {
  appCoverUrl,
  appName,
  appUuid,
  fetchAppCatalog,
  reorderApps,
  type AppRecord,
} from '@/services/apps';

const PREVIEW_COUNT = 8;

const { t, locale } = useI18n();

const apps = ref<AppRecord[]>([]);
const clientOrder = ref<string[]>([]);
const config = ref<Record<string, unknown>>({});
const loading = ref(true);
const busy = ref(false);
const error = ref('');
const failedCovers = ref(new Set<string>());
// One drag at a time: either a whole group or a single custom app.
const drag = ref<{ kind: 'group' | 'app'; key: string } | null>(null);
const dropKey = ref('');

const groups = computed(() => parseAppOrderGroups(config.value.app_order_groups));

// The host's /applist order.
const ordered = computed<AppRecord[]>(() => {
  if (!clientOrder.value.length) return apps.value;
  const byUuid = new Map(apps.value.map((app) => [appUuid(app), app]));
  return clientOrder.value
    .map((uuid) => byUuid.get(uuid))
    .filter((app): app is AppRecord => Boolean(app));
});
// Apps the host replaces with its built-in controls (Remote Input, Remote Monitor), always sent last.
const fixedApps = computed(() => {
  if (!clientOrder.value.length) return [];
  const listed = new Set(clientOrder.value);
  return apps.value.filter((app) => !listed.has(appUuid(app)));
});

function appsIn(group: AppOrderGroup): AppRecord[] {
  return ordered.value.filter((app) => appOrderGroup(app) === group);
}

const listedGroups = computed(() =>
  groups.value.map((key) => ({ key, apps: appsIn(key) })).filter((group) => group.apps.length),
);
const unlistedApps = computed(() =>
  ordered.value.filter((app) => !groups.value.includes(appOrderGroup(app))),
);

function isProvider(group: AppOrderGroup): group is AppOrderProvider {
  return group !== 'custom';
}

function sortOf(provider: AppOrderProvider): AppOrderSort {
  return parseAppOrderSort(config.value[`app_order_${provider}`]);
}

function groupTitle(group: AppOrderGroup): string {
  return t(`ui.appOrder.groups.${group}`);
}

function statLabel(provider: AppOrderProvider, app: AppRecord): string {
  const lastPlayed = Number(app['last-played'] ?? 0);
  const minutes = Number(app['playtime-minutes'] ?? 0);
  if (sortOf(provider) === 'recent' && lastPlayed > 0) {
    return new Date(lastPlayed * 1000).toLocaleDateString(locale.value, { dateStyle: 'medium' });
  }
  if (sortOf(provider) === 'playtime' && minutes > 0) {
    return t('ui.appOrder.hours', { hours: Math.round(minutes / 60) });
  }
  return '';
}

function coverFailed(app: AppRecord): boolean {
  return failedCovers.value.has(appUuid(app));
}

function markCoverFailed(app: AppRecord): void {
  failedCovers.value = new Set([...failedCovers.value, appUuid(app)]);
}

async function load(): Promise<void> {
  const [catalog, settings] = await Promise.all([
    fetchAppCatalog(),
    apiGet<Record<string, unknown>>('/api/config'),
  ]);
  apps.value = catalog.apps;
  clientOrder.value = catalog.clientOrder;
  config.value = settings ?? {};
}

async function run(action: () => Promise<unknown>): Promise<void> {
  if (busy.value) return;
  busy.value = true;
  error.value = '';
  try {
    await action();
    await load();
  } catch {
    error.value = t('ui.appOrder.errors.save');
  } finally {
    busy.value = false;
  }
}

function patchConfig(patch: Record<string, unknown>): void {
  void run(() => apiPatch('/api/config', patch));
}

function moveGroup(group: AppOrderGroup, toIndex: number): void {
  const visible = listedGroups.value.map((entry) => entry.key);
  const from = visible.indexOf(group);
  if (from < 0 || toIndex < 0 || toIndex >= visible.length || from === toIndex) return;
  visible.splice(toIndex, 0, ...visible.splice(from, 1));
  const hidden = groups.value.filter((key) => !visible.includes(key));
  patchConfig({ app_order_groups: [...visible, ...hidden].join(',') });
}

function moveApp(list: AppRecord[], uuid: string, toIndex: number): void {
  const ids = list.map(appUuid);
  const from = ids.indexOf(uuid);
  if (from < 0 || toIndex < 0 || toIndex >= ids.length || from === toIndex) return;
  ids.splice(toIndex, 0, ...ids.splice(from, 1));
  // The host appends every app left out of the list in its current order.
  void run(() => reorderApps(ids));
}

function onDragStart(event: DragEvent, kind: 'group' | 'app', key: string): void {
  event.stopPropagation();
  drag.value = { kind, key };
  event.dataTransfer?.setData('text/plain', key);
  if (event.dataTransfer) event.dataTransfer.effectAllowed = 'move';
}

function onDragOver(event: DragEvent, kind: 'group' | 'app', key: string): void {
  if (drag.value?.kind !== kind) return;
  event.preventDefault();
  event.stopPropagation();
  dropKey.value = key;
}

function onDropGroup(index: number): void {
  const dragged = drag.value;
  onDragEnd();
  if (dragged?.kind === 'group') moveGroup(dragged.key as AppOrderGroup, index);
}

function onDropApp(list: AppRecord[], index: number): void {
  const dragged = drag.value;
  onDragEnd();
  if (dragged?.kind === 'app') moveApp(list, dragged.key, index);
}

function onDragEnd(): void {
  drag.value = null;
  dropKey.value = '';
}

onMounted(() => {
  load()
    .catch(() => {
      error.value = t('ui.appOrder.errors.load');
    })
    .finally(() => {
      loading.value = false;
    });
});
</script>

<template>
  <div class="vs-page app-order-page">
    <PageHeader
      :title="t('ui.appOrder.page.title')"
      :description="t('ui.appOrder.page.description')"
    >
      <template #actions>
        <RouterLink class="button button--secondary" to="/library"
          ><UiIcon name="chevron-left" />{{ t('ui.appOrder.page.back') }}</RouterLink
        >
      </template>
    </PageHeader>

    <InlineAlert v-if="error" tone="danger" announce="assertive">{{ error }}</InlineAlert>

    <template v-if="!loading">
      <template v-if="groups.length">
        <ol class="app-order-groups" :aria-label="t('ui.appOrder.groupsLabel')">
          <li
            v-for="(group, index) in listedGroups"
            :key="group.key"
            class="app-order-group"
            :class="{
              'app-order-group--dragging': drag?.kind === 'group' && drag.key === group.key,
              'app-order-group--drop':
                drag?.kind === 'group' && dropKey === group.key && drag.key !== group.key,
            }"
            :data-group="group.key"
            @dragover="onDragOver($event, 'group', group.key)"
            @drop.prevent="onDropGroup(index)"
          >
            <header
              class="app-order-group__header"
              :draggable="!busy"
              @dragstart="onDragStart($event, 'group', group.key)"
              @dragend="onDragEnd"
            >
              <UiIcon name="grip" class="app-order-grip" :size="16" aria-hidden="true" />
              <span class="app-order-group__position">{{ index + 1 }}</span>
              <div class="app-order-group__heading">
                <h2>{{ groupTitle(group.key) }}</h2>
                <span>{{
                  t('ui.appOrder.count', { count: group.apps.length }, group.apps.length)
                }}</span>
              </div>
              <template v-if="isProvider(group.key)">
                <AppOrderSortSelect
                  :label="t('ui.appOrder.sortLabel', { group: groupTitle(group.key) })"
                  :value="sortOf(group.key)"
                  :disabled="busy"
                  @change="patchConfig({ [`app_order_${group.key}`]: $event })"
                />
                <RouterLink
                  class="app-order-group__link"
                  :to="`/integrations#integration-${group.key}`"
                >
                  {{ t('ui.appOrder.integrationSettings') }}
                </RouterLink>
              </template>
              <div class="app-order-group__actions">
                <AppButton
                  icon="chevron-up"
                  icon-only
                  size="compact"
                  variant="tertiary"
                  :label="t('ui.appOrder.moveUp', { name: groupTitle(group.key) })"
                  :disabled="busy || index === 0"
                  @click="moveGroup(group.key, index - 1)"
                />
                <AppButton
                  icon="chevron-down"
                  icon-only
                  size="compact"
                  variant="tertiary"
                  :label="t('ui.appOrder.moveDown', { name: groupTitle(group.key) })"
                  :disabled="busy || index === listedGroups.length - 1"
                  @click="moveGroup(group.key, index + 1)"
                />
              </div>
            </header>

            <ol v-if="!isProvider(group.key)" class="app-order-apps">
              <li
                v-for="(app, appIndex) in group.apps"
                :key="appUuid(app)"
                class="app-order-app"
                :class="{
                  'app-order-app--dragging': drag?.kind === 'app' && drag.key === appUuid(app),
                  'app-order-app--drop':
                    drag?.kind === 'app' && dropKey === appUuid(app) && drag.key !== appUuid(app),
                }"
                :draggable="!busy"
                @dragstart="onDragStart($event, 'app', appUuid(app))"
                @dragover="onDragOver($event, 'app', appUuid(app))"
                @drop.prevent.stop="onDropApp(group.apps, appIndex)"
                @dragend="onDragEnd"
              >
                <UiIcon name="grip" class="app-order-grip" :size="16" aria-hidden="true" />
                <span class="app-order-cover" aria-hidden="true">
                  <img
                    v-if="!coverFailed(app)"
                    :src="appCoverUrl(app)"
                    alt=""
                    loading="lazy"
                    @error="markCoverFailed(app)"
                  />
                  <span v-else>{{ (appName(app) || '?').slice(0, 1).toLocaleUpperCase() }}</span>
                </span>
                <span class="app-order-app__name">{{ appName(app) }}</span>
                <AppButton
                  icon="chevron-up"
                  icon-only
                  size="compact"
                  variant="tertiary"
                  :label="t('ui.appOrder.moveUp', { name: appName(app) })"
                  :disabled="busy || appIndex === 0"
                  @click="moveApp(group.apps, appUuid(app), appIndex - 1)"
                />
                <AppButton
                  icon="chevron-down"
                  icon-only
                  size="compact"
                  variant="tertiary"
                  :label="t('ui.appOrder.moveDown', { name: appName(app) })"
                  :disabled="busy || appIndex === group.apps.length - 1"
                  @click="moveApp(group.apps, appUuid(app), appIndex + 1)"
                />
              </li>
            </ol>

            <div v-else class="app-order-preview">
              <ul class="app-order-preview__covers">
                <li
                  v-for="app in group.apps.slice(0, PREVIEW_COUNT)"
                  :key="appUuid(app)"
                  :title="appName(app)"
                >
                  <span class="app-order-cover app-order-cover--large">
                    <img
                      v-if="!coverFailed(app)"
                      :src="appCoverUrl(app)"
                      :alt="appName(app)"
                      loading="lazy"
                      @error="markCoverFailed(app)"
                    />
                    <span v-else>{{ (appName(app) || '?').slice(0, 1).toLocaleUpperCase() }}</span>
                  </span>
                </li>
              </ul>
              <details class="app-order-preview__all">
                <summary>{{ t('ui.appOrder.showAll', { count: group.apps.length }) }}</summary>
                <ol>
                  <li v-for="app in group.apps" :key="appUuid(app)">
                    <span>{{ appName(app) }}</span>
                    <small>{{ statLabel(group.key, app) }}</small>
                  </li>
                </ol>
              </details>
            </div>
          </li>
        </ol>

        <section v-if="unlistedApps.length" class="app-order-group app-order-group--static">
          <header class="app-order-group__header">
            <div class="app-order-group__heading">
              <h2>{{ t('ui.appOrder.groups.other') }}</h2>
              <span>{{ t('ui.appOrder.otherHint') }}</span>
            </div>
          </header>
        </section>

        <p v-if="fixedApps.length" class="app-order-fixed">
          {{ t('ui.appOrder.fixedLast', { names: fixedApps.map(appName).join(', ') }) }}
        </p>

        <div class="app-order-footer">
          <AppButton
            size="compact"
            variant="tertiary"
            :label="t('ui.appOrder.ungroupAction')"
            :disabled="busy"
            @click="patchConfig({ app_order_groups: '' })"
          />
        </div>
      </template>

      <template v-else>
        <InlineAlert :title="t('ui.appOrder.ungroupedTitle')">
          {{ t('ui.appOrder.ungroupedBody') }}
          <template #actions>
            <AppButton
              size="compact"
              variant="primary"
              :label="t('ui.appOrder.groupAction')"
              :disabled="busy"
              @click="patchConfig({ app_order_groups: APP_ORDER_GROUPS.join(',') })"
            />
          </template>
        </InlineAlert>

        <section class="app-order-group">
          <header class="app-order-group__header">
            <div class="app-order-group__heading">
              <h2>{{ t('ui.appOrder.groups.all') }}</h2>
              <span>{{ t('ui.appOrder.count', { count: ordered.length }, ordered.length) }}</span>
            </div>
          </header>
          <ol class="app-order-apps">
            <li
              v-for="(app, appIndex) in ordered"
              :key="appUuid(app)"
              class="app-order-app"
              :class="{
                'app-order-app--dragging': drag?.kind === 'app' && drag.key === appUuid(app),
                'app-order-app--drop':
                  drag?.kind === 'app' && dropKey === appUuid(app) && drag.key !== appUuid(app),
              }"
              :draggable="!busy"
              @dragstart="onDragStart($event, 'app', appUuid(app))"
              @dragover="onDragOver($event, 'app', appUuid(app))"
              @drop.prevent.stop="onDropApp(ordered, appIndex)"
              @dragend="onDragEnd"
            >
              <UiIcon name="grip" class="app-order-grip" :size="16" aria-hidden="true" />
              <span class="app-order-cover" aria-hidden="true">
                <img
                  v-if="!coverFailed(app)"
                  :src="appCoverUrl(app)"
                  alt=""
                  loading="lazy"
                  @error="markCoverFailed(app)"
                />
                <span v-else>{{ (appName(app) || '?').slice(0, 1).toLocaleUpperCase() }}</span>
              </span>
              <span class="app-order-app__name">{{ appName(app) }}</span>
              <AppButton
                icon="chevron-up"
                icon-only
                size="compact"
                variant="tertiary"
                :label="t('ui.appOrder.moveUp', { name: appName(app) })"
                :disabled="busy || appIndex === 0"
                @click="moveApp(ordered, appUuid(app), appIndex - 1)"
              />
              <AppButton
                icon="chevron-down"
                icon-only
                size="compact"
                variant="tertiary"
                :label="t('ui.appOrder.moveDown', { name: appName(app) })"
                :disabled="busy || appIndex === ordered.length - 1"
                @click="moveApp(ordered, appUuid(app), appIndex + 1)"
              />
            </li>
          </ol>
        </section>
      </template>
    </template>
  </div>
</template>

<style scoped>
/* Unavailable moves fade out instead of taking the global disabled fill. */
.vs-button--tertiary:disabled {
  background: none;
  border-color: transparent;
  opacity: 0.3;
}

.app-order-page {
  display: grid;
  gap: var(--vs-space-12);
}

.app-order-groups {
  display: grid;
  gap: var(--vs-space-12);
  margin: 0;
  padding: 0;
  list-style: none;
}

.app-order-group {
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.app-order-group--dragging {
  opacity: 0.5;
}

.app-order-group--drop {
  box-shadow: inset 0 2px 0 var(--vs-color-accent-default);
}

.app-order-group__header {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8) var(--vs-space-12);
  padding: var(--vs-space-12);
  background: var(--vs-color-bg-subtle);
}

.app-order-group__header[draggable='true'] {
  cursor: grab;
}

.app-order-group__position {
  min-inline-size: 2ch;
  color: var(--vs-color-text-muted);
  font-variant-numeric: tabular-nums;
}

.app-order-group__heading {
  display: flex;
  flex: 1 1 10rem;
  align-items: baseline;
  gap: var(--vs-space-8);
  min-inline-size: 0;
}

.app-order-group__heading h2 {
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
}

.app-order-group__heading span,
.app-order-preview__all small {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.app-order-group__link {
  font-size: var(--vs-type-size-metadata);
}

.app-order-group__actions {
  display: flex;
  gap: var(--vs-space-4);
}

.app-order-grip {
  flex: none;
  color: var(--vs-color-text-muted);
}

.app-order-apps {
  margin: 0;
  padding: 0;
  list-style: none;
}

.app-order-app {
  display: flex;
  align-items: center;
  gap: var(--vs-space-12);
  min-block-size: 3rem;
  padding: var(--vs-space-4) var(--vs-space-12);
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.app-order-app[draggable='true'] {
  cursor: grab;
}

.app-order-app--dragging {
  opacity: 0.5;
}

.app-order-app--drop {
  box-shadow: inset 0 2px 0 var(--vs-color-accent-default);
}

.app-order-app__name {
  flex: 1 1 auto;
  min-inline-size: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.app-order-cover {
  display: grid;
  flex: none;
  place-items: center;
  overflow: hidden;
  inline-size: 1.75rem;
  aspect-ratio: 2 / 3;
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.app-order-cover--large {
  inline-size: 3.5rem;
}

.app-order-cover img {
  inline-size: 100%;
  block-size: 100%;
  object-fit: cover;
}

.app-order-preview {
  display: grid;
  gap: var(--vs-space-8);
  padding: var(--vs-space-12);
}

.app-order-preview__covers {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  margin: 0;
  padding: 0;
  list-style: none;
}

.app-order-preview__all summary {
  cursor: pointer;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.app-order-preview__all ol {
  display: grid;
  gap: var(--vs-space-4);
  margin: var(--vs-space-8) 0 0;
  padding-inline-start: var(--vs-space-24);
}

.app-order-preview__all li {
  display: flex;
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.app-order-fixed {
  margin: 0;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}

.app-order-footer {
  display: flex;
  justify-content: flex-end;
}
</style>
