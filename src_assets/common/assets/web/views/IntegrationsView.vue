<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiGet, apiPatch, apiPost } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
  UiIcon,
  type StatusTone,
} from '@/components/ui';
import { useSystemStore } from '@/stores/system';

interface PlayniteStatus {
  enabled?: boolean;
  available?: boolean;
  active?: boolean;
  installed?: boolean | null;
  extensions_dir?: string;
  installed_version?: string;
  packaged_version?: string;
  update_available?: boolean;
}

interface SteamStatus {
  enabled?: boolean;
  forced?: boolean;
  available?: boolean;
  game_count?: number;
  playnite_available?: boolean;
  auto_sync?: boolean;
  autosync_remove_uninstalled?: boolean;
  remove_uninstalled?: boolean;
  include_tools?: boolean;
  exclude_games?: Array<{ id?: string; name?: string }> | string[] | number;
}

interface SteamGame {
  appid: number | string;
  steam_id?: string;
  name?: string;
  install_dir?: string;
  excluded?: boolean;
  filtered?: boolean;
}

interface RtssStatus {
  enabled?: boolean;
  configured_provider?: string;
  active_provider?: string;
  path_exists?: boolean;
  hooks_found?: boolean;
  profile_found?: boolean;
  process_running?: boolean;
  message?: string;
}

interface LosslessStatus {
  status?: 'detected' | 'not-configured' | 'path-is-directory' | 'path-not-found' | 'unavailable';
  configured_path?: string;
  resolved_path?: string;
  suggested_path?: string;
  checked_exists?: boolean;
}

interface VigemStatus {
  installed?: boolean;
  version?: string;
  version_compatible?: boolean;
  packaged_version?: string;
  error?: string;
  // False when Vibeshine's own virtual gamepad driver is available, so a missing
  // ViGEmBus is an unused option rather than a problem.
  required?: boolean;
}

interface VulkanStatus {
  installed?: boolean;
  enabled?: boolean;
}

interface MutationResult {
  status?: boolean;
  error?: string;
}

type IntegrationId = 'steam' | 'playnite' | 'rtss' | 'lossless' | 'vigem' | 'vulkan';
type PendingAction = 'playnite-install' | 'playnite-uninstall' | 'vigem-install' | 'vulkan-register';

interface IntegrationSummary {
  id: IntegrationId;
  name: string;
  description: string;
  status: string;
  tone: StatusTone;
  details: string[];
}

const { t } = useI18n();
const system = useSystemStore();
const playnite = ref<PlayniteStatus | null>(null);
const steam = ref<SteamStatus | null>(null);
const steamGames = ref<SteamGame[]>([]);
const steamGamesLoading = ref(false);
const steamGamesError = ref('');
const steamExclusionBusy = ref<string | null>(null);
const rtss = ref<RtssStatus | null>(null);
const lossless = ref<LosslessStatus | null>(null);
const vigem = ref<VigemStatus | null>(null);
const vulkan = ref<VulkanStatus | null>(null);
const loading = ref(true);
const refreshing = ref(false);
const actionBusy = ref(false);
const syncing = ref(false);
const errors = ref<Partial<Record<IntegrationId, string>>>({});
const notice = ref('');
const confirmOpen = ref(false);
const pendingAction = ref<PendingAction | null>(null);

const isWindows = computed(() =>
  String(system.metadata?.platform ?? '')
    .toLocaleLowerCase()
    .includes('windows'),
);

function message(cause: unknown, fallback: string): string {
  return cause instanceof ApiError ? fallback : cause instanceof Error ? cause.message : fallback;
}

function steamGameId(game: SteamGame): string {
  return String(game.steam_id ?? game.appid ?? '').trim();
}

function steamGameName(game: SteamGame): string {
  return String(game.name ?? '').trim() || t('ui.integrations.steam.unknownGame', { id: steamGameId(game) });
}

function exclusionEntries(): Array<{ id: string; name: string }> {
  const entries = steam.value?.exclude_games;
  if (Array.isArray(entries)) {
    return entries
    .map((entry) => {
      if (typeof entry === 'string') return { id: entry, name: '' };
      return { id: String(entry?.id ?? '').trim(), name: String(entry?.name ?? '').trim() };
    })
    .filter((entry) => entry.id || entry.name);
  }
  // Older status responses only exposed an exclusion count. Preserve the
  // discovered per-game flags until the newer metadata field is available.
  return steamGames.value
    .filter((game) => game.excluded === true)
    .map((game) => ({ id: steamGameId(game), name: steamGameName(game) }));
}

