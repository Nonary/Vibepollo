<template>
  <div class="dashboard-page space-y-6 sm:space-y-8">
    <!-- Hero / Intro -->
    <section
      class="rounded-xl border border-dark/10 bg-light/70 p-4 shadow-sm backdrop-blur dark:border-light/10 dark:bg-surface/70 sm:p-5 md:p-6"
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
        <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0">
          <RouterLink to="/settings" custom v-slot="{ navigate, href }">
            <a :href="href" @click="navigate">
              <n-button tag="span" type="primary" strong class="w-full justify-center sm:w-auto">
                <i class="fas fa-sliders" />
                <span>{{ $t('navbar.configuration') }}</span>
              </n-button>
            </a>
          </RouterLink>
          <RouterLink to="/applications" custom v-slot="{ navigate, href }">
            <a :href="href" @click="navigate">
              <n-button tag="span" type="default" strong class="w-full justify-center sm:w-auto">
                <i class="fas fa-th" />
                <span>{{ $t('apps.applications_title') }}</span>
              </n-button>
            </a>
          </RouterLink>
        </div>
      </div>
    </section>

    <!-- Fatal startup errors moved into Version card to avoid layout shift -->

    <!-- Main Grid -->
    <div class="grid min-w-0 gap-4 sm:gap-5">
      <!-- Version Card -->
      <div class="min-w-0">
        <n-card v-if="installedVersion" :segmented="{ content: true, footer: true }">
          <template #header>
            <h2
              class="text-xl sm:text-2xl font-semibold tracking-tight mx-auto text-center break-words"
            >
              {{ $t('changelog.current_version_title', { version: displayVersion }) }}
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
                  <p class="text-sm m-0 font-medium">
                    {{ $t('playnite.extension_update_available') }}
                  </p>
                  <p class="text-xs opacity-80 m-0">
                    {{
                      (playnite?.installed_version || $t('_common.unknown')) +
                      ' → ' +
                      (playnite?.packaged_version || $t('_common.unknown'))
                    }}
                  </p>
                </div>
                <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0">
                  <PlayniteReinstallButton
                    size="small"
                    :strong="true"
                    :restart="true"
                    :label="$t('playnite.update_extension')"
                    @done="onPlayniteReinstallDone"
                  />
                </div>
              </div>
            </n-alert>
            <n-alert
              v-if="showPlayniteMissingPluginBanner"
              type="warning"
              :show-icon="true"
              class="rounded-xl"
            >
              <div
                class="flex flex-col md:flex-row md:items-center md:justify-between gap-3 w-full"
              >
                <div class="min-w-0">
                  <p class="text-sm m-0 font-medium">{{ $t('playnite.extension_missing') }}</p>
                  <p class="text-xs opacity-80 m-0">
                    {{ playniteMissingPluginBannerText }}
                  </p>
                </div>
                <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0">
                  <n-button
                    size="small"
                    type="primary"
                    strong
                    class="w-full justify-center sm:w-auto"
                    :loading="resolvingPlaynitePluginIssue"
                    :disabled="resolvingPlaynitePluginIssue || purgingPlayniteApps"
                    @click="resolvePlaynitePluginIssue"
                  >
                    <i class="fas fa-plug" />
                    <span>{{ $t('playnite.resolve_issue') }}</span>
                  </n-button>
                  <n-button
                    size="small"
                    type="error"
                    strong
                    secondary
                    class="w-full justify-center sm:w-auto"
                    :loading="purgingPlayniteApps"
                    :disabled="purgingPlayniteApps || resolvingPlaynitePluginIssue"
                    @click="openPurgePlayniteGamesConfirm"
                  >
                    <i class="fas fa-trash" />
                    <span>{{ $t('playnite.purge_games') }}</span>
                  </n-button>
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
                    {{ $t('config.crash_dump_title') }}
                  </p>
                  <p class="text-xs opacity-80 m-0">
                    {{ crashDumpMessage || $t('config.crash_dump_desc') }}
                  </p>
                  <p v-if="crashDumpDetails" class="text-xs opacity-60 m-0">
                    {{ crashDumpDetails }}
                  </p>
                </div>
                <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0">
                  <n-button
                    tag="a"
                    type="default"
                    strong
                    size="small"
                    class="w-full justify-center sm:w-auto"
                    href="https://github.com/Nonary/Vibepollo/issues/new?template=bug_report.yml"
                    target="_blank"
                    rel="noopener noreferrer"
                  >
                    <i class="fas fa-bug" />
                    <span>{{ $t('config.crash_dump_report') }}</span>
                  </n-button>
                  <n-button
                    type="primary"
                    strong
                    size="small"
                    class="w-full justify-center sm:w-auto"
                    :loading="exportCrashPending"
                    :disabled="exportCrashPending"
                    @click="exportCrashBundle"
                  >
                    <i class="fas fa-file-zipper" />
                    <span>
                      {{
                        exportCrashPending
                          ? $t('config.crash_dump_export_preparing')
                          : $t('config.crash_dump_export')
                      }}
                    </span>
                  </n-button>
                  <n-button
                    tertiary
                    size="small"
                    class="w-full justify-center sm:w-auto"
                    @click="dismissCrashBundle"
                  >
                    <i class="fas fa-xmark" />
                    <span>{{ $t('config.crash_dump_dismiss') }}</span>
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
                    {{ $t('config.vigem_missing_title') }}
                  </p>
                  <p class="text-xs opacity-80 m-0">
                    {{ $t('config.vigem_missing_desc') }}
                    <span v-if="vigemVersion" class="ml-2 opacity-60">
                      ({{ $t('config.vigem_detected_version') }}: {{ vigemVersion }})
                    </span>
                  </p>
                </div>
                <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0">
                  <n-button
                    tag="a"
                    type="primary"
                    strong
                    class="w-full justify-center sm:w-auto"
                    href="https://github.com/nefarius/ViGEmBus/releases/latest"
                    target="_blank"
                    rel="noopener noreferrer"
                  >
                    <i class="fas fa-download" />
                    <span>{{ $t('config.vigem_install') }}</span>
                  </n-button>
                </div>
              </div>
            </n-alert>
            <!-- Vulkan HDR layer not installed warning on Windows -->
            <n-alert
              v-if="showVulkanHdrLayerBanner"
              type="warning"
              :show-icon="true"
              class="rounded-xl"
            >
              <div
                class="flex flex-col md:flex-row md:items-center md:justify-between gap-3 w-full"
              >
                <div class="min-w-0">
                  <p class="text-sm m-0 font-medium">
                    {{ $t('vulkan_hdr.not_installed_title') }}
                  </p>
                  <p class="text-xs opacity-80 m-0">
                    {{ $t('vulkan_hdr.not_installed_desc') }}
                  </p>
                </div>
                <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0">
                  <n-button
                    type="primary"
                    strong
                    size="small"
                    class="w-full justify-center sm:w-auto"
                    :loading="vulkanHdrLayerInstalling"
                    :disabled="vulkanHdrLayerInstalling"
                    @click="installVulkanHdrLayer"
                  >
                    <i class="fas fa-download" />
                    <span>{{ $t('vulkan_hdr.install') }}</span>
                  </n-button>
                  <n-button
                    tertiary
                    size="small"
                    class="w-full justify-center sm:w-auto"
                    @click="dismissVulkanHdrLayerBanner"
                  >
                    <i class="fas fa-xmark" />
                    <span>{{ $t('vulkan_hdr.dismiss') }}</span>
                  </n-button>
                </div>
              </div>
            </n-alert>
            <n-alert
              v-if="showGoldenSnapshotOutOfDateBanner"
              type="warning"
              :show-icon="true"
              class="rounded-xl"
            >
              <div
                class="flex flex-col md:flex-row md:items-center md:justify-between gap-3 w-full"
              >
                <div class="min-w-0">
                  <p class="text-sm m-0 font-medium">
                    {{ $t('config.golden_snapshot_outdated_title') }}
                  </p>
                  <p class="text-xs opacity-80 m-0">
                    {{ $t('config.golden_snapshot_outdated_desc') }}
                  </p>
                </div>
                <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0">
                  <RouterLink
                    :to="{
                      path: '/settings',
                      query: { sec: 'av', jump: 'dd_always_restore_from_golden' },
                    }"
                    custom
                    v-slot="{ navigate, href }"
                  >
                    <a :href="href" @click="navigate">
                      <n-button
                        tag="span"
                        type="primary"
                        strong
                        size="small"
                        class="w-full justify-center sm:w-auto"
                      >
                        <i class="fas fa-rotate-right" />
                        <span>{{ $t('config.golden_snapshot_outdated_action') }}</span>
                      </n-button>
                    </a>
                  </RouterLink>
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
                      <i class="fas fa-file-lines" /> {{ $t('index.view_logs') }}
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
              :show-icon="false"
              class="rounded-xl dashboard-release-alert"
            >
              <div class="flex flex-col gap-3 w-full">
                <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-3">
                  <div class="flex items-center gap-4 min-w-0">
                    <span
                      class="dashboard-release-alert__icon inline-flex items-center justify-center rounded-full bg-warning/20 text-warning"
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
                  <div
                    class="dashboard-release-alert__actions grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0"
                  >
                    <n-button
                      type="default"
                      strong
                      size="small"
                      class="w-full justify-center sm:w-auto"
                      @click="showPreNotes = !showPreNotes"
                    >
                      <i class="fas fa-bars-staggered" />
                      <span>{{
                        showPreNotes ? $t('index.hide_notes') : $t('index.view_notes')
                      }}</span>
                    </n-button>
                    <n-button
                      tag="a"
                      size="small"
                      type="primary"
                      strong
                      class="w-full justify-center sm:w-auto"
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
              :show-icon="false"
              class="rounded-xl dashboard-release-alert"
            >
              <div class="flex flex-col gap-3 w-full">
                <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-3">
                  <div class="flex items-center gap-4 min-w-0">
                    <span
                      class="dashboard-release-alert__icon inline-flex items-center justify-center rounded-full bg-warning/20 text-warning"
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
                  <div
                    class="dashboard-release-alert__actions grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0"
                  >
                    <n-button
                      type="default"
                      strong
                      size="small"
                      class="w-full justify-center sm:w-auto"
                      @click="showStableNotes = !showStableNotes"
                    >
                      <i class="fas fa-bars-staggered" />
                      <span>{{
                        showStableNotes ? $t('index.hide_notes') : $t('index.view_notes')
                      }}</span>
                    </n-button>
                    <n-button
                      tag="a"
                      size="small"
                      type="primary"
                      strong
                      class="w-full justify-center sm:w-auto"
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
      </div>

      <!-- Resources -->
      <div class="min-w-0">
        <n-card>
          <template #header>
            <h2 class="text-xl sm:text-2xl font-semibold tracking-tight mx-auto text-center">
              {{ $t('index.web_links') }}
            </h2>
          </template>
          <div class="text-xs space-y-2">
            <ResourceCard />
          </div>
        </n-card>
      </div>

      <!-- Changelog -->
      <div class="min-w-0">
        <ChangelogPanel />
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { NCard, NAlert, useMessage, useDialog } from 'naive-ui';
import ResourceCard from '@/ResourceCard.vue';
import ChangelogPanel from '@/components/ChangelogPanel.vue';
import PlayniteReinstallButton from '@/components/PlayniteReinstallButton.vue';
import VibepolloVersion, { GitHubRelease } from '@/sunshine_version';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { useAppsStore } from '@/stores/apps';
import { http } from '@/http';
import type { CrashDumpStatus } from '@/utils/crashDump';
import { isCrashDumpEligible, sanitizeCrashDumpStatus } from '@/utils/crashDump';
import { toIntlLocale } from '@/utils/intlLocale';

