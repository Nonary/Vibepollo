<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import { storeToRefs } from 'pinia';
import { useI18n } from 'vue-i18n';
import { NAlert, NButton, NInput, NModal, NRadio, NRadioGroup } from 'naive-ui';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import ConfigSwitchField from '@/ConfigSwitchField.vue';
import NvidiaNvencEncoder from '@/configs/tabs/encoders/NvidiaNvencEncoder.vue';
import IntelQuickSyncEncoder from '@/configs/tabs/encoders/IntelQuickSyncEncoder.vue';
import AmdAmfEncoder from '@/configs/tabs/encoders/AmdAmfEncoder.vue';
import VideotoolboxEncoder from '@/configs/tabs/encoders/VideotoolboxEncoder.vue';
import SoftwareEncoder from '@/configs/tabs/encoders/SoftwareEncoder.vue';
import VAAPIEncoder from '@/configs/tabs/encoders/VAAPIEncoder.vue';
import VulkanEncoder from '@/configs/tabs/encoders/VulkanEncoder.vue';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';

const props = defineProps({
  currentTab: { type: String, default: '' },
});

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const { t } = useI18n();

// Fallback: if no currentTab provided, show all stacked (modern single page mode)
const showAll = () => !props.currentTab;

const platform = computed(() =>
  (metadata.value?.platform || config.value?.platform || '').toString().toLowerCase(),
);

const gpuList = computed(() => {
  const raw = (metadata.value as any)?.gpus;
  return Array.isArray(raw) ? raw : [];
});

const LOSSLESS_DEFAULT_PATH =
  'C:\\Program Files (x86)\\Steam\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe';

function normalizeWindowsPath(raw: string | null | undefined): string {
  if (!raw) return '';
  let value = String(raw).replace(/\//g, '\\').trim();
  if (!value) return '';
  let prefix = '';
  if (value.startsWith('\\\\?\\')) {
    prefix = '\\\\?\\';
    value = value.slice(4);
  } else if (value.startsWith('\\\\')) {
    prefix = '\\\\';
    value = value.slice(2);
  }
  value = value.replace(/\\{2,}/g, '\\');
  if (prefix === '\\\\' && value.startsWith('\\')) {
    value = value.slice(1);
  }
  return prefix + value;
}

const losslessStatus = ref<any | null>(null);
const losslessLoading = ref(false);
const losslessError = ref<string | null>(null);
const losslessBrowseVisible = ref(false);
const losslessBrowseSelection = ref('');
const losslessBrowsePath = ref('');
const losslessBrowseDirectory = ref('');
const losslessBrowseEntries = ref<Array<{ name: string; path: string; type: string }>>([]);
const losslessBrowseLoading = ref(false);
const losslessBrowseError = ref<string | null>(null);

const losslessResolvedPath = computed(() => {
  const raw = losslessStatus.value?.resolved_path;
  if (typeof raw !== 'string') return '';
  return normalizeWindowsPath(raw);
});

const losslessForceAdvanced = ref(false);

const hasNvidia = computed(() => {
  const metaFlag = (metadata.value as any)?.has_nvidia_gpu;
  if (typeof metaFlag === 'boolean') return metaFlag;
  if (gpuList.value.length) {
    return gpuList.value.some(
      (gpu: any) => Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0) === 0x10de,
    );
  }
  return true;
});

const hasIntel = computed(() => {
  const metaFlag = (metadata.value as any)?.has_intel_gpu;
  if (typeof metaFlag === 'boolean') return metaFlag;
  if (gpuList.value.length) {
    return gpuList.value.some(
      (gpu: any) => Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0) === 0x8086,
    );
  }
  return true;
});

const hasAmd = computed(() => {
  const metaFlag = (metadata.value as any)?.has_amd_gpu;
  if (typeof metaFlag === 'boolean') return metaFlag;
  if (gpuList.value.length) {
    return gpuList.value.some((gpu: any) => {
      const vendor = Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0);
      return vendor === 0x1002 || vendor === 0x1022;
    });
  }
  return true;
});

