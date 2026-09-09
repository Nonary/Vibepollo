<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiGet } from '@/api/client';
import { AppButton, StatusBadge, UiIcon } from '@/components/ui';

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

interface BrowseEntry {
  name: string;
  path: string;
  type: 'directory' | 'file';
}

interface BrowseResponse {
  path?: string;
  parent?: string;
  entries?: unknown;
}

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
const browseDialog = ref<HTMLDialogElement | null>(null);
const browsePath = ref('');
const browseParent = ref('');
const browseSelection = ref('');
const browseEntries = ref<BrowseEntry[]>([]);
const browseLoading = ref(false);
const browseError = ref('');
const browseTrigger = ref<HTMLElement | null>(null);

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

const browseSelectionName = computed(() => normalizeWindowsPath(browseSelection.value));

function parseBrowseEntries(value: unknown): BrowseEntry[] {
  if (!Array.isArray(value)) return [];
  return value
    .map((entry): BrowseEntry | null => {
      if (!entry || typeof entry !== 'object' || Array.isArray(entry)) return null;
      const path = normalizeWindowsPath((entry as { path?: unknown }).path);
      const name = String((entry as { name?: unknown }).name ?? '').trim();
      const rawType = (entry as { type?: unknown }).type;
      if (rawType !== 'directory' && rawType !== 'file') return null;
      const type = rawType;
      return name && path ? { name, path, type } : null;
    })
    .filter((entry): entry is BrowseEntry => Boolean(entry));
}

function parseBrowseResponse(value: unknown): BrowseResponse | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const response = value as BrowseResponse;
  if (!Array.isArray(response.entries)) return null;
  if (response.path !== undefined && typeof response.path !== 'string') return null;
  if (response.parent !== undefined && typeof response.parent !== 'string') return null;
  return response;
}

async function loadBrowseDirectory(path: string): Promise<void> {
  browseLoading.value = true;
  browseError.value = '';
  try {
    const query = new URLSearchParams({ type: 'executable' });
    const normalized = normalizeWindowsPath(path);
    if (normalized) query.set('path', normalized);
    const payload = await apiGet<unknown>(`/api/browse?${query.toString()}`);
    const response = parseBrowseResponse(payload);
    if (!response) throw new Error(t('ui.settings.integrations.browse_failed'));
    browsePath.value = normalizeWindowsPath(response.path);
    browseParent.value = normalizeWindowsPath(response.parent);
    browseEntries.value = parseBrowseEntries(response.entries);
  } catch (cause) {
    browseEntries.value = [];
    browseError.value =
      cause instanceof ApiError || !(cause instanceof Error)
        ? t('ui.settings.integrations.browse_failed')
        : cause.message;
  } finally {
    browseLoading.value = false;
  }
}

async function openBrowse(event?: MouseEvent): Promise<void> {
  if (props.kind !== 'lossless' || browseLoading.value) return;
  browseTrigger.value = (event?.currentTarget as HTMLElement | null) ?? null;
  browseSelection.value = normalizeWindowsPath(draftPath.value) || selectedDetectedPath.value || '';
  browsePath.value = '';
  browseParent.value = '';
  browseEntries.value = [];
  browseError.value = '';
  browseOpen.value = true;
  await nextTick();
  if (!browseDialog.value?.open) browseDialog.value?.showModal();
  await loadBrowseDirectory(browseSelection.value);
  await nextTick();
  browseDialog.value?.querySelector<HTMLElement>('[data-browse-path]')?.focus();
}

function closeBrowse(): void {
  browseOpen.value = false;
  if (browseDialog.value?.open) browseDialog.value.close();
  const trigger = browseTrigger.value;
  browseTrigger.value = null;
  if (trigger?.isConnected) nextTick(() => trigger.focus());
}

function selectBrowseEntry(entry: BrowseEntry): void {
  const path = normalizeWindowsPath(entry.path);
  if (!path) return;
  if (entry.type === 'directory') {
    void loadBrowseDirectory(path);
    return;
  }
  browseSelection.value = path;
}

function useBrowseSelection(): void {
  if (!browseSelectionName.value) return;
  updatePath(browseSelectionName.value);
  closeBrowse();
}

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

