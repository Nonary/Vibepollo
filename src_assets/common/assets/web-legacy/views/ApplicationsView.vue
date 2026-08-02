<template>
  <div class="applications-page space-y-5">
    <!-- Header card -->
    <section class="apps-header">
      <div class="apps-header__intro">
        <h2 class="apps-header__title">{{ $t('apps.applications_title') }}</h2>
        <p class="apps-header__description">
          {{ $t('apps.applications_intro') }}
          <span v-if="apps && apps.length" class="apps-header__count">
            <span aria-hidden="true">•</span>
            {{ $t('apps.app_count', { count: apps.length }) }}
          </span>
        </p>
      </div>

      <div class="apps-header__actions">
        <template v-if="isWindows">
          <n-button
            v-if="playniteEnabled"
            size="medium"
            type="default"
            strong
            :loading="syncBusy"
            :disabled="syncBusy"
            class="apps-header__action"
            :aria-label="$t('playnite.force_sync')"
            @click="forceSync"
          >
            <i class="fas fa-rotate-right" />
            <span>{{ $t('playnite.force_sync') }}</span>
          </n-button>

          <n-button
            v-else
            size="medium"
            type="default"
            strong
            class="apps-header__action"
            @click="gotoPlaynite"
          >
            <i class="fas fa-puzzle-piece" />
            <span>{{ $t('playnite.setup_integration') }}</span>
          </n-button>
        </template>

        <n-button
          type="primary"
          size="medium"
          strong
          class="apps-header__action apps-header__action--primary"
          @click="openAdd"
        >
          <i class="fas fa-plus" />
          <span>{{ $t('apps.add_application') }}</span>
        </n-button>
      </div>
    </section>

    <!-- List card -->
    <section class="apps-list-card">
      <div v-if="apps && apps.length" class="apps-list">
        <button
          v-for="(app, i) in apps"
          :key="appKey(app, i)"
          type="button"
          class="apps-row"
          @click="openEdit(app)"
          @keydown.enter.prevent="openEdit(app)"
          @keydown.space.prevent="openEdit(app)"
        >
          <div class="apps-row__art" aria-hidden="true">
            <img
              v-if="appHasPlayniteIcon(app)"
              class="apps-row__icon"
              :src="playniteIconUrl(app)"
              :alt="app.name || $t('apps.app_name')"
              loading="lazy"
              @error="onPlayniteIconError(app)"
            />
            <i
              v-else
              class="fas apps-row__fallback-icon"
              :class="app['playnite-id'] ? 'fa-gamepad' : 'fa-window-maximize'"
            />
          </div>

          <div class="apps-row__main">
            <div class="apps-row__title-line">
              <span class="apps-row__title">{{ app.name || $t('apps.untitled') }}</span>
              <span v-if="app['playnite-id']" class="apps-row__badge apps-row__badge--playnite">
                {{ $t('apps.playnite_badge') }}
                <span v-if="playniteSourceLabel(app)" class="apps-row__badge-detail">
                  · {{ playniteSourceLabel(app) }}
                </span>
              </span>
              <span v-else class="apps-row__badge apps-row__badge--custom">{{
                $t('apps.custom_badge')
              }}</span>
            </div>
            <div v-if="appSubtitle(app)" class="apps-row__subtitle" :title="appSubtitle(app)">
              {{ appSubtitle(app) }}
            </div>
          </div>

          <i class="fas fa-chevron-right apps-row__chevron" aria-hidden="true" />
        </button>
      </div>

      <div v-else class="apps-empty">
        <div class="apps-empty__icon">
          <i class="fas fa-th-large" aria-hidden="true" />
        </div>
        <h3 class="apps-empty__title">{{ $t('apps.no_applications_yet') }}</h3>
        <p class="apps-empty__description">
          {{ $t('apps.no_applications_hint') }}
        </p>
        <div class="apps-empty__actions">
          <n-button type="primary" size="medium" strong @click="openAdd">
            <i class="fas fa-plus" />
            <span>{{ $t('apps.add_application') }}</span>
          </n-button>
          <n-button
            v-if="isWindows && !playniteEnabled"
            size="medium"
            type="default"
            @click="gotoPlaynite"
          >
            <i class="fas fa-puzzle-piece" />
            <span>{{ $t('playnite.setup_integration') }}</span>
          </n-button>
        </div>
      </div>
    </section>

    <AppEditModal
      v-model="showModal"
      :app="currentApp"
      :index="currentAppIndex"
      @saved="reload"
      @deleted="reload"
    />
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, computed } from 'vue';
import AppEditModal from '@/components/AppEditModal.vue';
import { useAppsStore } from '@/stores/apps';
import { storeToRefs } from 'pinia';
import { NButton } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';
import { useRouter } from 'vue-router';
import { useAuthStore } from '@/stores/auth';
import { useI18n } from 'vue-i18n';
import type { App } from '@/stores/apps';