const installedVersion = ref<VibepolloVersion>(new VibepolloVersion('0.0.0'));
const githubRelease = ref<GitHubRelease | null>(null);
const preReleaseRelease = ref<GitHubRelease | null>(null);

const githubVersion = computed(() =>
  githubRelease.value
    ? VibepolloVersion.fromRelease(githubRelease.value)
    : new VibepolloVersion('0.0.0'),
);
const preReleaseVersion = computed(() =>
  preReleaseRelease.value
    ? VibepolloVersion.fromRelease(preReleaseRelease.value)
    : new VibepolloVersion('0.0.0'),
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
// Vulkan HDR layer health (Windows only)
const vulkanHdrLayer = ref<{ installed: boolean; enabled: boolean } | null>(null);
const vulkanHdrLayerInstalling = ref(false);
const vulkanHdrLayerBannerDismissed = ref(false);
if (typeof window !== 'undefined') {
  try {
    vulkanHdrLayerBannerDismissed.value =
      window.localStorage.getItem('vulkanHdrLayerBannerDismissed') === '1';
  } catch {}
}
// Playnite extension status
type PlayniteStatus = {
  installed: boolean | null;
  active: boolean;
  extensions_dir?: string;
  installed_version?: string;
  packaged_version?: string;
  update_available?: boolean;
};
type GoldenStatus = {
  exists?: boolean;
  snapshot_version?: number | null;
  latest_snapshot_version?: number;
  has_layout?: boolean;
  needs_layout_upgrade?: boolean;
  out_of_date?: boolean;
  comparison_available?: boolean;
  out_of_date_reason?: string;
  current_mismatch_reason?: string;
  restore_failure_count?: number;
  restore_failure_threshold?: number;
  restore_failure_window_hours?: number;
  restore_status_reason?: string;
  restore_last_failure_reason?: string;
  restore_first_failure_unix_ms?: number | null;
  restore_latest_failure_unix_ms?: number | null;
  restore_status_updated_at_unix_ms?: number | null;
};
const playnite = ref<PlayniteStatus | null>(null);

const crashDump = ref<CrashDumpStatus | null>(null);
const exportCrashPending = ref(false);
const goldenStatus = ref<GoldenStatus | null>(null);
const resolvingPlaynitePluginIssue = ref(false);
const purgingPlayniteApps = ref(false);

const configStore = useConfigStore();
const auth = useAuthStore();
const appsStore = useAppsStore();
let started = false; // prevent duplicate concurrent checks
const message = useMessage();
const dialog = useDialog();
const { t: $t, locale } = useI18n();
const crashDumpTimeFormatter = computed(
  () =>
    new Intl.DateTimeFormat(toIntlLocale(locale.value), {
      dateStyle: 'medium',
      timeStyle: 'short',
    }),
);

function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object';
}