function gameExcluded(game: SteamGame): boolean {
  const id = steamGameId(game);
  return game.excluded === true || exclusionEntries().some((entry) => entry.id === id);
}

function steamAutoSyncEnabled(): boolean {
  return steam.value?.auto_sync !== false;
}

function steamRemoveUninstalledEnabled(): boolean {
  return steam.value?.autosync_remove_uninstalled === true || steam.value?.remove_uninstalled === true;
}

function steamIncludeToolsEnabled(): boolean {
  return steam.value?.include_tools === true;
}

async function loadSteamGames(): Promise<void> {
  steamGamesLoading.value = true;
  steamGamesError.value = '';
  try {
    const payload = await apiGet<unknown>('/api/steam/games');
    const games =
      payload && typeof payload === 'object' && !Array.isArray(payload)
        ? (payload as { games?: unknown }).games
        : payload;
    steamGames.value = (Array.isArray(games) ? games : []).filter(
      (game): game is SteamGame => Boolean(game) && typeof game === 'object' && !Array.isArray(game),
    );
  } catch (cause) {
    steamGames.value = [];
    steamGamesError.value = message(cause, t('ui.integrations.errors.steamGames'));
  } finally {
    steamGamesLoading.value = false;
  }
}

async function load(preserveNotice = false): Promise<void> {
  if (refreshing.value) return;
  refreshing.value = true;
  if (!preserveNotice) notice.value = '';
  const nextErrors: Partial<Record<IntegrationId, string>> = {};

  if (!system.metadata) await system.refreshHost();

  const steamResult = (await Promise.allSettled([apiGet<SteamStatus>('/api/steam/status')]))[0];
  const windowsResults = isWindows.value
    ? await Promise.allSettled([
        apiGet<PlayniteStatus>('/api/playnite/status'),
        apiGet<RtssStatus>('/api/rtss/status'),
        apiGet<LosslessStatus>('/api/lossless_scaling/status'),
        apiGet<VigemStatus>('/api/vigembus/status'),
        apiGet<VulkanStatus>('/api/health/vulkan-hdr-layer'),
      ])
    : [];
  const [playniteResult, rtssResult, losslessResult, vigemResult, vulkanResult] = windowsResults;

  if (steamResult.status === 'fulfilled') steam.value = steamResult.value;
  else nextErrors.steam = message(steamResult.reason, t('ui.integrations.errors.steamStatus'));

  if (playniteResult?.status === 'fulfilled') playnite.value = playniteResult.value;
  else if (playniteResult) nextErrors.playnite = message(playniteResult.reason, t('ui.integrations.errors.playniteStatus'));

  if (rtssResult?.status === 'fulfilled') rtss.value = rtssResult.value;
  else if (rtssResult) nextErrors.rtss = message(rtssResult.reason, t('ui.integrations.errors.rtssStatus'));

  if (losslessResult?.status === 'fulfilled') lossless.value = losslessResult.value;
  else if (losslessResult) nextErrors.lossless = message(losslessResult.reason, t('ui.integrations.errors.losslessStatus'));

  if (vigemResult?.status === 'fulfilled') vigem.value = vigemResult.value;
  else if (vigemResult) nextErrors.vigem = message(vigemResult.reason, t('ui.integrations.errors.vigemStatus'));

  if (vulkanResult?.status === 'fulfilled') vulkan.value = vulkanResult.value;
  else if (vulkanResult) nextErrors.vulkan = message(vulkanResult.reason, t('ui.integrations.errors.vulkanStatus'));

  if (steamResult.status === 'fulfilled') await loadSteamGames();

  errors.value = nextErrors;
  loading.value = false;
  refreshing.value = false;
}

