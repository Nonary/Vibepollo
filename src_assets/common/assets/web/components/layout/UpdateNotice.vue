<script setup lang="ts">
import { onBeforeUnmount, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { apiGet } from '@/api/client';
import { AppButton, InlineAlert } from '@/components/ui';
import { useSystemStore } from '@/stores/system';
import type { ChangelogEntry } from '@/utils/changelog';
import { selectAvailableUpdate } from '@/utils/updates';

const system = useSystemStore();
const { t } = useI18n();
const available = ref<ChangelogEntry | null>(null);
const failed = ref(false);
const loading = ref(false);
let timer: ReturnType<typeof setTimeout> | undefined;
let stopped = false;
let controller: AbortController | undefined;

async function check(): Promise<void> {
  if (loading.value || !system.metadata?.version || stopped) return;
  clearTimeout(timer);
  loading.value = true;
  let nextCheck = 300000;
  controller = new AbortController();
  const timeout = setTimeout(() => controller?.abort(), 15000);
  try {
    const config = await apiGet<Record<string, unknown>>('/api/config', {
      signal: controller.signal,
    });
    const response = await fetch('https://api.github.com/repos/Nonary/Vibepollo/releases', {
      headers: { Accept: 'application/json' },
      credentials: 'omit',
      signal: controller.signal,
    });
    if (!response.ok) throw new Error('releases-unavailable');
    const releases = await response.json();
    if (!Array.isArray(releases)) throw new Error('invalid-releases');
    available.value = selectAvailableUpdate(
      system.metadata.version,
      releases,
      config.notify_pre_releases,
    );
    failed.value = false;
    const interval = Number(config.update_check_interval ?? 86400);
    nextCheck = Number.isFinite(interval) && interval >= 0 ? interval * 1000 : 86400000;
  } catch {
    if (!stopped) failed.value = true;
  } finally {
    clearTimeout(timeout);
    loading.value = false;
    if (!stopped && nextCheck > 0)
      timer = setTimeout(() => void check(), Math.min(2147483647, Math.max(60000, nextCheck)));
  }
}

watch(
  () => system.metadata?.version,
  () => void check(),
  { immediate: true },
);
onBeforeUnmount(() => {
  stopped = true;
  clearTimeout(timer);
  controller?.abort();
});
</script>

<template>
  <div v-if="available || failed" class="update-notice">
    <InlineAlert
      v-if="available"
      tone="info"
      announce="polite"
      :title="t('ui.updates.available', { version: available.tag })"
    >
      <a
        :href="
          available.url?.startsWith('https://github.com/Nonary/Vibepollo/releases/')
            ? available.url
            : 'https://github.com/Nonary/Vibepollo/releases'
        "
        target="_blank"
        rel="noopener noreferrer"
        >{{ t('ui.maintenance.releases.open') }}</a
      >
    </InlineAlert>
    <InlineAlert v-if="failed" tone="warning" :title="t('ui.maintenance.releases.unavailable')">
      <AppButton
        :label="t('ui.maintenance.releases.check')"
        :busy="loading"
        :busy-label="t('ui.maintenance.releases.loading')"
        variant="secondary"
        @click="check"
      />
    </InlineAlert>
  </div>
</template>

<style scoped>
.update-notice {
  display: grid;
  gap: var(--vs-space-12);
  margin: var(--vs-space-24);
}
</style>
