<template>
  <section class="space-y-3">
    <div class="flex items-center justify-between gap-3">
      <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">Setting Overrides</h3>
      <n-button
        v-if="overrideKeys.length"
        size="small"
        tertiary
        aria-label="Clear all overrides"
        @click="clearAll"
      >
        Reset All
      </n-button>
    </div>

    <p class="text-[11px] opacity-70">{{ descriptionText }}</p>

    <div class="relative">
      <n-input
        v-model:value="searchQuery"
        type="text"
        placeholder="Search settings to override..."
        @focus="onSearchFocus"
        @blur="onSearchBlur"
        @keydown.enter.prevent="addFirstResult"
      >
        <template #suffix>
          <i class="fas fa-magnifying-glass text-[12px] opacity-60" />
        </template>
      </n-input>

      <transition name="fade">
        <div
          v-if="searchOpen"
          class="absolute mt-2 w-full max-w-full z-30 bg-light/95 dark:bg-surface/95 backdrop-blur rounded-md shadow-lg border border-dark/10 dark:border-light/10 max-h-80 overflow-auto overflow-x-hidden overscroll-contain scroll-stable pr-2 py-1"
        >
          <div v-if="searchResults.length === 0" class="px-3 py-2 text-[12px] opacity-60">
            No results
          </div>
          <n-button
            v-for="r in searchResults"
            :key="r.key"
            type="default"
            strong
            block
            class="justify-start !px-3 !py-2.5 !h-auto text-left leading-5 text-[13px] whitespace-normal"
            @click="addOverride(r.key)"
          >
            <div class="w-full max-w-full text-left flex items-start gap-2 py-0.5">
              <span class="shrink-0 mt-0.5">
                <i class="fas fa-sliders text-primary text-[11px]" />
              </span>
              <span class="min-w-0">
                <span class="block font-medium break-words whitespace-normal">
                  {{ r.label }}
                </span>
                <span class="block text-[11px] opacity-60 leading-5 break-words whitespace-normal">
                  {{ r.path }} · <span class="font-mono">{{ r.key }}</span>
                </span>
                <span
                  v-if="r.desc"
                  class="block text-[11px] opacity-70 break-words whitespace-normal leading-5"
                >
                  {{ r.desc }}
                </span>
              </span>
            </div>
          </n-button>
        </div>
      </transition>
    </div>

    <div v-if="overrideEntries.length === 0" class="text-[12px] opacity-60">No overrides.</div>

    <div v-else class="space-y-2">
      <div
        v-for="entry in overrideEntries"
        :key="entry.key"
        class="rounded-md border border-dark/10 dark:border-light/10 p-3"
      >
        <div class="flex items-start justify-between gap-3">
          <div class="min-w-0">
            <div class="text-sm font-medium flex items-center gap-2 min-w-0">
              <span class="truncate">{{ entry.label }}</span>
              <n-tag size="small" class="font-mono">{{ entry.key }}</n-tag>
            </div>
            <div class="text-[11px] opacity-60 truncate">{{ entry.path }}</div>
            <div v-if="entry.desc" class="text-[11px] opacity-70 mt-1">{{ entry.desc }}</div>
            <div v-if="!entry.synthetic" class="text-[11px] opacity-50 mt-1">
              Global:
              <span class="font-mono">{{ formatValueForKey(entry.key, entry.globalValue) }}</span>
            </div>
          </div>
          <div class="shrink-0 flex items-center gap-2">
            <n-button size="small" tertiary @click="removeOverride(entry.key)">Reset</n-button>
          </div>
        </div>

        <div class="mt-3">
          <template v-if="isSyntheticKey(entry.key)">
            <n-input
              v-if="entry.key === SYN_KEYS.configureDisplayResolution"
              :value="forcedResolution"
              placeholder="e.g. 1920x1080"
              class="font-mono"
              @update:value="(v) => setForcedResolution(String(v || ''))"
            />
            <n-input
              v-else-if="entry.key === SYN_KEYS.configureDisplayRefreshRate"
              :value="forcedRefreshRate"
              placeholder="e.g. 60"
              class="font-mono"
              @update:value="(v) => setForcedRefreshRate(String(v || ''))"
            />
            <n-select
              v-else-if="entry.key === SYN_KEYS.configureDisplayHdr"
              :value="forcedHdr"
              :options="forcedHdrOptions"
              @update:value="(v) => setForcedHdr(String(v || ''))"
            />
          </template>

          <template v-else-if="editorKind(entry.key) === 'boolean'">
            <n-checkbox
              :checked="boolChecked(entry.key)"
              @update:checked="(v) => setBool(entry.key, !!v)"
            >
              Enabled
            </n-checkbox>
          </template>

          <template v-else-if="editorKind(entry.key) === 'select'">
            <n-select
              :value="selectValue(entry.key)"
              :options="selectOptions(entry.key)"
              filterable
              @update:value="(v) => setSelect(entry.key, v)"
            />
          </template>

          <template v-else-if="editorKind(entry.key) === 'number'">
            <n-input-number
              :value="numberValue(entry.key)"
              placeholder="(number)"
              @update:value="(v) => setNumber(entry.key, v)"
            />
          </template>

          <template v-else-if="editorKind(entry.key) === 'string'">
            <n-input
              :value="stringValue(entry.key)"
              placeholder="(value)"
              class="font-mono"
              @update:value="(v) => setString(entry.key, v)"
            />
          </template>

          <template v-else>
            <n-input
              :value="jsonDraft(entry.key)"
              type="textarea"
              :autosize="{ minRows: 2, maxRows: 10 }"
              class="font-mono"
              placeholder="JSON value"
              @update:value="(v) => updateJsonDraft(entry.key, v)"
              @blur="() => commitJson(entry.key)"
            />
            <div v-if="jsonError(entry.key)" class="text-[11px] text-danger mt-1">
              {{ jsonError(entry.key) }}
            </div>
          </template>
        </div>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { NButton, NCheckbox, NInput, NInputNumber, NSelect, NTag } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import {
  buildOverrideOptionsText,
  getOverrideSelectOptions,
  type OverrideSelectOption,
} from './configOverrideOptions';