const losslessConfiguredPath = computed(() => (config.value as any)?.lossless_scaling_path ?? '');
const losslessHasConfiguredPath = computed(() => !!normalizeWindowsPath(losslessConfiguredPath.value));
const losslessLegacyAutoDetect = computed<boolean>({
  get: () => !!(config.value as any)?.lossless_scaling_legacy_auto_detect,
  set: (value) => {
    (config.value as any).lossless_scaling_legacy_auto_detect = !!value;
  },
});
const losslessSuggestedPath = computed(() => {
  if (losslessConfiguredPath.value) return normalizeWindowsPath(losslessConfiguredPath.value);
  const suggested = losslessStatus.value?.suggested_path as string | undefined;
  return normalizeWindowsPath(suggested) || LOSSLESS_DEFAULT_PATH;
});
const losslessCandidates = computed(() => {
  const raw = losslessStatus.value?.candidates;
  if (!Array.isArray(raw)) return [] as string[];
  return raw
    .map((item: unknown) => (typeof item === 'string' ? normalizeWindowsPath(item) : ''))
    .filter((item) => !!item);
});
const losslessCheckedIsDirectory = computed(() => !!losslessStatus.value?.checked_is_directory);
const losslessPathExists = computed(() => !!losslessStatus.value?.checked_exists);
const losslessDetected = computed(() => {
  if (!losslessStatus.value) return false;
  if (losslessError.value) return false;
  if (losslessStatus.value.checked_exists && !losslessCheckedIsDirectory.value) return true;
  if (losslessStatus.value.resolved_path) return true;
  if (losslessStatus.value.configured_exists && !losslessStatus.value.configured_is_directory) {
    return true;
  }
  if (losslessHasConfiguredPath.value) return false;
  if (losslessStatus.value.default_exists) return true;
  if (losslessCandidates.value.length > 0) return true;
  return false;
});
const showLosslessAdvanced = computed(() => !losslessDetected.value || losslessForceAdvanced.value);
const losslessStatusClass = computed(() => {
  if (losslessLoading.value) {
    return 'bg-primary/10 text-primary';
  }
  if (losslessDetected.value) {
    return 'bg-success/10 text-success';
  }
  return 'bg-warning/10 text-warning';
});
const losslessStatusIcon = computed(() =>
  losslessDetected.value ? 'fas fa-check-circle' : 'fas fa-exclamation-triangle',
);
const losslessDefaultPath = computed(() => {
  const raw = losslessStatus.value?.default_path;
  return typeof raw === 'string' ? normalizeWindowsPath(raw) : '';
});
const losslessActivePath = computed(() => {
  if (!losslessStatus.value) return '';
  if (losslessStatus.value.resolved_path) return losslessResolvedPath.value;
  if (
    losslessStatus.value.checked_exists &&
    typeof losslessStatus.value.checked_path === 'string' &&
    !losslessCheckedIsDirectory.value
  ) {
    return normalizeWindowsPath(losslessStatus.value.checked_path);
  }
  if (
    losslessStatus.value.configured_exists &&
    typeof losslessStatus.value.configured_path === 'string' &&
    !losslessStatus.value.configured_is_directory
  ) {
    return normalizeWindowsPath(losslessStatus.value.configured_path);
  }
  if (losslessHasConfiguredPath.value) {
    return normalizeWindowsPath(losslessConfiguredPath.value);
  }
  if (losslessStatus.value.default_exists && losslessDefaultPath.value) {
    return losslessDefaultPath.value;
  }
  if (losslessCandidates.value.length > 0) {
    return losslessCandidates.value[0];
  }
  return '';
});
const losslessApiStatusMessage = computed(() => {
  switch (losslessStatus.value?.status) {
    case 'unavailable':
      return t('config.lossless.status_query_failed');
    case 'path-is-directory':
      return t('config.lossless.browser_directory_warning');
    case 'path-not-found':
      return t('config.lossless.status_path_unresolved');
    case 'not-configured':
      return t('config.lossless.status_not_found');
    default:
      return '';
  }
});
const losslessStatusText = computed(() => {
  if (losslessLoading.value) {
    return t('config.lossless.status_checking');
  }
  if (losslessError.value) {
    return losslessError.value;
  }
  if (losslessDetected.value) {
    return t('config.lossless.status_ready');
  }
  if (losslessHasConfiguredPath.value) {
    return t('config.lossless.status_custom_path_attention');
  }
  return t('config.lossless.status_unavailable');
});
const losslessStatusHint = computed(() => {
  if (losslessLoading.value) {
    return '';
  }
  if (losslessError.value) {
    return '';
  }
  if (losslessDetected.value) {
    if (losslessHasConfiguredPath.value) {
      return t('config.lossless.status_configured_hint');
    }
    return t('config.lossless.status_detected_hint');
  }
  if (losslessHasConfiguredPath.value) {
    return losslessApiStatusMessage.value || t('config.lossless.status_path_unresolved');
  }
  return losslessApiStatusMessage.value || t('config.lossless.status_not_found');
});