function isPlayniteFullscreenEntry(app: Record<string, unknown>): boolean {
  if (app['playnite-fullscreen'] === true) {
    return true;
  }
  if (typeof app.name === 'string' && app.name === 'Playnite (Fullscreen)') {
    return true;
  }
  const cmdValue = app.cmd;
  const cmdText = Array.isArray(cmdValue)
    ? cmdValue.filter((v): v is string => typeof v === 'string').join(' ')
    : typeof cmdValue === 'string'
      ? cmdValue
      : '';
  const cmdLower = cmdText.toLowerCase();
  return cmdLower.includes('playnite-launcher') && cmdLower.includes('--fullscreen');
}

function isPlayniteApp(app: Record<string, unknown>): boolean {
  if (typeof app['playnite-id'] === 'string' && app['playnite-id'].length > 0) {
    return true;
  }
  return isPlayniteFullscreenEntry(app);
}

function getAppsSnapshot(): Record<string, unknown>[] {
  return (appsStore.apps || []).filter((app): app is Record<string, unknown> => isRecord(app));
}

async function refreshAppsSnapshot() {
  try {
    await appsStore.loadApps(true);
  } catch {
    appsStore.setApps([]);
  }
}

async function refreshAppsSnapshotStrict() {
  const r = await http.get('/api/apps', { validateStatus: () => true });
  const apps = Array.isArray((r.data as any)?.apps) ? (r.data as any).apps : null;
  if (r.status !== 200 || !apps) {
    throw new Error(`HTTP ${r.status}`);
  }
  appsStore.setApps(apps);
}