type Entry = {
  key: string;
  label: string;
  desc: string;
  path: string;
  groupId: string;
  groupName: string;
  synthetic?: boolean;
  globalValue: unknown;
  options: OverrideSelectOption[];
  optionsText: string;
};

const overrides = defineModel<Record<string, unknown>>('overrides', { required: true });
const { t } = useI18n();

const props = withDefaults(
  defineProps<{
    scopeLabel?: string;
    description?: string;
  }>(),
  {
    scopeLabel: 'application',
    description: '',
  },
);

const descriptionText = computed(() => {
  if (props.description) return props.description;
  const scope = String(props.scopeLabel || 'application').toLowerCase().trim();
  if (scope === 'client') {
    return 'Override global settings for this client. Client overrides take precedence over app overrides and global config.';
  }
  return 'Override global settings for this application only. Network, security, and file-path settings are intentionally excluded.';
});

const configStore = useConfigStore();
const configRef = (configStore as any).config;
const tabsRef = (configStore as any).tabs;
const metadataRef = (configStore as any).metadata;

const DD_KEYS = {
  configurationOption: 'dd_configuration_option',
  resolutionOption: 'dd_resolution_option',
  manualResolution: 'dd_manual_resolution',
  refreshRateOption: 'dd_refresh_rate_option',
  manualRefreshRate: 'dd_manual_refresh_rate',
  hdrOption: 'dd_hdr_option',
  hdrRequestOverride: 'dd_hdr_request_override',
} as const;

function isHiddenOverrideKey(key: string): boolean {
  return key.startsWith('dd_');
}

function getConfigState(): any {
  return (configRef as any)?.value ?? configRef;
}

function getTabsState(): any[] {
  const v = (tabsRef as any)?.value ?? tabsRef;
  return Array.isArray(v) ? v : [];
}

