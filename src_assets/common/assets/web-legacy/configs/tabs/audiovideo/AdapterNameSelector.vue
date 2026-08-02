<script setup lang="ts">
import { $tp } from '@/platform-i18n';
import PlatformLayout from '@/PlatformLayout.vue';
import { NInput, NSelect } from 'naive-ui';

import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { computed, watchEffect } from 'vue';
import { useI18n } from 'vue-i18n';

type AdapterOption = {
  label: string;
  value: string;
  name: string;
  pnpId: string;
};

type DetectedAdapter = {
  name: string;
  pnpId: string;
  vendorId: number | null;
  dedicatedVideoMemory: number | null;
};

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const { t } = useI18n();
const platform = computed(() =>
  String(metadata.value?.platform || config.value.platform || '').toLowerCase(),
);

const detectedAdapters = computed<DetectedAdapter[]>(() => {
  const gpus = Array.isArray(metadata.value?.gpus) ? metadata.value.gpus : [];
  return gpus.map((gpu) => {
    const vendorId = Number(gpu.vendor_id);
    const dedicatedVideoMemory = Number(gpu.dedicated_video_memory);
    return {
      name: String(gpu.description || '').trim(),
      pnpId: String(gpu.pnp_id || '').trim(),
      vendorId: Number.isFinite(vendorId) ? vendorId : null,
      dedicatedVideoMemory:
        Number.isFinite(dedicatedVideoMemory) && dedicatedVideoMemory > 0
          ? dedicatedVideoMemory
          : null,
    };
  });
});

function equalsCi(lhs: string, rhs: string): boolean {
  return lhs.localeCompare(rhs, undefined, { sensitivity: 'accent' }) === 0;
}

function setAdapterPair(name: string, pnpId: string): void {
  store.setAdapterPreference(name, pnpId);
}

function vendorLabel(vendorId: number | null): string {
  switch (vendorId) {
    case 0x10de:
      return 'NVIDIA';
    case 0x1002:
    case 0x1022:
      return 'AMD';
    case 0x8086:
      return 'Intel';
    default:
      return '';
  }
}

function vramLabel(bytes: number | null): string {
  if (!bytes) return '';
  const gibibytes = bytes / 1024 ** 3;
  if (gibibytes >= 1) {
    const precision = gibibytes >= 10 || Number.isInteger(gibibytes) ? 0 : 1;
    return `${gibibytes.toFixed(precision)} GiB VRAM`;
  }
  return `${Math.round(bytes / 1024 ** 2)} MiB VRAM`;
}

function pnpAdapterLabel(adapter: DetectedAdapter): string {
  const name = adapter.name || adapter.pnpId;
  const details = [vendorLabel(adapter.vendorId), vramLabel(adapter.dedicatedVideoMemory)].filter(
    (detail) => detail.length > 0,
  );
  const summary = details.length > 0 ? `${name} (${details.join(', ')})` : name;
  return `${summary} - ${adapter.pnpId}`;
}

const windowsAdapterOptions = computed<AdapterOption[]>(() => {
  const options: AdapterOption[] = [
    {
      label: t('config.adapter_name_default'),
      value: '',
      name: '',
      pnpId: '',
    },
  ];

  const seenPnpIds = new Set<string>();
  for (const gpu of detectedAdapters.value.filter((adapter) => adapter.pnpId.length > 0)) {
    const pnpKey = gpu.pnpId.toLocaleUpperCase();
    if (seenPnpIds.has(pnpKey)) continue;
    seenPnpIds.add(pnpKey);
    const name = gpu.name || gpu.pnpId;
    options.push({
      label: pnpAdapterLabel(gpu),
      value: `pnp:${gpu.pnpId}`,
      name,
      pnpId: gpu.pnpId,
    });
  }

  const configuredPnpId = String(config.value.adapter_pnp_id || '').trim();
  const configuredName = String(config.value.adapter_name || '').trim();
  if (configuredPnpId && !seenPnpIds.has(configuredPnpId.toLocaleUpperCase())) {
    const name = configuredName || configuredPnpId;
    const unavailableLabel = t('config.adapter_name_not_detected');
    options.push({
      label: `${name} - ${configuredPnpId} - ${unavailableLabel}`,
      value: `pnp:${configuredPnpId}`,
      name,
      pnpId: configuredPnpId,
    });
  }

  const legacyAdapters = detectedAdapters.value.filter((adapter) => !adapter.pnpId && adapter.name);
  if (configuredName && !configuredPnpId) {
    legacyAdapters.push({
      name: configuredName,
      pnpId: '',
      vendorId: null,
      dedicatedVideoMemory: null,
    });
  }
  const seenLegacyNames = new Set<string>();
  for (const adapter of legacyAdapters) {
    const legacyKey = adapter.name.toLocaleUpperCase();
    if (seenLegacyNames.has(legacyKey)) continue;
    seenLegacyNames.add(legacyKey);
    options.push({
      label: adapter.name,
      value: `legacy:${adapter.name}`,
      name: adapter.name,
      pnpId: '',
    });
  }

  return options;
});

