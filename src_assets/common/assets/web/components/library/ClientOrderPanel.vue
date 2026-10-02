<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet, apiPatch } from '@/api/client';
import { AppButton, InlineAlert, UiIcon } from '@/components/ui';
import { appCoverUrl, appName, appUuid, reorderApps, type AppRecord } from '@/services/apps';

type Group = 'custom' | 'steam' | 'playnite' | 'lutris';
type ProviderGroup = Exclude<Group, 'custom'>;
type ProviderSort = 'name' | 'recent' | 'playtime';
type SectionKey = Group | 'other' | 'all';

interface Section {
  key: SectionKey;
  apps: AppRecord[];
}

const GROUPS: Group[] = ['custom', 'steam', 'playnite', 'lutris'];
const SORTS: ProviderSort[] = ['name', 'recent', 'playtime'];
const TRUE_VALUES = new Set(['true', 'enabled', '1', 'on', 'yes']);

const props = defineProps<{ apps: AppRecord[]; clientOrder: string[] }>();
const emit = defineEmits<{ changed: [] }>();
const { t, locale } = useI18n();

const config = ref<Record<string, unknown>>({});
const busy = ref(false);
const error = ref('');
const dragUuid = ref('');
const dropUuid = ref('');
const failedCovers = ref(new Set<string>());

function hasId(app: AppRecord, key: string): boolean {
  const value = app[key];
  return typeof value === 'string' && Boolean(value.trim());
}

// Mirrors proc::app_order::group_of on the host.
function groupOf(app: AppRecord): Group {
  if (hasId(app, 'playnite-id')) return 'playnite';
  if (hasId(app, 'steam-id')) return 'steam';
  if (hasId(app, 'lutris-id')) return 'lutris';
  return 'custom';
}

function configValue(key: string): string {
  const value = config.value[key];
  return value === undefined || value === null ? '' : String(value).trim().toLowerCase();
}

function isGroup(key: SectionKey | undefined): key is Group {
  return GROUPS.includes(key as Group);
}

function isProvider(key: SectionKey): key is ProviderGroup {
  return isGroup(key) && key !== 'custom';
}

const groups = computed<Group[]>(() => {
  const result: Group[] = [];
  for (const token of configValue('app_order_groups').split(',')) {
    const group = token.trim() as Group;
    if (isGroup(group) && !result.includes(group)) result.push(group);
  }
  return result;
});

const legacyOrdering = computed(() => TRUE_VALUES.has(configValue('legacy_ordering')));

// The host's /applist order; apps the host has not reloaded yet go last.
const ordered = computed<AppRecord[]>(() => {
  const byUuid = new Map(props.apps.map((app) => [appUuid(app), app]));
  const result = props.clientOrder
    .map((uuid) => byUuid.get(uuid))
    .filter((app): app is AppRecord => Boolean(app));
  const seen = new Set(result);
  return [...result, ...props.apps.filter((app) => !seen.has(app))];
});

const positions = computed(() => new Map(ordered.value.map((app, index) => [app, index + 1])));

const sections = computed<Section[]>(() => {
  if (!groups.value.length) return [{ key: 'all', apps: ordered.value }];
  const result: Section[] = groups.value
    .map((key) => ({ key, apps: ordered.value.filter((app) => groupOf(app) === key) }))
    .filter((section) => section.apps.length);
  const rest = ordered.value.filter((app) => !groups.value.includes(groupOf(app)));
  if (rest.length) result.push({ key: 'other', apps: rest });
  return result;
});

function sectionTitle(key: SectionKey): string {
  return t(`ui.library.clientOrder.groups.${key}`);
}

function reorderable(section: Section): boolean {
  return section.key === 'custom' || section.key === 'all';
}

function sortOf(group: ProviderGroup): ProviderSort {
  const value = configValue(`app_order_${group}`) as ProviderSort;
  return SORTS.includes(value) ? value : 'name';
}

function statLabel(section: Section, app: AppRecord): string {
  if (!isProvider(section.key)) return '';
  const mode = sortOf(section.key);
  const lastPlayed = Number(app['last-played'] ?? 0);
  const minutes = Number(app['playtime-minutes'] ?? 0);
  if (mode === 'recent' && lastPlayed > 0) {
    return new Date(lastPlayed * 1000).toLocaleDateString(locale.value, { dateStyle: 'medium' });
  }
  if (mode === 'playtime' && minutes > 0) {
    return t('ui.library.clientOrder.hours', { hours: Math.round(minutes / 60) });
  }
  return '';
}

function coverFailed(app: AppRecord): boolean {
  return failedCovers.value.has(appUuid(app));
}

function markCoverFailed(app: AppRecord): void {
  failedCovers.value = new Set([...failedCovers.value, appUuid(app)]);
}

async function loadConfig(): Promise<void> {
  config.value = (await apiGet<Record<string, unknown>>('/api/config')) ?? {};
}