function getMetadataState(): any {
  return (metadataRef as any)?.value ?? metadataRef;
}

function platformKey(): string {
  try {
    const meta = getMetadataState();
    const cfg = getConfigState();
    return String(meta?.platform ?? cfg?.platform ?? '')
      .toLowerCase()
      .trim();
  } catch {
    return '';
  }
}

const DISALLOWED_KEYS = new Set<string>([
  // Network / auth / security / identity
  'flags',
  'port',
  'address_family',
  'upnp',
  'origin_web_ui_allowed',
  'external_ip',
  'lan_encryption_mode',
  'wan_encryption_mode',
  'ping_timeout',
  'fec_percentage',
  'pkey',
  'cert',

  // Redundant with per-app display overrides in the app editor
  'virtual_display_mode',
  'virtual_display_layout',

  'file_apps',
  'credentials_file',
  'log_path',
  'file_state',
  'vibeshine_file_state',
  'sunshine_name',
  'locale',
  'min_log_level',
  'notify_pre_releases',
  'system_tray',
  'update_check_interval',
  'session_token_ttl_seconds',
  'remember_me_refresh_token_ttl_seconds',
  'global_prep_cmd',

  // Playnite sync/catalog settings (global)
  'playnite_auto_sync',
  'playnite_sync_all_installed',
  'playnite_recent_games',
  'playnite_recent_max_age_days',
  'playnite_autosync_delete_after_days',
  'playnite_autosync_require_replacement',
  'playnite_autosync_remove_uninstalled',
  'playnite_sync_categories',
  'playnite_sync_plugins',
  'playnite_exclude_categories',
  'playnite_exclude_plugins',
  'playnite_exclude_games',
  'playnite_fullscreen_entry_enabled',
  'playnite_install_dir',
  'playnite_extensions_dir',
]);

function isAllowedKey(key: string): boolean {
  if (!key) return false;
  if (DISALLOWED_KEYS.has(key)) return false;
  return true;
}

function prettifyKey(key: string): string {
  return key
    .split('_')
    .filter(Boolean)
    .map((p) => p.charAt(0).toUpperCase() + p.slice(1))
    .join(' ');
}

function labelFor(key: string): string {
  const k = `config.${key}`;
  const v = t(k);
  if (!v || v === k) return prettifyKey(key);
  return v;
}

function descFor(key: string): string {
  const k = `config.${key}_desc`;
  const v = t(k);
  if (!v || v === k) return '';
  return v;
}

function cloneValue(v: unknown): unknown {
  if (v === null || v === undefined) return v;
  if (typeof v !== 'object') return v;
  try {
    return JSON.parse(JSON.stringify(v));
  } catch {
    return v;
  }
}

function getGlobalValue(key: string): unknown {
  try {
    const state = getConfigState();
    const cur = state?.[key];
    if (cur !== undefined) return cur;
    return (configStore as any)?.defaults?.[key];
  } catch {
    return undefined;
  }
}

function ensureOverridesObject(): void {
  if (!overrides.value || typeof overrides.value !== 'object' || Array.isArray(overrides.value)) {
    overrides.value = {};
  }
}

function setOverrideKey(key: string, value: unknown): void {
  ensureOverridesObject();
  (overrides.value as any)[key] = value;
}

function clearOverrideKey(key: string): void {
  ensureOverridesObject();
  try {
    delete (overrides.value as any)[key];
  } catch {}
  clearJsonState(key);
}

const overrideKeys = computed<string[]>(() => {
  const o = overrides.value;
  if (!o || typeof o !== 'object' || Array.isArray(o)) return [];
  return Object.keys(o).filter((k) => typeof k === 'string' && k.length > 0);
});

const visibleOverrideKeys = computed<string[]>(() => overrideKeys.value.filter((k) => !isHiddenOverrideKey(k)));

const SYN_KEYS = {
  configureDisplayResolution: 'configure_display_resolution',
  configureDisplayRefreshRate: 'configure_display_refresh_rate',
  configureDisplayHdr: 'configure_display_hdr',
} as const;

