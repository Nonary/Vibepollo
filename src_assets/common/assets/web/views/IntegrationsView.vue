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
  SettingRow,
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
  importable_game_count?: number;
  excluded_game_count?: number;
  tool_game_count?: number;
  playnite_available?: boolean;
  auto_sync?: boolean;
  autosync_remove_uninstalled?: boolean;
  remove_uninstalled?: boolean;
  include_tools?: boolean;
  exclude_games?: Array<{ id?: string; name?: string }> | string[] | number;
}

interface MangoHudStatus {
  enabled?: boolean;
  configured_provider?: string;
  active_provider?: string;
  fps_limit?: number;
  fps_limit_millihz?: number;
  mangohud_available?: boolean;
  resolved_path?: string;
  message?: string;
}

interface SteamGame {
  appid: number | string;
  steam_id?: string;
  name?: string;
  install_dir?: string;
  excluded?: boolean;
  filtered?: boolean;
  app_type?: string;
}

interface LutrisStatus {
  enabled?: boolean;
  available?: boolean;
  game_count?: number;
  importable_game_count?: number;
  excluded_game_count?: number;
  steam_game_count?: number;
  database_path?: string;
  auto_sync?: boolean;
  autosync_remove_uninstalled?: boolean;
  include_steam?: boolean;
  exclude_games?: Array<{ id?: string; name?: string }> | string[] | number;
}