watch(browseOpen, (open) => {
  if (open) return;
  if (browseDialog.value?.open) browseDialog.value.close();
});

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

onBeforeUnmount(() => {
  if (browseDialog.value?.open) browseDialog.value.close();
});
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

    <div v-if="props.kind === 'lossless'" class="integration-path__browse">
      <div class="integration-path__browse-heading">
        <strong v-if="candidates.length">{{
          t('ui.settings.integrations.detected_installations')
        }}</strong>
        <AppButton
          variant="tertiary"
          size="compact"
          icon="library"
          :label="t('ui.settings.integrations.browse_host')"
          @click="openBrowse"
        />
      </div>
      <div v-if="candidates.length" class="integration-path__browse-body">
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
      <p class="integration-path__browse-hint">
        {{ t('ui.settings.integrations.browse_host_hint') }}
      </p>
    </div>

    <dialog
      v-if="browseOpen"
      ref="browseDialog"
      class="integration-path__dialog"
      :aria-labelledby="`${inputId}-browse-title`"
      @cancel.prevent="closeBrowse"
      @click="$event.target === $event.currentTarget && closeBrowse()"
    >
      <div class="integration-path__dialog-panel">
        <div class="integration-path__dialog-heading">
          <div>
            <h3 :id="`${inputId}-browse-title`">
              {{ t('ui.settings.integrations.browse_title') }}
            </h3>
            <p>{{ t('ui.settings.integrations.browse_description') }}</p>
          </div>
          <AppButton
            icon="x"
            icon-only
            variant="tertiary"
            :label="t('_common.close')"
            @click="closeBrowse"
          />
        </div>

        <label class="vs-field">
          <span class="vs-field__label">{{ t('ui.settings.integrations.browse_path') }}</span>
          <input
            :value="browsePath"
            class="vs-input monospace"
            type="text"
            data-browse-path
            @input="browsePath = normalizeWindowsPath(($event.target as HTMLInputElement).value)"
            @keydown.enter.prevent="loadBrowseDirectory(browsePath)"
          />
        </label>

        <div class="integration-path__dialog-toolbar">
          <AppButton
            variant="tertiary"
            size="compact"
            icon="chevron-left"
            :label="t('ui.settings.integrations.browse_parent')"
            :disabled="!browseParent || browseParent === browsePath"
            @click="loadBrowseDirectory(browseParent)"
          />
          <span class="integration-path__dialog-current monospace">
            {{ browsePath || t('ui.settings.integrations.browse_computer') }}
          </span>
          <AppButton
            variant="tertiary"
            size="compact"
            icon="refresh"
            :label="t('ui.settings.integrations.rescan')"
            :busy="browseLoading"
            :busy-label="t('ui.settings.integrations.checking')"
            @click="loadBrowseDirectory(browsePath)"
          />
        </div>

        <p v-if="browseError" class="integration-path__dialog-error" role="alert">
          {{ browseError }}
        </p>
        <p v-else-if="browseLoading" class="integration-path__dialog-state" role="status">
          {{ t('ui.settings.integrations.browse_loading') }}
        </p>
        <div v-else-if="!browseEntries.length" class="integration-path__dialog-state">
          {{ t('ui.settings.integrations.browse_empty') }}
        </div>
        <div
          v-else
          class="integration-path__entries"
          role="listbox"
          :aria-label="t('ui.settings.integrations.browse_entries')"
        >
          <button
            v-for="entry in browseEntries"
            :key="entry.path"
            type="button"
            class="integration-path__entry"
            :class="{ 'integration-path__entry--selected': browseSelectionName === entry.path }"
            role="option"
            :aria-selected="browseSelectionName === entry.path"
            @click="selectBrowseEntry(entry)"
          >
            <span class="integration-path__entry-icon" aria-hidden="true">
              <UiIcon :name="entry.type === 'directory' ? 'library' : 'logs'" :size="16" />
            </span>
            <span class="integration-path__entry-copy">
              <strong>{{ entry.name }}</strong>
              <small class="monospace">{{ entry.path }}</small>
            </span>
            <span class="integration-path__entry-type">
              {{
                entry.type === 'directory'
                  ? t('ui.settings.integrations.browse_folder')
                  : t('ui.settings.integrations.browse_executable')
              }}
            </span>
          </button>
        </div>

        <p v-if="browseSelectionName" class="integration-path__selected">
          {{ t('ui.settings.integrations.browse_selected', { path: browseSelectionName }) }}
        </p>
        <p
          v-if="losslessStatus?.status === 'path-is-directory'"
          class="integration-path__dialog-warning"
          role="alert"
        >
          {{ t('ui.settings.integrations.lossless.path_is_directory') }}
        </p>
        <div class="integration-path__dialog-actions">
          <AppButton variant="tertiary" :label="t('_common.cancel')" @click="closeBrowse" />
          <AppButton
            variant="primary"
            icon="check"
            :label="t('ui.settings.integrations.use_selected')"
            :disabled="!browseSelectionName || browseSelectionName === configuredPath"
            @click="useBrowseSelection"
          />
        </div>
      </div>
    </dialog>
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
.integration-path__browse-body,
.integration-path__dialog-heading,
.integration-path__dialog-toolbar,
.integration-path__dialog-actions,
.integration-path__entry {
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

.integration-path__browse-hint {
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-muted);
  font-size: 12px;
  line-height: 17px;
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

.integration-path__dialog {
  width: min(50rem, calc(100vw - 2rem));
  max-width: 100%;
  padding: 0;
  border: 0;
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-primary);
  box-shadow: var(--vs-shadow-overlay);
}