const SYNTHETIC_KEYS = new Set<string>(Object.values(SYN_KEYS));

function isSyntheticKey(key: string): boolean {
  return SYNTHETIC_KEYS.has(key);
}

function isWindowsPlatform(): boolean {
  return platformKey() === 'windows';
}

function getOverrideString(key: string): string | null {
  const o = overrides.value as any;
  if (!o || typeof o !== 'object' || Array.isArray(o)) return null;
  const v = o[key];
  if (v === undefined || v === null) return null;
  return String(v);
}

function globalDdConfigDisabled(): boolean {
  const gv = getGlobalValue(DD_KEYS.configurationOption);
  return String(gv ?? 'disabled') === 'disabled';
}

function ensureDdEnabledForDisplayOverrides(): void {
  if (!globalDdConfigDisabled()) return;
  const cur = getOverrideString(DD_KEYS.configurationOption);
  if (!cur || cur === 'disabled') {
    setOverrideKey(DD_KEYS.configurationOption, 'verify_only');
  }
}

function cleanupDdConfigurationOptionIfUnused(): void {
  if (!globalDdConfigDisabled()) return;
  const o = overrides.value as any;
  if (!o || typeof o !== 'object' || Array.isArray(o)) return;
  const ddKeys = Object.keys(o).filter((k) => k.startsWith('dd_'));
  const hasOtherDdKeys = ddKeys.some((k) => k !== DD_KEYS.configurationOption);
  if (!hasOtherDdKeys && o[DD_KEYS.configurationOption] === 'verify_only') {
    clearOverrideKey(DD_KEYS.configurationOption);
  }
}

function isForcedResolutionActive(): boolean {
  if (!isWindowsPlatform()) return false;
  const opt = getOverrideString(DD_KEYS.resolutionOption);
  if (opt === 'manual') return true;
  const o = overrides.value as any;
  return !!o && typeof o === 'object' && !Array.isArray(o) && o[DD_KEYS.manualResolution] !== undefined;
}

function isForcedRefreshRateActive(): boolean {
  if (!isWindowsPlatform()) return false;
  const opt = getOverrideString(DD_KEYS.refreshRateOption);
  if (opt === 'manual') return true;
  const o = overrides.value as any;
  return !!o && typeof o === 'object' && !Array.isArray(o) && o[DD_KEYS.manualRefreshRate] !== undefined;
}

function isForcedHdrActive(): boolean {
  if (!isWindowsPlatform()) return false;
  const req = getOverrideString(DD_KEYS.hdrRequestOverride);
  return req === 'force_on' || req === 'force_off';
}

const forcedResolution = computed<string>(() => getOverrideString(DD_KEYS.manualResolution) ?? '');
const forcedRefreshRate = computed<string>(() => getOverrideString(DD_KEYS.manualRefreshRate) ?? '');

const forcedHdrOptions = [
  { label: 'On', value: 'on' },
  { label: 'Off', value: 'off' },
];

const forcedHdr = computed<'on' | 'off'>(() => {
  const req = getOverrideString(DD_KEYS.hdrRequestOverride);
  return req === 'force_off' ? 'off' : 'on';
});

function setForcedResolution(value: string): void {
  if (!isWindowsPlatform()) return;
  ensureDdEnabledForDisplayOverrides();
  setOverrideKey(DD_KEYS.resolutionOption, 'manual');
  setOverrideKey(DD_KEYS.manualResolution, String(value ?? ''));
}

function clearForcedResolution(): void {
  clearOverrideKey(DD_KEYS.resolutionOption);
  clearOverrideKey(DD_KEYS.manualResolution);
  cleanupDdConfigurationOptionIfUnused();
}

function setForcedRefreshRate(value: string): void {
  if (!isWindowsPlatform()) return;
  ensureDdEnabledForDisplayOverrides();
  setOverrideKey(DD_KEYS.refreshRateOption, 'manual');
  setOverrideKey(DD_KEYS.manualRefreshRate, String(value ?? ''));
}