async function refreshPlayniteStatus() {
  try {
    const r = await http.get('/api/playnite/status', { validateStatus: () => true });
    if (r.status === 200 && r.data) {
      playnite.value = r.data as PlayniteStatus;
    } else {
      playnite.value = null;
    }
  } catch {
    playnite.value = null;
  }
}

async function refreshPlayniteAndApps() {
  await Promise.all([refreshPlayniteStatus(), refreshAppsSnapshot()]);
}

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
    installedVersion.value = new VibepolloVersion(serverVersion || '0.0.0');
    branch.value = cfg.branch || '';
    commit.value = cfg.commit || '';

    // Remote release checks (GitHub)
    try {
      githubRelease.value = await fetch(
        'https://api.github.com/repos/Nonary/Vibepollo/releases/latest',
      ).then((r) => r.json());
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn('[Dashboard] latest release fetch failed', e);
    }
    // Fetch list of releases to locate prereleases and determine installed stability
    try {
      const releases = await fetch('https://api.github.com/repos/Nonary/Vibepollo/releases').then(
        (r) => r.json(),
      );
      if (Array.isArray(releases)) {
        // Pick the latest prerelease by semver (not just the first one)
        const prereleases = releases.filter((r: any) => r && r.prerelease && !r.draft);
        if (prereleases.length > 0) {
          let best = prereleases[0];
          let bestV = VibepolloVersion.fromRelease(best);
          for (let i = 1; i < prereleases.length; i++) {
            const cand = prereleases[i];
            const candV = VibepolloVersion.fromRelease(cand);
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
    // Tag-based comparison handled below via VibepolloVersion

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
    await refreshVulkanHdrLayerStatus(plat);
    await refreshCrashDumpStatus(plat);
    await refreshGoldenStatus(plat);
    // Playnite status for extension version/update check
    await refreshPlayniteStatus();
    if (plat === 'windows') {
      await refreshAppsSnapshot();
    } else {
      appsStore.setApps([]);
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
  const filename = filenameHint || headerName || `sunshine_crashbundle-part${partIndex}.zip`;
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
    message.error($t('config.crash_dump_export_error'));
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

async function refreshGoldenStatus(platformOverride?: string) {
  const platform = ((platformOverride ?? configStore.metadata?.platform) || '').toLowerCase();
  if (platform !== 'windows') {
    goldenStatus.value = null;
    return;
  }
  try {
    const r = await http.get('/api/display/golden_status', { validateStatus: () => true });
    if (r.status === 200 && r.data) {
      goldenStatus.value = r.data as GoldenStatus;
    } else {
      goldenStatus.value = null;
    }
  } catch {
    goldenStatus.value = null;
  }
}

async function refreshVulkanHdrLayerStatus(platformOverride?: string) {
  const platform = ((platformOverride ?? configStore.metadata?.platform) || '').toLowerCase();
  if (platform !== 'windows') {
    vulkanHdrLayer.value = null;
    return;
  }
  try {
    const r = await http.get('/api/health/vulkan-hdr-layer', { validateStatus: () => true });
    if (r.status === 200 && r.data) {
      vulkanHdrLayer.value = {
        installed: !!r.data.installed,
        enabled: r.data.enabled !== false,
      };
    } else {
      vulkanHdrLayer.value = null;
    }
  } catch {
    vulkanHdrLayer.value = null;
  }
}

async function installVulkanHdrLayer() {
  if (vulkanHdrLayerInstalling.value) return;
  vulkanHdrLayerInstalling.value = true;
  try {
    const r = await http.post(
      '/api/health/vulkan-hdr-layer/register',
      {},
      { validateStatus: () => true },
    );
    const ok =
      r.status >= 200 && r.status < 300 && (r.data?.status === true || r.data?.installed === true);
    await refreshVulkanHdrLayerStatus();
    if (ok && vulkanHdrLayer.value?.installed) {
      message.success($t('vulkan_hdr.install_success'));
    } else {
      message.error($t('vulkan_hdr.install_error'));
    }
  } catch {
    await refreshVulkanHdrLayerStatus();
    message.error($t('vulkan_hdr.install_error'));
  } finally {
    vulkanHdrLayerInstalling.value = false;
  }
}

function dismissVulkanHdrLayerBanner() {
  vulkanHdrLayerBannerDismissed.value = true;
  if (typeof window !== 'undefined') {
    try {
      window.localStorage.setItem('vulkanHdrLayerBannerDismissed', '1');
    } catch {}
  }
}

async function dismissCrashBundle() {
  if (!crashDump.value?.available) return;
  const payload = {
    filename: crashDump.value.filename,
    captured_at: crashDump.value.captured_at,
  };
  if (!payload.filename || !payload.captured_at) {
    message.error($t('config.crash_dump_dismiss_error'));
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
      message.success($t('config.crash_dump_dismiss_success'));
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
      message.error(errMessage || $t('config.crash_dump_dismiss_error'));
    }
  } catch {
    await refreshCrashDumpStatus();
    message.error($t('config.crash_dump_dismiss_error'));
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
      parts.push(crashDumpTimeFormatter.value.format(captured));
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
      if (rel) return $t('config.crash_dump_detected_relative', { time: rel });
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

const showVulkanHdrLayerBanner = computed(() => {
  const plat = (configStore.metadata?.platform || '').toLowerCase();
  if (plat !== 'windows') return false;
  if (vulkanHdrLayerBannerDismissed.value) return false;
  return vulkanHdrLayer.value?.enabled !== false && vulkanHdrLayer.value?.installed === false;
});

const showGoldenSnapshotOutOfDateBanner = computed(() => {
  const plat = (configStore.metadata?.platform || '').toLowerCase();
  if (plat !== 'windows') return false;
  return (
    goldenStatus.value?.exists === true &&
    (goldenStatus.value?.needs_layout_upgrade === true || goldenStatus.value?.out_of_date === true)
  );
});

const playniteUpdateAvailable = computed(() => {
  return !!(playnite.value && playnite.value.installed && playnite.value.update_available);
});

const playniteApps = computed(() => getAppsSnapshot().filter((app) => isPlayniteApp(app)));
const playniteAutoSyncedAppsCount = computed(() => {
  return playniteApps.value.filter((app) => app['playnite-managed'] === 'auto').length;
});
const hasPlayniteFullscreenApp = computed(() => {
  return getAppsSnapshot().some((app) => isPlayniteFullscreenEntry(app));
});
const showPlayniteMissingPluginBanner = computed(() => {
  const plat = (configStore.metadata?.platform || '').toLowerCase();
  if (plat !== 'windows') return false;
  if (!playnite.value || playnite.value.active === true || playnite.value.installed !== false)
    return false;
  return playniteAutoSyncedAppsCount.value > 0 || hasPlayniteFullscreenApp.value;
});
const playniteMissingPluginBannerText = computed(() => {
  const details: string[] = [];
  if (playniteAutoSyncedAppsCount.value > 0) {
    const count = playniteAutoSyncedAppsCount.value;
    details.push($t('playnite.auto_synced_apps', { count }));
  }
  if (hasPlayniteFullscreenApp.value) {
    details.push($t('playnite.fullscreen_launcher_entry'));
  }
  const detected = details.join(', ') || $t('playnite.entries');
  return $t('playnite.missing_plugin_message', { details: detected });
});

async function resolvePlaynitePluginIssue() {
  if (resolvingPlaynitePluginIssue.value || purgingPlayniteApps.value) return;
  resolvingPlaynitePluginIssue.value = true;
  try {
    const r = await http.post(
      '/api/playnite/install',
      { restart: true },
      { validateStatus: () => true },
    );
    const body = r.data as any;
    const ok = r.status >= 200 && r.status < 300 && body && body.status === true;
    if (ok) {
      message.success($t('playnite.plugin_reinstalled'));
      await refreshPlayniteAndApps();
    } else {
      const err = (body && (body.error || body.message)) || `HTTP ${r.status}`;
      message.error($t('playnite.plugin_reinstall_failed', { error: err }));
    }
  } catch (e: any) {
    message.error(
      $t('playnite.plugin_reinstall_failed', {
        error: e?.message || $t('playnite.update_failed'),
      }),
    );
  } finally {
    resolvingPlaynitePluginIssue.value = false;
  }
}

async function purgePlayniteGames() {
  if (purgingPlayniteApps.value || resolvingPlaynitePluginIssue.value) return;
  purgingPlayniteApps.value = true;
  try {
    await refreshAppsSnapshotStrict();
    const snapshot = getAppsSnapshot();
    const playniteApps = snapshot.filter((app) => isPlayniteApp(app) && !!app.uuid);
    if (!playniteApps.length) {
      message.info($t('playnite.no_apps_to_purge'));
      await refreshPlayniteAndApps();
      return;
    }
    for (const app of playniteApps) {
      const r = await http.delete(`./api/apps/${encodeURIComponent(String(app.uuid))}`, {
        validateStatus: () => true,
      });
      const body = r.data as any;
      const ok = r.status >= 200 && r.status < 300 && body && body.status === true;
      if (!ok) {
        const err = (body && (body.error || body.message)) || `HTTP ${r.status}`;
        throw new Error(err);
      }
    }
    try {
      await configStore.fetchConfig(true);
    } catch {}
    await refreshPlayniteAndApps();
    const removed = playniteApps.length;
    message.success($t('playnite.apps_purged', { count: removed }));
  } catch (e: any) {
    message.error(
      $t('playnite.apps_purge_failed', {
        error: e?.message || $t('playnite.update_failed'),
      }),
    );
    await refreshPlayniteAndApps();
  } finally {
    purgingPlayniteApps.value = false;
  }
}

function openPurgePlayniteGamesConfirm() {
  dialog.warning({
    title: $t('playnite.purge_apps_title'),
    content: $t('playnite.purge_apps_body'),
    positiveText: $t('_common.delete'),
    negativeText: $t('_common.cancel'),
    onPositiveClick: async () => {
      await purgePlayniteGames();
    },
  });
}

async function onPlayniteReinstallDone(res: { ok: boolean; error?: string }) {
  if (res.ok) {
    message.success($t('playnite.extension_updated'));
  } else {
    message.error(
      res.error ? `${$t('playnite.update_failed')}: ${res.error}` : $t('playnite.update_failed'),
    );
  }
  await refreshPlayniteAndApps();
}
</script>

<style scoped>
.dashboard-page :deep(.n-card) {
  border-radius: 1rem;
  overflow: hidden;
  border: 1px solid rgb(var(--color-dark) / 0.1);
  background: rgb(var(--color-light) / 0.8);
  backdrop-filter: blur(6px);
}

.dark .dashboard-page :deep(.n-card) {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-surface) / 0.74);
}

.dashboard-page :deep(.n-card .n-card__header),
.dashboard-page :deep(.n-card .n-card-header),
.dashboard-page :deep(.n-card .n-card__footer),
.dashboard-page :deep(.n-card .n-card-footer) {
  border-radius: 0.95rem;
}

.dashboard-page :deep(.n-alert),
.dashboard-page :deep(.n-empty),
.dashboard-page :deep(.n-input .n-input-wrapper),
.dashboard-page :deep(.n-base-selection),
.dashboard-page :deep(.n-base-selection .n-base-selection-label),
.dashboard-page :deep(.n-data-table-wrapper),
.dashboard-page :deep(.n-table-wrapper) {
  border-radius: 0.8rem !important;
}

.dashboard-page :deep(.dashboard-release-alert .n-alert-body__content) {
  min-width: 0;
  width: 100%;
}

.dashboard-page :deep(.dashboard-release-alert__icon) {
  width: 2rem;
  height: 2rem;
  font-size: 0.95rem;
}

@media (min-width: 768px) {
  .dashboard-page :deep(.dashboard-release-alert__actions) {
    margin-left: auto;
    flex: 1 1 auto;
    justify-content: flex-end;
    transform: translateY(-8px);
  }
}
</style>
