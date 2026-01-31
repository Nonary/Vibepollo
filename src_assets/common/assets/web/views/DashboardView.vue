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
          <RouterLink to="/settings" custom v-slot="{ navigate, href }">
            <a :href="href" @click="navigate">
              <n-button tag="span" type="primary" strong>
                <i class="fas fa-sliders" />
                <span>Settings</span>
              </n-button>
            </a>
          </RouterLink>
          <RouterLink to="/applications" custom v-slot="{ navigate, href }">
            <a :href="href" @click="navigate">
              <n-button tag="span" type="default" strong>
                <i class="fas fa-th" />
                <span>Applications</span>
              </n-button>
            </a>
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
            <!-- Playnite extension update available -->
            <n-alert
              v-if="playniteUpdateAvailable"
              type="warning"
              :show-icon="true"
              class="rounded-xl"
            >
              <div
                class="flex flex-col md:flex-row md:items-center md:justify-between gap-3 w-full"
              >
                <div class="min-w-0">
                  <p class="text-sm m-0 font-medium">Playnite Extension update available</p>
                  <p class="text-xs opacity-80 m-0">
                    {{
                      (playnite?.installed_version || 'unknown') +
                      ' → ' +
                      (playnite?.packaged_version || 'unknown')
                    }}
                  </p>
                </div>
                <div class="flex items-center gap-2 shrink-0">
                  <PlayniteReinstallButton
                    size="small"
                    :strong="true"
                    :restart="true"
                    :label="'Update Playnite Extension'"
                    @done="onPlayniteReinstallDone"
                  />
                </div>
              </div>
            </n-alert>
            <!-- Crash dump detected banner -->
            <n-alert v-if="showCrashDumpBanner" type="error" :show-icon="true" class="rounded-xl">
              <div
                class="flex flex-col md:flex-row md:items-center md:justify-between gap-3 w-full"
              >
                <div class="min-w-0 space-y-1">
                  <p class="text-sm m-0 font-medium">
                    {{ $t('config.crash_dump_title') || 'Recent crash detected' }}
                  </p>
                  <p class="text-xs opacity-80 m-0">
                    {{
                      crashDumpMessage ||
                      $t('config.crash_dump_desc') ||
                      'Sunshine detected a recent crash dump. Please export a crash bundle and include it when filing an issue.'
                    }}
                  </p>
                  <p v-if="crashDumpDetails" class="text-xs opacity-60 m-0">
                    {{ crashDumpDetails }}
                  </p>
                </div>
                <div class="flex items-center gap-2 shrink-0">
                  <n-button
                    tag="a"
                    type="default"
                    strong
                    size="small"
                    href="https://github.com/Nonary/vibeshine/issues/new?template=bug_report.yml"
                    target="_blank"
                    rel="noopener noreferrer"
                  >
                    <i class="fas fa-bug" />
                    <span>{{ $t('config.crash_dump_report') || 'Report Issue' }}</span>
                  </n-button>
                  <n-button
                    type="primary"
                    strong
                    size="small"
                    :loading="exportCrashPending"
                    :disabled="exportCrashPending"
                    @click="exportCrashBundle"
                  >
                    <i class="fas fa-file-zipper" />
                    <span>
                      {{
                        exportCrashPending
                          ? translate(
                              'config.crash_dump_export_preparing',
                              'Preparing Crash Bundle...',
                            )
                          : translate('config.crash_dump_export', 'Export Crash Bundle')
                      }}
                    </span>
                  </n-button>
                  <n-button tertiary size="small" @click="dismissCrashBundle">
                    <i class="fas fa-xmark" />
                    <span>{{ $t('config.crash_dump_dismiss') || 'Dismiss' }}</span>
                  </n-button>
                </div>
              </div>
            </n-alert>
            <!-- ViGEm (Virtual Gamepad) missing warning on Windows -->
            <n-alert v-if="showVigemBanner" type="warning" :show-icon="true" class="rounded-xl">
              <div
                class="flex flex-col md:flex-row md:items-center md:justify-between gap-3 w-full"
              >
                <div class="min-w-0">
                  <p class="text-sm m-0 font-medium">
                    {{
                      $t('config.vigem_missing_title') ||
                      'Virtual Gamepad Driver (ViGEm) not installed'
                    }}
                  </p>
                  <p class="text-xs opacity-80 m-0">
                    {{
                      $t('config.vigem_missing_desc') ||
                      'Sunshine requires the ViGEmBus driver to emulate controllers on Windows. It is no longer bundled. Please download and install it manually:'
                    }}
                    <span v-if="vigemVersion" class="ml-2 opacity-60">
                      ({{ $t('config.vigem_detected_version') || 'Detected' }}: {{ vigemVersion }})
                    </span>
                  </p>
                </div>
                <div class="flex items-center gap-2 shrink-0">
                  <n-button
                    tag="a"
                    type="primary"
                    strong
                    href="https://github.com/nefarius/ViGEmBus/releases/latest"
                    target="_blank"
                    rel="noopener noreferrer"
                  >
                    <i class="fas fa-download" />
                    <span>{{ $t('config.vigem_install') || 'Download ViGEmBus' }}</span>
                  </n-button>
                </div>
              </div>
            </n-alert>
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
                  <RouterLink to="/troubleshooting#logs">
                    <n-button type="error" strong>
                      <i class="fas fa-file-lines" /> {{ $t('index.view_logs') || 'View Logs' }}
                    </n-button>
                  </RouterLink>
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
            <n-alert v-else-if="!stableBuildAvailable && !buildVersionIsDirty" type="success">
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
                    <n-button
                      type="default"
                      strong
                      size="small"
                      @click="showPreNotes = !showPreNotes"
                    >
                      <i class="fas fa-bars-staggered" />
                      <span>{{
                        showPreNotes
                          ? $t('index.hide_notes') || 'Hide Notes'
                          : $t('index.view_notes') || 'Release Notes'
                      }}</span>
                    </n-button>
                    <n-button
                      tag="a"
                      size="small"
                      type="primary"
                      strong
                      :href="preReleaseRelease?.html_url"
                      target="_blank"
                    >
                      <i class="fas fa-download" />
                      <span>{{ $t('index.download') }}</span>
                    </n-button>
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
                    <n-button
                      type="default"
                      strong
                      size="small"
                      @click="showStableNotes = !showStableNotes"
                    >
                      <i class="fas fa-bars-staggered" />
                      <span>{{
                        showStableNotes
                          ? $t('index.hide_notes') || 'Hide Notes'
                          : $t('index.view_notes') || 'Release Notes'
                      }}</span>
                    </n-button>
                    <n-button
                      tag="a"
                      size="small"
                      type="primary"
                      strong
                      :href="githubRelease?.html_url"
                      target="_blank"
                    >
                      <i class="fas fa-download" />
                      <span>{{ $t('index.download') }}</span>
                    </n-button>
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
import { useI18n } from 'vue-i18n';
import { NCard, NAlert, NGrid, NGi, useMessage } from 'naive-ui';
import ResourceCard from '@/ResourceCard.vue';
import PlayniteReinstallButton from '@/components/PlayniteReinstallButton.vue';
import SunshineVersion, { GitHubRelease } from '@/sunshine_version';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import type { CrashDumpStatus } from '@/utils/crashDump';
import { isCrashDumpEligible, sanitizeCrashDumpStatus } from '@/utils/crashDump';

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
const installedIsPrerelease = ref(false);
// ViGEm health
const vigemInstalled = ref<boolean | null>(null);
const vigemVersion = ref('');
// Playnite extension status
type PlayniteStatus = {
  installed: boolean;
  active: boolean;
  extensions_dir?: string;
  installed_version?: string;
  packaged_version?: string;
  update_available?: boolean;
};
const playnite = ref<PlayniteStatus | null>(null);
const updatingPlaynite = ref(false);