const appsStore = useAppsStore();
const { apps } = storeToRefs(appsStore);
const configStore = useConfigStore();
const auth = useAuthStore();
const router = useRouter();
const { t } = useI18n();

const syncBusy = ref(false);
const isWindows = computed(
  () => (configStore.metadata?.platform || '').toLowerCase() === 'windows',
);

const playniteInstalled = ref(false);
const playniteStatusReady = ref(false);
const playniteEnabled = computed(() => playniteInstalled.value);

const showModal = ref(false);
const currentApp = ref<App | null>(null);
const currentAppIndex = ref(-1);
const failedPlayniteIconKeys = ref<Set<string>>(new Set());

async function reload(): Promise<void> {
  await appsStore.loadApps(true);
}

function openAdd(): void {
  currentApp.value = null;
  currentAppIndex.value = -1;
  showModal.value = true;
}

function openEdit(app: App): void {
  currentApp.value = app;
  currentAppIndex.value = apps.value.findIndex((candidate) => candidate === app);
  showModal.value = true;
}

function appKey(app: App | null | undefined, index: number) {
  const id = app?.uuid || '';
  return `${app?.name || 'app'}|${id}|${index}`;
}

function firstString(...values: unknown[]): string {
  for (const value of values) {
    if (typeof value === 'string' && value.length > 0) return value;
  }
  return '';
}

function playniteIconKey(app: App): string {
  const identity = firstString(app.uuid, app['playnite-id'], app.name);
  return `${identity}|${app['playnite-icon-path'] || ''}|${app['playnite-icon-version'] || ''}`;
}

function appHasPlayniteIcon(app: App): boolean {
  const key = playniteIconKey(app);
  if (!app.uuid || !key || failedPlayniteIconKeys.value.has(key)) return false;
  return !!app['playnite-icon-path'];
}

function playniteIconUrl(app: App): string {
  const version = app['playnite-icon-version'];
  const base = `/api/apps/${encodeURIComponent(app.uuid || '')}/icon`;
  return version ? `${base}?v=${version}` : base;
}

function onPlayniteIconError(app: App): void {
  const key = playniteIconKey(app);
  if (!key) return;
  const next = new Set(failedPlayniteIconKeys.value);
  next.add(key);
  failedPlayniteIconKeys.value = next;
}

function appSubtitle(app: App): string {
  const wd = (app['working-dir'] || '').toString().trim();
  if (wd) return wd;
  const cmd = app.cmd;
  if (Array.isArray(cmd)) return cmd.filter((v) => typeof v === 'string').join(' ');
  if (typeof cmd === 'string') return cmd;
  return '';
}

function playniteSourceLabel(app: App): string {
  if (app['playnite-managed'] === 'manual') return t('apps.playnite_source_manual');
  const src = app['playnite-source'];
  if (typeof src === 'string' && src.length > 0) {
    const normalized = src.replace(/\s/g, '');
    if (normalized === 'recent') return t('apps.playnite_source_recent');
    if (normalized === 'category') return t('apps.playnite_source_category');
    if (normalized === 'recent+category' || normalized === 'category+recent')
      return t('apps.playnite_source_both');
    return t('apps.playnite_source_unknown');
  }
  return t('apps.playnite_source_managed');
}

async function forceSync(): Promise<void> {
  syncBusy.value = true;
  try {
    await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    await reload();
  } catch {
  } finally {
    syncBusy.value = false;
  }
}