async function refreshLosslessStatus() {
  if (platform.value !== 'windows') {
    return;
  }
  losslessLoading.value = true;
  losslessError.value = null;
  try {
    const params: Record<string, string> = {};
    if (losslessConfiguredPath.value) {
      params['path'] = normalizeWindowsPath(String(losslessConfiguredPath.value));
    }
    const response = await http.get('/api/lossless_scaling/status', {
      params,
      validateStatus: () => true,
    });
    if (response.status >= 200 && response.status < 300) {
      const payload = response.data ?? {};
      if (payload && typeof payload === 'object') {
        if (typeof payload.suggested_path === 'string') {
          payload.suggested_path = normalizeWindowsPath(payload.suggested_path);
        }
        if (typeof payload.resolved_path === 'string') {
          payload.resolved_path = normalizeWindowsPath(payload.resolved_path);
        }
        if (typeof payload.default_path === 'string') {
          payload.default_path = normalizeWindowsPath(payload.default_path);
        }
        if (typeof payload.configured_path === 'string') {
          payload.configured_path = normalizeWindowsPath(payload.configured_path);
        }
        if (typeof payload.checked_path === 'string') {
          payload.checked_path = normalizeWindowsPath(payload.checked_path);
        }
        if (Array.isArray(payload.candidates)) {
          payload.candidates = payload.candidates
            .map((item: unknown) => (typeof item === 'string' ? normalizeWindowsPath(item) : ''))
            .filter((item: string) => !!item);
        }
      }
      losslessStatus.value = payload;
      losslessError.value = null;
    } else {
      losslessError.value = t('config.lossless.status_query_failed');
      losslessStatus.value = null;
    }
  } catch (err) {
    losslessError.value = t('config.lossless.status_query_failed');
    losslessStatus.value = null;
  } finally {
    losslessLoading.value = false;
  }
}

function applyLosslessSuggestion() {
  if (!config.value) return;
  (config.value as any).lossless_scaling_path = losslessSuggestedPath.value;
}

function applyLosslessBrowseSelection() {
  if (!config.value) return;
  const selected = normalizeWindowsPath(losslessBrowsePath.value || losslessBrowseSelection.value);
  if (!selected) return;
  (config.value as any).lossless_scaling_path = selected;
  losslessBrowseVisible.value = false;
}

function showLosslessOverride() {
  losslessForceAdvanced.value = true;
}

function hideLosslessOverride() {
  losslessForceAdvanced.value = false;
}

async function openLosslessBrowse() {
  if (platform.value !== 'windows') return;
  if (!losslessStatus.value && !losslessLoading.value) {
    await refreshLosslessStatus();
  }
  const initial =
    normalizeWindowsPath(losslessConfiguredPath.value) ||
    losslessActivePath.value ||
    losslessCandidates.value[0] ||
    losslessSuggestedPath.value ||
    '';
  losslessBrowseSelection.value = initial;
  losslessBrowsePath.value = initial;
  await loadLosslessBrowseDirectory(initial);
  losslessBrowseVisible.value = true;
}

async function rescanLosslessCandidates() {
  await refreshLosslessStatus();
  const existing = normalizeWindowsPath(losslessBrowseSelection.value);
  if (existing) {
    losslessBrowseSelection.value = existing;
    return;
  }
  const first = losslessCandidates.value[0];
  if (first) {
    losslessBrowseSelection.value = first;
    losslessBrowsePath.value = first;
  }
  await loadLosslessBrowseDirectory(losslessBrowsePath.value);
}