.integration-path__dialog::backdrop {
  background: rgb(0 0 0 / 45%);
}

.integration-path__dialog-panel {
  display: grid;
  max-height: min(42rem, calc(100vh - 2rem));
  gap: var(--vs-space-16);
  padding: var(--vs-space-20);
  overflow: auto;
}

.integration-path__dialog-heading,
.integration-path__dialog-toolbar,
.integration-path__dialog-actions {
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-8);
}

.integration-path__dialog-heading h3 {
  margin: 0;
  font-size: var(--vs-type-size-section);
}

.integration-path__dialog-heading p {
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.integration-path__dialog-current {
  min-width: 0;
  flex: 1;
  overflow-wrap: anywhere;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  text-align: center;
}

.integration-path__entries {
  display: grid;
  max-height: 18rem;
  overflow: auto;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
}

.integration-path__entry {
  width: 100%;
  align-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) var(--vs-space-12);
  border: 0;
  border-bottom: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-primary);
  text-align: start;
  cursor: pointer;
}

.integration-path__entry:last-child {
  border-bottom: 0;
}

.integration-path__entry:hover,
.integration-path__entry:focus-visible,
.integration-path__entry--selected {
  background: var(--vs-color-bg-subtle);
}

.integration-path__entry-icon {
  flex: 0 0 auto;
  color: var(--vs-color-accent-default);
}

.integration-path__entry-copy {
  display: grid;
  min-width: 0;
  flex: 1;
}

.integration-path__entry-copy strong,
.integration-path__entry-copy small {
  overflow-wrap: anywhere;
}

.integration-path__entry-copy small,
.integration-path__entry-type,
.integration-path__selected,
.integration-path__dialog-state,
.integration-path__dialog-error,
.integration-path__dialog-warning {
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-metadata);
}

.integration-path__entry-copy small,
.integration-path__entry-type,
.integration-path__selected,
.integration-path__dialog-state {
  color: var(--vs-color-text-secondary);
}

.integration-path__entry-type {
  flex: 0 0 auto;
}

.integration-path__dialog-error,
.integration-path__dialog-warning {
  margin: 0;
  color: var(--vs-color-status-danger);
}

.integration-path__dialog-warning {
  color: var(--vs-color-status-warning);
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

  .integration-path__dialog {
    width: calc(100vw - 1rem);
  }

  .integration-path__dialog-panel {
    padding: var(--vs-space-16);
  }

  .integration-path__dialog-heading,
  .integration-path__dialog-toolbar,
  .integration-path__dialog-actions,
  .integration-path__entry {
    align-items: stretch;
    flex-direction: column;
  }

  .integration-path__dialog-heading {
    flex-direction: row;
    align-items: flex-start;
  }

  .integration-path__dialog-current {
    order: -1;
    text-align: start;
  }

  .integration-path__entry-type {
    align-self: flex-start;
  }
}
</style>