async function run(action: () => Promise<unknown>): Promise<void> {
  if (busy.value) return;
  busy.value = true;
  error.value = '';
  try {
    await action();
    await loadConfig();
    emit('changed');
  } catch {
    error.value = t('ui.library.clientOrder.errors.save');
  } finally {
    busy.value = false;
  }
}

function patchConfig(patch: Record<string, unknown>): void {
  void run(() => apiPatch('/api/config', patch));
}

function moveGroup(group: Group, step: -1 | 1): void {
  const visible = sections.value.map((section) => section.key).filter(isGroup);
  const index = visible.indexOf(group);
  const target = index + step;
  if (index < 0 || target < 0 || target >= visible.length) return;
  [visible[index], visible[target]] = [visible[target], visible[index]];
  const hidden = groups.value.filter((key) => !visible.includes(key));
  patchConfig({ app_order_groups: [...visible, ...hidden].join(',') });
}

function moveApp(section: Section, uuid: string, toIndex: number): void {
  const ids = section.apps.map(appUuid);
  const from = ids.indexOf(uuid);
  if (from < 0 || toIndex < 0 || toIndex >= ids.length || from === toIndex) return;
  ids.splice(toIndex, 0, ...ids.splice(from, 1));
  // The host appends every app left out of the list in its current order.
  void run(() => reorderApps(ids));
}

function onDragStart(event: DragEvent, app: AppRecord): void {
  dragUuid.value = appUuid(app);
  event.dataTransfer?.setData('text/plain', dragUuid.value);
  if (event.dataTransfer) event.dataTransfer.effectAllowed = 'move';
}

function onDragOver(event: DragEvent, section: Section, app: AppRecord): void {
  if (!dragUuid.value || !section.apps.some((candidate) => appUuid(candidate) === dragUuid.value)) {
    return;
  }
  event.preventDefault();
  dropUuid.value = appUuid(app);
}

function onDrop(section: Section, index: number): void {
  const uuid = dragUuid.value;
  onDragEnd();
  if (uuid) moveApp(section, uuid, index);
}

function onDragEnd(): void {
  dragUuid.value = '';
  dropUuid.value = '';
}

onMounted(() => {
  loadConfig().catch(() => {
    error.value = t('ui.library.clientOrder.errors.load');
  });
});
</script>