async function loadLosslessBrowseDirectory(path: string) {
  losslessBrowseLoading.value = true;
  losslessBrowseError.value = null;
  try {
    const response = await http.get('/api/browse', {
      params: { type: 'executable', path: normalizeWindowsPath(path) },
      validateStatus: () => true,
    });
    if (response.status >= 200 && response.status < 300) {
      losslessBrowseDirectory.value = normalizeWindowsPath(response.data?.path ?? '');
      const entries = Array.isArray(response.data?.entries) ? response.data.entries : [];
      losslessBrowseEntries.value = entries
        .map((entry: any) => ({
          name: String(entry?.name ?? ''),
          path: normalizeWindowsPath(entry?.path ?? ''),
          type: String(entry?.type ?? ''),
        }))
        .filter(
          (entry: { name: string; path: string; type: string }) =>
            !!entry.name && !!entry.path,
        );
    } else {
      losslessBrowseError.value = t('config.lossless.browser_failed');
      losslessBrowseEntries.value = [];
    }
  } catch (err) {
    losslessBrowseError.value = t('config.lossless.browser_failed');
    losslessBrowseEntries.value = [];
  } finally {
    losslessBrowseLoading.value = false;
  }
}

async function selectLosslessBrowseEntry(entry: { path: string; type: string }) {
  const path = normalizeWindowsPath(entry.path);
  if (!path) return;
  if (entry.type === 'directory') {
    losslessBrowsePath.value = path;
    await loadLosslessBrowseDirectory(path);
    return;
  }
  losslessBrowseSelection.value = path;
  losslessBrowsePath.value = path;
}

onMounted(() => {
  if (platform.value === 'windows') {
    refreshLosslessStatus().catch(() => {});
  }
});

watch(
  () => losslessConfiguredPath.value,
  () => {
    if (platform.value === 'windows') {
      refreshLosslessStatus().catch(() => {});
    }
  },
);

watch(
  () => (config.value as any)?.lossless_scaling_path,
  (value) => {
    if (typeof value !== 'string') return;
    const normalized = normalizeWindowsPath(value);
    if (normalized !== value) {
      (config.value as any).lossless_scaling_path = normalized;
    }
  },
);

const shouldShowNvenc = computed(() => (showAll() || props.currentTab === 'nv') && hasNvidia.value);
const shouldShowQsv = computed(
  () => (showAll() || props.currentTab === 'qsv') && hasIntel.value && platform.value === 'windows',
);
const shouldShowAmd = computed(
  () => (showAll() || props.currentTab === 'amd') && hasAmd.value && platform.value === 'windows',
);
const shouldShowVideotoolbox = computed(
  () => (showAll() || props.currentTab === 'vt') && platform.value === 'macos',
);
const shouldShowVaapi = computed(
  () => (showAll() || props.currentTab === 'vaapi') && platform.value === 'linux',
);
const shouldShowVulkan = computed(
  () => (showAll() || props.currentTab === 'vulkan') && platform.value === 'linux',
);
const shouldShowSoftware = computed(() => showAll() || props.currentTab === 'sw');
</script>

