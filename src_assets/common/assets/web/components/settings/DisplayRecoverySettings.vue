<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet, apiPost } from '@/api/client';
import { AppButton, StatusBadge } from '@/components/ui';
import type { StatusTone } from '@/components/ui/types';

interface GoldenStatus {
  exists?: boolean;
  needs_layout_upgrade?: boolean;
  out_of_date?: boolean;
}

interface MutationResponse {
  status?: boolean;
  error?: string;
}

const props = defineProps<{
  hotkey?: unknown;
  modifiers?: unknown;
  preferGolden?: unknown;
}>();

const emit = defineEmits<{
  'update:hotkey': [value: string];
  'update:modifiers': [value: string];
  'update:preferGolden': [value: boolean];
}>();

const { t } = useI18n();
const golden = ref<GoldenStatus | null>(null);
const statusLoading = ref(true);
const captureBusy = ref(false);
const statusError = ref('');
const notice = ref('');
const hotkeyCaptureActive = ref(false);
const hotkeyCaptureError = ref('');

function modifierParts(value: unknown): string[] {
  const tokens = String(value ?? '')
    .toLocaleLowerCase()
    .split(/[\s+|,;]+/)
    .filter(Boolean);
  const parts: string[] = [];
  if (tokens.includes('ctrl') || tokens.includes('control')) parts.push('Ctrl');
  if (tokens.includes('alt')) parts.push('Alt');
  if (tokens.includes('shift')) parts.push('Shift');
  if (tokens.includes('win') || tokens.includes('windows') || tokens.includes('meta')) {
    parts.push('Win');
  }
  return parts;
}

const hotkeyParts = computed(() => {
  const key = String(props.hotkey ?? '').trim();
  return key ? [...modifierParts(props.modifiers), key.toUpperCase()] : [];
});
const hotkeyConfiguredSafely = computed(
  () => Boolean(String(props.hotkey ?? '').trim()) && modifierParts(props.modifiers).length > 0,
);
const preferGoldenEnabled = computed(() =>
  ['1', 'true', 'yes', 'on', 'enabled'].includes(
    String(props.preferGolden ?? '').toLocaleLowerCase(),
  ),
);

const goldenState = computed<{ label: string; detail: string; tone: StatusTone }>(() => {
  if (statusLoading.value) {
    return {
      label: t('ui.settings.recovery.checking'),
      detail: t('ui.settings.recovery.checking_detail'),
      tone: 'neutral',
    };
  }
  if (!golden.value) {
    return {
      label: t('ui.settings.recovery.unavailable'),
      detail: t('ui.settings.recovery.unavailable_detail'),
      tone: 'danger',
    };
  }
  if (!golden.value.exists) {
    return {
      label: t('ui.settings.recovery.not_created'),
      detail: t('ui.settings.recovery.not_created_detail'),
      tone: 'warning',
    };
  }
  if (golden.value.out_of_date || golden.value.needs_layout_upgrade) {
    return {
      label: t('ui.settings.recovery.refresh_recommended'),
      detail: t('ui.settings.recovery.refresh_recommended_detail'),
      tone: 'warning',
    };
  }
  return {
    label: t('ui.settings.recovery.ready'),
    detail: t('ui.settings.recovery.ready_detail'),
    tone: 'success',
  };
});

async function loadGoldenStatus(): Promise<void> {
  statusLoading.value = true;
  statusError.value = '';
  try {
    golden.value = await apiGet<GoldenStatus>('/api/display/golden_status');
  } catch {
    golden.value = null;
    statusError.value = t('ui.settings.recovery.status_error');
  } finally {
    statusLoading.value = false;
  }
}

async function captureGoldenSnapshot(): Promise<void> {
  if (captureBusy.value) return;
  const confirmation = golden.value?.exists
    ? t('ui.settings.recovery.replace_confirm')
    : t('ui.settings.recovery.create_confirm');
  if (!window.confirm(confirmation)) return;

  captureBusy.value = true;
  statusError.value = '';
  notice.value = '';
  try {
    const result = await apiPost<MutationResponse>('/api/display/export_golden', {});
    if (result.status === false) throw new Error(result.error || 'capture-failed');
    notice.value = t('ui.settings.recovery.snapshot_requested');
    window.setTimeout(() => void loadGoldenStatus(), 1000);
  } catch {
    statusError.value = t('ui.settings.recovery.snapshot_error');
  } finally {
    captureBusy.value = false;
  }
}

function normalizeHotkeyKey(raw: string): string | null {
  if (/^F\d{1,2}$/i.test(raw)) {
    const number = Number(raw.slice(1));
    return Number.isInteger(number) && number >= 1 && number <= 24 ? `F${number}` : null;
  }
  if (/^[a-z0-9]$/i.test(raw)) return raw.toUpperCase();
  return null;
}

