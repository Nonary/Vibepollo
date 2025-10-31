<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import { storeToRefs } from 'pinia';
import { useI18n } from 'vue-i18n';
import { NAlert, NButton, NInput, NModal, NRadio, NRadioGroup, NSelect } from 'naive-ui';
import NvidiaNvencEncoder from '@/configs/tabs/encoders/NvidiaNvencEncoder.vue';
import IntelQuickSyncEncoder from '@/configs/tabs/encoders/IntelQuickSyncEncoder.vue';
import AmdAmfEncoder from '@/configs/tabs/encoders/AmdAmfEncoder.vue';
import VideotoolboxEncoder from '@/configs/tabs/encoders/VideotoolboxEncoder.vue';
import SoftwareEncoder from '@/configs/tabs/encoders/SoftwareEncoder.vue';
import VAAPIEncoder from '@/configs/tabs/encoders/VAAPIEncoder.vue';
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

const losslessResolvedPath = computed(() => {
  const raw = losslessStatus.value?.resolved_path;
  if (typeof raw !== 'string') return '';
  return normalizeWindowsPath(raw);
});

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

const captureOptions = computed(() => {
  const opts = [{ label: t('_common.autodetect'), value: '' }];
  switch (platform.value) {
    case 'windows':
      opts.push(
        { label: 'Windows Graphics Capture (variable)', value: 'wgc' },
        { label: 'Windows Graphics Capture (constant)', value: 'wgcc' },
        { label: 'Desktop Duplication API', value: 'ddx' },
      );
      break;
    case 'linux':
      opts.push(
        { label: 'NvFBC', value: 'nvfbc' },
        { label: 'wlroots', value: 'wlr' },
        { label: 'KMS', value: 'kms' },
        { label: 'X11', value: 'x11' },
      );
      break;
    default:
      break;
  }
  return opts;
});

const losslessConfiguredPath = computed(() => (config.value as any)?.lossless_scaling_path ?? '');
const losslessSuggestedPath = computed(() => {
  if (losslessConfiguredPath.value) return normalizeWindowsPath(losslessConfiguredPath.value);
  const suggested = losslessStatus.value?.suggested_path as string | undefined;
  return normalizeWindowsPath(suggested) || LOSSLESS_DEFAULT_PATH;
});
const losslessPathExists = computed(() => !!losslessStatus.value?.checked_exists);
const losslessCandidates = computed(() => {
  const raw = losslessStatus.value?.candidates;
  if (!Array.isArray(raw)) return [] as string[];
  return raw
    .map((item: unknown) => (typeof item === 'string' ? normalizeWindowsPath(item) : ''))
    .filter((item) => !!item);
});
const losslessCheckedIsDirectory = computed(
  () => !!losslessStatus.value?.checked_is_directory,
);

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
        if (Array.isArray(payload.candidates)) {
          payload.candidates = payload.candidates
            .map((item: unknown) => (typeof item === 'string' ? normalizeWindowsPath(item) : ''))
            .filter((item: string) => !!item);
        }
      }
      losslessStatus.value = payload;
      losslessError.value = null;
    } else {
      losslessError.value = 'Unable to query Lossless Scaling status.';
      losslessStatus.value = null;
    }
  } catch (err) {
    losslessError.value = 'Unable to query Lossless Scaling status.';
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
  const selected = normalizeWindowsPath(losslessBrowseSelection.value);
  if (!selected) return;
  (config.value as any).lossless_scaling_path = selected;
  losslessBrowseVisible.value = false;
}

async function openLosslessBrowse() {
  if (platform.value !== 'windows') return;
  if (!losslessStatus.value && !losslessLoading.value) {
    await refreshLosslessStatus();
  }
  const initial =
    normalizeWindowsPath(losslessConfiguredPath.value) ||
    losslessResolvedPath.value ||
    losslessCandidates.value[0] ||
    '';
  losslessBrowseSelection.value = initial;
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
  }
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

