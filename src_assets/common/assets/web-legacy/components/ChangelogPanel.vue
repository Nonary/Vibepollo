<template>
  <n-card class="changelog-panel" :segmented="{ content: true }">
    <template #header>
      <h2 class="text-xl sm:text-2xl font-semibold tracking-tight mx-auto text-center">
        {{ $t('changelog.title') }}
      </h2>
    </template>

    <div class="changelog-body">
      <header class="changelog-header">
        <div class="changelog-header__intro">
          <p class="changelog-header__description">
            {{ $t('changelog.description', { version: installedVersion }) }}
          </p>
          <div class="changelog-header__tags">
            <n-tag round size="small" type="info">
              {{ $t('changelog.installed') }}: {{ installedVersion }}
            </n-tag>
            <n-tag v-if="latestAvailable" round size="small" type="success">
              {{ $t('changelog.latest_available') }}: {{ latestAvailable.tag }}
            </n-tag>
            <n-tag v-if="bundledOnly" round size="small" type="warning">
              {{ $t('changelog.bundled_only') }}
            </n-tag>
          </div>
        </div>
        <div class="changelog-header__controls">
          <n-radio-group v-model:value="filter" name="changelog-filter" size="small">
            <n-radio-button value="current">{{ $t('changelog.filter_current') }}</n-radio-button>
            <n-radio-button value="line">{{ $t('changelog.filter_line') }}</n-radio-button>
            <n-radio-button value="all">{{ $t('changelog.filter_all') }}</n-radio-button>
          </n-radio-group>
          <n-button size="small" :loading="loading" @click="refresh">
            <i class="fas fa-rotate-right" />
            <span>{{ $t('_common.refresh') }}</span>
          </n-button>
        </div>
      </header>

      <n-alert v-if="githubError" type="warning" :show-icon="true" class="changelog-alert">
        {{ $t('changelog.github_error') }}
      </n-alert>

      <div class="changelog-meta">
        <span class="changelog-meta__title">{{ filterTitle }}</span>
        <span class="changelog-meta__count">
          {{ $t('changelog.release_count', { count: filteredReleases.length }) }}
        </span>
      </div>

      <div class="changelog-list">
        <n-spin :show="loading">
          <n-empty
            v-if="!loading && filteredReleases.length === 0"
            :description="$t('changelog.no_releases')"
          />
          <ol v-else class="changelog-timeline">
            <li
              v-for="release in filteredReleases"
              :key="release.tag"
              class="changelog-entry"
              :class="{
                'changelog-entry--installed': isInstalled(release),
                'changelog-entry--latest': isLatest(release),
              }"
            >
              <div class="changelog-marker" />
              <article class="changelog-card">
                <header class="changelog-card__header">
                  <div class="min-w-0">
                    <h3 class="changelog-card__title">
                      {{ release.name || release.tag }}
                    </h3>
                    <p class="changelog-card__meta">
                      <span class="font-mono">{{ release.tag }}</span>
                      <span v-if="release.date" class="changelog-card__sep">•</span>
                      <span v-if="release.date">{{ release.date }}</span>
                    </p>
                  </div>
                  <div class="changelog-card__tags">
                    <n-tag v-if="isInstalled(release)" size="small" type="info" round>
                      {{ $t('changelog.current_badge') }}
                    </n-tag>
                    <n-tag v-if="isLatest(release)" size="small" type="success" round>
                      {{ $t('changelog.latest_badge') }}
                    </n-tag>
                    <n-tag
                      size="small"
                      :type="release.channel === 'stable' ? 'success' : 'warning'"
                      round
                    >
                      {{ channelLabel(release) }}
                    </n-tag>
                    <n-tag size="small" type="default" round>{{ sourceLabel(release) }}</n-tag>
                  </div>
                </header>

                <div class="changelog-card__body">
                  <template v-if="release.sections.length > 0">
                    <section
                      v-for="section in release.sections"
                      :key="section.heading"
                      class="changelog-section"
                    >
                      <h4 class="changelog-section__heading">{{ section.heading }}</h4>
                      <p
                        v-for="paragraph in section.body"
                        :key="paragraph"
                        class="changelog-section__paragraph"
                      >
                        {{ paragraph }}
                      </p>
                      <ul v-if="section.bullets.length" class="changelog-section__list">
                        <li v-for="bullet in section.bullets" :key="bullet">{{ bullet }}</li>
                      </ul>
                    </section>
                  </template>
                  <pre v-else class="changelog-plain">{{ release.body }}</pre>
                </div>

                <footer v-if="release.url" class="changelog-card__footer">
                  <a
                    class="changelog-card__link"
                    :href="release.url"
                    target="_blank"
                    rel="noopener noreferrer"
                  >
                    <i class="fas fa-arrow-up-right-from-square" />
                    <span>{{ $t('changelog.open_github') }}</span>
                  </a>
                </footer>
              </article>
            </li>
          </ol>
        </n-spin>
      </div>
    </div>
  </n-card>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { NAlert, NButton, NCard, NEmpty, NRadioButton, NRadioGroup, NSpin, NTag } from 'naive-ui';