function clearForcedRefreshRate(): void {
  clearOverrideKey(DD_KEYS.refreshRateOption);
  clearOverrideKey(DD_KEYS.manualRefreshRate);
  cleanupDdConfigurationOptionIfUnused();
}

function setForcedHdr(value: string): void {
  if (!isWindowsPlatform()) return;
  ensureDdEnabledForDisplayOverrides();
  setOverrideKey(DD_KEYS.hdrOption, 'auto');
  setOverrideKey(DD_KEYS.hdrRequestOverride, value === 'off' ? 'force_off' : 'force_on');
}

function clearForcedHdr(): void {
  clearOverrideKey(DD_KEYS.hdrRequestOverride);
  clearOverrideKey(DD_KEYS.hdrOption);
  cleanupDdConfigurationOptionIfUnused();
}

const activeSyntheticKeys = computed<string[]>(() => {
  const keys: string[] = [];
  if (isForcedResolutionActive()) keys.push(SYN_KEYS.configureDisplayResolution);
  if (isForcedRefreshRateActive()) keys.push(SYN_KEYS.configureDisplayRefreshRate);
  if (isForcedHdrActive()) keys.push(SYN_KEYS.configureDisplayHdr);
  return keys;
});

function addSyntheticOverride(key: string): void {
  if (!isWindowsPlatform()) return;
  if (key === SYN_KEYS.configureDisplayResolution) {
    setForcedResolution(forcedResolution.value);
  } else if (key === SYN_KEYS.configureDisplayRefreshRate) {
    setForcedRefreshRate(forcedRefreshRate.value);
  } else if (key === SYN_KEYS.configureDisplayHdr) {
    setForcedHdr(forcedHdr.value);
  }
}

function removeSyntheticOverride(key: string): void {
  if (key === SYN_KEYS.configureDisplayResolution) {
    clearForcedResolution();
  } else if (key === SYN_KEYS.configureDisplayRefreshRate) {
    clearForcedRefreshRate();
  } else if (key === SYN_KEYS.configureDisplayHdr) {
    clearForcedHdr();
  }
}

const allEntries = computed<Entry[]>(() => {
  const out: Entry[] = [];
  const tabList = getTabsState();
  const platform = platformKey();
  for (const tab of tabList) {
    const groupId = String((tab as any)?.id ?? '');
    const groupName = String((tab as any)?.name ?? groupId);
    const options = (tab as any)?.options ?? {};
    if (!options || typeof options !== 'object') continue;
    for (const key of Object.keys(options)) {
      if (!isAllowedKey(key)) continue;
      const globalValue = getGlobalValue(key);
      const selectOptions = getOverrideSelectOptions(key, {
        t,
        platform,
        metadata: getMetadataState(),
        currentValue: globalValue,
      });
      out.push({
        key,
        label: labelFor(key),
        desc: descFor(key),
        path: `${groupName} > ${labelFor(key)}`,
        groupId,
        groupName,
        globalValue,
        options: selectOptions,
        optionsText: buildOverrideOptionsText(selectOptions),
      });
    }
  }

  if (platform === 'windows') {
    const groupId = 'display';
    const groupName = 'Display';
    out.push(
      {
        key: SYN_KEYS.configureDisplayResolution,
        label: 'Configure Resolution',
        desc: 'Configure a specific display resolution during streams (uses display automation behind the scenes).',
        path: `${groupName} > Configure Resolution`,
        groupId,
        groupName,
        synthetic: true,
        globalValue: undefined,
        options: [],
        optionsText: '',
      },
      {
        key: SYN_KEYS.configureDisplayRefreshRate,
        label: 'Configure Refresh Rate',
        desc: 'Configure a specific display refresh rate during streams (uses display automation behind the scenes).',
        path: `${groupName} > Configure Refresh Rate`,
        groupId,
        groupName,
        synthetic: true,
        globalValue: undefined,
        options: [],
        optionsText: '',
      },
      {
        key: SYN_KEYS.configureDisplayHdr,
        label: 'Configure HDR',
        desc: 'Configure HDR on or off during streams (uses display automation behind the scenes).',
        path: `${groupName} > Configure HDR`,
        groupId,
        groupName,
        synthetic: true,
        globalValue: undefined,
        options: forcedHdrOptions as any,
        optionsText: buildOverrideOptionsText(forcedHdrOptions as any),
      },
    );
  }
  return out;
});