<template>
  <div class="config-page space-y-6">
    <div class="space-y-4">
      <ConfigFieldRenderer setting-key="capture" v-model="config.capture" />
      <ConfigFieldRenderer setting-key="encoder" v-model="config.encoder" />
      <ConfigFieldRenderer setting-key="prefer_10bit_sdr" v-model="config.prefer_10bit_sdr" />

      <section
        v-if="platform === 'windows' && hasNvidia"
        class="space-y-4 rounded-xl border border-dark/35 p-4 dark:border-light/25"
      >
        <div class="space-y-1">
          <h3 class="text-sm font-medium">{{ $t('config.rtx_hdr_title') }}</h3>
          <p class="text-[11px] opacity-70">{{ $t('config.rtx_hdr_intro') }}</p>
        </div>
        <div class="grid gap-4">
          <div class="grid gap-3 md:grid-cols-2">
            <ConfigFieldRenderer
              setting-key="rtx_hdr_peak_brightness"
              v-model="config.rtx_hdr_peak_brightness"
              :desc="t('config.rtx_hdr_peak_brightness_desc')"
            />
            <ConfigFieldRenderer
              setting-key="rtx_hdr_sdr_brightness"
              v-model="config.rtx_hdr_sdr_brightness"
              :desc="t('config.rtx_hdr_sdr_brightness_desc')"
            />
            <ConfigFieldRenderer
              setting-key="rtx_hdr_middle_gray"
              v-model="config.rtx_hdr_middle_gray"
              :desc="t('config.rtx_hdr_middle_gray_desc')"
            />
            <ConfigFieldRenderer
              setting-key="rtx_hdr_contrast"
              v-model="config.rtx_hdr_contrast"
              :desc="t('config.rtx_hdr_contrast_desc')"
            />
            <ConfigFieldRenderer
              setting-key="rtx_hdr_saturation"
              v-model="config.rtx_hdr_saturation"
              :desc="t('config.rtx_hdr_saturation_desc')"
            />
          </div>
        </div>
      </section>

      <fieldset
        v-if="platform === 'windows'"
        class="space-y-4 rounded-xl border border-dark/35 p-4 dark:border-light/25"
      >
        <legend class="px-2 text-sm font-medium">{{ $t('apps.framegen.lossless_title') }}</legend>
        <div :class="['rounded-lg px-4 py-3 text-[12px]', losslessStatusClass]">
          <div class="flex items-center justify-between gap-3">
            <div class="flex items-center gap-2">
              <i :class="losslessStatusIcon" />
              <span class="font-medium leading-tight">{{ losslessStatusText }}</span>
            </div>
            <div class="flex items-center gap-2">
              <n-button
                size="tiny"
                type="default"
                strong
                :loading="losslessLoading"
                @click="refreshLosslessStatus"
              >
                <i class="fas fa-sync" />
                <span class="ml-1">{{ $t('config.lossless.check') }}</span>
              </n-button>
              <n-button
                v-if="losslessDetected && !losslessForceAdvanced"
                size="tiny"
                tertiary
                @click="showLosslessOverride"
              >
                {{ $t('config.lossless.override_path') }}
              </n-button>
              <n-button
                v-else-if="losslessDetected && losslessForceAdvanced"
                size="tiny"
                tertiary
                @click="hideLosslessOverride"
              >
                {{ $t('config.lossless.hide_override') }}
              </n-button>
            </div>
          </div>
          <p v-if="losslessStatusHint" class="mt-2 text-xs opacity-80">
            {{ losslessStatusHint }}
          </p>
          <p v-if="!losslessLoading && losslessActivePath" class="mt-1 text-xs opacity-70">
            {{ $t('config.lossless.using_path', { path: losslessActivePath }) }}
          </p>
        </div>

        <p class="mt-3 text-[11px] opacity-70">
          {{ $t('config.lossless.app_hint') }}
        </p>

        <div
          class="mt-3 rounded-lg bg-amber-50 dark:bg-amber-950/30 border border-amber-200 dark:border-amber-800 p-3"
        >
          <div class="flex items-start gap-2">
            <i
              class="fas fa-exclamation-triangle text-amber-600 dark:text-amber-400 flex-shrink-0 mt-0.5"
            />
            <ConfigSwitchField
              id="lossless_scaling_legacy_auto_detect"
              v-model="losslessLegacyAutoDetect"
              :label="$t('config.lossless_scaling_legacy_auto_detect_label')"
              :desc="$t('config.lossless_scaling_legacy_auto_detect_desc')"
              class="flex-1"
              size="small"
            />
          </div>
        </div>

        <div v-if="showLosslessAdvanced" class="space-y-2">
          <ConfigFieldRenderer
            setting-key="lossless_scaling_path"
            v-model="config.lossless_scaling_path"
            :label="$t('config.lossless_scaling_path')"
            desc=""
            :placeholder="LOSSLESS_DEFAULT_PATH"
            clearable
          >
            <template #actions>
              <div class="flex items-center gap-2 text-xs">
                <n-button size="tiny" tertiary @click="applyLosslessSuggestion">
                  {{ $t('config.lossless.use_suggested') }}
                </n-button>
                <n-button size="tiny" tertiary @click="openLosslessBrowse">
                  {{ $t('_common.browse') }}
                </n-button>
              </div>
            </template>
            {{ $t('config.lossless.default_installation', { path: LOSSLESS_DEFAULT_PATH }) }}
          </ConfigFieldRenderer>
        </div>
      </fieldset>
    </div>

    <div v-if="shouldShowNvenc" class="encoder-outline">
      <NvidiaNvencEncoder />
    </div>

    <div v-if="shouldShowQsv" class="encoder-outline">
      <IntelQuickSyncEncoder />
    </div>

    <AmdAmfEncoder v-if="shouldShowAmd" />
    <VideotoolboxEncoder v-if="shouldShowVideotoolbox" />
    <VAAPIEncoder v-if="shouldShowVaapi" />
    <VulkanEncoder v-if="shouldShowVulkan" />

    <div v-if="shouldShowSoftware" class="encoder-outline">
      <SoftwareEncoder />
    </div>

    <n-modal
      v-model:show="losslessBrowseVisible"
      preset="card"
      class="max-w-2xl"
      :title="$t('config.lossless.browser_title')"
    >
      <div class="space-y-4">
        <n-alert type="info" size="small" v-if="!losslessCandidates.length">
          {{ $t('config.lossless.browser_not_found') }}
        </n-alert>
        <div class="space-y-2">
          <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
            {{ $t('config.lossless.browser_path_label') }}
          </div>
          <n-input
            v-model:value="losslessBrowsePath"
            placeholder="C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling\LosslessScaling.exe"
            clearable
            @keyup.enter="applyLosslessBrowseSelection"
          />
        </div>
        <div v-if="losslessCandidates.length" class="space-y-2">
          <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
            {{ $t('config.lossless.browser_detected_installations') }}
          </div>
          <n-radio-group
            v-model:value="losslessBrowseSelection"
            class="space-y-2"
            @update:value="losslessBrowsePath = String($event ?? '')"
          >
            <div
              v-for="candidate in losslessCandidates"
              :key="candidate"
              class="rounded-md border border-dark/10 px-3 py-2 text-xs dark:border-light/10"
            >
              <n-radio :value="candidate">{{ candidate }}</n-radio>
            </div>
          </n-radio-group>
        </div>
        <div class="space-y-2">
          <div class="flex items-center justify-between gap-2">
            <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
              {{ $t('config.lossless.browser_executables') }}
            </div>
            <n-button
              size="tiny"
              tertiary
              :loading="losslessBrowseLoading"
              @click="loadLosslessBrowseDirectory(losslessBrowsePath)"
            >
              {{ $t('config.lossless.browser_open_folder') }}
            </n-button>
          </div>
          <div class="truncate text-xs opacity-60">
            {{ losslessBrowseDirectory || $t('config.lossless.browser_computer') }}
          </div>
          <n-alert v-if="losslessBrowseError" type="warning" size="small">
            {{ losslessBrowseError }}
          </n-alert>
          <div class="max-h-64 overflow-auto rounded-md border border-dark/10 dark:border-light/10">
            <button
              v-for="entry in losslessBrowseEntries"
              :key="entry.path"
              type="button"
              class="flex w-full items-center justify-between gap-3 border-b border-dark/5 px-3 py-2 text-left text-xs last:border-b-0 hover:bg-dark/5 dark:border-light/10 dark:hover:bg-light/10"
              @click="selectLosslessBrowseEntry(entry)"
            >
              <span class="min-w-0 truncate">{{ entry.name }}</span>
              <span class="shrink-0 opacity-60">
                {{
                  entry.type === 'directory'
                    ? $t('config.lossless.browser_folder')
                    : $t('config.lossless.browser_executable')
                }}
              </span>
            </button>
            <div
              v-if="!losslessBrowseLoading && !losslessBrowseEntries.length"
              class="px-3 py-3 text-xs opacity-60"
            >
              {{ $t('config.lossless.browser_empty') }}
            </div>
          </div>
        </div>
        <n-alert
          v-if="losslessCheckedIsDirectory && !losslessPathExists"
          type="warning"
          size="small"
        >
          {{ $t('config.lossless.browser_directory_warning') }}
        </n-alert>
        <div class="flex items-center justify-between pt-2">
          <n-button
            size="small"
            tertiary
            @click="rescanLosslessCandidates"
            :loading="losslessLoading"
          >
            {{ $t('config.lossless.rescan') }}
          </n-button>
          <div class="flex items-center gap-2">
            <n-button size="small" tertiary @click="losslessBrowseVisible = false">{{
              $t('_common.cancel')
            }}</n-button>
            <n-button
              size="small"
              type="primary"
              :disabled="!losslessBrowsePath && !losslessBrowseSelection"
              @click="applyLosslessBrowseSelection"
            >
              {{ $t('config.lossless.use_path') }}
            </n-button>
          </div>
        </div>
      </div>
    </n-modal>
  </div>
</template>

<style scoped>
.encoder-outline {
  @apply border border-dark/35 dark:border-light/25 rounded-xl p-4 bg-light/60 dark:bg-dark/40 space-y-4;
}
</style>
