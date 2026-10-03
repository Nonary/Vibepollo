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
import { appName, appUuid, fetchAppCatalog, reorderApps, type AppRecord } from '@/services/apps';

const { t, locale } = useI18n();

const apps = ref<AppRecord[]>([]);
const clientOrder = ref<string[]>([]);
const config = ref<Record<string, unknown>>({});
const loading = ref(true);
const busy = ref(false);
const error = ref('');

const groups = computed(() => parseAppOrderGroups(config.value.app_order_groups));

// The host's /applist order; entries it does not send are left out.
const ordered = computed<AppRecord[]>(() => {
  if (!clientOrder.value.length) return apps.value;
  const byUuid = new Map(apps.value.map((app) => [appUuid(app), app]));
  return clientOrder.value
    .map((uuid) => byUuid.get(uuid))
    .filter((app): app is AppRecord => Boolean(app));
});

const listedGroups = computed(() =>
  groups.value
    .map((key) => ({ key, apps: ordered.value.filter((app) => appOrderGroup(app) === key) }))
    .filter((group) => group.apps.length),
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
  visible.splice(toIndex, 0, ...visible.splice(visible.indexOf(group), 1));
  const hidden = groups.value.filter((key) => !visible.includes(key));
  patchConfig({ app_order_groups: [...visible, ...hidden].join(',') });
}

function moveApp(list: AppRecord[], from: number, toIndex: number): void {
  const ids = list.map(appUuid);
  ids.splice(toIndex, 0, ...ids.splice(from, 1));
  // The host appends every app left out of the list in its current order.
  void run(() => reorderApps(ids));
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
      <InlineAlert v-if="!groups.length" :title="t('ui.appOrder.ungroupedTitle')">
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

      <template v-else>
        <ol class="app-order-groups" :aria-label="t('ui.appOrder.groupsLabel')">
          <li
            v-for="(group, index) in listedGroups"
            :key="group.key"
            class="app-order-group"
            :data-group="group.key"
          >
            <header class="app-order-group__header">
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
            </header>

            <ol v-if="!isProvider(group.key)" class="app-order-apps">
              <li v-for="(app, appIndex) in group.apps" :key="appUuid(app)" class="app-order-app">
                <span class="app-order-app__name">{{ appName(app) }}</span>
                <AppButton
                  icon="chevron-up"
                  icon-only
                  size="compact"
                  variant="tertiary"
                  :label="t('ui.appOrder.moveUp', { name: appName(app) })"
                  :disabled="busy || appIndex === 0"
                  @click="moveApp(group.apps, appIndex, appIndex - 1)"
                />
                <AppButton
                  icon="chevron-down"
                  icon-only
                  size="compact"
                  variant="tertiary"
                  :label="t('ui.appOrder.moveDown', { name: appName(app) })"
                  :disabled="busy || appIndex === group.apps.length - 1"
                  @click="moveApp(group.apps, appIndex, appIndex + 1)"
                />
              </li>
            </ol>

            <details v-else class="app-order-preview">
              <summary>{{ t('ui.appOrder.showAll', { count: group.apps.length }) }}</summary>
              <ol>
                <li v-for="app in group.apps" :key="appUuid(app)">
                  <span>{{ appName(app) }}</span>
                  <small>{{ statLabel(group.key, app) }}</small>
                </li>
              </ol>
            </details>
          </li>
        </ol>

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

.app-order-page,
.app-order-groups {
  display: grid;
  gap: var(--vs-space-12);
}

.app-order-groups,
.app-order-apps {
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

.app-order-group__header {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8) var(--vs-space-12);
  padding: var(--vs-space-12);
  background: var(--vs-color-bg-subtle);
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
.app-order-group__link,
.app-order-preview summary,
.app-order-preview small {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.app-order-app {
  display: flex;
  align-items: center;
  gap: var(--vs-space-12);
  min-block-size: 3rem;
  padding: var(--vs-space-4) var(--vs-space-12);
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.app-order-app__name {
  flex: 1 1 auto;
  min-inline-size: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.app-order-preview {
  padding: var(--vs-space-12);
}

.app-order-preview summary {
  cursor: pointer;
}

.app-order-preview ol {
  display: grid;
  gap: var(--vs-space-4);
  margin: var(--vs-space-8) 0 0;
  padding-inline-start: var(--vs-space-24);
}

.app-order-preview li {
  display: flex;
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.app-order-footer {
  display: flex;
  justify-content: flex-end;
}
</style>