interface LutrisGame {
  id: number | string;
  lutris_id?: string;
  name?: string;
  runner?: string;
  platform?: string;
  steam_backed?: boolean;
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

type IntegrationId = 'steam' | 'lutris' | 'mangohud' | 'playnite' | 'rtss' | 'lossless' | 'vigem' | 'vulkan';
type SteamGameFilter = 'all' | 'included' | 'excluded';
type PendingAction =
  | 'playnite-install'
  | 'playnite-uninstall'
  | 'vigem-install'
  | 'vulkan-register';

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
const steamGameSearch = ref('');
const steamGameFilter = ref<SteamGameFilter>('all');
const steamExclusionDraft = ref<Set<string>>(new Set());
const steamExclusionOriginal = ref<Set<string>>(new Set());
const steamExclusionsSaving = ref(false);
const lutris = ref<LutrisStatus | null>(null);
const lutrisGames = ref<LutrisGame[]>([]);
const lutrisGamesLoading = ref(false);
const lutrisGamesError = ref('');
const lutrisGameSearch = ref('');
const lutrisGameFilter = ref<SteamGameFilter>('all');
const lutrisExclusionDraft = ref<Set<string>>(new Set());
const lutrisExclusionOriginal = ref<Set<string>>(new Set());
const lutrisExclusionsSaving = ref(false);
const mangohud = ref<MangoHudStatus | null>(null);
const mangoDraft = ref({ enabled: false, provider: 'auto', fpsLimit: 0 });
const mangoOriginal = ref({ enabled: false, provider: 'auto', fpsLimit: 0 });
const mangoSaving = ref(false);
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
const isLinux = computed(() =>
  String(system.metadata?.platform ?? '')
    .toLocaleLowerCase()
    .includes('linux'),
);

function message(cause: unknown, fallback: string): string {
  return cause instanceof ApiError ? fallback : cause instanceof Error ? cause.message : fallback;
}

function steamGameId(game: SteamGame): string {
  return String(game.steam_id ?? game.appid ?? '').trim();
}

function steamGameName(game: SteamGame): string {
  return (
    String(game.name ?? '').trim() ||
    t('ui.integrations.steam.unknownGame', { id: steamGameId(game) })
  );
}

function lutrisGameId(game: LutrisGame): string {
  return String(game.lutris_id ?? game.id ?? '').trim();
}

function lutrisGameName(game: LutrisGame): string {
  return String(game.name ?? '').trim() || t('ui.integrations.lutris.unknownGame', { id: lutrisGameId(game) });
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

function resetSteamExclusionDraft(): void {
  const ids = new Set(
    exclusionEntries()
      .map((entry) => entry.id)
      .filter(Boolean),
  );
  steamExclusionOriginal.value = new Set(ids);
  steamExclusionDraft.value = new Set(ids);
}

function resetLutrisExclusionDraft(): void {
  const entries = lutris.value?.exclude_games;
  const ids = new Set<string>();
  if (Array.isArray(entries)) {
    for (const entry of entries) {
      const id = typeof entry === 'string' ? entry : String(entry?.id ?? '').trim();
      if (id) ids.add(id);
    }
  } else {
    for (const game of lutrisGames.value) if (game.excluded) ids.add(lutrisGameId(game));
  }
  lutrisExclusionOriginal.value = new Set(ids);
  lutrisExclusionDraft.value = new Set(ids);
}

function lutrisGameExcluded(game: LutrisGame): boolean {
  return lutrisExclusionDraft.value.has(lutrisGameId(game));
}

const lutrisExcludedCount = computed(() => lutrisExclusionDraft.value.size);
const lutrisExclusionsDirty = computed(() => {
  const current = lutrisExclusionDraft.value;
  const original = lutrisExclusionOriginal.value;
  return current.size !== original.size || [...current].some((id) => !original.has(id));
});

const filteredLutrisGames = computed(() => {
  const query = lutrisGameSearch.value.trim().toLocaleLowerCase();
  return [...lutrisGames.value]
    .filter((game) => {
      const excluded = lutrisGameExcluded(game);
      if (lutrisGameFilter.value === 'included' && excluded) return false;
      if (lutrisGameFilter.value === 'excluded' && !excluded) return false;
      return !query || lutrisGameName(game).toLocaleLowerCase().includes(query) ||
        lutrisGameId(game).includes(query) || String(game.runner ?? '').toLocaleLowerCase().includes(query);
    })
    .sort((left, right) => lutrisGameName(left).localeCompare(lutrisGameName(right)));
});

function setLutrisDraftExclusion(game: LutrisGame, excluded: boolean): void {
  const id = lutrisGameId(game);
  const next = new Set(lutrisExclusionDraft.value);
  if (excluded) next.add(id); else next.delete(id);
  lutrisExclusionDraft.value = next;
}

function setVisibleLutrisDraftExclusions(excluded: boolean): void {
  const next = new Set(lutrisExclusionDraft.value);
  for (const game of filteredLutrisGames.value) {
    const id = lutrisGameId(game);
    if (excluded) next.add(id); else next.delete(id);
  }
  lutrisExclusionDraft.value = next;
}

function gameExcluded(game: SteamGame): boolean {
  return steamExclusionDraft.value.has(steamGameId(game));
}

const steamExcludedCount = computed(() => steamExclusionDraft.value.size);
const steamExclusionsDirty = computed(() => {
  const current = steamExclusionDraft.value;
  const original = steamExclusionOriginal.value;
  return current.size !== original.size || [...current].some((id) => !original.has(id));
});

const filteredSteamGames = computed(() => {
  const query = steamGameSearch.value.trim().toLocaleLowerCase();
  return [...steamGames.value]
    .filter((game) => {
      const excluded = gameExcluded(game);
      if (steamGameFilter.value === 'included' && excluded) return false;
      if (steamGameFilter.value === 'excluded' && !excluded) return false;
      if (!query) return true;
      return (
        steamGameName(game).toLocaleLowerCase().includes(query) || steamGameId(game).includes(query)
      );
    })
    .sort((left, right) => steamGameName(left).localeCompare(steamGameName(right)));
});

function setDraftExclusion(game: SteamGame, excluded: boolean): void {
  const id = steamGameId(game);
  if (!id) return;
  const next = new Set(steamExclusionDraft.value);
  if (excluded) next.add(id);
  else next.delete(id);
  steamExclusionDraft.value = next;
}

function setVisibleDraftExclusions(excluded: boolean): void {
  const next = new Set(steamExclusionDraft.value);
  for (const game of filteredSteamGames.value) {
    const id = steamGameId(game);
    if (!id) continue;
    if (excluded) next.add(id);
    else next.delete(id);
  }
  steamExclusionDraft.value = next;
}

function resetMangoDraft(): void {
  const next = {
    enabled: mangohud.value?.enabled === true,
    provider: String(mangohud.value?.configured_provider || 'auto'),
    fpsLimit: Number(mangohud.value?.fps_limit ?? 0),
  };
  mangoOriginal.value = { ...next };
  mangoDraft.value = { ...next };
}

const mangoDirty = computed(
  () =>
    mangoDraft.value.enabled !== mangoOriginal.value.enabled ||
    mangoDraft.value.provider !== mangoOriginal.value.provider ||
    Number(mangoDraft.value.fpsLimit) !== Number(mangoOriginal.value.fpsLimit),
);

function steamAutoSyncEnabled(): boolean {
  return steam.value?.auto_sync !== false;
}

function steamRemoveUninstalledEnabled(): boolean {
  return (
    steam.value?.autosync_remove_uninstalled === true || steam.value?.remove_uninstalled === true
  );
}

function steamIncludeToolsEnabled(): boolean {
  return steam.value?.include_tools === true;
}

async function loadSteamGames(resetDraft = true): Promise<void> {
  steamGamesLoading.value = true;
  steamGamesError.value = '';
  try {
    const payload = await apiGet<unknown>('/api/steam/games');
    const games =
      payload && typeof payload === 'object' && !Array.isArray(payload)
        ? (payload as { games?: unknown }).games
        : payload;
    steamGames.value = (Array.isArray(games) ? games : []).filter(
      (game): game is SteamGame =>
        Boolean(game) && typeof game === 'object' && !Array.isArray(game),
    );
    if (resetDraft) resetSteamExclusionDraft();
  } catch (cause) {
    steamGames.value = [];
    steamGamesError.value = message(cause, t('ui.integrations.errors.steamGames'));
  } finally {
    steamGamesLoading.value = false;
  }
}

async function loadLutrisGames(resetDraft = true): Promise<void> {
  lutrisGamesLoading.value = true;
  lutrisGamesError.value = '';
  try {
    const payload = await apiGet<{ games?: LutrisGame[] }>('/api/lutris/games');
    lutrisGames.value = Array.isArray(payload.games) ? payload.games : [];
    if (resetDraft) resetLutrisExclusionDraft();
  } catch (cause) {
    lutrisGames.value = [];
    lutrisGamesError.value = message(cause, t('ui.integrations.errors.lutrisGames'));
  } finally {
    lutrisGamesLoading.value = false;
  }
}

async function load(preserveNotice = false): Promise<void> {
  if (refreshing.value) return;
  refreshing.value = true;
  if (!preserveNotice) notice.value = '';
  const nextErrors: Partial<Record<IntegrationId, string>> = {};

  if (!system.metadata) await system.refreshHost();

  const steamResult = (await Promise.allSettled([apiGet<SteamStatus>('/api/steam/status')]))[0];
  const lutrisResult = isLinux.value
    ? (await Promise.allSettled([apiGet<LutrisStatus>('/api/lutris/status')]))[0]
    : undefined;
  const mangoResult = isLinux.value
    ? (await Promise.allSettled([apiGet<MangoHudStatus>('/api/frame-limiter/status')]))[0]
    : undefined;
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

  if (lutrisResult?.status === 'fulfilled') lutris.value = lutrisResult.value;
  else if (lutrisResult)
    nextErrors.lutris = message(lutrisResult.reason, t('ui.integrations.errors.lutrisStatus'));

  if (mangoResult?.status === 'fulfilled') {
    mangohud.value = mangoResult.value;
    resetMangoDraft();
  } else if (mangoResult) {
    nextErrors.mangohud = message(mangoResult.reason, t('ui.integrations.errors.mangohudStatus'));
  }

  if (playniteResult?.status === 'fulfilled') playnite.value = playniteResult.value;
  else if (playniteResult)
    nextErrors.playnite = message(
      playniteResult.reason,
      t('ui.integrations.errors.playniteStatus'),
    );

  if (rtssResult?.status === 'fulfilled') rtss.value = rtssResult.value;
  else if (rtssResult)
    nextErrors.rtss = message(rtssResult.reason, t('ui.integrations.errors.rtssStatus'));

  if (losslessResult?.status === 'fulfilled') lossless.value = losslessResult.value;
  else if (losslessResult)
    nextErrors.lossless = message(
      losslessResult.reason,
      t('ui.integrations.errors.losslessStatus'),
    );

  if (vigemResult?.status === 'fulfilled') vigem.value = vigemResult.value;
  else if (vigemResult)
    nextErrors.vigem = message(vigemResult.reason, t('ui.integrations.errors.vigemStatus'));

  if (vulkanResult?.status === 'fulfilled') vulkan.value = vulkanResult.value;
  else if (vulkanResult)
    nextErrors.vulkan = message(vulkanResult.reason, t('ui.integrations.errors.vulkanStatus'));

  if (steamResult.status === 'fulfilled') await loadSteamGames();
  if (lutrisResult?.status === 'fulfilled') await loadLutrisGames();

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
      value.game_count !== undefined
        ? t('ui.integrations.steam.gameCount', { count: value.game_count })
        : '',
      value.importable_game_count !== undefined
        ? t('ui.integrations.steam.importableCount', { count: value.importable_game_count })
        : '',
      value.forced ? t('ui.integrations.steam.linuxForced') : '',
    ].filter(Boolean),
  };
}