function playniteSummary(): IntegrationSummary {
  const value = playnite.value;
  const name = t('navbar.playnite');
  const shortDescription = t('ui.integrations.playnite.shortDescription');
  if (!isWindows.value) return unavailableSummary('playnite', name, shortDescription);
  if (!value) return failedSummary('playnite', name, shortDescription);
  if (value.enabled === false) {
    return {
      id: 'playnite',
      name,
      description: t('ui.integrations.playnite.description'),
      status: t('_common.disabled'),
      tone: 'neutral',
      details: [],
    };
  }
  const status = value.update_available
    ? t('ui.integrations.status.updateAvailable')
    : value.active
      ? t('playnite.status_connected')
      : value.installed === true
        ? t('changelog.installed')
        : value.installed === false
          ? t('ui.integrations.status.notInstalled')
          : t('ui.integrations.status.installationUnknown');
  const tone: StatusTone = value.update_available
    ? 'warning'
    : value.active
      ? 'success'
      : value.installed === true
        ? 'info'
        : 'neutral';
  const details = [
    value.installed_version
      ? t('ui.integrations.details.installedVersion', { version: value.installed_version })
      : '',
    value.packaged_version
      ? t('ui.integrations.details.bundledVersion', { version: value.packaged_version })
      : '',
    value.extensions_dir || '',
  ].filter(Boolean);
  return {
    id: 'playnite',
    name,
    description: t('ui.integrations.playnite.description'),
    status,
    tone,
    details,
  };
}

function steamSummary(): IntegrationSummary {
  const value = steam.value;
  const name = t('ui.integrations.steam.name');
  const description = t('ui.integrations.steam.description');
  if (!value) return failedSummary('steam', name, description);
  const enabled = value.enabled !== false;
  const status = !enabled
    ? t('_common.disabled')
    : !value.available
      ? t('ui.integrations.status.notDetected')
      : value.forced
        ? t('ui.integrations.steam.forced')
        : t('ui.integrations.status.ready');
  return {
    id: 'steam',
    name,
    description,
    status,
    tone: enabled && value.available ? 'success' : enabled ? 'info' : 'neutral',
    details: [
      value.game_count !== undefined ? t('ui.integrations.steam.gameCount', { count: value.game_count }) : '',
      value.forced ? t('ui.integrations.steam.linuxForced') : '',
    ].filter(Boolean),
  };
}

function rtssSummary(): IntegrationSummary {
  const value = rtss.value;
  const name = t('ui.integrations.rtss.name');
  const shortDescription = t('ui.integrations.rtss.shortDescription');
  if (!isWindows.value) return unavailableSummary('rtss', name, shortDescription);
  if (!value) return failedSummary('rtss', name, shortDescription);
  const ready = Boolean(value.path_exists && value.hooks_found);
  const status = value.active_provider === 'rtss'
    ? t('_common.active')
    : ready
      ? t('ui.integrations.status.ready')
      : value.path_exists
        ? t('ui.integrations.status.repairNeeded')
        : t('ui.integrations.status.notDetected');
  const tone: StatusTone = value.active_provider === 'rtss' || ready ? 'success' : value.enabled ? 'warning' : 'neutral';
  return {
    id: 'rtss',
    name,
    description: t('ui.integrations.rtss.description'),
    status,
    tone,
    details: [
      value.configured_provider
        ? t('rtss.status_configured_provider', { provider: value.configured_provider })
        : '',
      value.process_running ? t('ui.integrations.rtss.processRunning') : '',
      value.message || '',
    ].filter(Boolean),
  };
}

function losslessSummary(): IntegrationSummary {
  const value = lossless.value;
  const name = t('ui.integrations.lossless.name');
  const shortDescription = t('ui.integrations.lossless.shortDescription');
  if (!isWindows.value) return unavailableSummary('lossless', name, shortDescription);
  if (!value) return failedSummary('lossless', name, shortDescription);
  const detected = value.status === 'detected';
  const problematic = value.status === 'path-is-directory' || value.status === 'path-not-found';
  const labels: Record<string, string> = {
    detected: t('ui.integrations.status.detected'),
    'not-configured': t('ui.integrations.status.notConfigured'),
    'path-is-directory': t('ui.integrations.lossless.pathIsDirectory'),
    'path-not-found': t('ui.integrations.lossless.pathNotFound'),
    unavailable: t('config.lossless.status_unavailable'),
  };
  return {
    id: 'lossless',
    name,
    description: t('ui.integrations.lossless.description'),
    status: labels[value.status ?? 'unavailable'] ?? t('config.lossless.status_unavailable'),
    tone: detected ? 'success' : problematic ? 'warning' : 'neutral',
    details: [value.resolved_path || value.configured_path || value.suggested_path || ''].filter(Boolean),
  };
}