import { loadChangelog } from '@/services/changelog';
import type { ChangelogEntry } from '@/utils/changelog';
import { normalizeChangelogTag, parseChangelogVersion } from '@/utils/changelog';

type FilterMode = 'current' | 'line' | 'all';

const { t: $t } = useI18n();
const releases = ref<ChangelogEntry[]>([]);
const installedVersion = ref('0.0.0');
const latestAvailable = ref<ChangelogEntry | null>(null);
const githubError = ref<string | null>(null);
const bundledOnly = ref(false);
const loading = ref(false);
const filter = ref<FilterMode>('line');

const installedInfo = computed(() => parseChangelogVersion(installedVersion.value));

const filteredReleases = computed(() => {
  if (filter.value === 'all') return releases.value;
  if (filter.value === 'line') {
    return releases.value.filter((release) => release.releaseLine === installedInfo.value.releaseLine);
  }
  return releases.value.filter((release) => release.coreVersion === installedInfo.value.coreVersion);
});

const filterTitle = computed(() => {
  if (filter.value === 'all') return $t('changelog.all_releases_title');
  if (filter.value === 'line') {
    return $t('changelog.release_line_title', { line: `${installedInfo.value.releaseLine}.*` });
  }
  return $t('changelog.current_version_title', { version: `${installedInfo.value.coreVersion}*` });
});

function isInstalled(release: ChangelogEntry): boolean {
  return normalizeChangelogTag(release.tag).toLowerCase() === normalizeChangelogTag(installedVersion.value).toLowerCase();
}

function isLatest(release: ChangelogEntry): boolean {
  return !!latestAvailable.value && release.tag === latestAvailable.value.tag;
}

function channelLabel(release: ChangelogEntry): string {
  if (release.channel === 'stable') return $t('changelog.channel_stable');
  if (release.channel === 'alpha') return $t('changelog.channel_alpha');
  if (release.channel === 'beta') return $t('changelog.channel_beta');
  if (release.channel === 'rc') return $t('changelog.channel_rc');
  return $t('changelog.channel_other');
}

function sourceLabel(release: ChangelogEntry): string {
  return release.source === 'github' ? $t('changelog.source_github') : $t('changelog.source_bundled');
}

async function refresh(): Promise<void> {
  loading.value = true;
  try {
    const result = await loadChangelog();
    releases.value = result.releases;
    installedVersion.value = result.installedVersion;
    latestAvailable.value = result.latestAvailable;
    githubError.value = result.githubError;
    bundledOnly.value = result.bundledOnly;
  } finally {
    loading.value = false;
  }
}

onMounted(() => {
  void refresh();
});
</script>

<style scoped>
.changelog-body {
  display: flex;
  flex-direction: column;
  gap: 1rem;
}

.changelog-header {
  display: flex;
  flex-direction: column;
  gap: 1rem;
  padding-bottom: 1rem;
  border-bottom: 1px solid rgb(var(--color-dark) / 0.08);
}

.dark .changelog-header {
  border-bottom-color: rgb(var(--color-light) / 0.1);
}

@media (min-width: 900px) {
  .changelog-header {
    flex-direction: row;
    align-items: flex-start;
    justify-content: space-between;
    gap: 1.5rem;
  }
}

.changelog-header__intro {
  min-width: 0;
  flex: 1 1 auto;
}

.changelog-header__description {
  margin: 0;
  font-size: 0.875rem;
  line-height: 1.5;
  opacity: 0.8;
}

.changelog-header__tags {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
  margin-top: 0.75rem;
}

.changelog-header__controls {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
  flex-shrink: 0;
}

@media (min-width: 900px) {
  .changelog-header__controls {
    justify-content: flex-end;
  }
}

.changelog-alert {
  border-radius: 0.8rem;
}

.changelog-meta {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  justify-content: space-between;
  gap: 0.5rem;
  font-size: 0.8125rem;
}

.changelog-meta__title {
  font-weight: 600;
  letter-spacing: 0.01em;
}

.changelog-meta__count {
  font-size: 0.75rem;
  opacity: 0.7;
}

.changelog-list {
  position: relative;
  max-height: min(42rem, calc(100vh - 18rem));
  overflow-y: auto;
  padding-right: 0.25rem;
  margin-right: -0.25rem;
}

.changelog-list::-webkit-scrollbar {
  width: 8px;
}

.changelog-list::-webkit-scrollbar-thumb {
  background: rgb(var(--color-dark) / 0.18);
  border-radius: 999px;
}

.dark .changelog-list::-webkit-scrollbar-thumb {
  background: rgb(var(--color-light) / 0.18);
}

.changelog-timeline {
  position: relative;
  display: flex;
  flex-direction: column;
  gap: 1.25rem;
  margin: 0;
  padding: 0.25rem 0 0.25rem 1.5rem;
  list-style: none;
}