function lutrisSummary(): IntegrationSummary {
  const value = lutris.value;
  const name = t('ui.integrations.lutris.name');
  const description = t('ui.integrations.lutris.description');
  if (!isLinux.value) return unavailableSummary('lutris', name, description);
  if (!value) return failedSummary('lutris', name, description);
  const enabled = value.enabled !== false;
  return {
    id: 'lutris',
    name,
    description,
    status: !enabled ? t('_common.disabled') : value.available ? t('ui.integrations.status.ready') : t('ui.integrations.status.notDetected'),
    tone: enabled && value.available ? 'success' : enabled ? 'warning' : 'neutral',
    details: [
      value.game_count !== undefined ? t('ui.integrations.lutris.gameCount', { count: value.game_count }) : '',
      value.importable_game_count !== undefined ? t('ui.integrations.lutris.importableCount', { count: value.importable_game_count }) : '',
      value.steam_game_count ? t('ui.integrations.lutris.steamHandledBySteam', { count: value.steam_game_count }) : '',
    ].filter(Boolean),
  };
}

function mangoHudSummary(): IntegrationSummary {
  const value = mangohud.value;
  const name = t('ui.integrations.mangohud.name');
  const description = t('ui.integrations.mangohud.description');
  if (!isLinux.value) return unavailableSummary('mangohud', name, description);
  if (!value) return failedSummary('mangohud', name, description);
  const selected = value.configured_provider === 'auto' || value.configured_provider === 'mangohud';
  const available = value.mangohud_available === true;
  const enabled = value.enabled === true && selected;
  return {
    id: 'mangohud',
    name,
    description,
    status: !available
      ? t('ui.integrations.status.notDetected')
      : enabled
        ? t('_common.active')
        : selected
          ? t('ui.integrations.status.ready')
          : t('_common.disabled'),
    tone: !available ? 'warning' : enabled ? 'success' : selected ? 'info' : 'neutral',
    details: [
      value.fps_limit
        ? t('ui.integrations.mangohud.fixedLimit', { fps: value.fps_limit })
        : t('ui.integrations.mangohud.streamLimit'),
      value.resolved_path || '',
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
  const status =
    value.active_provider === 'rtss'
      ? t('_common.active')
      : ready
        ? t('ui.integrations.status.ready')
        : value.path_exists
          ? t('ui.integrations.status.repairNeeded')
          : t('ui.integrations.status.notDetected');
  const tone: StatusTone =
    value.active_provider === 'rtss' || ready ? 'success' : value.enabled ? 'warning' : 'neutral';
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
    details: [value.resolved_path || value.configured_path || value.suggested_path || ''].filter(
      Boolean,
    ),
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
      value.version
        ? t('ui.integrations.details.installedVersion', { version: value.version })
        : '',
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

function unavailableSummary(
  id: IntegrationId,
  name: string,
  description: string,
): IntegrationSummary {
  return {
    id,
    name,
    description,
    status: t('ui.integrations.status.windowsOnly'),
    tone: 'neutral',
    details: [],
  };
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

const summaries = computed(() => {
  if (isLinux.value) return [steamSummary(), lutrisSummary(), mangoHudSummary()];
  if (isWindows.value) {
    return [
      steamSummary(),
      playniteSummary(),
      rtssSummary(),
      losslessSummary(),
      vigemSummary(),
      vulkanSummary(),
    ];
  }
  return [steamSummary()];
});

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
      [action.startsWith('playnite')
        ? 'playnite'
        : action.startsWith('vigem')
          ? 'vigem'
          : 'vulkan']: message(cause, t('ui.integrations.errors.actionFailed')),
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
    if (result.status === false)
      throw new Error(result.error || t('ui.integrations.errors.steamSyncRejected'));
    notice.value = t('ui.integrations.notices.steamSynced');
    await load(true);
  } catch (cause) {
    errors.value = {
      ...errors.value,
      steam: message(cause, t('ui.integrations.errors.steamSyncFailed')),
    };
  } finally {
    syncing.value = false;
  }
}

async function syncLutris(): Promise<void> {
  if (syncing.value) return;
  syncing.value = true;
  notice.value = '';
  try {
    const result = await apiPost<MutationResult>('/api/lutris/force_sync', {});
    if (result.status === false) throw new Error(result.error || t('ui.integrations.errors.lutrisSyncRejected'));
    notice.value = t('ui.integrations.notices.lutrisSynced');
    await load(true);
  } catch (cause) {
    errors.value = { ...errors.value, lutris: message(cause, t('ui.integrations.errors.lutrisSyncFailed')) };
  } finally {
    syncing.value = false;
  }
}

async function setProviderEnabled(provider: 'steam' | 'lutris' | 'playnite', enabled: boolean): Promise<void> {
  if (provider === 'steam' && steam.value?.forced) return;
  try {
    await apiPatch('/api/config', { [`${provider}_enabled`]: enabled });
    notice.value = t('ui.integrations.notices.providerUpdated');
    await load(true);
  } catch (cause) {
    errors.value = {
      ...errors.value,
      [provider]: message(cause, t('ui.integrations.errors.providerUpdateFailed')),
    };
  }
}

async function setLutrisPolicy(
  key: 'lutris_auto_sync' | 'lutris_autosync_remove_uninstalled' | 'lutris_include_steam',
  value: boolean,
): Promise<void> {
  try {
    await apiPatch('/api/config', { [key]: value });
    if (lutris.value) {
      if (key === 'lutris_auto_sync') lutris.value.auto_sync = value;
      else if (key === 'lutris_autosync_remove_uninstalled') lutris.value.autosync_remove_uninstalled = value;
      else lutris.value.include_steam = value;
    }
    notice.value = t('ui.integrations.notices.providerUpdated');
    if (key === 'lutris_include_steam') await loadLutrisGames(false);
  } catch (cause) {
    errors.value = { ...errors.value, lutris: message(cause, t('ui.integrations.errors.providerUpdateFailed')) };
  }
}

async function saveLutrisExclusions(): Promise<void> {
  if (!lutrisExclusionsDirty.value || lutrisExclusionsSaving.value) return;
  lutrisExclusionsSaving.value = true;
  try {
    const gamesById = new Map(lutrisGames.value.map((game) => [lutrisGameId(game), game]));
    const next = [...lutrisExclusionDraft.value]
      .map((id) => ({ id, name: gamesById.has(id) ? lutrisGameName(gamesById.get(id)!) : '' }))
      .sort((left, right) => left.name.localeCompare(right.name) || left.id.localeCompare(right.id));
    await apiPatch('/api/config', { lutris_exclude_games: next });
    if (lutris.value) lutris.value.exclude_games = next;
    lutrisExclusionOriginal.value = new Set(lutrisExclusionDraft.value);
    notice.value = t('ui.integrations.notices.lutrisExclusionsUpdated');
  } catch (cause) {
    errors.value = { ...errors.value, lutris: message(cause, t('ui.integrations.errors.exclusionsUpdateFailed')) };
  } finally {
    lutrisExclusionsSaving.value = false;
  }
}

async function setSteamPolicy(
  key: 'steam_auto_sync' | 'steam_autosync_remove_uninstalled',
  value: boolean,
): Promise<void> {
  try {
    await apiPatch('/api/config', { [key]: value });
    if (steam.value) {
      if (key === 'steam_auto_sync') steam.value.auto_sync = value;
      else steam.value.autosync_remove_uninstalled = value;
    }
    notice.value = t('ui.integrations.notices.providerUpdated');
  } catch (cause) {
    errors.value = {
      ...errors.value,
      steam: message(cause, t('ui.integrations.errors.providerUpdateFailed')),
    };
  }
}

async function setSteamIncludeTools(value: boolean): Promise<void> {
  try {
    await apiPatch('/api/config', { steam_include_tools: value });
    if (steam.value) steam.value.include_tools = value;
    notice.value = t('ui.integrations.notices.providerUpdated');
    await loadSteamGames(false);
  } catch (cause) {
    errors.value = {
      ...errors.value,
      steam: message(cause, t('ui.integrations.errors.providerUpdateFailed')),
    };
  }
}

async function saveSteamExclusions(): Promise<void> {
  if (!steamExclusionsDirty.value || steamExclusionsSaving.value) return;
  steamExclusionsSaving.value = true;
  try {
    const gamesById = new Map(steamGames.value.map((game) => [steamGameId(game), game]));
    const next = [...steamExclusionDraft.value]
      .map((id) => ({ id, name: gamesById.has(id) ? steamGameName(gamesById.get(id)!) : '' }))
      .sort(
        (left, right) => left.name.localeCompare(right.name) || left.id.localeCompare(right.id),
      );
    await apiPatch('/api/config', { steam_exclude_games: next });
    if (steam.value) steam.value.exclude_games = next;
    steamExclusionOriginal.value = new Set(steamExclusionDraft.value);
    notice.value = t('ui.integrations.notices.exclusionsUpdated');
  } catch (cause) {
    errors.value = {
      ...errors.value,
      steam: message(cause, t('ui.integrations.errors.exclusionsUpdateFailed')),
    };
  } finally {
    steamExclusionsSaving.value = false;
  }
}

async function saveMangoSettings(): Promise<void> {
  if (!mangoDirty.value || mangoSaving.value) return;
  mangoSaving.value = true;
  try {
    const fps = Math.max(0, Math.min(1000, Number(mangoDraft.value.fpsLimit) || 0));
    mangoDraft.value.fpsLimit = fps;
    await apiPatch('/api/config', {
      frame_limiter_enable: mangoDraft.value.enabled,
      frame_limiter_provider: mangoDraft.value.provider,
      frame_limiter_fps_limit: fps,
    });
    notice.value = t('ui.integrations.notices.mangohudUpdated');
    await load(true);
  } catch (cause) {
    errors.value = {
      ...errors.value,
      mangohud: message(cause, t('ui.integrations.errors.mangohudUpdateFailed')),
    };
  } finally {
    mangoSaving.value = false;
  }
}

onMounted(() => void load());
</script>

<template>
  <div class="page page--narrow integrations-page">
    <PageHeader :title="t('ui.integrations.title')" :description="t('ui.integrations.description')">
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
    <InlineAlert v-if="errorCount" tone="warning" :title="t('ui.integrations.checksFailed')">
      {{
        t(
          errorCount === 1
            ? 'ui.integrations.failedCount.one'
            : 'ui.integrations.failedCount.other',
          { count: errorCount },
        )
      }}
    </InlineAlert>

    <div v-if="loading" class="integration-list" :aria-label="t('ui.integrations.loadingStatus')">
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
          <UiIcon
            :name="
              summary.id === 'vigem' || summary.id === 'steam'
                ? 'gamepad'
                : summary.id === 'playnite'
                  ? 'library'
                  : 'integrations'
            "
            :size="20"
          />
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
          <section
            v-if="summary.id === 'steam'"
            class="integration-settings"
            aria-labelledby="steam-policy-heading"
          >
            <div class="integration-settings__heading">
              <div>
                <h3 id="steam-policy-heading">{{ t('ui.integrations.steam.policyTitle') }}</h3>
                <small>{{ t('ui.integrations.steam.policyDescription') }}</small>
              </div>
            </div>

            <div class="integration-settings__rows">
              <SettingRow
                :label="t('ui.integrations.steam.autoSync')"
                :description="t('ui.integrations.steam.autoSyncDescription')"
                control-id="steam-auto-sync"
              >
                <input
                  id="steam-auto-sync"
                  class="integration-switch"
                  type="checkbox"
                  :checked="steamAutoSyncEnabled()"
                  @change="
                    setSteamPolicy('steam_auto_sync', ($event.target as HTMLInputElement).checked)
                  "
                />
              </SettingRow>
              <SettingRow
                :label="t('ui.integrations.steam.removeUninstalled')"
                :description="t('ui.integrations.steam.removeUninstalledDescription')"
                control-id="steam-remove-uninstalled"
              >
                <input
                  id="steam-remove-uninstalled"
                  class="integration-switch"
                  type="checkbox"
                  :checked="steamRemoveUninstalledEnabled()"
                  @change="
                    setSteamPolicy(
                      'steam_autosync_remove_uninstalled',
                      ($event.target as HTMLInputElement).checked,
                    )
                  "
                />
              </SettingRow>
              <details class="integration-advanced">
                <summary>{{ t('ui.integrations.steam.advancedSettings') }}</summary>
                <SettingRow
                  :label="t('ui.integrations.steam.includeTools')"
                  :description="t('ui.integrations.steam.includeToolsDescription')"
                  control-id="steam-include-tools"
                >
                  <input
                    id="steam-include-tools"
                    class="integration-switch"
                    type="checkbox"
                    :checked="steamIncludeToolsEnabled()"
                    @change="setSteamIncludeTools(($event.target as HTMLInputElement).checked)"
                  />
                </SettingRow>
              </details>
            </div>

            <div class="game-manager" aria-labelledby="steam-exclusions-heading">
              <div class="game-manager__heading">
                <div>
                  <h4 id="steam-exclusions-heading">
                    {{ t('ui.integrations.steam.exclusionsTitle') }}
                  </h4>
                  <small>{{ t('ui.integrations.steam.exclusionsDescription') }}</small>
                </div>
                <StatusBadge
                  :label="t('ui.integrations.steam.excludedCount', { count: steamExcludedCount })"
                  :tone="steamExclusionsDirty ? 'warning' : 'neutral'"
                  compact
                />
              </div>
              <div class="game-manager__toolbar">
                <label class="game-manager__search">
                  <span class="vs-sr-only">{{ t('ui.integrations.steam.searchGames') }}</span>
                  <UiIcon name="search" :size="16" aria-hidden="true" />
                  <input
                    v-model="steamGameSearch"
                    type="search"
                    :placeholder="t('ui.integrations.steam.searchGames')"
                  />
                </label>
                <label class="game-manager__filter">
                  <span class="vs-sr-only">{{ t('ui.integrations.steam.filterGames') }}</span>
                  <select v-model="steamGameFilter">
                    <option value="all">{{ t('ui.integrations.steam.filters.all') }}</option>
                    <option value="included">
                      {{ t('ui.integrations.steam.filters.included') }}
                    </option>
                    <option value="excluded">
                      {{ t('ui.integrations.steam.filters.excluded') }}
                    </option>
                  </select>
                </label>
              </div>
              <div class="game-manager__bulk">
                <span>{{
                  t('ui.integrations.steam.resultCount', { count: filteredSteamGames.length })
                }}</span>
                <div>
                  <AppButton
                    :label="t('ui.integrations.steam.includeResults')"
                    variant="tertiary"
                    size="compact"
                    :disabled="!filteredSteamGames.length"
                    @click="setVisibleDraftExclusions(false)"
                  />
                  <AppButton
                    :label="t('ui.integrations.steam.excludeResults')"
                    variant="tertiary"
                    size="compact"
                    :disabled="!filteredSteamGames.length"
                    @click="setVisibleDraftExclusions(true)"
                  />
                </div>
              </div>
              <p v-if="steamGamesLoading" class="game-manager__notice">
                {{ t('ui.integrations.steam.loadingGames') }}
              </p>
              <p
                v-else-if="steamGamesError"
                class="game-manager__notice game-manager__notice--error"
              >
                {{ steamGamesError }}
              </p>
              <p v-else-if="!steamGames.length" class="game-manager__notice">
                {{ t('ui.integrations.steam.noGames') }}
              </p>
              <p v-else-if="!filteredSteamGames.length" class="game-manager__notice">
                {{ t('ui.integrations.steam.noMatchingGames') }}
              </p>
              <div v-else class="game-manager__list">
                <div
                  v-for="game in filteredSteamGames"
                  :key="steamGameId(game)"
                  class="game-manager__game"
                >
                  <span class="game-manager__game-icon" aria-hidden="true"
                    ><UiIcon name="gamepad" :size="16"
                  /></span>
                  <span class="game-manager__game-copy">
                    <strong>{{ steamGameName(game) }}</strong>
                    <small>
                      {{ t('ui.integrations.steam.appId', { id: steamGameId(game) }) }}
                      <template v-if="game.filtered">
                        · {{ t('ui.integrations.steam.filteredTool') }}</template
                      >
                    </small>
                  </span>
                  <StatusBadge
                    :label="
                      gameExcluded(game)
                        ? t('ui.integrations.steam.excluded')
                        : t('ui.integrations.steam.included')
                    "
                    :tone="gameExcluded(game) ? 'neutral' : 'success'"
                    compact
                  />
                  <AppButton
                    :label="
                      gameExcluded(game)
                        ? t('ui.integrations.steam.include')
                        : t('ui.integrations.steam.exclude')
                    "
                    variant="tertiary"
                    size="compact"
                    @click="setDraftExclusion(game, !gameExcluded(game))"
                  />
                </div>
              </div>
              <div class="game-manager__footer">
                <span v-if="steamExclusionsDirty">{{ t('ui.integrations.unsavedChanges') }}</span>
                <span v-else>{{ t('ui.integrations.saved') }}</span>
                <div>
                  <AppButton
                    :label="t('_common.cancel')"
                    variant="tertiary"
                    size="compact"
                    :disabled="!steamExclusionsDirty"
                    @click="resetSteamExclusionDraft"
                  />
                  <AppButton
                    :label="t('_common.apply')"
                    variant="primary"
                    size="compact"
                    :busy="steamExclusionsSaving"
                    :busy-label="t('ui.integrations.applying')"
                    :disabled="!steamExclusionsDirty"
                    @click="saveSteamExclusions"
                  />
                </div>
              </div>
            </div>
          </section>

          <section
            v-else-if="summary.id === 'lutris'"
            class="integration-settings"
            aria-labelledby="lutris-policy-heading"
          >
            <div class="integration-settings__heading">
              <div>
                <h3 id="lutris-policy-heading">{{ t('ui.integrations.lutris.policyTitle') }}</h3>
                <small>{{ t('ui.integrations.lutris.policyDescription') }}</small>
              </div>
            </div>
            <div class="integration-settings__rows">
              <SettingRow :label="t('ui.integrations.lutris.autoSync')" :description="t('ui.integrations.lutris.autoSyncDescription')" control-id="lutris-auto-sync">
                <input id="lutris-auto-sync" class="integration-switch" type="checkbox" :checked="lutris?.auto_sync !== false" @change="setLutrisPolicy('lutris_auto_sync', ($event.target as HTMLInputElement).checked)" />
              </SettingRow>
              <SettingRow :label="t('ui.integrations.lutris.removeUninstalled')" :description="t('ui.integrations.lutris.removeUninstalledDescription')" control-id="lutris-remove-uninstalled">
                <input id="lutris-remove-uninstalled" class="integration-switch" type="checkbox" :checked="lutris?.autosync_remove_uninstalled !== false" @change="setLutrisPolicy('lutris_autosync_remove_uninstalled', ($event.target as HTMLInputElement).checked)" />
              </SettingRow>
              <details class="integration-advanced">
                <summary>{{ t('ui.integrations.lutris.advancedSettings') }}</summary>
                <SettingRow :label="t('ui.integrations.lutris.includeSteam')" :description="t('ui.integrations.lutris.includeSteamDescription')" control-id="lutris-include-steam">
                  <input id="lutris-include-steam" class="integration-switch" type="checkbox" :checked="lutris?.include_steam === true" @change="setLutrisPolicy('lutris_include_steam', ($event.target as HTMLInputElement).checked)" />
                </SettingRow>
              </details>
            </div>

            <div class="game-manager" aria-labelledby="lutris-exclusions-heading">
              <div class="game-manager__heading">
                <div>
                  <h4 id="lutris-exclusions-heading">{{ t('ui.integrations.lutris.exclusionsTitle') }}</h4>
                  <small>{{ t('ui.integrations.lutris.exclusionsDescription') }}</small>
                </div>
                <StatusBadge :label="t('ui.integrations.lutris.excludedCount', { count: lutrisExcludedCount })" :tone="lutrisExclusionsDirty ? 'warning' : 'neutral'" compact />
              </div>
              <div class="game-manager__toolbar">
                <label class="game-manager__search">
                  <span class="vs-sr-only">{{ t('ui.integrations.lutris.searchGames') }}</span>
                  <UiIcon name="search" :size="16" aria-hidden="true" />
                  <input v-model="lutrisGameSearch" type="search" :placeholder="t('ui.integrations.lutris.searchGames')" />
                </label>
                <label class="game-manager__filter">
                  <span class="vs-sr-only">{{ t('ui.integrations.lutris.filterGames') }}</span>
                  <select v-model="lutrisGameFilter">
                    <option value="all">{{ t('ui.integrations.steam.filters.all') }}</option>
                    <option value="included">{{ t('ui.integrations.steam.filters.included') }}</option>
                    <option value="excluded">{{ t('ui.integrations.steam.filters.excluded') }}</option>
                  </select>
                </label>
              </div>
              <div class="game-manager__bulk">
                <span>{{ t('ui.integrations.lutris.resultCount', { count: filteredLutrisGames.length }) }}</span>
                <div>
                  <AppButton :label="t('ui.integrations.steam.includeResults')" variant="tertiary" size="compact" :disabled="!filteredLutrisGames.length" @click="setVisibleLutrisDraftExclusions(false)" />
                  <AppButton :label="t('ui.integrations.steam.excludeResults')" variant="tertiary" size="compact" :disabled="!filteredLutrisGames.length" @click="setVisibleLutrisDraftExclusions(true)" />
                </div>
              </div>
              <p v-if="lutrisGamesLoading" class="game-manager__notice">{{ t('ui.integrations.lutris.loadingGames') }}</p>
              <p v-else-if="lutrisGamesError" class="game-manager__notice game-manager__notice--error">{{ lutrisGamesError }}</p>
              <p v-else-if="!lutrisGames.length" class="game-manager__notice">{{ t('ui.integrations.lutris.noGames') }}</p>
              <p v-else-if="!filteredLutrisGames.length" class="game-manager__notice">{{ t('ui.integrations.lutris.noMatchingGames') }}</p>
              <div v-else class="game-manager__list">
                <div v-for="game in filteredLutrisGames" :key="lutrisGameId(game)" class="game-manager__game">
                  <span class="game-manager__game-icon" aria-hidden="true"><UiIcon name="gamepad" :size="16" /></span>
                  <span class="game-manager__game-copy">
                    <strong>{{ lutrisGameName(game) }}</strong>
                    <small>{{ t('ui.integrations.lutris.gameMetadata', { id: lutrisGameId(game), runner: game.runner || game.platform || 'unknown' }) }}<template v-if="game.steam_backed"> · {{ t('ui.integrations.lutris.steamGame') }}</template></small>
                  </span>
                  <StatusBadge :label="lutrisGameExcluded(game) ? t('ui.integrations.steam.excluded') : t('ui.integrations.steam.included')" :tone="lutrisGameExcluded(game) ? 'neutral' : 'success'" compact />
                  <AppButton :label="lutrisGameExcluded(game) ? t('ui.integrations.steam.include') : t('ui.integrations.steam.exclude')" variant="tertiary" size="compact" @click="setLutrisDraftExclusion(game, !lutrisGameExcluded(game))" />
                </div>
              </div>
              <div class="game-manager__footer">
                <span v-if="lutrisExclusionsDirty">{{ t('ui.integrations.unsavedChanges') }}</span><span v-else>{{ t('ui.integrations.saved') }}</span>
                <div>
                  <AppButton :label="t('_common.cancel')" variant="tertiary" size="compact" :disabled="!lutrisExclusionsDirty" @click="resetLutrisExclusionDraft" />
                  <AppButton :label="t('_common.apply')" variant="primary" size="compact" :busy="lutrisExclusionsSaving" :busy-label="t('ui.integrations.applying')" :disabled="!lutrisExclusionsDirty" @click="saveLutrisExclusions" />
                </div>
              </div>
            </div>
          </section>

          <section
            v-else-if="summary.id === 'mangohud'"
            class="integration-settings"
            aria-labelledby="mangohud-settings-heading"
          >
            <div class="integration-settings__heading">
              <div>
                <h3 id="mangohud-settings-heading">
                  {{ t('ui.integrations.mangohud.settingsTitle') }}
                </h3>
                <small>{{ t('ui.integrations.mangohud.settingsDescription') }}</small>
              </div>
            </div>
            <div class="integration-settings__rows">
              <SettingRow
                :label="t('ui.integrations.mangohud.alwaysLimit')"
                :description="t('ui.integrations.mangohud.alwaysLimitDescription')"
                control-id="mangohud-enabled"
              >
                <input
                  id="mangohud-enabled"
                  v-model="mangoDraft.enabled"
                  class="integration-switch"
                  type="checkbox"
                />
              </SettingRow>
              <SettingRow
                :label="t('ui.integrations.mangohud.provider')"
                :description="t('ui.integrations.mangohud.providerDescription')"
                control-id="mangohud-provider"
              >
                <select
                  id="mangohud-provider"
                  v-model="mangoDraft.provider"
                  class="integration-control"
                >
                  <option value="auto">{{ t('ui.integrations.mangohud.providerAuto') }}</option>
                  <option value="mangohud">
                    {{ t('ui.integrations.mangohud.providerMangoHud') }}
                  </option>
                  <option value="none">{{ t('ui.integrations.mangohud.providerNone') }}</option>
                </select>
              </SettingRow>
              <SettingRow
                :label="t('ui.integrations.mangohud.fpsLimit')"
                :description="t('ui.integrations.mangohud.fpsLimitDescription')"
                control-id="mangohud-fps-limit"
              >
                <div class="integration-number">
                  <input
                    id="mangohud-fps-limit"
                    v-model.number="mangoDraft.fpsLimit"
                    class="integration-control"
                    type="number"
                    min="0"
                    max="1000"
                    step="0.001"
                  />
                  <span>FPS</span>
                </div>
              </SettingRow>
            </div>
            <div class="integration-settings__footer">
              <span v-if="mangoDirty">{{ t('ui.integrations.unsavedChanges') }}</span>
              <span v-else>{{ t('ui.integrations.saved') }}</span>
              <div>
                <AppButton
                  :label="t('_common.cancel')"
                  variant="tertiary"
                  size="compact"
                  :disabled="!mangoDirty"
                  @click="resetMangoDraft"
                />
                <AppButton
                  :label="t('_common.apply')"
                  variant="primary"
                  size="compact"
                  :busy="mangoSaving"
                  :busy-label="t('ui.integrations.applying')"
                  :disabled="!mangoDirty"
                  @click="saveMangoSettings"
                />
              </div>
            </div>
          </section>
        </div>
        <div v-if="summary.id === 'steam' || summary.id === 'lutris' || isWindows" class="integration-row__actions">
          <template v-if="summary.id === 'steam'">
            <AppButton
              v-if="!steam?.forced"
              :label="
                steam?.enabled === false
                  ? t('ui.integrations.actions.enable')
                  : t('ui.integrations.actions.disable')
              "
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
          <template v-else-if="summary.id === 'lutris'">
            <AppButton :label="lutris?.enabled === false ? t('ui.integrations.actions.enable') : t('ui.integrations.actions.disable')" variant="tertiary" size="compact" @click="setProviderEnabled('lutris', lutris?.enabled === false)" />
            <AppButton icon="refresh" :label="t('ui.integrations.actions.rescan')" variant="secondary" size="compact" :busy="syncing" :busy-label="t('ui.integrations.syncing')" :disabled="lutris?.enabled === false || lutris?.available === false" @click="syncLutris" />
          </template>
          <template v-else-if="summary.id === 'playnite'">
            <AppButton
              :label="
                playnite?.enabled === false
                  ? t('ui.integrations.actions.enable')
                  : t('ui.integrations.actions.disable')
              "
              variant="tertiary"
              size="compact"
              @click="setProviderEnabled('playnite', playnite?.enabled === false)"
            />
            <AppButton
              v-if="
                playnite?.enabled !== false &&
                (playnite?.installed !== true || playnite?.update_available)
              "
              :label="
                playnite?.update_available
                  ? t('ui.integrations.actions.update')
                  : t('ui.integrations.actions.install')
              "
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
            :label="
              vigem?.installed
                ? t('ui.integrations.actions.repair')
                : t('ui.integrations.actions.install')
            "
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

.integration-settings {
  display: grid;
  max-width: 52rem;
  gap: var(--vs-space-16);
  padding-top: var(--vs-space-16);
}

.integration-settings__heading,
.game-manager__heading,
.game-manager__footer,
.integration-settings__footer {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.integration-settings h3,
.integration-settings h4 {
  font-size: var(--vs-type-size-label);
}

.integration-settings small,
.game-manager small {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.integration-settings__rows {
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.integration-settings__rows :deep(.vs-setting-row) {
  padding: var(--vs-space-12);
}

.integration-settings__rows :deep(.vs-setting-row + .vs-setting-row) {
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.integration-switch {
  width: 1.15rem;
  height: 1.15rem;
  accent-color: var(--vs-color-accent);
}

.integration-advanced {
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.integration-advanced summary {
  padding: var(--vs-space-12);
  color: var(--vs-color-text-secondary);
  cursor: pointer;
  font-size: var(--vs-type-size-metadata);
  font-weight: 600;
}

.integration-advanced :deep(.vs-setting-row) {
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.integration-control,
.game-manager input,
.game-manager select {
  min-height: 2.25rem;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-primary);
  padding: var(--vs-space-4) var(--vs-space-8);
}

.integration-control:focus-visible,
.game-manager input:focus-visible,
.game-manager select:focus-visible,
.integration-advanced summary:focus-visible {
  outline: var(--vs-focus-width) solid var(--vs-color-focus);
  outline-offset: var(--vs-focus-offset);
}

.integration-number {
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
}

.integration-number input {
  width: 8rem;
}

.integration-number span {
  color: var(--vs-color-text-tertiary);
  font-size: var(--vs-type-size-metadata);
  font-weight: 600;
}

.game-manager {
  display: grid;
  overflow: hidden;
  gap: 0;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
}

.game-manager__heading {
  padding: var(--vs-space-12);
  background: var(--vs-color-bg-subtle);
}

.game-manager__heading > div,
.integration-settings__heading > div {
  display: grid;
  gap: var(--vs-space-2);
}

.game-manager__toolbar {
  display: grid;
  grid-template-columns: minmax(12rem, 1fr) auto;
  gap: var(--vs-space-8);
  padding: var(--vs-space-12);
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.game-manager__search {
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
  min-height: 2.25rem;
  padding-inline-start: var(--vs-space-8);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-text-tertiary);
}

.game-manager__search input {
  width: 100%;
  min-height: 2rem;
  padding-inline-start: 0;
  border: 0;
  outline: 0;
}

.game-manager__bulk {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-8);
  padding: var(--vs-space-4) var(--vs-space-12) var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.game-manager__bulk > div,
.game-manager__footer > div,
.integration-settings__footer > div {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-4);
}

.game-manager__list {
  display: grid;
  overflow-y: auto;
  max-height: 23rem;
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.game-manager__game {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr) auto auto;
  align-items: center;
  gap: var(--vs-space-8);
  min-height: 3.5rem;
  padding: var(--vs-space-8) var(--vs-space-12);
}

.game-manager__game + .game-manager__game {
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.game-manager__game-icon {
  display: grid;
  width: 2rem;
  height: 2rem;
  place-items: center;
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-text-secondary);
}

.game-manager__game-copy {
  display: grid;
  min-width: 0;
  gap: var(--vs-space-2);
}

.game-manager__game-copy strong,
.game-manager__game-copy small {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.game-manager__notice {
  padding: var(--vs-space-16);
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.game-manager__notice--error {
  color: var(--vs-color-status-danger);
}

.game-manager__footer,
.integration-settings__footer {
  align-items: center;
  padding: var(--vs-space-12);
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.integration-settings__footer {
  padding-inline: 0;
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

  .integration-settings {
    max-width: none;
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

  .game-manager__toolbar {
    grid-template-columns: minmax(0, 1fr);
  }

  .game-manager__filter select {
    width: 100%;
  }

  .game-manager__bulk,
  .game-manager__heading,
  .game-manager__footer,
  .integration-settings__footer {
    align-items: stretch;
    flex-direction: column;
  }

  .game-manager__bulk > div,
  .game-manager__footer > div,
  .integration-settings__footer > div {
    justify-content: flex-start;
  }

  .game-manager__game {
    grid-template-columns: auto minmax(0, 1fr) auto;
  }

  .game-manager__game > :deep(.vs-button) {
    grid-column: 2 / -1;
    justify-self: start;
  }
}

@media (forced-colors: active) {
  .integration-list,
  .integration-row__icon {
    border: var(--vs-border-width) solid CanvasText;
  }
}
</style>