function vigemSummary(): IntegrationSummary {
  const value = vigem.value;
  const name = t('ui.integrations.vigem.name');
  const shortDescription = t('ui.integrations.vigem.shortDescription');
  if (!isWindows.value) return unavailableSummary('vigem', name, shortDescription);
  if (!value) return failedSummary('vigem', name, shortDescription);
  const compatible = Boolean(value.installed && value.version_compatible);
  // The server reports required=false when another driver is already providing
  // controllers. Flagging ViGEmBus in that case sends people off installing a
  // dependency the host does not use.
  const required = value.required !== false;
  const notNeeded = !compatible && !required;
  return {
    id: 'vigem',
    name,
    description: notNeeded
      ? t('ui.integrations.vigem.supersededDescription')
      : t('ui.integrations.vigem.description'),
    status: compatible
      ? t('ui.integrations.status.ready')
      : notNeeded
        ? t('ui.integrations.status.notRequired')
        : value.installed
          ? t('ui.integrations.status.updateRequired')
          : t('ui.integrations.status.notInstalled'),
    tone: compatible ? 'success' : notNeeded ? 'neutral' : 'warning',
    details: [
      value.version ? t('ui.integrations.details.installedVersion', { version: value.version }) : '',
      value.packaged_version
        ? t('ui.integrations.details.bundledVersion', { version: value.packaged_version })
        : '',
      value.error || '',
    ].filter(Boolean),
  };
}

function vulkanSummary(): IntegrationSummary {
  const value = vulkan.value;
  const name = t('vulkan_hdr.troubleshooting_title');
  const shortDescription = t('ui.integrations.vulkan.shortDescription');
  if (!isWindows.value) return unavailableSummary('vulkan', name, shortDescription);
  if (!value) return failedSummary('vulkan', name, shortDescription);
  const healthy = Boolean(value.installed && value.enabled);
  return {
    id: 'vulkan',
    name,
    description: t('ui.integrations.vulkan.description'),
    status: healthy
      ? t('ui.integrations.vulkan.enabledAndRegistered')
      : value.enabled
        ? t('ui.integrations.vulkan.registrationMissing')
        : value.installed
          ? t('ui.integrations.vulkan.registeredDisabled')
          : t('_common.disabled'),
    tone: healthy ? 'success' : value.enabled && !value.installed ? 'warning' : 'neutral',
    details: [
      value.enabled
        ? t('ui.integrations.vulkan.enabledInConfiguration')
        : t('ui.integrations.vulkan.disabledInConfiguration'),
    ],
  };
}

function unavailableSummary(id: IntegrationId, name: string, description: string): IntegrationSummary {
  return { id, name, description, status: t('ui.integrations.status.windowsOnly'), tone: 'neutral', details: [] };
}

function failedSummary(id: IntegrationId, name: string, description: string): IntegrationSummary {
  return {
    id,
    name,
    description,
    status: t('ui.integrations.status.unavailable'),
    tone: 'danger',
    details: [errors.value[id] || t('ui.integrations.errors.statusLoad')],
  };
}

const summaries = computed(() => [
  steamSummary(),
  playniteSummary(),
  rtssSummary(),
  losslessSummary(),
  vigemSummary(),
  vulkanSummary(),
]);

const errorCount = computed(() => Object.keys(errors.value).length);

const dialogCopy = computed(() => {
  switch (pendingAction.value) {
    case 'playnite-install':
      return {
        title: playnite.value?.installed
          ? t('ui.integrations.confirm.playniteUpdateTitle')
          : t('ui.integrations.confirm.playniteInstallTitle'),
        description: t('ui.integrations.confirm.playniteInstallDescription'),
        confirm: playnite.value?.installed
          ? t('ui.integrations.actions.updateExtension')
          : t('ui.integrations.actions.installExtension'),
        tone: 'default' as const,
      };
    case 'playnite-uninstall':
      return {
        title: t('ui.integrations.confirm.playniteRemoveTitle'),
        description: t('ui.integrations.confirm.playniteRemoveDescription'),
        confirm: t('ui.integrations.actions.removeExtension'),
        tone: 'danger' as const,
      };
    case 'vigem-install':
      return {
        title: vigem.value?.installed
          ? t('ui.integrations.confirm.vigemRepairTitle')
          : t('ui.integrations.confirm.vigemInstallTitle'),
        description: t('ui.integrations.confirm.vigemDescription'),
        confirm: vigem.value?.installed
          ? t('ui.integrations.actions.repairDriver')
          : t('ui.integrations.actions.installDriver'),
        tone: 'default' as const,
      };
    case 'vulkan-register':
      return {
        title: t('ui.integrations.confirm.vulkanRegisterTitle'),
        description: t('ui.integrations.confirm.vulkanRegisterDescription'),
        confirm: t('ui.integrations.actions.registerLayer'),
        tone: 'default' as const,
      };
    default:
      return {
        title: t('ui.integrations.confirm.defaultTitle'),
        description: '',
        confirm: t('_common.continue'),
        tone: 'default' as const,
      };
  }
});