const encoderOptions = computed(() => {
  const opts = [{ label: t('_common.autodetect'), value: '' }];
  if (platform.value === 'windows') {
    if (hasNvidia.value) opts.push({ label: 'NVIDIA NVENC', value: 'nvenc' });
    if (hasIntel.value) opts.push({ label: 'Intel QuickSync', value: 'quicksync' });
    if (hasAmd.value) opts.push({ label: 'AMD AMF/VCE', value: 'amdvce' });
  } else if (platform.value === 'linux') {
    opts.push({ label: 'NVIDIA NVENC', value: 'nvenc' }, { label: 'VA-API', value: 'vaapi' });
  } else if (platform.value === 'macos') {
    opts.push({ label: 'VideoToolbox', value: 'videotoolbox' });
  }
  opts.push({ label: t('config.encoder_software'), value: 'software' });
  return opts;
});

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
const shouldShowSoftware = computed(() => showAll() || props.currentTab === 'sw');
</script>

<template>
  <div class="config-page space-y-6">
    <div class="space-y-4">
      <div>
        <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
        <n-select
          id="capture"
          v-model:value="config.capture"
          :options="captureOptions"
          :data-search-options="captureOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
        />
        <n-text depth="3" class="text-[11px] block mt-1">{{ $t('config.capture_desc') }}</n-text>
      </div>
      <div>
        <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
        <n-select
          id="encoder"
          v-model:value="config.encoder"
          :options="encoderOptions"
          :data-search-options="encoderOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
        />
        <n-text depth="3" class="text-[11px] block mt-1">{{ $t('config.encoder_desc') }}</n-text>
      </div>
      <div v-if="platform === 'windows'" class="space-y-2">
        <div class="flex items-center justify-between">
          <label for="lossless_scaling_path" class="form-label">
            Lossless Scaling executable
          </label>
          <div class="flex items-center gap-2 text-xs">
            <n-button size="tiny" tertiary @click="applyLosslessSuggestion">
              Use Suggested
            </n-button>
            <n-button size="tiny" tertiary @click="openLosslessBrowse">Browse…</n-button>
            <n-button size="tiny" tertiary :loading="losslessLoading" @click="refreshLosslessStatus">
              Check
            </n-button>
          </div>
        </div>
        <n-input
          id="lossless_scaling_path"
          v-model:value="config.lossless_scaling_path"
          :placeholder="LOSSLESS_DEFAULT_PATH"
          clearable
        />
        <div class="text-[11px] opacity-60">Default installation: {{ LOSSLESS_DEFAULT_PATH }}</div>
        <div class="text-[11px]" :class="losslessPathExists ? 'text-green-500' : 'text-red-500'">
          <span v-if="losslessLoading">Checking…</span>
          <span v-else-if="losslessError">{{ losslessError }}</span>
          <span v-else>{{ losslessStatus?.message ?? 'Status unavailable.' }}</span>
        </div>
        <div v-if="!losslessLoading && losslessResolvedPath" class="text-[11px] opacity-60">
          Resolved: {{ losslessResolvedPath }}
        </div>
      </div>
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

    <div v-if="shouldShowSoftware" class="encoder-outline">
      <SoftwareEncoder />
    </div>

    <n-modal
      v-model:show="losslessBrowseVisible"
      preset="card"
      class="max-w-2xl"
      title="Select Lossless Scaling Executable"
    >
      <div class="space-y-4">
        <n-alert type="info" size="small" v-if="!losslessCandidates.length">
          Sunshine searched common Steam and program directories but could not locate LosslessScaling.exe.
          Install Lossless Scaling from Steam or set the full path manually.
        </n-alert>
        <div v-else class="space-y-2">
          <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
            Detected installations
          </div>
          <n-radio-group v-model:value="losslessBrowseSelection" class="space-y-2">
            <div
              v-for="candidate in losslessCandidates"
              :key="candidate"
              class="rounded-md border border-dark/10 px-3 py-2 text-xs dark:border-light/10"
            >
              <n-radio :value="candidate">{{ candidate }}</n-radio>
            </div>
          </n-radio-group>
        </div>
        <n-alert
          v-if="losslessCheckedIsDirectory && !losslessPathExists"
          type="warning"
          size="small"
        >
          The current configuration points at a folder. Choose LosslessScaling.exe directly.
        </n-alert>
        <div class="flex items-center justify-between pt-2">
          <n-button size="small" tertiary @click="rescanLosslessCandidates" :loading="losslessLoading">
            Rescan
          </n-button>
          <div class="flex items-center gap-2">
            <n-button size="small" tertiary @click="losslessBrowseVisible = false">Cancel</n-button>
            <n-button
              size="small"
              type="primary"
              :disabled="!losslessBrowseSelection"
              @click="applyLosslessBrowseSelection"
            >
              Use Selected Path
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