<template>
  <section class="client-order" :aria-label="t('ui.library.clientOrder.label')">
    <p class="client-order__intro">{{ t('ui.library.clientOrder.description') }}</p>

    <InlineAlert
      v-if="!legacyOrdering"
      tone="warning"
      :title="t('ui.library.clientOrder.legacyTitle')"
    >
      {{ t('ui.library.clientOrder.legacyBody') }}
      <template #actions>
        <AppButton
          size="compact"
          :label="t('ui.library.clientOrder.legacyAction')"
          :disabled="busy"
          @click="patchConfig({ legacy_ordering: true })"
        />
      </template>
    </InlineAlert>

    <InlineAlert v-if="!groups.length" :title="t('ui.library.clientOrder.ungroupedTitle')">
      {{ t('ui.library.clientOrder.ungroupedBody') }}
      <template #actions>
        <AppButton
          size="compact"
          variant="primary"
          :label="t('ui.library.clientOrder.groupAction')"
          :disabled="busy"
          @click="patchConfig({ app_order_groups: GROUPS.join(',') })"
        />
      </template>
    </InlineAlert>

    <InlineAlert v-if="error" tone="danger" announce="assertive">{{ error }}</InlineAlert>

    <div
      v-for="(section, sectionIndex) in sections"
      :key="section.key"
      class="client-order__section"
      :data-section="section.key"
    >
      <header class="client-order__header">
        <div class="client-order__heading">
          <h3>{{ sectionTitle(section.key) }}</h3>
          <span v-if="reorderable(section)">{{ t('ui.library.clientOrder.dragHint') }}</span>
          <span v-else-if="section.key === 'other'">{{
            t('ui.library.clientOrder.otherHint')
          }}</span>
        </div>
        <label v-if="isProvider(section.key)" class="client-order__sort">
          <span class="vs-sr-only">
            {{ t('ui.library.clientOrder.sortLabel', { group: sectionTitle(section.key) }) }}
          </span>
          <select
            class="vs-select"
            :value="sortOf(section.key)"
            :disabled="busy"
            @change="
              patchConfig({
                [`app_order_${section.key}`]: ($event.target as HTMLSelectElement).value,
              })
            "
          >
            <option v-for="mode in SORTS" :key="mode" :value="mode">
              {{ t(`ui.library.clientOrder.sorts.${mode}`) }}
            </option>
          </select>
        </label>
        <div v-if="isGroup(section.key)" class="client-order__group-actions">
          <AppButton
            icon="chevron-up"
            icon-only
            size="compact"
            variant="tertiary"
            :label="t('ui.library.clientOrder.moveGroupUp', { group: sectionTitle(section.key) })"
            :disabled="busy || sectionIndex === 0"
            @click="moveGroup(section.key as Group, -1)"
          />
          <AppButton
            icon="chevron-down"
            icon-only
            size="compact"
            variant="tertiary"
            :label="t('ui.library.clientOrder.moveGroupDown', { group: sectionTitle(section.key) })"
            :disabled="busy || !isGroup(sections[sectionIndex + 1]?.key)"
            @click="moveGroup(section.key as Group, 1)"
          />
        </div>
      </header>

      <ol class="client-order__list">
        <li
          v-for="(app, index) in section.apps"
          :key="appUuid(app)"
          class="client-order__row"
          :class="{
            'client-order__row--dragging': dragUuid === appUuid(app),
            'client-order__row--drop': dropUuid === appUuid(app) && dragUuid !== appUuid(app),
          }"
          :draggable="reorderable(section) && !busy"
          @dragstart="onDragStart($event, app)"
          @dragover="onDragOver($event, section, app)"
          @dragleave="dropUuid = ''"
          @drop.prevent="onDrop(section, index)"
          @dragend="onDragEnd"
        >
          <UiIcon
            name="grip"
            class="client-order__grip"
            :class="{ 'client-order__grip--hidden': !reorderable(section) }"
            :size="16"
            aria-hidden="true"
          />
          <span class="client-order__position">{{ positions.get(app) }}</span>
          <span class="client-order__cover" aria-hidden="true">
            <img
              v-if="!coverFailed(app)"
              :src="appCoverUrl(app)"
              alt=""
              loading="lazy"
              @error="markCoverFailed(app)"
            />
            <span v-else>{{ (appName(app) || '?').slice(0, 1).toLocaleUpperCase() }}</span>
          </span>
          <span class="client-order__name">{{ appName(app) || t('ui.library.unnamed') }}</span>
          <span v-if="statLabel(section, app)" class="client-order__meta">
            {{ statLabel(section, app) }}
          </span>
          <div v-if="reorderable(section)" class="client-order__row-actions">
            <AppButton
              icon="chevron-up"
              icon-only
              size="compact"
              variant="tertiary"
              :label="t('ui.library.clientOrder.moveUp', { name: appName(app) })"
              :disabled="busy || index === 0"
              @click="moveApp(section, appUuid(app), index - 1)"
            />
            <AppButton
              icon="chevron-down"
              icon-only
              size="compact"
              variant="tertiary"
              :label="t('ui.library.clientOrder.moveDown', { name: appName(app) })"
              :disabled="busy || index === section.apps.length - 1"
              @click="moveApp(section, appUuid(app), index + 1)"
            />
          </div>
        </li>
      </ol>
    </div>

    <div v-if="groups.length" class="client-order__footer">
      <AppButton
        size="compact"
        variant="tertiary"
        :label="t('ui.library.clientOrder.ungroupAction')"
        :disabled="busy"
        @click="patchConfig({ app_order_groups: '' })"
      />
    </div>
  </section>
</template>

<style scoped>
.client-order {
  display: grid;
  gap: var(--vs-space-12);
}

.client-order__intro,
.client-order__heading span,
.client-order__meta {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.client-order__section {
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.client-order__header {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) var(--vs-space-12);
  background: var(--vs-color-bg-subtle);
}

.client-order__heading {
  display: flex;
  flex: 1 1 12rem;
  align-items: baseline;
  gap: var(--vs-space-8);
  min-inline-size: 0;
}

.client-order__heading h3 {
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
}

.client-order__sort .vs-select {
  inline-size: auto;
}

.client-order__group-actions,
.client-order__row-actions {
  display: flex;
  gap: var(--vs-space-4);
}

.client-order__list {
  margin: 0;
  padding: 0;
  list-style: none;
}

.client-order__row {
  display: flex;
  align-items: center;
  gap: var(--vs-space-12);
  min-block-size: 3rem;
  padding: var(--vs-space-4) var(--vs-space-12);
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.client-order__row[draggable='true'] {
  cursor: grab;
}

.client-order__row--dragging {
  opacity: 0.5;
}

.client-order__row--drop {
  box-shadow: inset 0 2px 0 var(--vs-color-accent-default);
}

.client-order__grip {
  flex: none;
  color: var(--vs-color-text-muted);
}

.client-order__grip--hidden {
  visibility: hidden;
}

.client-order__position {
  flex: none;
  min-inline-size: 2ch;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
  font-variant-numeric: tabular-nums;
  text-align: end;
}

.client-order__cover {
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

.client-order__cover img {
  inline-size: 100%;
  block-size: 100%;
  object-fit: cover;
}

.client-order__name {
  flex: 1 1 auto;
  min-inline-size: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.client-order__footer {
  display: flex;
  justify-content: flex-end;
}
</style>