function requestAction(action: PendingAction): void {
  pendingAction.value = action;
  confirmOpen.value = true;
}

function updateConfirmOpen(value: boolean): void {
  confirmOpen.value = value;
  if (!value && !actionBusy.value) pendingAction.value = null;
}

async function runConfirmedAction(): Promise<void> {
  const action = pendingAction.value;
  if (!action || actionBusy.value) return;
  actionBusy.value = true;
  notice.value = '';
  try {
    let result: MutationResult;
    if (action === 'playnite-install') {
      result = await apiPost<MutationResult>('/api/playnite/install', { restart: false });
      notice.value = t('ui.integrations.notices.playniteInstalled');
    } else if (action === 'playnite-uninstall') {
      result = await apiPost<MutationResult>('/api/playnite/uninstall', { restart: false });
      notice.value = t('ui.integrations.notices.playniteRemoved');
    } else if (action === 'vigem-install') {
      result = await apiPost<MutationResult>('/api/vigembus/install', {});
      notice.value = t('ui.integrations.notices.vigemCompleted');
    } else {
      result = await apiPost<MutationResult>('/api/health/vulkan-hdr-layer/register', {});
      notice.value = t('ui.integrations.notices.vulkanRefreshed');
    }
    if (result.status === false) {
      throw new Error(result.error || t('ui.integrations.errors.actionIncomplete'));
    }
    await load(true);
  } catch (cause) {
    notice.value = '';
    errors.value = {
      ...errors.value,
      [action.startsWith('playnite') ? 'playnite' : action.startsWith('vigem') ? 'vigem' : 'vulkan']:
        message(cause, t('ui.integrations.errors.actionFailed')),
    };
  } finally {
    actionBusy.value = false;
    pendingAction.value = null;
  }
}

async function syncPlaynite(): Promise<void> {
  if (syncing.value) return;
  syncing.value = true;
  notice.value = '';
  try {
    const result = await apiPost<MutationResult>('/api/playnite/force_sync', {});
    if (result.status === false) {
      throw new Error(result.error || t('ui.integrations.errors.playniteSyncRejected'));
    }
    notice.value = t('ui.integrations.notices.playniteSynced');
    await load(true);
  } catch (cause) {
    errors.value = {
      ...errors.value,
      playnite: message(cause, t('ui.integrations.errors.playniteSyncFailed')),
    };
  } finally {
    syncing.value = false;
  }
}

async function syncSteam(): Promise<void> {
  if (syncing.value) return;
  syncing.value = true;
  notice.value = '';
  try {
    const result = await apiPost<MutationResult>('/api/steam/force_sync', {});
    if (result.status === false) throw new Error(result.error || t('ui.integrations.errors.steamSyncRejected'));
    notice.value = t('ui.integrations.notices.steamSynced');
    await load(true);
  } catch (cause) {
    errors.value = { ...errors.value, steam: message(cause, t('ui.integrations.errors.steamSyncFailed')) };
  } finally {
    syncing.value = false;
  }
}

async function setProviderEnabled(provider: 'steam' | 'playnite', enabled: boolean): Promise<void> {
  if (provider === 'steam' && steam.value?.forced) return;
  try {
    await apiPatch('/api/config', { [`${provider}_enabled`]: enabled });
    notice.value = t('ui.integrations.notices.providerUpdated');
    await load(true);
  } catch (cause) {
    errors.value = { ...errors.value, [provider]: message(cause, t('ui.integrations.errors.providerUpdateFailed')) };
  }
}

