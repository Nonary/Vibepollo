<template>
  <div class="changelog-panel space-y-5">
    <section class="rounded-xl border border-dark/10 bg-light/70 p-4 shadow-sm backdrop-blur dark:border-light/10 dark:bg-surface/70 sm:p-5">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
        <div class="min-w-0">
          <h2 class="text-2xl font-semibold tracking-tight">{{ $t('changelog.title') }}</h2>
          <p class="mt-1 text-sm opacity-80">
            {{ $t('changelog.description', { version: installedVersion }) }}
          </p>
          <div class="mt-3 flex flex-wrap gap-2 text-xs">
            <n-tag round type="info">{{ $t('changelog.installed') }}: {{ installedVersion }}</n-tag>
            <n-tag v-if="latestAvailable" round type="success">
              {{ $t('changelog.latest_available') }}: {{ latestAvailable.tag }}
            </n-tag>
            <n-tag v-if="bundledOnly" round type="warning">
              {{ $t('changelog.bundled_only') }}
            </n-tag>
          </div>
        </div>
        <div class="flex shrink-0 flex-col gap-2 sm:items-end">
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
      </div>
    </section>

    <n-alert v-if="githubError" type="warning" :show-icon="true" class="rounded-xl">
      {{ $t('changelog.github_error') }}
    </n-alert>

    <n-card :bordered="true" class="changelog-list-card">
      <template #header>
        <div class="flex flex-col gap-1 sm:flex-row sm:items-center sm:justify-between">
          <span>{{ filterTitle }}</span>
          <span class="text-xs font-normal opacity-70">
            {{ $t('changelog.release_count', { count: filteredReleases.length }) }}
          </span>
        </div>
      </template>

      <n-spin :show="loading">
        <n-empty v-if="!loading && filteredReleases.length === 0" :description="$t('changelog.no_releases')" />
        <ol v-else class="changelog-timeline">
          <li
            v-for="release in filteredReleases"
            :key="release.tag"
            class="changelog-entry"
            :class="{ 'changelog-entry--installed': isInstalled(release), 'changelog-entry--latest': isLatest(release) }"
          >
            <div class="changelog-marker" />
            <article class="rounded-xl border border-dark/10 bg-white/70 p-4 dark:border-light/10 dark:bg-black/10">
              <header class="flex flex-col gap-3 md:flex-row md:items-start md:justify-between">
                <div class="min-w-0">
                  <h3 class="break-words text-lg font-semibold leading-snug">
                    {{ release.name || release.tag }}
                  </h3>
                  <p class="mt-1 text-xs opacity-70">
                    <span>{{ release.tag }}</span>
                    <span v-if="release.date"> • {{ release.date }}</span>
                  </p>
                </div>
                <div class="flex flex-wrap gap-2 md:justify-end">
                  <n-tag v-if="isInstalled(release)" size="small" type="info" round>
                    {{ $t('changelog.current_badge') }}
                  </n-tag>
                  <n-tag v-if="isLatest(release)" size="small" type="success" round>
                    {{ $t('changelog.latest_badge') }}
                  </n-tag>
                  <n-tag size="small" :type="release.channel === 'stable' ? 'success' : 'warning'" round>
                    {{ channelLabel(release) }}
                  </n-tag>
                  <n-tag size="small" type="default" round>{{ sourceLabel(release) }}</n-tag>
                </div>
              </header>

              <div class="mt-4 space-y-4 text-sm leading-relaxed">
                <template v-if="release.sections.length > 0">
                  <section v-for="section in release.sections" :key="section.heading" class="space-y-2">
                    <h4 class="text-sm font-semibold uppercase tracking-wide opacity-80">
                      {{ section.heading }}
                    </h4>
                    <p v-for="paragraph in section.body" :key="paragraph" class="m-0 whitespace-pre-wrap opacity-90">
                      {{ paragraph }}
                    </p>
                    <ul v-if="section.bullets.length" class="list-disc space-y-1 pl-5">
                      <li v-for="bullet in section.bullets" :key="bullet">{{ bullet }}</li>
                    </ul>
                  </section>
                </template>
                <pre v-else class="changelog-plain">{{ release.body }}</pre>
              </div>

              <div v-if="release.url" class="mt-4">
                <a class="text-brand hover:underline" :href="release.url" target="_blank" rel="noopener noreferrer">
                  {{ $t('changelog.open_github') }}
                </a>
              </div>
            </article>
          </li>
        </ol>
      </n-spin>
    </n-card>
  </div>
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
.changelog-list-card :deep(.n-card__content) {
  max-height: min(38rem, calc(100vh - 17rem));
  overflow: auto;
}

.changelog-timeline {
  position: relative;
  display: grid;
  gap: 1rem;
  margin: 0;
  padding: 0 0 0 1.4rem;
  list-style: none;
}

.changelog-timeline::before {
  position: absolute;
  top: 0.5rem;
  bottom: 0.5rem;
  left: 0.35rem;
  width: 2px;
  content: '';
  background: rgb(var(--color-dark) / 0.14);
}

.dark .changelog-timeline::before {
  background: rgb(var(--color-light) / 0.18);
}

.changelog-entry {
  position: relative;
}

.changelog-marker {
  position: absolute;
  top: 1.1rem;
  left: -1.28rem;
  width: 0.75rem;
  height: 0.75rem;
  border: 2px solid rgb(var(--color-primary));
  border-radius: 9999px;
  background: rgb(var(--color-light));
}

.dark .changelog-marker {
  background: rgb(var(--color-surface));
}

.changelog-entry--installed .changelog-marker,
.changelog-entry--latest .changelog-marker {
  box-shadow: 0 0 0 4px rgb(var(--color-primary) / 0.18);
  background: rgb(var(--color-primary));
}

.changelog-entry--installed article {
  border-color: rgb(var(--color-primary) / 0.45);
}

.changelog-plain {
  margin: 0;
  white-space: pre-wrap;
  font-family: inherit;
}
</style>
