<script setup lang="ts">
import { computed, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { AppButton, InlineAlert } from '@/components/ui';

interface MappingEntry {
  requested_resolution: string;
  requested_fps: string;
  final_resolution: string;
  final_refresh_rate: string;
}

interface ModeRemapping {
  mixed: MappingEntry[];
  resolution_only: MappingEntry[];
  refresh_rate_only: MappingEntry[];
}

type MappingType = keyof ModeRemapping;

const props = withDefaults(
  defineProps<{
    modelValue?: unknown;
    resolutionMode?: string;
    refreshMode?: string;
    simple?: boolean;
  }>(),
  {
    resolutionMode: 'auto',
    refreshMode: 'auto',
    simple: false,
  },
);

const emit = defineEmits<{
  'update:modelValue': [value: ModeRemapping];
  'validity-change': [valid: boolean];
}>();

const { t } = useI18n();

const commonResolutions = [
  { value: '1280x720', label: '720p' },
  { value: '1920x1080', label: '1080p' },
  { value: '2560x1440', label: '1440p' },
  { value: '3440x1440', label: 'Ultrawide 1440p' },
  { value: '3840x2160', label: '2160p / 4K' },
  { value: '5120x1440', label: 'Super ultrawide 1440p' },
  { value: '5120x2880', label: '2880p / 5K' },
  { value: '7680x4320', label: '4320p / 8K' },
];

function stringValue(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function normalizeEntry(value: unknown): MappingEntry {
  const entry = value && typeof value === 'object' ? (value as Record<string, unknown>) : {};
  return {
    requested_resolution: stringValue(entry.requested_resolution),
    requested_fps: stringValue(entry.requested_fps),
    final_resolution: stringValue(entry.final_resolution),
    final_refresh_rate: stringValue(entry.final_refresh_rate),
  };
}

function normalize(value: unknown): ModeRemapping {
  const source = value && typeof value === 'object' ? (value as Record<string, unknown>) : {};
  return {
    mixed: Array.isArray(source.mixed) ? source.mixed.map(normalizeEntry) : [],
    resolution_only: Array.isArray(source.resolution_only)
      ? source.resolution_only.map(normalizeEntry)
      : [],
    refresh_rate_only: Array.isArray(source.refresh_rate_only)
      ? source.refresh_rate_only.map(normalizeEntry)
      : [],
  };
}

const mappingKeys = [
  'requested_resolution',
  'requested_fps',
  'final_resolution',
  'final_refresh_rate',
] as const;

function hasValidStoredShape(value: unknown): boolean {
  if (value === undefined || value === null) return true;
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const source = value as Record<string, unknown>;
  const listNames: MappingType[] = ['mixed', 'resolution_only', 'refresh_rate_only'];
  if (Object.keys(source).some((key) => !listNames.includes(key as MappingType))) return false;
  return listNames.every((listName) => {
    const list = source[listName];
    return (
      Array.isArray(list) &&
      list.every(
        (entry) =>
          Boolean(entry) &&
          typeof entry === 'object' &&
          !Array.isArray(entry) &&
          Object.entries(entry as Record<string, unknown>).every(
            ([key, entryValue]) =>
              mappingKeys.includes(key as (typeof mappingKeys)[number]) &&
              typeof entryValue === 'string',
          ),
      )
    );
  });
}

const sourceValid = computed(() => hasValidStoredShape(props.modelValue));
const remapping = computed(() => normalize(props.modelValue));
const mappingType = computed<MappingType>(() => {
  const resolutionAutomatic = props.resolutionMode === 'auto';
  const refreshAutomatic = props.refreshMode === 'auto';
  if (resolutionAutomatic && refreshAutomatic) return 'mixed';
  if (resolutionAutomatic) return 'resolution_only';
  if (refreshAutomatic) return 'refresh_rate_only';
  return 'mixed';
});

function isSimpleResolutionEntry(entry: MappingEntry): boolean {
  return !entry.requested_fps.trim() && !entry.final_refresh_rate.trim();
}

function simpleResolutionEntries(value: ModeRemapping): MappingEntry[] {
  const seen = new Set<string>();
  const result: MappingEntry[] = [];
  for (const entry of [...value.mixed, ...value.resolution_only]) {
    if (!isSimpleResolutionEntry(entry)) continue;
    const key = `${entry.requested_resolution.trim()}=>${entry.final_resolution.trim()}`;
    if (seen.has(key)) continue;
    seen.add(key);
    result.push(normalizeEntry(entry));
  }
  return result;
}

function replaceSimpleEntries(
  existing: MappingEntry[],
  replacements: MappingEntry[],
): MappingEntry[] {
  const remaining = replacements.map(normalizeEntry);
  const result: MappingEntry[] = [];
  for (const entry of existing) {
    if (!isSimpleResolutionEntry(entry)) {
      result.push(normalizeEntry(entry));
      continue;
    }
    const replacement = remaining.shift();
    if (replacement) result.push(replacement);
  }
  result.push(...remaining);
  return result;
}

function applySimpleEntries(value: ModeRemapping, entries: MappingEntry[]): ModeRemapping {
  value.mixed = replaceSimpleEntries(value.mixed, entries);
  value.resolution_only = replaceSimpleEntries(value.resolution_only, entries);
  return value;
}

function hasSimplePolicyConflict(value: ModeRemapping): boolean {
  const mixedByRequest = new Map<string, string>();
  const resolutionOnlyByRequest = new Map<string, string>();
  const collect = (
    entries: MappingEntry[],
    target: Map<string, string>,
    resolutionOnly: boolean,
  ) => {
    for (const entry of entries) {
      if (resolutionOnly && hasRefreshValues(entry)) return true;
      if (!isSimpleResolutionEntry(entry)) continue;
      const requested = entry.requested_resolution.trim();
      const final = entry.final_resolution.trim();
      if (!requested || !final) continue;
      const existing = target.get(requested);
      if (existing !== undefined && existing !== final) return true;
      target.set(requested, final);
    }
    return false;
  };
  if (collect(value.mixed, mixedByRequest, false)) return true;
  if (collect(value.resolution_only, resolutionOnlyByRequest, true)) return true;
  for (const [requested, final] of mixedByRequest) {
    const otherFinal = resolutionOnlyByRequest.get(requested);
    if (otherFinal !== undefined && otherFinal !== final) return true;
  }
  return false;
}

const simplePolicyConflict = computed(
  () => props.simple && sourceValid.value && hasSimplePolicyConflict(remapping.value),
);
const simplePoliciesUnsynced = computed(() => {
  if (!props.simple || !sourceValid.value || simplePolicyConflict.value) return false;
  const ruleSet = (entries: MappingEntry[]) =>
    new Set(
      entries
        .filter(isSimpleResolutionEntry)
        .map((entry) => `${entry.requested_resolution.trim()}=>${entry.final_resolution.trim()}`),
    );
  const mixed = ruleSet(remapping.value.mixed);
  const resolutionOnly = ruleSet(remapping.value.resolution_only);
  return mixed.size !== resolutionOnly.size || [...mixed].some((rule) => !resolutionOnly.has(rule));
});
const simpleRulesMayBeShadowed = computed(() => {
  if (!props.simple || !sourceValid.value) return false;
  return remapping.value.mixed.some((entry, index, entries) => {
    if (!isSimpleResolutionEntry(entry) || !entry.requested_resolution.trim()) return false;
    return entries.slice(0, index).some((earlier) => {
      if (isSimpleResolutionEntry(earlier)) return false;
      const earlierResolution = earlier.requested_resolution.trim();
      return !earlierResolution || earlierResolution === entry.requested_resolution.trim();
    });
  });
});

const entries = computed(() =>
  props.simple ? simpleResolutionEntries(remapping.value) : remapping.value[mappingType.value],
);
const showsResolution = computed(() => props.simple || mappingType.value !== 'refresh_rate_only');
const showsRefresh = computed(() => !props.simple && mappingType.value !== 'resolution_only');
const manualEnforcementActive = computed(() =>
  props.simple
    ? props.resolutionMode !== 'auto'
    : props.resolutionMode === 'manual' || props.refreshMode === 'manual',
);
const advancedInactive = computed(
  () => !props.simple && props.resolutionMode !== 'auto' && props.refreshMode !== 'auto',
);
const editorDisabled = computed(
  () =>
    !sourceValid.value ||
    simplePolicyConflict.value ||
    simplePoliciesUnsynced.value ||
    advancedInactive.value,
);

function resolutionValid(value: string): boolean {
  if (!value) return true;
  const match = /^(\d+)x(\d+)$/.exec(value.trim());
  if (!match) return false;
  return [match[1], match[2]].every((part) => Number(part) <= 0xffffffff);
}

function requestedFpsValid(value: string): boolean {
  if (!value.trim()) return true;
  if (!/^\d+$/.test(value.trim())) return false;
  const parsed = Number(value);
  return Number.isInteger(parsed) && parsed > 0 && parsed <= 1000;
}

function finalRefreshValid(value: string): boolean {
  if (!value.trim()) return true;
  if (!/^\d+(?:\.\d+)?$/.test(value.trim())) return false;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed > 0 && parsed <= 1000;
}

function finalValuePresent(entry: MappingEntry): boolean {
  if (props.simple || mappingType.value === 'resolution_only') {
    return Boolean(entry.final_resolution.trim());
  }
  if (mappingType.value === 'refresh_rate_only') return Boolean(entry.final_refresh_rate.trim());
  return Boolean(entry.final_resolution.trim() || entry.final_refresh_rate.trim());
}

function hasRefreshValues(entry: MappingEntry): boolean {
  return Boolean(entry.requested_fps.trim() || entry.final_refresh_rate.trim());
}

const entriesValid = computed(
  () =>
    sourceValid.value &&
    !simplePolicyConflict.value &&
    entries.value.every(
      (entry) =>
        (!showsResolution.value ||
          (resolutionValid(entry.requested_resolution) &&
            resolutionValid(entry.final_resolution))) &&
        (!showsRefresh.value ||
          (requestedFpsValid(entry.requested_fps) &&
            finalRefreshValid(entry.final_refresh_rate))) &&
        (!props.simple || Boolean(entry.requested_resolution.trim())) &&
        finalValuePresent(entry),
    ),
);

watch(entriesValid, (valid) => emit('validity-change', valid), { immediate: true });

function updateEntry(index: number, key: keyof MappingEntry, event: Event): void {
  const next = normalize(props.modelValue);
  if (props.simple) {
    const simpleEntries = simpleResolutionEntries(next);
    const current = simpleEntries[index] ?? normalizeEntry(null);
    simpleEntries[index] = {
      ...current,
      [key]: (event.target as HTMLInputElement).value,
    };
    emit('update:modelValue', applySimpleEntries(next, simpleEntries));
    return;
  }
  const current = next[mappingType.value][index] ?? normalizeEntry(null);
  next[mappingType.value][index] = {
    ...current,
    [key]: (event.target as HTMLInputElement).value,
  };
  emit('update:modelValue', next);
}

function addEntry(): void {
  const next = normalize(props.modelValue);
  if (props.simple) {
    const simpleEntries = simpleResolutionEntries(next);
    simpleEntries.push(normalizeEntry(null));
    emit('update:modelValue', applySimpleEntries(next, simpleEntries));
    return;
  }
  next[mappingType.value].push(normalizeEntry(null));
  emit('update:modelValue', next);
}

function removeEntry(index: number): void {
  const next = normalize(props.modelValue);
  if (props.simple) {
    const simpleEntries = simpleResolutionEntries(next);
    simpleEntries.splice(index, 1);
    emit('update:modelValue', applySimpleEntries(next, simpleEntries));
    return;
  }
  next[mappingType.value].splice(index, 1);
  emit('update:modelValue', next);
}

function synchronizeSimplePolicies(): void {
  const next = normalize(props.modelValue);
  emit('update:modelValue', applySimpleEntries(next, simpleResolutionEntries(next)));
}
</script>

<template>
  <div class="display-overrides">
    <datalist id="display-resolution-suggestions">
      <option
        v-for="resolution in commonResolutions"
        :key="resolution.value"
        :value="resolution.value"
      >
        {{ resolution.label }}
      </option>
    </datalist>

    <InlineAlert
      v-if="!sourceValid"
      tone="danger"
      :title="t('ui.settings.overrides.invalid_saved_title')"
    >
      {{ t('ui.settings.overrides.invalid_saved_description') }}
    </InlineAlert>

    <InlineAlert
      v-if="simplePolicyConflict"
      tone="warning"
      :title="t('ui.settings.overrides.policy_conflict_title')"
    >
      {{ t('ui.settings.overrides.policy_conflict_description') }}
    </InlineAlert>

    <InlineAlert
      v-if="simplePoliciesUnsynced"
      tone="info"
      :title="t('ui.settings.overrides.policy_unsynced_title')"
    >
      {{ t('ui.settings.overrides.policy_unsynced_description') }}
      <template #actions>
        <AppButton
          :label="t('ui.settings.overrides.policy_unsynced_action')"
          size="compact"
          variant="secondary"
          @click="synchronizeSimplePolicies"
        />
      </template>
    </InlineAlert>

    <InlineAlert
      v-if="advancedInactive"
      tone="warning"
      :title="t('ui.settings.overrides.inactive_title')"
    >
      {{ t('ui.settings.overrides.inactive_description') }}
    </InlineAlert>

    <InlineAlert
      v-if="simpleRulesMayBeShadowed"
      tone="warning"
      :title="t('ui.settings.overrides.shadowed_title')"
    >
      {{ t('ui.settings.overrides.shadowed_description') }}
    </InlineAlert>

    <InlineAlert
      v-if="manualEnforcementActive"
      tone="info"
      :title="t('ui.settings.overrides.manual_title')"
    >
      {{ t('ui.settings.overrides.manual_description') }}
    </InlineAlert>

    <p class="display-overrides__hint">
      {{ t(simple ? 'ui.settings.overrides.simple_hint' : 'ui.settings.overrides.hint') }}
    </p>

    <div v-if="sourceValid && entries.length" class="display-overrides__list">
      <article
        v-for="(entry, index) in entries"
        :key="index"
        :class="['display-override', { 'display-override--simple': simple }]"
      >
        <div class="display-override__heading">
          <strong v-if="!simple">{{
            t('ui.settings.overrides.rule', { number: index + 1 })
          }}</strong>
          <AppButton
            :label="t('ui.settings.overrides.remove')"
            :aria-label="t('ui.settings.overrides.remove_rule', { number: index + 1 })"
            icon="trash"
            icon-only
            size="compact"
            variant="tertiary"
            :disabled="editorDisabled"
            @click="removeEntry(index)"
          />
        </div>

        <div v-if="showsResolution" class="display-override__mapping">
          <label v-if="showsResolution">
            <span>{{ t('ui.settings.overrides.requested_resolution') }}</span>
            <input
              :id="`setting-dd_mode_remapping-${index}-requested-resolution`"
              class="vs-input monospace"
              type="text"
              inputmode="text"
              list="display-resolution-suggestions"
              placeholder="2560x1440"
              :value="entry.requested_resolution"
              :aria-invalid="
                (simple && !entry.requested_resolution.trim()) ||
                !resolutionValid(entry.requested_resolution)
              "
              :disabled="editorDisabled"
              @input="updateEntry(index, 'requested_resolution', $event)"
            />
            <small v-if="simple && !entry.requested_resolution.trim()">
              {{ t('ui.settings.overrides.requested_required') }}
            </small>
            <small v-if="!resolutionValid(entry.requested_resolution)">
              {{ t('ui.settings.overrides.resolution_error') }}
            </small>
          </label>

          <span class="display-override__arrow" aria-hidden="true">&rarr;</span>

          <label>
            <span>{{ t('ui.settings.overrides.final_resolution') }}</span>
            <input
              class="vs-input monospace"
              type="text"
              inputmode="text"
              list="display-resolution-suggestions"
              placeholder="3840x2160"
              :value="entry.final_resolution"
              :aria-invalid="!resolutionValid(entry.final_resolution)"
              :disabled="editorDisabled"
              @input="updateEntry(index, 'final_resolution', $event)"
            />
            <small v-if="!resolutionValid(entry.final_resolution)">
              {{ t('ui.settings.overrides.resolution_error') }}
            </small>
          </label>
        </div>

        <details
          v-if="!simple && mappingType === 'mixed'"
          class="display-override__refresh"
          :open="hasRefreshValues(entry) || undefined"
        >
          <summary>{{ t('ui.settings.overrides.refresh_optional') }}</summary>
          <div class="display-override__mapping">
            <label>
              <span>{{ t('ui.settings.overrides.requested_fps') }}</span>
              <input
                class="vs-input monospace"
                type="number"
                min="1"
                max="1000"
                step="1"
                placeholder="60"
                :value="entry.requested_fps"
                :aria-invalid="!requestedFpsValid(entry.requested_fps)"
                :disabled="editorDisabled"
                @input="updateEntry(index, 'requested_fps', $event)"
              />
              <small v-if="!requestedFpsValid(entry.requested_fps)">
                {{ t('ui.settings.overrides.requested_fps_error') }}
              </small>
            </label>

            <span class="display-override__arrow" aria-hidden="true">&rarr;</span>

            <label>
              <span>{{ t('ui.settings.overrides.final_refresh') }}</span>
              <input
                class="vs-input monospace"
                type="number"
                min="0.001"
                max="1000"
                step="0.001"
                placeholder="120"
                :value="entry.final_refresh_rate"
                :aria-invalid="!finalRefreshValid(entry.final_refresh_rate)"
                :disabled="editorDisabled"
                @input="updateEntry(index, 'final_refresh_rate', $event)"
              />
              <small v-if="!finalRefreshValid(entry.final_refresh_rate)">
                {{ t('ui.settings.overrides.refresh_error') }}
              </small>
            </label>
          </div>
        </details>

        <div v-else-if="showsRefresh" class="display-override__mapping">
          <label>
            <span>{{ t('ui.settings.overrides.requested_fps') }}</span>
            <input
              class="vs-input monospace"
              type="number"
              min="1"
              max="1000"
              step="1"
              placeholder="60"
              :value="entry.requested_fps"
              :aria-invalid="!requestedFpsValid(entry.requested_fps)"
              :disabled="editorDisabled"
              @input="updateEntry(index, 'requested_fps', $event)"
            />
            <small v-if="!requestedFpsValid(entry.requested_fps)">
              {{ t('ui.settings.overrides.requested_fps_error') }}
            </small>
          </label>

          <span class="display-override__arrow" aria-hidden="true">&rarr;</span>

          <label>
            <span>{{ t('ui.settings.overrides.final_refresh') }}</span>
            <input
              class="vs-input monospace"
              type="number"
              min="0.001"
              max="1000"
              step="0.001"
              placeholder="120"
              :value="entry.final_refresh_rate"
              :aria-invalid="!finalRefreshValid(entry.final_refresh_rate)"
              :disabled="editorDisabled"
              @input="updateEntry(index, 'final_refresh_rate', $event)"
            />
            <small v-if="!finalRefreshValid(entry.final_refresh_rate)">
              {{ t('ui.settings.overrides.refresh_error') }}
            </small>
          </label>
        </div>

        <p v-if="!finalValuePresent(entry)" class="display-override__error">
          {{ t('ui.settings.overrides.final_required') }}
        </p>
      </article>
    </div>

    <div v-else-if="sourceValid" class="display-overrides__empty">
      <strong>{{ t('ui.settings.overrides.empty_title') }}</strong>
      <span>{{ t('ui.settings.overrides.empty_description') }}</span>
    </div>

    <AppButton
      :label="t('ui.settings.overrides.add')"
      icon="plus"
      variant="secondary"
      :disabled="editorDisabled"
      @click="addEntry"
    />
  </div>
</template>

<style scoped>
.display-overrides {
  display: grid;
  gap: var(--vs-space-12);
}

.display-overrides__hint {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-body);
}

.display-overrides__list {
  display: grid;
  gap: var(--vs-space-12);
}

.display-override {
  display: grid;
  gap: var(--vs-space-12);
  padding: var(--vs-space-16);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.display-override--simple {
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: end;
  gap: var(--vs-space-12);
  padding: var(--vs-space-12);
}

.display-override--simple .display-override__heading {
  grid-column: 2;
  grid-row: 1;
  align-self: end;
  padding-bottom: 3px;
}

.display-override--simple .display-override__mapping {
  grid-column: 1;
  grid-row: 1;
}

.display-override__heading {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.display-override__mapping {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto minmax(0, 1fr);
  align-items: start;
  gap: var(--vs-space-12);
}

.display-override__mapping label {
  display: grid;
  min-width: 0;
  align-content: start;
  gap: var(--vs-space-4);
}

.display-override__mapping label > span {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-semibold);
}

.display-override__mapping input {
  width: 100%;
}

.display-override__mapping input[aria-invalid='true'] {
  border-color: var(--vs-color-status-danger);
}

.display-override__mapping small,
.display-override__error {
  color: var(--vs-color-status-danger);
  font-size: var(--vs-type-size-helper);
}

.display-override__arrow {
  align-self: end;
  padding-bottom: 10px;
  color: var(--vs-color-text-muted);
  font-size: 20px;
}

.display-override__refresh {
  padding-top: var(--vs-space-8);
  border-top: 1px solid var(--vs-color-border-subtle);
}

.display-override__refresh summary {
  color: var(--vs-color-text-secondary);
  cursor: pointer;
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-semibold);
}

.display-override__refresh[open] summary {
  margin-bottom: var(--vs-space-12);
}

.display-overrides__empty {
  display: grid;
  gap: var(--vs-space-4);
  padding: var(--vs-space-20);
  border: 1px dashed var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-text-secondary);
  text-align: center;
}

.display-overrides__empty strong {
  color: var(--vs-color-text-primary);
}

.display-overrides > :deep(.vs-button) {
  justify-self: start;
}

@media (max-width: 639px) {
  .display-override--simple {
    grid-template-columns: minmax(0, 1fr);
  }

  .display-override--simple .display-override__heading {
    grid-column: 1;
    grid-row: 1;
    justify-content: flex-end;
  }

  .display-override--simple .display-override__mapping {
    grid-column: 1;
    grid-row: 2;
  }

  .display-override__mapping {
    grid-template-columns: minmax(0, 1fr);
  }

  .display-override__arrow {
    justify-self: center;
    padding-bottom: 0;
    transform: rotate(90deg);
  }
}
</style>