async function setSteamPolicy(key: 'steam_auto_sync' | 'steam_autosync_remove_uninstalled', value: boolean): Promise<void> {
  try {
    await apiPatch('/api/config', { [key]: value });
    notice.value = t('ui.integrations.notices.providerUpdated');
    await load(true);
  } catch (cause) {
    errors.value = { ...errors.value, steam: message(cause, t('ui.integrations.errors.providerUpdateFailed')) };
  }
}

async function setSteamIncludeTools(value: boolean): Promise<void> {
  try {
    await apiPatch('/api/config', { steam_include_tools: value });
    notice.value = t('ui.integrations.notices.providerUpdated');
    await load(true);
  } catch (cause) {
    errors.value = { ...errors.value, steam: message(cause, t('ui.integrations.errors.providerUpdateFailed')) };
  }
}

async function setSteamExclusion(game: SteamGame, excluded: boolean): Promise<void> {
  const id = steamGameId(game);
  if (!id || steamExclusionBusy.value) return;
  steamExclusionBusy.value = id;
  try {
    const next = exclusionEntries().filter((entry) => entry.id !== id);
    if (excluded) next.push({ id, name: steamGameName(game) });
    await apiPatch('/api/config', { steam_exclude_games: next });
    if (steam.value) steam.value.exclude_games = next;
    notice.value = t('ui.integrations.notices.exclusionsUpdated');
  } catch (cause) {
    errors.value = { ...errors.value, steam: message(cause, t('ui.integrations.errors.exclusionsUpdateFailed')) };
  } finally {
    steamExclusionBusy.value = null;
  }
}

onMounted(() => void load());
</script>