function gotoPlaynite(): void {
  try {
    router.push({ path: '/settings', query: { sec: 'playnite' } });
  } catch {
    // ignore navigation errors
  }
}

async function fetchPlayniteStatus(): Promise<void> {
  if (!auth.isAuthenticated) return;
  try {
    const r = await http.get('/api/playnite/status', { validateStatus: () => true });
    if (
      r.status === 200 &&
      r.data &&
      typeof r.data === 'object' &&
      r.data !== null &&
      'installed' in (r.data as Record<string, unknown>)
    ) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const data = r.data as any;
      playniteInstalled.value = data.installed === true || data.active === true;
    }
  } catch {
    // ignore; will retry on next auth change
  } finally {
    playniteStatusReady.value = true;
  }
}

onMounted(async () => {
  try {
    await configStore.fetchConfig?.();
  } catch {}
  if (auth.isAuthenticated) {
    void fetchPlayniteStatus();
  } else {
    playniteStatusReady.value = false;
  }
  try {
    await appsStore.loadApps(true);
  } catch {}
});

auth.onLogin(() => {
  playniteStatusReady.value = false;
  void fetchPlayniteStatus();
});
</script>

<style scoped>
.applications-page {
  width: 100%;
}

/* ---------- Header ---------- */
.apps-header {
  display: flex;
  flex-direction: column;
  gap: 1rem;
  padding: 1rem 1.125rem;
  border-radius: 1rem;
  border: 1px solid rgb(var(--color-dark) / 0.1);
  background: rgb(var(--color-light) / 0.8);
  backdrop-filter: blur(6px);
  box-shadow: 0 1px 2px rgb(0 0 0 / 0.04);
}

.dark .apps-header {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-surface) / 0.74);
}

@media (min-width: 768px) {
  .apps-header {
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 1.25rem 1.5rem;
    gap: 1.5rem;
  }
}

.apps-header__intro {
  min-width: 0;
  flex: 1 1 auto;
}

.apps-header__title {
  margin: 0;
  font-size: 1.25rem;
  font-weight: 600;
  letter-spacing: -0.005em;
  line-height: 1.2;
}

@media (min-width: 768px) {
  .apps-header__title {
    font-size: 1.5rem;
  }
}

.apps-header__description {
  margin: 0.25rem 0 0;
  font-size: 0.8125rem;
  line-height: 1.5;
  opacity: 0.75;
}

.apps-header__count {
  display: inline-flex;
  align-items: center;
  gap: 0.4rem;
  font-weight: 500;
  opacity: 0.85;
  margin-left: 0.25rem;
}

.apps-header__actions {
  display: grid;
  grid-template-columns: 1fr;
  gap: 0.5rem;
  flex-shrink: 0;
}

@media (min-width: 480px) {
  .apps-header__actions {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (min-width: 768px) {
  .apps-header__actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: flex-end;
    gap: 0.5rem;
  }
}

.apps-header__action {
  width: 100%;
  justify-content: center;
}

@media (min-width: 768px) {
  .apps-header__action {
    width: auto;
  }
}

/* ---------- List card ---------- */
.apps-list-card {
  border-radius: 1rem;
  border: 1px solid rgb(var(--color-dark) / 0.1);
  background: rgb(var(--color-light) / 0.8);
  backdrop-filter: blur(6px);
  overflow: hidden;
  box-shadow: 0 1px 2px rgb(0 0 0 / 0.04);
}

.dark .apps-list-card {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-surface) / 0.74);
}

.apps-list {
  display: flex;
  flex-direction: column;
}

.apps-row {
  display: flex;
  align-items: center;
  gap: 0.875rem;
  padding: 0.625rem 1rem;
  width: 100%;
  text-align: left;
  background: transparent;
  border: 0;
  border-top: 1px solid rgb(var(--color-dark) / 0.06);
  cursor: pointer;
  transition: background 0.12s ease;
  color: inherit;
  min-height: 3.25rem;
}

.dark .apps-row {
  border-top-color: rgb(var(--color-light) / 0.08);
}

.apps-row:first-child {
  border-top: 0;
}

.apps-row:hover {
  background: rgb(var(--color-dark) / 0.04);
}

.dark .apps-row:hover {
  background: rgb(var(--color-light) / 0.06);
}