function captureHotkey(event: KeyboardEvent): void {
  if (event.key === 'Tab') return;
  if (event.key === 'Escape') {
    event.preventDefault();
    hotkeyCaptureActive.value = false;
    (event.currentTarget as HTMLInputElement | null)?.blur();
    return;
  }
  if (['Shift', 'Control', 'Alt', 'Meta'].includes(event.key)) return;
  event.preventDefault();
  hotkeyCaptureError.value = '';
  const key = normalizeHotkeyKey(event.key);
  if (!key) {
    hotkeyCaptureError.value = t('ui.settings.recovery.hotkey_invalid');
    return;
  }
  const modifiers: string[] = [];
  if (event.ctrlKey) modifiers.push('ctrl');
  if (event.altKey) modifiers.push('alt');
  if (event.shiftKey) modifiers.push('shift');
  if (event.metaKey) modifiers.push('win');
  if (!modifiers.length) {
    hotkeyCaptureError.value = t('ui.settings.recovery.hotkey_modifier_required');
    return;
  }
  emit('update:hotkey', key);
  emit('update:modifiers', modifiers.length ? modifiers.join('+') : 'none');
  hotkeyCaptureActive.value = false;
  (event.currentTarget as HTMLInputElement | null)?.blur();
}

function useRecommendedHotkey(): void {
  hotkeyCaptureError.value = '';
  emit('update:hotkey', 'F12');
  emit('update:modifiers', 'ctrl+alt+shift');
}

function clearHotkey(): void {
  hotkeyCaptureError.value = '';
  emit('update:hotkey', '');
  emit('update:modifiers', '');
}

function updatePreferGolden(event: Event): void {
  emit('update:preferGolden', (event.target as HTMLInputElement).checked);
}

onMounted(() => void loadGoldenStatus());
</script>

<template>
  <div class="display-recovery-settings">
    <article class="recovery-item">
      <div class="recovery-item__copy">
        <div class="recovery-item__title">
          <strong>{{ t('ui.settings.recovery.golden_title') }}</strong>
          <StatusBadge :label="goldenState.label" :tone="goldenState.tone" compact />
        </div>
        <p>{{ goldenState.detail }}</p>
      </div>
      <AppButton
        :label="
          !golden && !statusLoading
            ? t('ui.settings.recovery.retry_status')
            : golden?.exists
              ? t('ui.settings.recovery.update_snapshot')
              : t('ui.settings.recovery.create_snapshot')
        "
        :busy="captureBusy"
        :busy-label="t('ui.settings.recovery.saving_snapshot')"
        :disabled="statusLoading"
        variant="secondary"
        @click="golden ? captureGoldenSnapshot() : loadGoldenStatus()"
      />

      <div class="recovery-preference">
        <div class="recovery-item__copy">
          <div class="recovery-item__title">
            <strong>{{ t('ui.settings.recovery.prefer_golden_title') }}</strong>
            <StatusBadge
              v-if="golden?.exists"
              :label="t('ui.settings.recommended')"
              tone="success"
              compact
            />
          </div>
          <p>
            {{
              golden?.exists
                ? t('ui.settings.recovery.prefer_golden_detail')
                : t('ui.settings.recovery.prefer_golden_unavailable')
            }}
          </p>
        </div>
        <label class="vs-switch">
          <input
            type="checkbox"
            :checked="preferGoldenEnabled"
            :disabled="statusLoading || !golden?.exists"
            @change="updatePreferGolden"
          />
          <span class="vs-switch__track" aria-hidden="true" />
          <span class="visually-hidden">{{ t('ui.settings.recovery.prefer_golden_title') }}</span>
        </label>
      </div>
    </article>

    <article class="recovery-item recovery-item--hotkey">
      <div class="recovery-item__copy">
        <div class="recovery-item__title">
          <strong>{{ t('ui.settings.recovery.hotkey_title') }}</strong>
          <StatusBadge
            :label="
              hotkeyConfiguredSafely
                ? t('ui.settings.recovery.hotkey_configured')
                : hotkeyParts.length
                  ? t('ui.settings.recovery.hotkey_unsafe')
                  : t('ui.settings.recovery.hotkey_not_configured')
            "
            :tone="hotkeyConfiguredSafely ? 'info' : 'warning'"
            compact
          />
        </div>
        <p>{{ t('ui.settings.recovery.hotkey_detail') }}</p>
      </div>

      <div class="hotkey-control">
        <label class="hotkey-recorder">
          <span class="visually-hidden">{{ t('ui.settings.recovery.hotkey_title') }}</span>
          <input
            class="hotkey-recorder__input"
            type="text"
            readonly
            :value="hotkeyParts.join(' + ')"
            :placeholder="t('ui.settings.recovery.hotkey_placeholder')"
            @focus="hotkeyCaptureActive = true"
            @blur="hotkeyCaptureActive = false"
            @keydown="captureHotkey"
          />
          <span v-if="hotkeyParts.length" class="hotkey-keys" aria-hidden="true">
            <kbd v-for="part in hotkeyParts" :key="part">{{ part }}</kbd>
          </span>
          <span v-else class="hotkey-recorder__empty" aria-hidden="true">
            {{ t('ui.settings.recovery.hotkey_placeholder') }}
          </span>
        </label>
        <div class="hotkey-actions">
          <AppButton
            v-if="!hotkeyConfiguredSafely"
            :label="t('ui.settings.recovery.use_recommended_hotkey')"
            size="compact"
            variant="secondary"
            @click="useRecommendedHotkey"
          />
          <AppButton
            v-if="hotkeyParts.length"
            :label="t('ui.settings.recovery.clear_hotkey')"
            size="compact"
            variant="tertiary"
            @click="clearHotkey"
          />
        </div>
      </div>
      <p v-if="hotkeyCaptureActive" class="recovery-message" role="status">
        {{ t('ui.settings.recovery.hotkey_listening') }}
      </p>
      <p
        v-if="hotkeyParts.length && !hotkeyConfiguredSafely"
        class="recovery-message recovery-message--error"
        role="alert"
      >
        {{ t('ui.settings.recovery.hotkey_unsafe_detail') }}
      </p>
      <p v-if="hotkeyCaptureError" class="recovery-message recovery-message--error" role="alert">
        {{ hotkeyCaptureError }}
      </p>
    </article>

    <p v-if="notice" class="recovery-message" role="status">{{ notice }}</p>
    <p v-if="statusError" class="recovery-message recovery-message--error" role="alert">
      {{ statusError }}
    </p>
  </div>