const crashDump = ref<CrashDumpStatus | null>(null);
const exportCrashPending = ref(false);

const configStore = useConfigStore();
const auth = useAuthStore();
let started = false; // prevent duplicate concurrent checks
const message = useMessage();
const { t: $t } = useI18n();

const translate = (key: string, fallback: string) => {
  const value = $t(key);
  return value === key ? fallback : value;
};

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
    // Fetch list of releases to locate prereleases and determine installed stability
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
        // Determine if installed tag corresponds to a prerelease on GitHub
        const installedTag = installedVersion.value?.version || '';
        const installedTagV = installedTag.toLowerCase().startsWith('v')
          ? installedTag
          : 'v' + installedTag;
        const match = releases.find(
          (r: any) =>
            r &&
            !r.draft &&
            typeof r.tag_name === 'string' &&
            (r.tag_name === installedTag || r.tag_name === installedTagV),
        );
        installedIsPrerelease.value = !!(match && match.prerelease === true);
      }
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn('[Dashboard] releases list fetch failed', e);
    }
    // Tag-based comparison handled below via SunshineVersion

    const plat = (configStore.metadata?.platform || '').toLowerCase();
    // ViGEm health (Windows only)
    try {
      const controllerEnabled = cfg.controller === 'enabled';
      if (plat === 'windows' && controllerEnabled) {
        const r = await http.get('/api/health/vigem', { validateStatus: () => true });
        if (r.status === 200 && r.data) {
          vigemInstalled.value = !!r.data.installed;
          vigemVersion.value = r.data.version || '';
        } else {
          vigemInstalled.value = null;
        }
      } else {
        vigemInstalled.value = null;
      }
    } catch (e) {
      vigemInstalled.value = null;
    }
    await refreshCrashDumpStatus(plat);
    // Playnite status for extension version/update check
    try {
      const r = await http.get('/api/playnite/status', { validateStatus: () => true });
      if (r.status === 200 && r.data) {
        playnite.value = r.data as PlayniteStatus;
      } else {
        playnite.value = null;
      }
    } catch (e) {
      playnite.value = null;
    }
  } catch (e) {
    // eslint-disable-next-line no-console
    console.error('[Dashboard] version checks failed', e);
  }
  try {
    // logs only after auth
    const r = await http.get('./api/logs', {
      responseType: 'text',
      transformResponse: [(v) => v],
    });
    if (r.status === 200 && typeof r.data === 'string') {
      logs.value = r.data;
    }
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

function exportCrashBundle() {
  return void exportCrashBundleAsync();
}

function parseContentDispositionFilename(header?: string): string | null {
  if (!header) return null;
  const filenameStar = /filename\*=UTF-8''([^;]+)/i.exec(header);
  if (filenameStar?.[1]) {
    try {
      return decodeURIComponent(filenameStar[1]);
    } catch {
      return filenameStar[1];
    }
  }
  const filenameMatch = /filename="?([^\";]+)"?/i.exec(header);
  return filenameMatch?.[1] || null;
}

function triggerDownload(blob: Blob, filename: string) {
  const url = window.URL.createObjectURL(blob);
  const link = window.document.createElement('a');
  link.href = url;
  link.download = filename;
  link.click();
  window.URL.revokeObjectURL(url);
}

async function downloadCrashBundlePart(partIndex: number, filenameHint?: string) {
  const r = await http.get(`/api/logs/export_crash?part=${partIndex}`, {
    responseType: 'blob',
    validateStatus: () => true,
  });
  if (r.status !== 200) {
    throw new Error('crash bundle download failed');
  }
  const headerName = parseContentDispositionFilename(r.headers?.['content-disposition']);
  const filename =
    filenameHint || headerName || `sunshine_crashbundle-part${partIndex}.zip`;
  triggerDownload(r.data as Blob, filename);
}

async function exportCrashBundleAsync() {
  if (exportCrashPending.value) return;
  exportCrashPending.value = true;
  try {
    if (typeof window === 'undefined') return;
    const manifest = await http.get('/api/logs/export_crash/manifest', {
      validateStatus: () => true,
    });
    const parts = Array.isArray(manifest.data?.parts) ? manifest.data.parts : [];
    if (manifest.status === 200 && parts.length > 0) {
      const ordered = [...parts].sort((a, b) => Number(a.index) - Number(b.index));
      for (const part of ordered) {
        const index = Number(part.index) || 0;
        if (index <= 0) continue;
        await downloadCrashBundlePart(index, part.filename);
      }
    } else {
      await downloadCrashBundlePart(1);
    }
  } catch {
    message.error(
      translate('config.crash_dump_export_error', 'Failed to export crash bundle.'),
    );
  } finally {
    exportCrashPending.value = false;
  }
}

async function refreshCrashDumpStatus(platformOverride?: string) {
  const platform = ((platformOverride ?? configStore.metadata?.platform) || '').toLowerCase();
  if (platform !== 'windows') {
    crashDump.value = null;
    return;
  }
  try {
    const r = await http.get('/api/health/crashdump', { validateStatus: () => true });
    if (r.status === 200 && r.data) {
      const sanitized = sanitizeCrashDumpStatus(r.data as CrashDumpStatus);
      crashDump.value = sanitized ?? { available: false };
    } else {
      crashDump.value = { available: false };
    }
  } catch {
    crashDump.value = null;
  }
}

async function dismissCrashBundle() {
  if (!crashDump.value?.available) return;
  const payload = {
    filename: crashDump.value.filename,
    captured_at: crashDump.value.captured_at,
  };
  if (!payload.filename || !payload.captured_at) {
    message.error($t('config.crash_dump_dismiss_error') || 'Failed to dismiss crash notification.');
    return;
  }
  try {
    const r = await http.post('/api/health/crashdump/dismiss', payload, {
      validateStatus: () => true,
    });
    if (r.status === 200 && r.data?.status === true) {
      crashDump.value = {
        ...crashDump.value,
        dismissed: true,
        dismissed_at: r.data.dismissed_at || new Date().toISOString(),
      };
      message.success($t('config.crash_dump_dismiss_success') || 'Crash notification dismissed.');
      await refreshCrashDumpStatus();
    } else {
      const errData = r.data && (r.data.error || r.data.message);
      const errMessage = typeof errData === 'string' ? errData : '';
      if (errMessage) {
        const lower = errMessage.toLowerCase();
        if (
          lower.includes('metadata mismatch') ||
          lower.includes('no recent sunshine crash dumps')
        ) {
          await refreshCrashDumpStatus();
        }
      }
      message.error(
        errMessage ||
          $t('config.crash_dump_dismiss_error') ||
          'Failed to dismiss crash notification.',
      );
    }
  } catch {
    await refreshCrashDumpStatus();
    message.error($t('config.crash_dump_dismiss_error') || 'Failed to dismiss crash notification.');
  }
}

function humanFileSize(bytes?: number | null) {
  if (typeof bytes !== 'number' || !Number.isFinite(bytes) || bytes <= 0) {
    return '';
  }
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  const formatter = new Intl.NumberFormat(undefined, {
    maximumFractionDigits: value >= 10 ? 0 : 1,
  });
  return `${formatter.format(value)} ${units[unit]}`;
}

function formatRelativeTime(date: Date) {
  try {
    const diffMs = Date.now() - date.getTime();
    if (!Number.isFinite(diffMs)) return '';
    const minutes = Math.round(diffMs / 60000);
    const rtf = new Intl.RelativeTimeFormat(undefined, { numeric: 'auto' });
    if (Math.abs(minutes) < 60) {
      return rtf.format(-minutes, 'minute');
    }
    const hours = Math.round(minutes / 60);
    if (Math.abs(hours) < 48) {
      return rtf.format(-hours, 'hour');
    }
    const days = Math.round(hours / 24);
    return rtf.format(-days, 'day');
  } catch {
    return '';
  }
}

const crashDumpDetails = computed(() => {
  if (!crashDump.value || !crashDump.value.available) return '';
  const parts: string[] = [];
  if (crashDump.value.filename) parts.push(crashDump.value.filename);
  if (typeof crashDump.value.size_bytes === 'number') {
    const size = humanFileSize(crashDump.value.size_bytes);
    if (size) parts.push(size);
  }
  if (crashDump.value.captured_at) {
    const captured = new Date(crashDump.value.captured_at);
    if (!Number.isNaN(captured.getTime())) {
      parts.push(captured.toLocaleString());
    }
  }
  return parts.join(' • ');
});

const crashDumpMessage = computed(() => {
  if (!crashDump.value || !crashDump.value.available) return '';
  if (crashDump.value.captured_at) {
    const captured = new Date(crashDump.value.captured_at);
    if (!Number.isNaN(captured.getTime())) {
      const rel = formatRelativeTime(captured);
      if (rel) return `Crash detected ${rel}.`;
    }
  }
  return '';
});

const installedVersionNotStable = computed(() => {
  // Consider non-main/master branches as non-stable
  if (branch.value && !['master', 'main'].includes(branch.value)) return true;
  // If GitHub flags the installed tag as a prerelease, consider it non-stable
  if (installedIsPrerelease.value) return true;
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

const showCrashDumpBanner = computed(() => {
  const plat = (configStore.metadata?.platform || '').toLowerCase();
  if (plat !== 'windows') return false;
  if (!isCrashDumpEligible(crashDump.value)) return false;
  return crashDump.value?.dismissed !== true;
});

const showVigemBanner = computed(() => {
  const plat = (configStore.metadata?.platform || '').toLowerCase();
  const controllerEnabled = (configStore.config as any)?.controller === 'enabled';
  return plat === 'windows' && controllerEnabled && vigemInstalled.value === false;
});

const playniteUpdateAvailable = computed(() => {
  return !!(playnite.value && playnite.value.installed && playnite.value.update_available);
});

async function onPlayniteReinstallDone(res: { ok: boolean; error?: string }) {
  if (res.ok) {
    message.success('Playnite Extension updated');
  } else {
    message.error('Update failed' + (res.error ? `: ${res.error}` : ''));
  }
  try {
    const s = await http.get('/api/playnite/status', { validateStatus: () => true });
    if (s.status === 200 && s.data) playnite.value = s.data as PlayniteStatus;
  } catch {}
}
</script>

<style scoped></style>