const windowsAdapterOptionMap = computed(
  () => new Map(windowsAdapterOptions.value.map((option) => [option.value, option])),
);

const windowsAdapterValue = computed({
  get(): string {
    const configuredPnpId = String(config.value.adapter_pnp_id || '').trim();
    if (configuredPnpId) {
      return (
        windowsAdapterOptions.value.find(
          (option) => option.pnpId && equalsCi(option.pnpId, configuredPnpId),
        )?.value || `pnp:${configuredPnpId}`
      );
    }
    const configuredName = String(config.value.adapter_name || '').trim();
    if (!configuredName) return '';
    return (
      windowsAdapterOptions.value.find(
        (option) => !option.pnpId && option.name && equalsCi(option.name, configuredName),
      )?.value || `legacy:${configuredName}`
    );
  },
  set(value: string | null) {
    const selected = String(value || '').trim();
    if (!selected) {
      setAdapterPair('', '');
      return;
    }

    const option = windowsAdapterOptionMap.value.get(selected);
    if (option) {
      setAdapterPair(option.name, option.pnpId);
      return;
    }

    // NSelect's tag mode returns raw text for a value not present in the
    // option map. Treat it as a legacy name, even if it begins with a prefix.
    setAdapterPair(selected, '');
  },
});

const legacyAdapterName = computed({
  get(): string {
    return String(config.value.adapter_name || '');
  },
  set(value: string | null) {
    setAdapterPair(String(value || ''), '');
  },
});

watchEffect(() => {
  if (platform.value && platform.value !== 'windows' && config.value.adapter_pnp_id) {
    setAdapterPair(String(config.value.adapter_name || ''), '');
  }
});
</script>

<template>
  <div v-if="platform !== 'macos'" class="mb-4">
    <label for="adapter_name" class="form-label">{{ $t('config.adapter_name') }}</label>
    <PlatformLayout>
      <template #windows>
        <n-select
          id="adapter_name"
          v-model:value="windowsAdapterValue"
          :options="windowsAdapterOptions"
          clearable
          filterable
          tag
          :placeholder="$t('config.adapter_name_default')"
        />
      </template>
      <template #freebsd>
        <n-input
          id="adapter_name"
          v-model:value="legacyAdapterName"
          type="text"
          :placeholder="$tp('config.adapter_name_placeholder', '/dev/dri/renderD128')"
        />
      </template>
      <template #linux>
        <n-input
          id="adapter_name"
          v-model:value="legacyAdapterName"
          type="text"
          :placeholder="$tp('config.adapter_name_placeholder', '/dev/dri/renderD128')"
        />
      </template>
    </PlatformLayout>
    <div class="text-[11px] opacity-60">
      <PlatformLayout>
        <template #windows>
          {{ $t('config.adapter_name_desc_windows') }}
        </template>
        <template #freebsd>
          {{ $t('config.adapter_name_desc_linux_1') }}<br />
          <pre>ls /dev/dri/renderD*  # {{ $t('config.adapter_name_desc_linux_2') }}</pre>
          <pre>
              vainfo --display drm --device /dev/dri/renderD129 | \
                grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
            </pre
          >
          {{ $t('config.adapter_name_desc_linux_3') }}<br />
          <i>VAProfileH264High : VAEntrypointEncSlice</i>
        </template>
        <template #linux>
          {{ $t('config.adapter_name_desc_linux_1') }}<br />
          <pre>ls /dev/dri/renderD*  # {{ $t('config.adapter_name_desc_linux_2') }}</pre>
          <pre>
              vainfo --display drm --device /dev/dri/renderD129 | \
                grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
            </pre
          >
          {{ $t('config.adapter_name_desc_linux_3') }}<br />
          <i>VAProfileH264High : VAEntrypointEncSlice</i>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