const searchQuery = ref('');
const searchOpen = ref(false);

const searchResults = computed<Entry[]>(() => {
  const v = (searchQuery.value || '').trim().toLowerCase();
  const terms = v.split(/\s+/).filter(Boolean);
  if (!terms.length) return [];

  const used = new Set([...visibleOverrideKeys.value, ...activeSyntheticKeys.value]);
  const scoreFor = (it: Entry): number => {
    const lv = it.label.toLowerCase();
    const kv = it.key.toLowerCase();
    const pv = it.path.toLowerCase();
    const dv = (it.desc || '').toLowerCase();
    const ov = (it.optionsText || '').toLowerCase();
    let total = 0;
    for (const term of terms) {
      let s = 0;
      if (lv.includes(term)) {
        s += 100 - lv.indexOf(term);
        if (lv.startsWith(term)) s += 40;
      } else if (kv.includes(term)) {
        s += 80 - kv.indexOf(term);
        if (kv.startsWith(term)) s += 30;
      } else if (ov.includes(term)) {
        s += 60 - ov.indexOf(term) / 10;
      } else if (pv.includes(term)) {
        s += 50 - pv.indexOf(term) / 50;
      } else if (dv.includes(term)) {
        s += 20 - dv.indexOf(term) / 200;
      } else {
        return 0;
      }
      total += s;
    }
    total -= (pv.length + dv.length + ov.length) / 1500;
    return total;
  };

  return allEntries.value
    .filter((e) => !used.has(e.key) && !isHiddenOverrideKey(e.key))
    .map((it) => ({ it, s: scoreFor(it) }))
    .filter((x) => x.s > 0)
    .sort((a, b) => b.s - a.s)
    .slice(0, 15)
    .map((x) => x.it);
});

function onSearchFocus() {
  searchOpen.value = (searchQuery.value || '').trim().length > 0;
}
function onSearchBlur() {
  setTimeout(() => {
    searchOpen.value = false;
  }, 120);
}

watch(searchQuery, (q) => {
  searchOpen.value = (q || '').trim().length > 0;
});

function addFirstResult() {
  if (searchResults.value.length) {
    addOverride(searchResults.value[0].key);
  }
}

function addOverride(key: string) {
  if (!isAllowedKey(key)) return;
  if (isHiddenOverrideKey(key)) return;
  if (isSyntheticKey(key)) {
    addSyntheticOverride(key);
    searchQuery.value = '';
    searchOpen.value = false;
    return;
  }
  ensureOverridesObject();
  if ((overrides.value as any)[key] !== undefined) return;
  const current = getGlobalValue(key);
  (overrides.value as any)[key] = cloneValue(current);
  searchQuery.value = '';
  searchOpen.value = false;
}

function removeOverride(key: string) {
  if (isSyntheticKey(key)) {
    removeSyntheticOverride(key);
    return;
  }
  ensureOverridesObject();
  try {
    delete (overrides.value as any)[key];
  } catch {}
  clearJsonState(key);
}

function clearAll() {
  overrides.value = {};
  jsonDrafts.value = {};
  jsonErrors.value = {};
}

const overrideEntries = computed<Entry[]>(() => {
  const keys = Array.from(new Set([...visibleOverrideKeys.value, ...activeSyntheticKeys.value]));
  const byKey = new Map(allEntries.value.map((e) => [e.key, e] as const));
  return keys
    .map((k) => {
      const base = byKey.get(k);
      return {
        key: k,
        label: base?.label ?? prettifyKey(k),
        desc: base?.desc ?? '',
        path: base?.path ?? k,
        groupId: base?.groupId ?? 'unknown',
        groupName: base?.groupName ?? 'Unknown',
        synthetic: base?.synthetic,
        globalValue: base?.globalValue,
        options: base?.options ?? [],
        optionsText: base?.optionsText ?? '',
      } as Entry;
    })
    .sort((a, b) => a.path.localeCompare(b.path));
});

