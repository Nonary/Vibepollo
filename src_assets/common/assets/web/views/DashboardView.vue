<template>
  <div class="space-y-8 px-2 md:px-4">
    <!-- Hero / Intro -->
    <section
      class="rounded-xl border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 backdrop-blur p-5 md:p-6 shadow-sm"
    >
      <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-4">
        <div class="min-w-0">
          <h2 class="text-xl md:text-2xl font-semibold tracking-tight">
            {{ $t('index.welcome') }}
          </h2>
          <p class="text-sm opacity-80 mt-1 leading-relaxed">
            {{ $t('index.description') }}
          </p>
        </div>
        <div class="flex items-center gap-2 shrink-0">
          <RouterLink to="/settings" class="btn btn-primary">
            <i class="fas fa-sliders" />
            <span>Settings</span>
          </RouterLink>
          <RouterLink to="/applications" class="btn btn-secondary">
            <i class="fas fa-th" />
            <span>Applications</span>
          </RouterLink>
        </div>
      </div>
    </section>

    <!-- Fatal startup errors moved into Version card to avoid layout shift -->

    <!-- Main Grid -->
    <n-grid cols="24" x-gap="16" y-gap="16" responsive="screen">
      <!-- Version Card -->
      <n-gi :span="24" :xl="16">
        <n-card v-if="installedVersion" :segmented="{ content: true, footer: true }">
          <template #header>
            <h2 class="text-2xl font-semibold tracking-tight mx-auto text-center">
              {{ 'Version ' + displayVersion }}
            </h2>
          </template>
          <div class="space-y-4 text-sm">
            <!-- Fatal Errors (embedded) -->
            <n-alert
              v-if="fancyLogs.some((l) => l.level === 'Fatal')"
              type="error"
              :show-icon="true"
            >
              <div class="space-y-3">
                <p class="text-sm leading-relaxed" v-html="$t('index.startup_errors')"></p>
                <ul class="list-disc pl-5 space-y-1 text-xs">
                  <li v-for="(v, i) in fancyLogs.filter((x) => x.level === 'Fatal')" :key="i">
                    {{ v.value }}
                  </li>
                </ul>
                <div>
                  <a class="btn btn-danger" href="./troubleshooting#logs">
                    <i class="fas fa-file-lines" /> {{ $t('index.view_logs') || 'View Logs' }}
                  </a>
                </div>
              </div>
            </n-alert>
            <div v-if="loading" class="text-xs italic flex items-center gap-2">
              <i class="fas fa-spinner animate-spin" /> {{ $t('index.loading_latest') }}
            </div>
            <div v-if="branch || commit" class="flex items-center gap-2 text-xs">
              <span
                v-if="branch"
                class="inline-flex items-center gap-1 px-2 py-0.5 rounded-md bg-dark/5 dark:bg-light/10"
              >
                <i class="fas fa-code-branch" /> {{ branch }}
              </span>
              <span
                v-if="commit"
                class="inline-flex items-center gap-1 px-2 py-0.5 rounded-md bg-dark/5 dark:bg-light/10 font-mono"
              >
                <i class="fas fa-hashtag" /> {{ commit.substring(0, 7) }}
              </span>
            </div>
            <n-alert v-if="buildVersionIsDirty" type="success" :show-icon="true">
              {{ $t('index.version_dirty') }} 🌇
            </n-alert>
            <n-alert v-if="installedVersionNotStable" type="info" :show-icon="true">
              {{ $t('index.installed_version_not_stable') }}
            </n-alert>
            <!-- Git compare alerts removed; date-based update checks only -->
            <n-alert
              v-else-if="
                (!preReleaseBuildAvailable || !notifyPreReleases) &&
                !stableBuildAvailable &&
                !buildVersionIsDirty
              "
              type="success"
            >
              {{ $t('index.version_latest') }}
            </n-alert>

            <!-- Pre-release notice (modern banner) -->
            <n-alert
              v-if="notifyPreReleases && preReleaseBuildAvailable"
              type="warning"
              :show-icon="true"
              class="rounded-xl"
            >
              <div class="flex flex-col gap-3 w-full">
                <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-3">
                  <div class="flex items-center gap-3">
                    <span
                      class="inline-flex h-9 w-9 items-center justify-center rounded-full bg-warning/20 text-warning"
                    >
                      <i class="fas fa-flask" />
                    </span>
                    <div class="min-w-0">
                      <p class="text-sm m-0 font-medium">{{ $t('index.new_pre_release') }}</p>
                      <p class="text-xs opacity-80 m-0">
                        {{ displayVersion }} → {{ preReleaseVersion.version }}
                      </p>
                    </div>
                  </div>
                  <div class="flex items-center gap-2 shrink-0">
                    <button class="btn btn-secondary btn-sm" @click="showPreNotes = !showPreNotes">
                      <i class="fas fa-bars-staggered" />
                      <span>{{
                        showPreNotes
                          ? $t('index.hide_notes') || 'Hide Notes'
                          : $t('index.view_notes') || 'Release Notes'
                      }}</span>
                    </button>
                    <a
                      class="btn btn-primary btn-sm"
                      :href="preReleaseRelease?.html_url"
                      target="_blank"
                    >
                      <i class="fas fa-download" />
                      <span>{{ $t('index.download') }}</span>
                    </a>
                  </div>
                </div>
                <div
                  v-if="showPreNotes"
                  class="rounded-lg border border-dark/10 dark:border-light/10 bg-surface/60 dark:bg-dark/40 p-3 overflow-auto max-h-72 text-xs"
                >
                  <p class="font-semibold mb-2">{{ preReleaseRelease?.name }}</p>
                  <pre class="font-mono whitespace-pre-wrap">{{ preReleaseRelease?.body }}</pre>
                </div>
              </div>
            </n-alert>

            <!-- Stable update available (modern banner) -->
            <n-alert
              v-if="stableBuildAvailable"
              type="warning"
              :show-icon="true"
              class="rounded-xl"
            >
              <div class="flex flex-col gap-3 w-full">
                <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-3">
                  <div class="flex items-center gap-3">
                    <span
                      class="inline-flex h-9 w-9 items-center justify-center rounded-full bg-warning/20 text-warning"
                    >
                      <i class="fas fa-bolt" />
                    </span>
                    <div class="min-w-0">
                      <p class="text-sm m-0 font-medium">{{ $t('index.new_stable') }}</p>
                      <p class="text-xs opacity-80 m-0">
                        {{ displayVersion }} → {{ githubVersion.version }}
                      </p>
                    </div>
                  </div>
                  <div class="flex items-center gap-2 shrink-0">
                    <button
                      class="btn btn-secondary btn-sm"
                      @click="showStableNotes = !showStableNotes"
                    >
                      <i class="fas fa-bars-staggered" />
                      <span>{{
                        showStableNotes
                          ? $t('index.hide_notes') || 'Hide Notes'
                          : $t('index.view_notes') || 'Release Notes'
                      }}</span>
                    </button>
                    <a
                      class="btn btn-primary btn-sm"
                      :href="githubRelease?.html_url"
                      target="_blank"
                    >
                      <i class="fas fa-download" />
                      <span>{{ $t('index.download') }}</span>
                    </a>
                  </div>
                </div>
                <div
                  v-if="showStableNotes"
                  class="rounded-lg border border-dark/10 dark:border-light/10 bg-surface/60 dark:bg-dark/40 p-3 overflow-auto max-h-72 text-xs"
                >
                  <p class="font-semibold mb-2">{{ githubRelease?.name }}</p>
                  <pre class="font-mono whitespace-pre-wrap">{{ githubRelease?.body }}</pre>
                </div>
              </div>
            </n-alert>
          </div>
        </n-card>
      </n-gi>

      <!-- Resources -->
      <n-gi :span="24" :xl="8">
        <n-card>
          <template #header>
            <h2 class="text-2xl font-semibold tracking-tight mx-auto text-center">Web Links</h2>
          </template>
          <div class="text-xs space-y-2">
            <ResourceCard />
          </div>
        </n-card>
      </n-gi>
    </n-grid>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, computed } from 'vue';