.apps-row:focus-visible {
  outline: 2px solid rgb(var(--color-primary) / 0.45);
  outline-offset: -2px;
  background: rgb(var(--color-dark) / 0.04);
}

.dark .apps-row:focus-visible {
  background: rgb(var(--color-light) / 0.06);
}

@media (min-width: 640px) {
  .apps-row {
    padding: 0.625rem 1.25rem;
  }
}

.apps-row__art {
  width: 3.25rem;
  height: 3.25rem;
  flex: 0 0 3.25rem;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  border-radius: 0.5rem;
  background: rgb(var(--color-dark) / 0.08);
  color: rgb(var(--color-dark) / 0.55);
  box-shadow: inset 0 0 0 1px rgb(var(--color-dark) / 0.08);
}

.dark .apps-row__art {
  background: rgb(var(--color-light) / 0.08);
  color: rgb(var(--color-light) / 0.62);
  box-shadow: inset 0 0 0 1px rgb(var(--color-light) / 0.08);
}

.apps-row__icon {
  width: 100%;
  height: 100%;
  display: block;
  object-fit: contain;
  padding: 0.25rem;
  /* High-quality smooth scaling; avoid forcing a GPU layer, which can soften the image. */
  image-rendering: auto;
}

.apps-row__fallback-icon {
  font-size: 1rem;
  opacity: 0.78;
}

/* Main text column */
.apps-row__main {
  min-width: 0;
  flex: 1 1 auto;
  display: flex;
  flex-direction: column;
  gap: 0.125rem;
}

.apps-row__title-line {
  display: flex;
  align-items: baseline;
  gap: 0.5rem;
  min-width: 0;
}

.apps-row__title {
  font-size: 0.9375rem;
  font-weight: 600;
  line-height: 1.3;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  flex: 0 1 auto;
  min-width: 0;
}

.apps-row__subtitle {
  font-size: 0.75rem;
  line-height: 1.35;
  opacity: 0.55;
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

/* Badges */
.apps-row__badge {
  display: inline-flex;
  align-items: center;
  gap: 0.25rem;
  font-size: 0.6875rem;
  font-weight: 600;
  padding: 0.1rem 0.5rem;
  border-radius: 9999px;
  letter-spacing: 0.02em;
  flex-shrink: 0;
  align-self: center;
  white-space: nowrap;
}

.apps-row__badge-detail {
  font-weight: 500;
  opacity: 0.75;
}

.apps-row__badge--playnite {
  color: rgb(var(--color-primary));
  background: rgb(var(--color-primary) / 0.12);
}

.dark .apps-row__badge--playnite {
  background: rgb(var(--color-primary) / 0.18);
}

.apps-row__badge--custom {
  color: rgb(var(--color-dark));
  background: rgb(var(--color-dark) / 0.08);
  opacity: 0.75;
}

.dark .apps-row__badge--custom {
  color: rgb(var(--color-light));
  background: rgb(var(--color-light) / 0.08);
}

/* Chevron */
.apps-row__chevron {
  flex-shrink: 0;
  font-size: 0.75rem;
  opacity: 0.3;
  transition:
    transform 0.12s ease,
    opacity 0.12s ease;
  margin-left: 0.25rem;
}

.apps-row:hover .apps-row__chevron,
.apps-row:focus-visible .apps-row__chevron {
  opacity: 0.7;
  transform: translateX(2px);
}

/* ---------- Empty state ---------- */
.apps-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  padding: 3rem 1.5rem;
  gap: 0.75rem;
}

.apps-empty__icon {
  width: 3.5rem;
  height: 3.5rem;
  border-radius: 9999px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 1.25rem;
  background: rgb(var(--color-primary) / 0.12);
  color: rgb(var(--color-primary));
  margin-bottom: 0.5rem;
}

.apps-empty__title {
  margin: 0;
  font-size: 1.125rem;
  font-weight: 600;
}

.apps-empty__description {
  margin: 0;
  max-width: 28rem;
  font-size: 0.875rem;
  line-height: 1.55;
  opacity: 0.7;
}

.apps-empty__actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: center;
  gap: 0.5rem;
  margin-top: 1rem;
}
</style>