<template>
  <div class="page page--narrow integrations-page">
    <PageHeader
      :title="t('ui.integrations.title')"
      :description="t('ui.integrations.description')"
    >
      <template #actions>
        <AppButton
          icon="refresh"
          :label="t('ui.integrations.actions.refreshStatus')"
          variant="secondary"
          :busy="refreshing"
          :busy-label="t('ui.integrations.refreshing')"
          @click="load()"
        />
      </template>
    </PageHeader>

    <InlineAlert
      v-if="notice"
      tone="success"
      :title="t('ui.integrations.updated')"
      announce="polite"
    >
      {{ notice }}
    </InlineAlert>
    <InlineAlert
      v-if="errorCount"
      tone="warning"
      :title="t('ui.integrations.checksFailed')"
    >
      {{
        t(
          errorCount === 1
            ? 'ui.integrations.failedCount.one'
            : 'ui.integrations.failedCount.other',
          { count: errorCount },
        )
      }}
    </InlineAlert>

    <div
      v-if="loading"
      class="integration-list"
      :aria-label="t('ui.integrations.loadingStatus')"
    >
      <LoadingSkeleton v-for="index in 5" :key="index" variant="block" height="112px" />
    </div>

    <section
      v-else
      class="integration-list"
      :aria-label="t('ui.integrations.installedIntegrations')"
    >
      <article
        v-for="summary in summaries"
        :key="summary.id"
        class="integration-row"
        :aria-labelledby="`integration-${summary.id}`"
      >
        <span class="integration-row__icon" aria-hidden="true">
          <UiIcon :name="summary.id === 'vigem' || summary.id === 'steam' ? 'gamepad' : summary.id === 'playnite' ? 'library' : 'integrations'" :size="20" />
        </span>
        <div class="integration-row__main">
          <div class="integration-row__title">
            <h2 :id="`integration-${summary.id}`">{{ summary.name }}</h2>
            <StatusBadge :label="summary.status" :tone="summary.tone" compact />
          </div>
          <p>{{ summary.description }}</p>
          <ul v-if="summary.details.length" class="integration-details">
            <li v-for="detail in summary.details" :key="detail">{{ detail }}</li>
          </ul>
          <div v-if="summary.id === 'steam'" class="steam-policy" aria-labelledby="steam-policy-heading">
            <h3 id="steam-policy-heading">{{ t('ui.integrations.steam.policyTitle') }}</h3>
            <label class="steam-policy__toggle">
              <input
                type="checkbox"
                :checked="steamAutoSyncEnabled()"
                @change="setSteamPolicy('steam_auto_sync', ($event.target as HTMLInputElement).checked)"
              />
              <span>
                <strong>{{ t('ui.integrations.steam.autoSync') }}</strong>
                <small>{{ t('ui.integrations.steam.autoSyncDescription') }}</small>
              </span>
            </label>
            <label class="steam-policy__toggle">
              <input
                type="checkbox"
                :checked="steamRemoveUninstalledEnabled()"
                @change="setSteamPolicy('steam_autosync_remove_uninstalled', ($event.target as HTMLInputElement).checked)"
              />
              <span>
                <strong>{{ t('ui.integrations.steam.removeUninstalled') }}</strong>
                <small>{{ t('ui.integrations.steam.removeUninstalledDescription') }}</small>
              </span>
            </label>
            <label class="steam-policy__toggle steam-policy__toggle--advanced">
              <input
                type="checkbox"
                :checked="steamIncludeToolsEnabled()"
                @change="setSteamIncludeTools(($event.target as HTMLInputElement).checked)"
              />
              <span>
                <strong>{{ t('ui.integrations.steam.includeTools') }}</strong>
                <small>{{ t('ui.integrations.steam.includeToolsDescription') }}</small>
              </span>
            </label>
            <div class="steam-policy__exclusions">
              <strong>{{ t('ui.integrations.steam.exclusionsTitle') }}</strong>
              <small>{{ t('ui.integrations.steam.exclusionsDescription') }}</small>
              <p v-if="steamGamesLoading" class="steam-policy__notice">{{ t('ui.integrations.steam.loadingGames') }}</p>
              <p v-else-if="steamGamesError" class="steam-policy__notice steam-policy__notice--error">{{ steamGamesError }}</p>
              <p v-else-if="!steamGames.length" class="steam-policy__notice">{{ t('ui.integrations.steam.noGames') }}</p>
              <label v-for="game in steamGames" v-else :key="steamGameId(game)" class="steam-policy__game">
                <input
                  type="checkbox"
                  :checked="gameExcluded(game)"
                  :disabled="steamExclusionBusy === steamGameId(game)"
                  :aria-label="t('ui.integrations.steam.excludeGame', { name: steamGameName(game) })"
                  @change="setSteamExclusion(game, ($event.target as HTMLInputElement).checked)"
                />
                <span>{{ steamGameName(game) }}</span>
                <small v-if="game.filtered && !game.excluded" class="steam-policy__game-filtered">
                  {{ t('ui.integrations.steam.filteredTool') }}
                </small>
                <code>{{ steamGameId(game) }}</code>
              </label>
            </div>
          </div>
        </div>
        <div v-if="summary.id === 'steam' || isWindows" class="integration-row__actions">
          <template v-if="summary.id === 'steam'">
            <AppButton
              v-if="!steam?.forced"
              :label="steam?.enabled === false ? t('ui.integrations.actions.enable') : t('ui.integrations.actions.disable')"
              variant="tertiary"
              size="compact"
              @click="setProviderEnabled('steam', steam?.enabled === false)"
            />
            <AppButton
              icon="refresh"
              :label="t('ui.integrations.actions.rescan')"
              variant="secondary"
              size="compact"
              :busy="syncing"
              :busy-label="t('ui.integrations.syncing')"
              :disabled="steam?.enabled === false"
              @click="syncSteam"
            />
          </template>
          <template v-else-if="summary.id === 'playnite'">
            <AppButton
              :label="playnite?.enabled === false ? t('ui.integrations.actions.enable') : t('ui.integrations.actions.disable')"
              variant="tertiary"
              size="compact"
              @click="setProviderEnabled('playnite', playnite?.enabled === false)"
            />
            <AppButton
              v-if="playnite?.enabled !== false && (playnite?.installed !== true || playnite?.update_available)"
              :label="playnite?.update_available ? t('ui.integrations.actions.update') : t('ui.integrations.actions.install')"
              variant="secondary"
              size="compact"
              @click="requestAction('playnite-install')"
            />
            <AppButton
              v-if="playnite?.enabled !== false && playnite?.installed === true"
              icon="refresh"
              :label="t('ui.integrations.actions.rescan')"
              variant="tertiary"
              size="compact"
              :disabled="!playnite.active"
              :busy="syncing"
              :busy-label="t('ui.integrations.syncing')"
              @click="syncPlaynite"
            />
            <AppButton
              v-if="playnite?.enabled !== false && playnite?.installed === true"
              class="integration-action--danger"
              :label="t('_common.remove')"
              variant="tertiary"
              size="compact"
              @click="requestAction('playnite-uninstall')"
            />
          </template>
          <AppButton
            v-else-if="summary.id === 'vigem' && (!vigem?.installed || !vigem?.version_compatible)"
            :label="vigem?.installed ? t('ui.integrations.actions.repair') : t('ui.integrations.actions.install')"
            variant="secondary"
            size="compact"
            @click="requestAction('vigem-install')"
          />
          <AppButton
            v-else-if="summary.id === 'vulkan' && vulkan?.enabled && !vulkan?.installed"
            :label="t('ui.integrations.actions.repairRegistration')"
            variant="secondary"
            size="compact"
            @click="requestAction('vulkan-register')"
          />
        </div>
      </article>
    </section>

    <ConfirmDialog
      :open="confirmOpen"
      :title="dialogCopy.title"
      :description="dialogCopy.description"
      :confirm-label="dialogCopy.confirm"
      :tone="dialogCopy.tone"
      :busy="actionBusy"
      :busy-label="t('ui.integrations.applying')"
      @update:open="updateConfirmOpen"
      @confirm="runConfirmedAction"
    />
  </div>