import { NCard, NAlert, NGrid, NGi } from 'naive-ui';
import ResourceCard from '@/ResourceCard.vue';
import SunshineVersion, { GitHubRelease } from '@/sunshine_version';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';

const installedVersion = ref<SunshineVersion>(new SunshineVersion('0.0.0'));
const githubRelease = ref<GitHubRelease | null>(null);
const preReleaseRelease = ref<GitHubRelease | null>(null);

const githubVersion = computed(() =>
  githubRelease.value
    ? SunshineVersion.fromRelease(githubRelease.value)
    : new SunshineVersion('0.0.0'),
);
const preReleaseVersion = computed(() =>
  preReleaseRelease.value
    ? SunshineVersion.fromRelease(preReleaseRelease.value)
    : new SunshineVersion('0.0.0'),
);
const notifyPreReleases = ref(false);
const showPreNotes = ref(false);
const showStableNotes = ref(false);
const loading = ref(true);
const logs = ref('');
const branch = ref('');
const commit = ref('');

const configStore = useConfigStore();
const auth = useAuthStore();
let started = false; // prevent duplicate concurrent checks

async function runVersionChecks() {
  if (started) return; // guard
  started = true;
  loading.value = true;
  try {
    // Use config store (it already handles deep cloning & defaults)
    const cfg = await configStore.fetchConfig();
    if (!cfg) {
      // still not available (possibly lost auth); allow retry later
      started = false;
      loading.value = false;
      return;
    }
    // Normalize notify pre-release flag to boolean
    notifyPreReleases.value =
      cfg.notify_pre_releases === true || cfg.notify_pre_releases === 'enabled';
    const serverVersion = configStore.metadata?.version || cfg.version;
    installedVersion.value = new SunshineVersion(serverVersion || '0.0.0');
    branch.value = cfg.branch || '';
    commit.value = cfg.commit || '';

    // Remote release checks (GitHub)
    try {
      githubRelease.value = await fetch(
        'https://api.github.com/repos/Nonary/vibeshine/releases/latest',
      ).then((r) => r.json());
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn('[Dashboard] latest release fetch failed', e);
    }
    try {
      const releases = await fetch('https://api.github.com/repos/Nonary/vibeshine/releases').then(
        (r) => r.json(),
      );
      if (Array.isArray(releases)) {
        // Pick the latest prerelease by semver (not just the first one)
        const prereleases = releases.filter((r: any) => r && r.prerelease && !r.draft);
        if (prereleases.length > 0) {
          let best = prereleases[0];
          let bestV = SunshineVersion.fromRelease(best);
          for (let i = 1; i < prereleases.length; i++) {
            const cand = prereleases[i];
            const candV = SunshineVersion.fromRelease(cand);
            if (candV.isGreater(bestV)) {
              best = cand;
              bestV = candV;
            }
          }
          preReleaseRelease.value = best as GitHubRelease;
        }
      }
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn('[Dashboard] releases list fetch failed', e);
    }
    // Tag-based comparison handled below via SunshineVersion
  } catch (e) {
    // eslint-disable-next-line no-console
    console.error('[Dashboard] version checks failed', e);
  }
  try {
    // logs only after auth
    logs.value = await fetch('./api/logs').then((r) => r.text());
  } catch (e) {
    // eslint-disable-next-line no-console
    console.error('[Dashboard] logs fetch failed', e);
  }
  loading.value = false;
}