</template>

<style scoped>
.display-recovery-settings {
  display: grid;
  width: 100%;
  gap: 0;
}

.recovery-item {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16) var(--vs-space-20);
  background: transparent;
}

.recovery-item + .recovery-item {
  border-top: 1px solid var(--vs-color-border-subtle);
}

.recovery-item--hotkey {
  align-items: start;
}

.recovery-item__copy,
.hotkey-control {
  min-width: 0;
}

.recovery-item__copy {
  display: grid;
  gap: var(--vs-space-8);
}

.hotkey-control {
  display: grid;
  grid-template-columns: minmax(220px, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-8);
}

.recovery-preference {
  display: flex;
  grid-column: 1 / -1;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-16);
  padding-top: var(--vs-space-12);
  border-top: 1px solid var(--vs-color-border-subtle);
}

.recovery-item__title {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.recovery-item p,
.recovery-message {
  margin: 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-body);
}

.hotkey-recorder {
  position: relative;
  display: block;
  min-width: 250px;
}

.hotkey-recorder__input {
  position: absolute;
  width: 1px;
  height: 1px;
  opacity: 0;
}

.hotkey-keys,
.hotkey-recorder__empty {
  display: flex;
  min-height: 42px;
  align-items: center;
  gap: var(--vs-space-4);
  padding: var(--vs-space-8) var(--vs-space-12);
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
  cursor: text;
}

.hotkey-recorder:focus-within .hotkey-keys,
.hotkey-recorder:focus-within .hotkey-recorder__empty {
  border-color: var(--vs-color-accent-default);
  box-shadow: 0 0 0 3px color-mix(in srgb, var(--vs-color-accent-default) 28%, transparent);
}

.hotkey-keys kbd {
  padding: 2px 7px;
  border: 1px solid var(--vs-color-border-strong);
  border-bottom-width: 2px;
  border-radius: 5px;
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-text-primary);
  font: inherit;
  font-weight: var(--vs-type-weight-semibold);
}

.hotkey-recorder__empty {
  color: var(--vs-color-text-muted);
}

.hotkey-actions {
  display: flex;
  justify-content: flex-start;
}

.recovery-message--error {
  color: var(--vs-color-status-danger);
}

@media (max-width: 899px) {
  .recovery-item {
    grid-template-columns: minmax(0, 1fr);
  }

  .hotkey-control {
    grid-template-columns: minmax(0, 1fr);
  }

  .recovery-preference {
    align-items: flex-start;
  }

  .hotkey-recorder {
    min-width: 0;
  }

  .hotkey-actions {
    justify-content: flex-start;
  }
}
</style>
