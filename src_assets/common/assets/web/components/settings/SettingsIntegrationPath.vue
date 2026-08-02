<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet } from '@/api/client';
import { AppButton, StatusBadge } from '@/components/ui';

type IntegrationKind = 'rtss' | 'lossless';
type StatusTone = 'neutral' | 'info' | 'success' | 'warning' | 'danger';

interface RtssStatus {
  active_provider?: string;
  configured_provider?: string;
  configured_path?: string;
  path_configured?: boolean;
  resolved_path?: string;
  path_exists?: boolean;
  hooks_found?: boolean;
  process_running?: boolean;
  enabled?: boolean;
}

interface LosslessStatus {
  status?: string;
  configured_path?: string;
  checked_path?: string;
  checked_exists?: boolean;
  checked_is_directory?: boolean;
  suggested_path?: string;
  resolved_path?: string;
  candidates?: string[];
}

type IntegrationStatus = RtssStatus | LosslessStatus;

const props = withDefaults(
  defineProps<{
    kind: IntegrationKind;
    inputId: string;
    modelValue?: unknown;
  }>(),
  {
    modelValue: () => '',
  },
);

const emit = defineEmits<{
  'update:modelValue': [value: string];
}>();

const { t } = useI18n();

const status = ref<IntegrationStatus | null>(null);
const loading = ref(false);
const loadError = ref(false);
const browseOpen = ref(false);
const userEdited = ref(false);
const draftPath = ref(normalizeWindowsPath(props.modelValue));
const candidateSelection = ref('');

