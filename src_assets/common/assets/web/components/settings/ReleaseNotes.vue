<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { AppButton, InlineAlert } from '@/components/ui';
import {
  githubReleaseToChangelogEntry,
  mergeChangelogEntries,
  type ChangelogEntry,
  type GitHubReleaseLike,
} from '@/utils/changelog';
const props = defineProps<{ installedVersion?: string }>();
const { t } = useI18n();
const releases = ref<ChangelogEntry[]>([]);
const loading = ref(false);
const failed = ref(false);
async function load(remote = false) {
  if (loading.value) return;
  loading.value = true;
  failed.value = false;
  try {
    const response = await fetch(
      remote ? 'https://api.github.com/repos/Nonary/Vibepollo/releases' : '/assets/changelog.json',
      { headers: { Accept: 'application/json' }, credentials: 'omit' },
    );
    if (!response.ok) throw new Error('releases-unavailable');
    const data = await response.json();
    const incoming = remote
      ? (Array.isArray(data) ? data : [])
          .map((entry: GitHubReleaseLike) => githubReleaseToChangelogEntry(entry))
          .filter((entry): entry is ChangelogEntry => entry !== null)
      : Array.isArray(data.releases)
        ? data.releases
        : [];
    releases.value = mergeChangelogEntries(releases.value, incoming);
  } catch {
    failed.value = true;
  } finally {
    loading.value = false;
  }
}
onMounted(() => void load());
</script>
<template>
  <section class="release-notes">
    <div class="release-notes__heading">
      <h2>{{ t('ui.maintenance.releases.title') }}</h2>
      <AppButton
        :label="t('ui.maintenance.releases.check')"
        :busy="loading"
        :busy-label="t('ui.maintenance.releases.loading')"
        variant="secondary"
        @click="load(true)"
      />
    </div>
    <p>
      {{ t('ui.maintenance.releases.current') }}:
      {{ props.installedVersion || t('_common.unknown') }}
    </p>
    <InlineAlert v-if="failed" tone="warning" :title="t('ui.maintenance.releases.unavailable')" />
    <p v-else-if="!loading && !releases.length">{{ t('ui.maintenance.releases.empty') }}</p>
    <details v-for="release in releases.slice(0, 10)" :key="release.tag">
      <summary>{{ release.name || release.tag }} · {{ release.date }}</summary>
      <p class="release-notes__body">{{ release.body }}</p>
      <a
        v-if="release.url?.startsWith('https://github.com/Nonary/Vibepollo/releases/')"
        :href="release.url"
        target="_blank"
        rel="noopener noreferrer"
        >{{ t('ui.maintenance.releases.open') }}</a
      >
    </details>
  </section>
</template>
<style scoped>
.release-notes__heading {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-12);
  align-items: center;
  justify-content: space-between;
}
h2 {
  font-size: 18px;
  margin: 0;
}
details {
  padding-block: var(--vs-space-16);
  border-top: 1px solid var(--vs-color-border-subtle);
}
summary {
  cursor: pointer;
  font-weight: 600;
}
.release-notes__body {
  white-space: pre-wrap;
  overflow-wrap: anywhere;
  color: var(--vs-color-text-secondary);
}
</style>