function formatValue(v: unknown): string {
  if (v === null) return 'null';
  if (v === undefined) return '-';
  if (typeof v === 'string') return v.length > 120 ? `${v.slice(0, 117)}...` : v;
  try {
    const s = JSON.stringify(v);
    return s.length > 120 ? `${s.slice(0, 117)}...` : s;
  } catch {
    return String(v);
  }
}

function formatValueForKey(key: string, value: unknown): string {
  const options = getOverrideSelectOptions(key, {
    t,
    platform: platformKey(),
    metadata: getMetadataState(),
    currentValue: value,
  });
  if (options.length) {
    const found = options.find((o) => o.value === (value as any));
    if (found) {
      const raw = String(found.value ?? '');
      if (raw === '') return found.label || raw;
      if (found.label && found.label !== raw) return `${found.label} (${raw})`;
      return raw;
    }
  }
  return formatValue(value);
}

// --- Editors ---------------------------------------------------------------

type BoolPair = { truthy: any; falsy: any; truthyNorm?: string; falsyNorm?: string };
const BOOL_STRING_PAIRS = [
  ['enabled', 'disabled'],
  ['enable', 'disable'],
  ['yes', 'no'],
  ['on', 'off'],
  ['true', 'false'],
  ['1', '0'],
] as const;

const NUMERIC_OVERRIDE_KEYS = new Set<string>(['frame_limiter_fps_limit']);

function boolPairFromValue(value: unknown): BoolPair | null {
  if (value === true || value === false) return { truthy: true, falsy: false };
  if (value === 1 || value === 0) return { truthy: 1, falsy: 0 };
  if (typeof value !== 'string') return null;
  const norm = value.toLowerCase().trim();
  for (const [t, f] of BOOL_STRING_PAIRS) {
    if (norm === t || norm === f) {
      return { truthy: t, falsy: f, truthyNorm: t, falsyNorm: f };
    }
  }
  return null;
}

function selectOptions(key: string): OverrideSelectOption[] {
  const cur = (overrides.value as any)?.[key];
  const global = getGlobalValue(key);
  const currentValue = cur !== undefined ? cur : global;
  return getOverrideSelectOptions(key, {
    t,
    platform: platformKey(),
    metadata: getMetadataState(),
    currentValue,
  });
}

function selectValue(key: string): string | number {
  const cur = (overrides.value as any)?.[key];
  if (typeof cur === 'string' || (typeof cur === 'number' && Number.isFinite(cur))) return cur;
  const gv = getGlobalValue(key);
  if (typeof gv === 'string' || (typeof gv === 'number' && Number.isFinite(gv))) return gv;
  const opts = selectOptions(key);
  return opts.length ? opts[0].value : '';
}

function setSelect(key: string, value: unknown): void {
  ensureOverridesObject();
  if (typeof value === 'string' || (typeof value === 'number' && Number.isFinite(value))) {
    (overrides.value as any)[key] = value;
  }
}

function editorKind(key: string): 'boolean' | 'select' | 'number' | 'string' | 'json' {
  const opts = selectOptions(key);
  if (opts && opts.length) return 'select';

  const gv = getGlobalValue(key);
  if (NUMERIC_OVERRIDE_KEYS.has(key)) return 'number';
  if (boolPairFromValue(gv)) return 'boolean';
  if (typeof gv === 'number') return 'number';
  if (typeof gv === 'string') return 'string';
  if (gv && typeof gv === 'object') return 'json';

  const ov = (overrides.value as any)?.[key];
  if (boolPairFromValue(ov)) return 'boolean';
  if (typeof ov === 'number') return 'number';
  if (typeof ov === 'string') return 'string';
  if (ov && typeof ov === 'object') return 'json';
  return 'string';
}