onMounted(async () => {
  await auth.waitForAuthentication();
  await runVersionChecks();
});

const installedVersionNotStable = computed(() => {
  // treat non-main/master branches as pre-release builds automatically
  if (branch.value && !['master', 'main'].includes(branch.value)) return true;
  // treat any semantic prerelease (e.g., 0.0.0-beta.1) as not stable
  if (installedVersion.value?.preRelease && installedVersion.value.preRelease.length > 0)
    return true;
  // If installed tag is ahead of the latest stable tag, assume pre-release/dev build
  if (githubRelease.value) {
    return installedVersion.value.isGreater(githubVersion.value);
  }
  return false;
});
// If build is untagged (e.g., 0.0.0), display the current pre-release tag instead (when available)
const displayVersion = computed(() => {
  const v = installedVersion.value?.version || '0.0.0';
  if (!v || v === '0.0.0') {
    const pre = preReleaseRelease.value?.tag_name || '';
    if (pre) return pre.replace(/^v/i, '');
  }
  return v;
});
const stableBuildAvailable = computed(() => {
  if (!githubRelease.value) return false;
  return githubVersion.value.isGreater(installedVersion.value);
});
const preReleaseBuildAvailable = computed(() => {
  if (!preReleaseRelease.value || !githubRelease.value) return false;
  // Only show pre-release if it's enabled and the pre tag is newer than both installed and stable
  return (
    preReleaseVersion.value.isGreater(installedVersion.value) &&
    preReleaseVersion.value.isGreater(githubVersion.value)
  );
});
const buildVersionIsDirty = computed(() => {
  if (!installedVersion.value) return false;
  return (
    installedVersion.value.version?.split('.').length === 5 &&
    installedVersion.value.version.indexOf('dirty') !== -1
  );
});
// No git compare; ahead/behind not applicable in date-based flow
const fancyLogs = computed(() => {
  if (!logs.value) return [];
  const regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g;
  const rawLogLines = logs.value.split(regex).splice(1);
  const logLines = [];
  for (let i = 0; i < rawLogLines.length; i += 2) {
    logLines.push({
      timestamp: rawLogLines[i],
      level: rawLogLines[i + 1].split(':')[0],
      value: rawLogLines[i + 1],
    });
  }
  return logLines;
});
</script>

<style scoped></style>