.changelog-timeline::before {
  position: absolute;
  top: 0.75rem;
  bottom: 0.75rem;
  left: 0.4375rem;
  width: 2px;
  content: '';
  background: linear-gradient(
    to bottom,
    rgb(var(--color-dark) / 0.04) 0%,
    rgb(var(--color-dark) / 0.16) 12%,
    rgb(var(--color-dark) / 0.16) 88%,
    rgb(var(--color-dark) / 0.04) 100%
  );
}

.dark .changelog-timeline::before {
  background: linear-gradient(
    to bottom,
    rgb(var(--color-light) / 0.04) 0%,
    rgb(var(--color-light) / 0.18) 12%,
    rgb(var(--color-light) / 0.18) 88%,
    rgb(var(--color-light) / 0.04) 100%
  );
}

.changelog-entry {
  position: relative;
}

.changelog-marker {
  position: absolute;
  top: 1rem;
  left: -1.375rem;
  width: 0.75rem;
  height: 0.75rem;
  border: 2px solid rgb(var(--color-primary));
  border-radius: 9999px;
  background: rgb(var(--color-light));
  transition: box-shadow 0.2s ease, background 0.2s ease;
}

.dark .changelog-marker {
  background: rgb(var(--color-surface));
}

.changelog-entry--installed .changelog-marker,
.changelog-entry--latest .changelog-marker {
  box-shadow: 0 0 0 4px rgb(var(--color-primary) / 0.2);
  background: rgb(var(--color-primary));
}

.changelog-card {
  position: relative;
  padding: 1rem 1.125rem;
  border-radius: 0.75rem;
  background: rgb(var(--color-dark) / 0.025);
  border: 1px solid transparent;
  transition: background 0.2s ease, border-color 0.2s ease;
}

.dark .changelog-card {
  background: rgb(var(--color-light) / 0.04);
}

.changelog-card:hover {
  background: rgb(var(--color-dark) / 0.045);
}

.dark .changelog-card:hover {
  background: rgb(var(--color-light) / 0.06);
}

.changelog-entry--installed .changelog-card {
  background: rgb(var(--color-primary) / 0.06);
  border-color: rgb(var(--color-primary) / 0.25);
}

.dark .changelog-entry--installed .changelog-card {
  background: rgb(var(--color-primary) / 0.1);
  border-color: rgb(var(--color-primary) / 0.35);
}

.changelog-card__header {
  display: flex;
  flex-direction: column;
  gap: 0.625rem;
  align-items: flex-start;
}

@media (min-width: 640px) {
  .changelog-card__header {
    flex-direction: row;
    justify-content: space-between;
    align-items: flex-start;
    gap: 1rem;
  }
}

.changelog-card__title {
  margin: 0;
  font-size: 1rem;
  font-weight: 600;
  line-height: 1.4;
  word-break: break-word;
}

.changelog-card__meta {
  margin: 0.25rem 0 0;
  font-size: 0.75rem;
  opacity: 0.7;
  display: inline-flex;
  flex-wrap: wrap;
  gap: 0.4rem;
  align-items: center;
}

.changelog-card__sep {
  opacity: 0.5;
}

.changelog-card__tags {
  display: flex;
  flex-wrap: wrap;
  gap: 0.375rem;
}

@media (min-width: 640px) {
  .changelog-card__tags {
    justify-content: flex-end;
  }
}

.changelog-card__body {
  margin-top: 0.875rem;
  font-size: 0.875rem;
  line-height: 1.55;
  display: flex;
  flex-direction: column;
  gap: 0.875rem;
}

.changelog-section {
  display: flex;
  flex-direction: column;
  gap: 0.4rem;
}

.changelog-section__heading {
  margin: 0;
  font-size: 0.7rem;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  opacity: 0.65;
}

.changelog-section__paragraph {
  margin: 0;
  white-space: pre-wrap;
  opacity: 0.92;
}

.changelog-section__list {
  margin: 0;
  padding-left: 1.25rem;
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
  opacity: 0.92;
}

.changelog-section__list > li::marker {
  color: rgb(var(--color-primary) / 0.7);
}

.changelog-card__footer {
  margin-top: 0.875rem;
  padding-top: 0.75rem;
  border-top: 1px dashed rgb(var(--color-dark) / 0.1);
}

.dark .changelog-card__footer {
  border-top-color: rgb(var(--color-light) / 0.12);
}

.changelog-card__link {
  display: inline-flex;
  align-items: center;
  gap: 0.4rem;
  font-size: 0.8125rem;
  font-weight: 500;
  color: rgb(var(--color-primary));
  text-decoration: none;
}

.changelog-card__link:hover {
  text-decoration: underline;
}

.changelog-plain {
  margin: 0;
  white-space: pre-wrap;
  font-family: inherit;
  font-size: 0.875rem;
  line-height: 1.55;
  opacity: 0.92;
}
</style>