function boolChecked(key: string): boolean {
  const pair =
    boolPairFromValue(getGlobalValue(key)) ??
    boolPairFromValue((overrides.value as any)?.[key]);
  const cur = (overrides.value as any)?.[key];
  if (!pair) return !!cur;
  if (cur === pair.truthy) return true;
  if (cur === pair.falsy) return false;
  if (typeof cur === 'boolean') return cur;
  if (typeof cur === 'number') return cur !== 0;
  if (typeof cur === 'string') {
    const norm = cur.toLowerCase().trim();
    if (pair.truthyNorm && norm === pair.truthyNorm) return true;
    if (pair.falsyNorm && norm === pair.falsyNorm) return false;
    if (norm === String(pair.truthy)) return true;
    if (norm === String(pair.falsy)) return false;
  }
  return false;
}

function setBool(key: string, checked: boolean): void {
  ensureOverridesObject();
  const pair = boolPairFromValue(getGlobalValue(key)) ??
    boolPairFromValue((overrides.value as any)?.[key]) ?? {
      truthy: true,
      falsy: false,
    };
  (overrides.value as any)[key] = checked ? pair.truthy : pair.falsy;
}

function numberValue(key: string): number | null {
  const cur = (overrides.value as any)?.[key];
  if (typeof cur === 'number' && Number.isFinite(cur)) return cur;
  if (typeof cur === 'string') {
    const n = Number(cur);
    if (Number.isFinite(n)) return n;
  }
  const gv = getGlobalValue(key);
  if (typeof gv === 'number' && Number.isFinite(gv)) return gv;
  return null;
}

function setNumber(key: string, value: number | null): void {
  if (value === null || value === undefined) {
    removeOverride(key);
    return;
  }
  ensureOverridesObject();
  (overrides.value as any)[key] = value;
}

function stringValue(key: string): string {
  const cur = (overrides.value as any)?.[key];
  if (cur === null || cur === undefined) return '';
  if (typeof cur === 'string') return cur;
  try {
    return JSON.stringify(cur);
  } catch {
    return String(cur);
  }
}

function setString(key: string, value: string): void {
  ensureOverridesObject();
  (overrides.value as any)[key] = String(value ?? '');
}

const jsonDrafts = ref<Record<string, string>>({});
const jsonErrors = ref<Record<string, string>>({});

function clearJsonState(key: string) {
  const d = { ...jsonDrafts.value };
  const e = { ...jsonErrors.value };
  delete d[key];
  delete e[key];
  jsonDrafts.value = d;
  jsonErrors.value = e;
}

function jsonDraft(key: string): string {
  if (Object.prototype.hasOwnProperty.call(jsonDrafts.value, key)) {
    return jsonDrafts.value[key] ?? '';
  }
  const cur = (overrides.value as any)?.[key];
  let text = '';
  try {
    text = JSON.stringify(cur, null, 2);
  } catch {
    text = String(cur ?? '');
  }
  jsonDrafts.value = { ...jsonDrafts.value, [key]: text };
  return text;
}

function updateJsonDraft(key: string, value: string) {
  jsonDrafts.value = { ...jsonDrafts.value, [key]: String(value ?? '') };
}

function jsonError(key: string): string {
  return jsonErrors.value[key] || '';
}

function commitJson(key: string) {
  const raw = (jsonDrafts.value[key] ?? '').trim();
  if (!raw) {
    removeOverride(key);
    jsonErrors.value = { ...jsonErrors.value, [key]: '' };
    return;
  }
  try {
    const parsed = JSON.parse(raw);
    ensureOverridesObject();
    (overrides.value as any)[key] = parsed;
    jsonErrors.value = { ...jsonErrors.value, [key]: '' };
  } catch (e: any) {
    jsonErrors.value = {
      ...jsonErrors.value,
      [key]: e?.message ? String(e.message) : 'Invalid JSON',
    };
  }
}
</script>

<style scoped>
.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.25s;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>