</template>

<style scoped>
.integrations-page {
  display: grid;
  gap: var(--vs-space-20);
}

.integration-list {
  display: grid;
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.integration-row {
  display: grid;
  min-height: 7rem;
  grid-template-columns: auto minmax(0, 1fr) auto;
  align-items: start;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16) var(--vs-space-20);
}

.integration-row + .integration-row {
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.integration-row__icon {
  display: grid;
  width: 2.5rem;
  height: 2.5rem;
  place-items: center;
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-text-secondary);
}

.integration-row__main {
  display: grid;
  min-width: 0;
  gap: var(--vs-space-4);
}

.integration-row__title {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.integration-row h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.integration-row p,
.integration-details {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.integration-details {
  display: flex;
  min-width: 0;
  flex-wrap: wrap;
  gap: var(--vs-space-4) var(--vs-space-16);
  padding: 0;
  margin-top: var(--vs-space-4);
  list-style: none;
}

.integration-details li {
  overflow-wrap: anywhere;
}

.steam-policy {
  display: grid;
  max-width: 42rem;
  gap: var(--vs-space-8);
  padding-top: var(--vs-space-8);
}

.steam-policy h3 {
  font-size: var(--vs-type-size-label);
}

.steam-policy__toggle,
.steam-policy__game {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-8);
}

.steam-policy__toggle input,
.steam-policy__game input {
  flex: 0 0 auto;
  margin-top: 0.2rem;
}

.steam-policy__toggle span,
.steam-policy__exclusions {
  display: grid;
  gap: var(--vs-space-2);
}

.steam-policy small {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.steam-policy__exclusions {
  gap: var(--vs-space-4);
  padding-top: var(--vs-space-4);
}

.steam-policy__game {
  align-items: center;
  padding: var(--vs-space-4) 0;
}

.steam-policy__game code {
  margin-inline-start: auto;
  color: var(--vs-color-text-tertiary);
  font-size: var(--vs-type-size-metadata);
}

.steam-policy__game-filtered {
  color: var(--vs-color-text-tertiary);
  font-style: italic;
}

.steam-policy__notice {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.steam-policy__notice--error {
  color: var(--vs-color-status-danger);
}

.integration-row__actions {
  display: flex;
  max-width: 18rem;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: var(--vs-space-4);
}

.integration-action--danger {
  color: var(--vs-color-status-danger);
}

@media (max-width: 47.999rem) {
  .integration-row {
    grid-template-columns: auto minmax(0, 1fr);
    padding-inline: var(--vs-space-16);
  }

  .integration-row__actions {
    grid-column: 1 / -1;
    max-width: none;
    justify-content: flex-start;
    padding-inline-start: calc(2.5rem + var(--vs-space-16));
  }
}

@media (max-width: 29.999rem) {
  .integration-row {
    grid-template-columns: minmax(0, 1fr);
  }

  .integration-row__icon {
    display: none;
  }

  .integration-row__actions {
    padding-inline-start: 0;
  }
}

@media (forced-colors: active) {
  .integration-list,
  .integration-row__icon {
    border: var(--vs-border-width) solid CanvasText;
  }
}
</style>