function normalizeWindowsPath(raw: unknown): string {
  if (raw === null || raw === undefined) return '';
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

const configuredPath = computed(() => normalizeWindowsPath(props.modelValue));

const losslessStatus = computed<LosslessStatus | null>(() =>
  props.kind === 'lossless' ? (status.value as LosslessStatus | null) : null,
);

const rtssStatus = computed<RtssStatus | null>(() =>
  props.kind === 'rtss' ? (status.value as RtssStatus | null) : null,
);

const candidates = computed(() => {
  if (props.kind !== 'lossless') return [];
  const raw = losslessStatus.value?.candidates;
  if (!Array.isArray(raw)) return [];
  return raw
    .map((candidate) => normalizeWindowsPath(candidate))
    .filter((candidate, index, all) => candidate && all.indexOf(candidate) === index);
});

const detectedPath = computed(() => {
  if (props.kind === 'rtss') {
    return rtssStatus.value?.path_exists
      ? normalizeWindowsPath(rtssStatus.value.resolved_path)
      : '';
  }

  if (losslessStatus.value?.status !== 'detected') return '';
  return (
    normalizeWindowsPath(losslessStatus.value.resolved_path) ||
    candidates.value[0] ||
    normalizeWindowsPath(losslessStatus.value.suggested_path)
  );
});

const selectedDetectedPath = computed(
  () => normalizeWindowsPath(candidateSelection.value) || detectedPath.value,
);

const ready = computed(() => {
  if (props.kind === 'rtss') {
    return Boolean(rtssStatus.value?.path_exists && rtssStatus.value.hooks_found);
  }
  return Boolean(
    losslessStatus.value?.status === 'detected' &&
      detectedPath.value &&
      !losslessStatus.value.checked_is_directory,
  );
});

const needsAttention = computed(() => {
  if (props.kind === 'rtss') {
    return Boolean(rtssStatus.value?.path_exists && !rtssStatus.value.hooks_found);
  }
  return Boolean(
    losslessStatus.value?.status === 'path-is-directory' ||
      (losslessStatus.value?.status === 'path-not-found' && configuredPath.value),
  );
});

const notFound = computed(() => {
  if (props.kind === 'rtss') {
    return Boolean(rtssStatus.value?.path_configured && !rtssStatus.value.path_exists);
  }
  return Boolean(losslessStatus.value?.status === 'path-not-found' && configuredPath.value);
});

const statusTone = computed<StatusTone>(() => {
  if (loading.value) return 'neutral';
  if (loadError.value) return 'warning';
  if (ready.value) return 'success';
  if (notFound.value) return 'danger';
  if (needsAttention.value) return 'warning';
  return 'neutral';
});

const statusLabel = computed(() => {
  if (loading.value) return t('ui.settings.integrations.checking');
  if (loadError.value) return t('ui.settings.integrations.status_unavailable');
  if (ready.value) return t('ui.settings.integrations.ready');
  if (notFound.value) return t('ui.settings.integrations.not_found');
  if (needsAttention.value) return t('ui.settings.integrations.needs_attention');
  return t('ui.settings.integrations.not_detected');
});

const statusDescription = computed(() => {
  if (loading.value) return t('ui.settings.integrations.checking_description');
  if (loadError.value) return t('ui.settings.integrations.status_unavailable_description');

  if (props.kind === 'rtss') {
    if (ready.value) {
      return rtssStatus.value?.active_provider === 'rtss'
        ? t('ui.settings.integrations.rtss.active')
        : rtssStatus.value?.configured_provider === 'rtss'
          ? t('ui.settings.integrations.rtss.selected')
          : rtssStatus.value?.enabled === false
            ? t('ui.settings.integrations.rtss.disabled')
            : rtssStatus.value?.configured_provider === 'auto'
              ? t('ui.settings.integrations.rtss.auto')
              : t('ui.settings.integrations.rtss.ready');
    }
    if (needsAttention.value) return t('ui.settings.integrations.rtss.hooks_missing');
    if (notFound.value) return t('ui.settings.integrations.rtss.path_missing');
    return t('ui.settings.integrations.rtss.not_detected');
  }

  if (losslessStatus.value?.status === 'path-is-directory') {
    return t('ui.settings.integrations.lossless.path_is_directory');
  }
  if (notFound.value) return t('ui.settings.integrations.lossless.path_missing');
  if (ready.value) return t('ui.settings.integrations.lossless.ready');
  return t('ui.settings.integrations.lossless.not_detected');
});

const automaticHintVisible = computed(() => Boolean(detectedPath.value) && !configuredPath.value);

const canSaveDetectedPath = computed(
  () => Boolean(selectedDetectedPath.value) && selectedDetectedPath.value !== configuredPath.value,
);

function syncDetectedPath(): void {
  if (!userEdited.value && !configuredPath.value && detectedPath.value) {
    draftPath.value = detectedPath.value;
  }
}

async function refresh(): Promise<void> {
  if (loading.value) return;
  loading.value = true;
  loadError.value = false;

  try {
    const queryPath =
      props.kind === 'lossless'
        ? configuredPath.value || (userEdited.value ? normalizeWindowsPath(draftPath.value) : '')
        : '';
    const query = queryPath ? `?path=${encodeURIComponent(queryPath)}` : '';
    const endpoint = props.kind === 'rtss' ? '/api/rtss/status' : '/api/lossless_scaling/status';
    status.value = await apiGet<IntegrationStatus>(`${endpoint}${query}`);

    const firstCandidate = candidates.value[0];
    candidateSelection.value =
      candidateSelection.value && candidates.value.includes(candidateSelection.value)
        ? candidateSelection.value
        : firstCandidate || detectedPath.value;
    syncDetectedPath();
  } catch {
    status.value = null;
    loadError.value = true;
  } finally {
    loading.value = false;
  }
}

function updatePath(value: string): void {
  userEdited.value = true;
  draftPath.value = normalizeWindowsPath(value);
  emit('update:modelValue', draftPath.value);
}

function useDetectedPath(): void {
  const path = selectedDetectedPath.value;
  if (!path) return;
  updatePath(path);
}

watch(
  () => props.modelValue,
  (value) => {
    const next = normalizeWindowsPath(value);
    if (next === draftPath.value) return;
    draftPath.value = next;
    userEdited.value = false;
  },
);

watch(
  () => props.kind,
  () => {
    status.value = null;
    loadError.value = false;
    browseOpen.value = false;
    userEdited.value = false;
    draftPath.value = configuredPath.value;
    void refresh();
  },
);

onMounted(() => void refresh());
</script>

<template>
  <div class="integration-path">
    <div class="integration-path__status">
      <div class="integration-path__status-copy">
        <StatusBadge :tone="statusTone" :label="statusLabel" compact />
        <p>{{ statusDescription }}</p>
      </div>
      <div class="integration-path__actions">
        <AppButton
          v-if="canSaveDetectedPath"
          variant="tertiary"
          size="compact"
          icon="check"
          :label="t('ui.settings.integrations.use_detected')"
          @click="useDetectedPath"
        />
        <AppButton
          variant="tertiary"
          size="compact"
          icon="refresh"
          :label="t('ui.settings.integrations.rescan')"
          :busy="loading"
          :busy-label="t('ui.settings.integrations.checking')"
          @click="refresh"
        />
      </div>
    </div>

    <div class="integration-path__input-row">
      <input
        :id="inputId"
        class="vs-input monospace"
        type="text"
        :value="draftPath"
        :aria-describedby="`${inputId}-status`"
        @input="updatePath(($event.target as HTMLInputElement).value)"
      />
    </div>

    <p v-if="automaticHintVisible" class="integration-path__hint" :id="`${inputId}-status`">
      {{ t('ui.settings.integrations.automatic_path_hint') }}
    </p>
    <p v-else class="integration-path__hint" :id="`${inputId}-status`">
      {{ t('ui.settings.integrations.explicit_path_hint') }}
    </p>

    <div v-if="props.kind === 'lossless' && candidates.length" class="integration-path__browse">
      <div class="integration-path__browse-heading">
        <strong>{{ t('ui.settings.integrations.detected_installations') }}</strong>
        <AppButton
          variant="tertiary"
          size="compact"
          :icon="browseOpen ? 'chevron-down' : 'chevron-right'"
          :label="
            t(
              browseOpen
                ? 'ui.settings.integrations.hide_detected'
                : 'ui.settings.integrations.browse_detected',
            )
          "
          @click="browseOpen = !browseOpen"
        />
      </div>
      <div v-if="browseOpen" class="integration-path__browse-body">
        <label class="vs-sr-only" :for="`${inputId}-candidates`">
          {{ t('ui.settings.integrations.choose_detected') }}
        </label>
        <select :id="`${inputId}-candidates`" v-model="candidateSelection" class="vs-select">
          <option v-for="candidate in candidates" :key="candidate" :value="candidate">
            {{ candidate }}
          </option>
        </select>
        <AppButton
          variant="secondary"
          size="compact"
          icon="check"
          :label="t('ui.settings.integrations.use_selected')"
          @click="useDetectedPath"
        />
      </div>
    </div>
  </div>
</template>

<style scoped>
.integration-path {
  display: grid;
  gap: var(--vs-space-8);
}

.integration-path__status,
.integration-path__status-copy,
.integration-path__actions,
.integration-path__browse-heading,
.integration-path__browse-body {
  display: flex;
}

.integration-path__status {
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.integration-path__status-copy {
  min-width: 0;
  flex: 1;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.integration-path__status-copy p {
  flex: 1 1 100%;
  margin: 0;
  color: var(--vs-color-text-secondary);
  font-size: 13px;
  line-height: 18px;
}

.integration-path__actions {
  flex: 0 0 auto;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: var(--vs-space-4);
}

.integration-path__input-row {
  min-width: 0;
}

.integration-path__input-row .vs-input {
  width: 100%;
}

.integration-path__hint {
  margin: 0;
  color: var(--vs-color-text-muted);
  font-size: 12px;
  line-height: 17px;
}

.integration-path__browse {
  padding: var(--vs-space-8) var(--vs-space-12);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.integration-path__browse-heading {
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-12);
  color: var(--vs-color-text-secondary);
  font-size: 12px;
}

.integration-path__browse-body {
  align-items: center;
  gap: var(--vs-space-8);
  margin-top: var(--vs-space-8);
}

.integration-path__browse-body .vs-select {
  min-width: 0;
  flex: 1;
}

@media (max-width: 767px) {
  .integration-path__status {
    flex-direction: column;
  }

  .integration-path__actions {
    justify-content: flex-start;
  }

  .integration-path__browse-body {
    align-items: stretch;
    flex-direction: column;
  }
}
</style>
