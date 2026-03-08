<template>
  <section
    class="rounded-2xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-surface/40 p-4 space-y-4"
  >
    <div class="flex flex-col gap-3 md:flex-row md:items-start md:justify-between">
      <div class="space-y-1">
        <h3 class="text-base font-semibold text-dark dark:text-light">Setting Overrides</h3>
        <p class="text-[12px] leading-relaxed opacity-70">{{ descriptionText }}</p>
      </div>
      <div class="flex flex-wrap items-center gap-2">
        <n-tag size="small" type="primary">{{ activeOverrideCount }} active</n-tag>
        <n-button size="small" type="primary" @click="openAddSettings">Add Setting</n-button>
        <n-button v-if="showResetAll" size="small" tertiary @click="clearAll">Delete All</n-button>
      </div>
    </div>

    <div class="min-w-0 space-y-3">
      <div class="flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
        <div class="space-y-1">
          <h4 class="text-xs font-semibold uppercase tracking-wide opacity-70">Active Overrides</h4>
          <p class="text-[12px] opacity-60 leading-relaxed">
            Adjust the values below to override the current global setting only for this
            {{ scopeSummaryLabel }}.
          </p>
        </div>
        <div
          class="rounded-full bg-dark/5 dark:bg-light/10 px-3 py-1 text-[11px] font-medium opacity-70"
        >
          {{ activeOverrideCount }} configured
        </div>
      </div>

      <div
        v-if="overrideEntries.length === 0"
        class="rounded-xl border border-dashed border-dark/15 dark:border-light/15 px-4 py-8 text-center space-y-3"
      >
        <div class="text-sm font-medium">No {{ scopeSummaryLabel }}-specific overrides yet.</div>
        <p class="mx-auto max-w-xl text-[12px] leading-relaxed opacity-60">
          Add settings from the picker, then tune them here using the same controls as the main
          configuration tabs.
        </p>
        <n-button size="small" type="primary" @click="openAddSettings">Add Setting</n-button>
      </div>

      <div
        v-else
        class="rounded-xl border border-dark/10 dark:border-light/10 bg-white/40 dark:bg-white/5 divide-y divide-dark/10 dark:divide-light/10"
      >
        <div v-for="entry in overrideEntries" :key="entry.key" class="px-4 py-4">
          <ConfigInputField
            v-if="isSyntheticKey(entry.key) && entry.key === SYN_KEYS.configureDisplayResolution"
            :id="entry.key"
            :label="entry.label"
            :desc="entry.desc"
            size="small"
            monospace
            placeholder="e.g. 1920x1080"
            :model-value="forcedResolution"
            @update:model-value="(v) => setForcedResolution(String(v || ''))"
          >
            <template #actions>
              <n-button size="tiny" tertiary @click="removeOverride(entry.key)">Delete</n-button>
            </template>
            <template #meta>
              <span class="hidden sm:inline">
                <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }}
              </span>
            </template>
          </ConfigInputField>

          <ConfigInputField
            v-else-if="
              isSyntheticKey(entry.key) && entry.key === SYN_KEYS.configureDisplayRefreshRate
            "
            :id="entry.key"
            :label="entry.label"
            :desc="entry.desc"
            size="small"
            monospace
            inputmode="numeric"
            placeholder="e.g. 60"
            :model-value="forcedRefreshRate"
            @update:model-value="(v) => setForcedRefreshRate(String(v || ''))"
          >
            <template #actions>
              <n-button size="tiny" tertiary @click="removeOverride(entry.key)">Delete</n-button>
            </template>
            <template #meta>
              <span class="hidden sm:inline">
                <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }}
              </span>
            </template>
          </ConfigInputField>

          <ConfigSelectField
            v-else-if="isSyntheticKey(entry.key) && entry.key === SYN_KEYS.configureDisplayHdr"
            :id="entry.key"
            :label="entry.label"
            :desc="entry.desc"
            size="small"
            :options="forcedHdrOptions"
            :model-value="forcedHdr"
            @update:model-value="(v) => setForcedHdr(String(v || ''))"
          >
            <template #actions>
              <n-button size="tiny" tertiary @click="removeOverride(entry.key)">Delete</n-button>
            </template>
            <template #meta>
              <span class="hidden sm:inline">
                <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }}
              </span>
            </template>
          </ConfigSelectField>

          <ConfigFieldRenderer
            v-else-if="editorKind(entry.key) !== 'json'"
            :setting-key="entry.key"
            :label="entry.label"
            :desc="entry.desc"
            :default-value="entry.globalValue"
            :size="'small'"
            :model-value="rawOverrideValue(entry.key)"
            :placeholder="overridePlaceholder(entry.key)"
            :filterable="editorKind(entry.key) === 'select'"
            :monospace="editorKind(entry.key) === 'string'"
            @update:model-value="(v) => setRenderedOverrideValue(entry.key, v)"
          >
            <template #actions>
              <n-button size="tiny" tertiary @click="removeOverride(entry.key)">Delete</n-button>
            </template>
            <template #meta>
              <span class="hidden sm:inline">
                <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }} ·
              </span>
              <span>
                Inherited:
                <span class="font-mono">{{ formatValueForKey(entry.key, entry.globalValue) }}</span>
              </span>
            </template>
          </ConfigFieldRenderer>

          <ConfigInputField
            v-else
            :id="entry.key"
            :label="entry.label"
            :desc="entry.desc"
            type="textarea"
            size="small"
            monospace
            :autosize="{ minRows: 2, maxRows: 10 }"
            placeholder="JSON value"
            :model-value="jsonDraft(entry.key)"
            @update:model-value="(v) => updateJsonDraft(entry.key, v)"
            @blur="() => commitJson(entry.key)"
          >
            <template #actions>
              <n-button size="tiny" tertiary @click="removeOverride(entry.key)">Delete</n-button>
            </template>
            <template #meta>
              <span class="hidden sm:inline">
                <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }} ·
              </span>
              <span>
                Inherited:
                <span class="font-mono">{{ formatValueForKey(entry.key, entry.globalValue) }}</span>
              </span>
            </template>
            <div v-if="jsonError(entry.key)" class="text-[11px] text-danger">
              {{ jsonError(entry.key) }}
            </div>
          </ConfigInputField>
        </div>
      </div>
    </div>
  </section>

  <div
    v-if="browseModalOpen"
    class="fixed inset-0 z-[1100] px-2 py-2 md:px-3 md:py-3 xl:px-5 xl:py-4"
  >
    <div class="absolute inset-0 bg-dark/50 dark:bg-black/70" />
    <div
      class="relative mx-auto flex h-full max-w-[112rem] flex-col overflow-hidden rounded-[1.75rem] border border-dark/10 dark:border-light/10 bg-white/95 shadow-2xl dark:bg-surface/95"
    >
      <div
        class="sticky top-0 z-20 border-b border-dark/10 dark:border-light/10 bg-white/95 px-4 py-4 backdrop-blur dark:bg-surface/95"
      >
        <div class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
          <div class="flex min-w-0 items-start gap-2">
            <n-button size="small" quaternary @click="cancelAddSettings">
              <i class="fas fa-arrow-left text-[12px]" />
              <span class="ml-1">Back</span>
            </n-button>
            <div class="min-w-0 space-y-1">
              <div class="text-base font-semibold text-dark dark:text-light">
                Add Setting Overrides
              </div>
              <p class="text-[12px] leading-relaxed opacity-70">
                Browse all supported settings, stage the ones you want, then save to add them to
                this {{ scopeSummaryLabel }}.
              </p>
            </div>
          </div>
          <div class="flex items-center gap-2 self-start sm:self-start">
            <n-tag size="small" :bordered="false">
              {{ modalOverrideEntries.length }} in editor
            </n-tag>
          </div>
        </div>
      </div>

      <div class="min-h-0 flex-1 overflow-hidden px-4 py-4">
        <div
          class="grid h-full min-h-0 gap-4 xl:grid-cols-[minmax(29rem,0.92fr)_minmax(40rem,1.08fr)] 2xl:grid-cols-[minmax(31rem,0.94fr)_minmax(48rem,1.12fr)]"
        >
          <aside
            class="flex min-h-0 flex-col rounded-xl border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-white/5"
          >
            <div class="border-b border-dark/10 px-4 py-4 dark:border-light/10">
              <div class="flex items-center justify-between gap-3">
                <div class="space-y-1">
                  <h4 class="text-xs font-semibold uppercase tracking-wide opacity-70">
                    Override Editor
                  </h4>
                  <p class="text-[12px] leading-relaxed opacity-60">
                    Added settings appear here immediately so you can refine them before saving.
                  </p>
                </div>
                <div
                  class="rounded-full bg-primary/10 px-3 py-1 text-[11px] font-medium text-primary"
                >
                  {{ modalOverrideEntries.length }}
                </div>
              </div>
            </div>

            <div class="app-scrollbar app-scrollbar-edge min-h-0 flex-1 overflow-y-scroll">
              <div v-if="modalOverrideEntries.length === 0" class="px-4 py-4">
                <div
                  class="rounded-xl border border-dashed border-dark/15 dark:border-light/15 px-4 py-8 text-center space-y-3"
                >
                  <div class="text-sm font-medium">No override fields in the editor yet.</div>
                  <p class="mx-auto max-w-xl text-[12px] leading-relaxed opacity-60">
                    Pick a setting from the browser on the right to add it here, then edit it before
                    saving.
                  </p>
                </div>
              </div>

              <div
                v-else
                class="m-4 rounded-xl border border-dark/10 dark:border-light/10 bg-white/60 dark:bg-surface/40 divide-y divide-dark/10 dark:divide-light/10"
              >
                <div v-for="entry in modalOverrideEntries" :key="entry.key" class="px-4 py-4">
                  <ConfigInputField
                    v-if="
                      isSyntheticKey(entry.key) && entry.key === SYN_KEYS.configureDisplayResolution
                    "
                    :id="`modal-${entry.key}`"
                    :label="entry.label"
                    :desc="entry.desc"
                    size="small"
                    monospace
                    placeholder="e.g. 1920x1080"
                    :model-value="draftForcedResolution"
                    @update:model-value="(v) => setDraftForcedResolution(String(v || ''))"
                  >
                    <template #actions>
                      <n-button size="tiny" tertiary @click="removeDraftOverride(entry.key)">
                        Delete
                      </n-button>
                    </template>
                    <template #meta>
                      <span class="hidden sm:inline">
                        <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }}
                      </span>
                    </template>
                  </ConfigInputField>

                  <ConfigInputField
                    v-else-if="
                      isSyntheticKey(entry.key) &&
                      entry.key === SYN_KEYS.configureDisplayRefreshRate
                    "
                    :id="`modal-${entry.key}`"
                    :label="entry.label"
                    :desc="entry.desc"
                    size="small"
                    monospace
                    inputmode="numeric"
                    placeholder="e.g. 60"
                    :model-value="draftForcedRefreshRate"
                    @update:model-value="(v) => setDraftForcedRefreshRate(String(v || ''))"
                  >
                    <template #actions>
                      <n-button size="tiny" tertiary @click="removeDraftOverride(entry.key)">
                        Delete
                      </n-button>
                    </template>
                    <template #meta>
                      <span class="hidden sm:inline">
                        <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }}
                      </span>
                    </template>
                  </ConfigInputField>

                  <ConfigSelectField
                    v-else-if="
                      isSyntheticKey(entry.key) && entry.key === SYN_KEYS.configureDisplayHdr
                    "
                    :id="`modal-${entry.key}`"
                    :label="entry.label"
                    :desc="entry.desc"
                    size="small"
                    :options="forcedHdrOptions"
                    :model-value="draftForcedHdr"
                    @update:model-value="(v) => setDraftForcedHdr(String(v || ''))"
                  >
                    <template #actions>
                      <n-button size="tiny" tertiary @click="removeDraftOverride(entry.key)">
                        Delete
                      </n-button>
                    </template>
                    <template #meta>
                      <span class="hidden sm:inline">
                        <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }}
                      </span>
                    </template>
                  </ConfigSelectField>

                  <ConfigFieldRenderer
                    v-else-if="editorKind(entry.key, 'draft') !== 'json'"
                    :setting-key="entry.key"
                    :label="entry.label"
                    :desc="entry.desc"
                    :default-value="entry.globalValue"
                    :size="'small'"
                    :model-value="rawOverrideValueFor('draft', entry.key)"
                    :placeholder="overridePlaceholder(entry.key, 'draft')"
                    :filterable="editorKind(entry.key, 'draft') === 'select'"
                    :monospace="editorKind(entry.key, 'draft') === 'string'"
                    @update:model-value="(v) => setRenderedOverrideValueFor('draft', entry.key, v)"
                  >
                    <template #actions>
                      <n-button size="tiny" tertiary @click="removeDraftOverride(entry.key)">
                        Delete
                      </n-button>
                    </template>
                    <template #meta>
                      <span class="hidden sm:inline">
                        <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }} ·
                      </span>
                      <span>
                        Inherited:
                        <span class="font-mono">{{
                          formatValueForKey(entry.key, entry.globalValue)
                        }}</span>
                      </span>
                    </template>
                  </ConfigFieldRenderer>

                  <ConfigInputField
                    v-else
                    :id="`modal-${entry.key}`"
                    :label="entry.label"
                    :desc="entry.desc"
                    type="textarea"
                    size="small"
                    monospace
                    :autosize="{ minRows: 2, maxRows: 10 }"
                    placeholder="JSON value"
                    :model-value="jsonDraftFor('draft', entry.key)"
                    @update:model-value="(v) => updateJsonDraftFor('draft', entry.key, v)"
                    @blur="() => commitJsonFor('draft', entry.key)"
                  >
                    <template #actions>
                      <n-button size="tiny" tertiary @click="removeDraftOverride(entry.key)">
                        Delete
                      </n-button>
                    </template>
                    <template #meta>
                      <span class="hidden sm:inline">
                        <span class="font-mono">{{ entry.key }}</span> · {{ entry.groupName }} ·
                      </span>
                      <span>
                        Inherited:
                        <span class="font-mono">{{
                          formatValueForKey(entry.key, entry.globalValue)
                        }}</span>
                      </span>
                    </template>
                    <div v-if="jsonErrorFor('draft', entry.key)" class="text-[11px] text-danger">
                      {{ jsonErrorFor('draft', entry.key) }}
                    </div>
                  </ConfigInputField>
                </div>
              </div>
            </div>
          </aside>

          <div
            class="flex min-h-0 flex-col rounded-xl border border-dark/10 dark:border-light/10 bg-white/60 dark:bg-white/5"
          >
            <div class="border-b border-dark/10 px-4 py-4 dark:border-light/10">
              <div class="space-y-3">
                <div class="flex flex-col gap-3 xl:flex-row xl:items-start xl:justify-between">
                  <div class="space-y-1">
                    <h4 class="text-xs font-semibold uppercase tracking-wide opacity-70">
                      Browse Available Settings
                    </h4>
                    <p class="text-[12px] opacity-70 leading-relaxed">
                      Explore every supported override by section. Search is optional and only
                      narrows the list.
                    </p>
                  </div>
                  <div class="self-start text-[11px] opacity-60">
                    {{ filteredAvailableCount }} showing
                    <span v-if="filteredAvailableCount !== availableEntries.length">
                      of {{ availableEntries.length }}
                    </span>
                  </div>
                </div>

                <div class="flex flex-col gap-3">
                  <n-input
                    v-model:value="searchQuery"
                    type="text"
                    clearable
                    placeholder="Filter by setting name, key, description, or option value"
                    @keydown.enter.prevent="addFirstFilteredEntry"
                  >
                    <template #suffix>
                      <i class="fas fa-magnifying-glass text-[12px] opacity-60" />
                    </template>
                  </n-input>
                  <n-button
                    v-if="hasFilterControls"
                    size="small"
                    tertiary
                    class="self-start"
                    @click="resetFilters"
                  >
                    Clear Filters
                  </n-button>
                </div>

                <div class="flex flex-wrap gap-2">
                  <button
                    type="button"
                    :class="filterChipClass(selectedGroupId === ALL_GROUPS_ID)"
                    @click="selectedGroupId = ALL_GROUPS_ID"
                  >
                    <span>All sections</span>
                    <span class="rounded-full bg-dark/5 dark:bg-light/10 px-2 py-0.5 text-[10px]">
                      {{ availableEntries.length }}
                    </span>
                  </button>
                  <button
                    v-for="group in availableGroups"
                    :key="group.id"
                    type="button"
                    :class="filterChipClass(selectedGroupId === group.id)"
                    @click="selectedGroupId = group.id"
                  >
                    <span>{{ group.name }}</span>
                    <span class="rounded-full bg-dark/5 dark:bg-light/10 px-2 py-0.5 text-[10px]">
                      {{ group.count }}
                    </span>
                  </button>
                </div>
              </div>
            </div>

            <div class="app-scrollbar app-scrollbar-edge min-h-0 flex-1 overflow-y-scroll">
              <div class="space-y-4 p-4">
                <template v-if="filteredAvailableGroups.length">
                  <section
                    v-for="group in filteredAvailableGroups"
                    :key="group.id"
                    class="space-y-2"
                  >
                    <div class="flex items-center justify-between gap-3">
                      <h4 class="text-xs font-semibold uppercase tracking-wide opacity-70">
                        {{ group.name }}
                      </h4>
                      <span class="text-[11px] opacity-50">{{ group.entries.length }}</span>
                    </div>

                    <div class="grid gap-2 xl:grid-cols-2">
                      <button
                        v-for="entry in group.entries"
                        :key="entry.key"
                        type="button"
                        class="group rounded-xl border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/40 p-3 text-left transition-colors hover:border-primary/35 hover:bg-primary/5"
                        @click="queueOverrideAddition(entry.key)"
                      >
                        <div class="flex items-start justify-between gap-3">
                          <div class="min-w-0 space-y-1">
                            <div class="text-sm font-semibold leading-snug">{{ entry.label }}</div>
                            <div class="text-[11px] opacity-60">{{ entry.groupName }}</div>
                            <div class="hidden break-all text-[11px] font-mono opacity-60 md:block">
                              {{ entry.key }}
                            </div>
                          </div>
                          <div class="flex shrink-0 items-center gap-2">
                            <span
                              class="rounded-full bg-dark/5 dark:bg-light/10 px-2 py-1 text-[10px] font-semibold uppercase tracking-wide opacity-70"
                            >
                              {{ entryTypeLabel(entry.key) }}
                            </span>
                            <span
                              class="inline-flex items-center gap-1 rounded-full border border-primary/20 bg-primary/10 px-2 py-1 text-[11px] font-medium text-primary"
                            >
                              <i class="fas fa-plus text-[10px]" />
                              Add
                            </span>
                          </div>
                        </div>

                        <p v-if="entry.desc" class="mt-2 text-[12px] opacity-70 leading-relaxed">
                          {{ entry.desc }}
                        </p>

                        <div class="mt-3 space-y-1 text-[11px] opacity-60">
                          <div v-if="!entry.synthetic">
                            Inherited:
                            <span class="font-mono">
                              {{ formatValueForKey(entry.key, entry.globalValue) }}
                            </span>
                          </div>
                          <div v-if="availableOptionsPreview(entry)" class="hidden sm:block">
                            Choices:
                            <span class="font-medium">{{ availableOptionsPreview(entry) }}</span>
                          </div>
                          <div class="hidden break-words lg:block">{{ entry.path }}</div>
                        </div>
                      </button>
                    </div>
                  </section>
                </template>

                <div
                  v-else
                  class="rounded-xl border border-dashed border-dark/15 dark:border-light/15 px-4 py-6 text-center space-y-2"
                >
                  <div class="text-sm font-medium">
                    {{
                      availableEntries.length === 0
                        ? 'All supported settings are already added.'
                        : 'No settings match the current filters.'
                    }}
                  </div>
                  <p class="text-[12px] opacity-60 leading-relaxed">
                    {{
                      availableEntries.length === 0
                        ? 'Delete an existing override to free up its setting slot.'
                        : 'Try a broader term or switch back to all sections.'
                    }}
                  </p>
                  <n-button v-if="hasFilterControls" size="small" tertiary @click="resetFilters">
                    Reset Filters
                  </n-button>
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>

      <div
        class="sticky bottom-0 z-20 border-t border-dark/10 dark:border-light/10 bg-white/95 px-4 py-3 backdrop-blur dark:bg-surface/95"
      >
        <div class="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
          <div class="text-[12px] leading-relaxed opacity-70">
            Review the override fields on the left, then save when you are done.
          </div>
          <div class="flex flex-wrap items-center justify-end gap-2">
            <n-button size="small" tertiary @click="cancelAddSettings">Cancel</n-button>
            <n-button size="small" type="primary" @click="savePendingAdditions">Save</n-button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import ConfigInputField from '@/ConfigInputField.vue';
import ConfigSelectField from '@/ConfigSelectField.vue';
import { computed, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { NButton, NInput, NTag } from 'naive-ui';
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

type AvailableGroup = {
  id: string;
  name: string;
  count: number;
};

type ScoredEntry = Entry & {
  matchScore: number;
};

type FilteredGroup = {
  id: string;
  name: string;
  entries: ScoredEntry[];
};

type EditTarget = 'live' | 'draft';

const overrides = defineModel<Record<string, unknown>>('overrides', { required: true });
const draftOverrides = ref<Record<string, unknown>>({});
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
  const scope = String(props.scopeLabel || 'application')
    .toLowerCase()
    .trim();
  if (scope === 'client') {
    return 'Override global settings for this client. Client overrides take precedence over app overrides and global config.';
  }
  return 'Override global settings for this application only. Network, security, and file-path settings are intentionally excluded.';
});

const scopeSummaryLabel = computed(() =>
  String(props.scopeLabel || 'application')
    .toLowerCase()
    .trim() === 'client'
    ? 'client'
    : 'application',
);

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

const HIDDEN_OVERRIDE_KEYS = new Set<string>([
  DD_KEYS.configurationOption,
  DD_KEYS.resolutionOption,
  DD_KEYS.manualResolution,
  DD_KEYS.refreshRateOption,
  DD_KEYS.manualRefreshRate,
  DD_KEYS.hdrOption,
  DD_KEYS.hdrRequestOverride,
]);

function isHiddenOverrideKey(key: string): boolean {
  return HIDDEN_OVERRIDE_KEYS.has(key);
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

function getOverridesSource(target: EditTarget): Record<string, unknown> {
  const source = target === 'draft' ? draftOverrides.value : overrides.value;
  if (!source || typeof source !== 'object' || Array.isArray(source)) {
    return {};
  }
  return source as Record<string, unknown>;
}

function ensureOverridesObjectFor(target: EditTarget): void {
  if (target === 'draft') {
    if (
      !draftOverrides.value ||
      typeof draftOverrides.value !== 'object' ||
      Array.isArray(draftOverrides.value)
    ) {
      draftOverrides.value = {};
    }
    return;
  }

  if (!overrides.value || typeof overrides.value !== 'object' || Array.isArray(overrides.value)) {
    overrides.value = {};
  }
}

function setOverrideKeyFor(target: EditTarget, key: string, value: unknown): void {
  ensureOverridesObjectFor(target);
  if (target === 'draft') {
    (draftOverrides.value as any)[key] = value;
    return;
  }
  (overrides.value as any)[key] = value;
}

function clearOverrideKeyFor(target: EditTarget, key: string): void {
  ensureOverridesObjectFor(target);
  try {
    if (target === 'draft') {
      delete (draftOverrides.value as any)[key];
    } else {
      delete (overrides.value as any)[key];
    }
  } catch {}
  clearJsonStateFor(target, key);
}

function setOverrideKey(key: string, value: unknown): void {
  setOverrideKeyFor('live', key, value);
}

function clearOverrideKey(key: string): void {
  clearOverrideKeyFor('live', key);
}

const overrideKeys = computed<string[]>(() => {
  return Object.keys(getOverridesSource('live')).filter(
    (k) => typeof k === 'string' && k.length > 0,
  );
});

const visibleOverrideKeys = computed<string[]>(() =>
  overrideKeys.value.filter((k) => !isHiddenOverrideKey(k)),
);

const draftOverrideKeys = computed<string[]>(() =>
  Object.keys(getOverridesSource('draft')).filter((k) => typeof k === 'string' && k.length > 0),
);

const visibleDraftOverrideKeys = computed<string[]>(() =>
  draftOverrideKeys.value.filter((k) => !isHiddenOverrideKey(k)),
);

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

function getOverrideStringFor(target: EditTarget, key: string): string | null {
  const o = getOverridesSource(target) as any;
  if (!o || typeof o !== 'object' || Array.isArray(o)) return null;
  const v = o[key];
  if (v === undefined || v === null) return null;
  return String(v);
}

function globalDdConfigDisabled(): boolean {
  const gv = getGlobalValue(DD_KEYS.configurationOption);
  return String(gv ?? 'disabled') === 'disabled';
}

function ensureDdEnabledForDisplayOverrides(target: EditTarget): void {
  if (!globalDdConfigDisabled()) return;
  const cur = getOverrideStringFor(target, DD_KEYS.configurationOption);
  if (!cur || cur === 'disabled') {
    setOverrideKeyFor(target, DD_KEYS.configurationOption, 'verify_only');
  }
}

function cleanupDdConfigurationOptionIfUnused(target: EditTarget): void {
  if (!globalDdConfigDisabled()) return;
  const o = getOverridesSource(target) as any;
  if (!o || typeof o !== 'object' || Array.isArray(o)) return;
  const ddKeys = Object.keys(o).filter((k) => k.startsWith('dd_'));
  const hasOtherDdKeys = ddKeys.some((k) => k !== DD_KEYS.configurationOption);
  if (!hasOtherDdKeys && o[DD_KEYS.configurationOption] === 'verify_only') {
    clearOverrideKeyFor(target, DD_KEYS.configurationOption);
  }
}

function isForcedResolutionActiveFor(target: EditTarget): boolean {
  if (!isWindowsPlatform()) return false;
  const opt = getOverrideStringFor(target, DD_KEYS.resolutionOption);
  if (opt === 'manual') return true;
  const o = getOverridesSource(target) as any;
  return (
    !!o && typeof o === 'object' && !Array.isArray(o) && o[DD_KEYS.manualResolution] !== undefined
  );
}

function isForcedRefreshRateActiveFor(target: EditTarget): boolean {
  if (!isWindowsPlatform()) return false;
  const opt = getOverrideStringFor(target, DD_KEYS.refreshRateOption);
  if (opt === 'manual') return true;
  const o = getOverridesSource(target) as any;
  return (
    !!o && typeof o === 'object' && !Array.isArray(o) && o[DD_KEYS.manualRefreshRate] !== undefined
  );
}

function isForcedHdrActiveFor(target: EditTarget): boolean {
  if (!isWindowsPlatform()) return false;
  const req = getOverrideStringFor(target, DD_KEYS.hdrRequestOverride);
  return req === 'force_on' || req === 'force_off';
}

const forcedResolution = computed<string>(
  () => getOverrideStringFor('live', DD_KEYS.manualResolution) ?? '',
);
const forcedRefreshRate = computed<string>(
  () => getOverrideStringFor('live', DD_KEYS.manualRefreshRate) ?? '',
);
const draftForcedResolution = computed<string>(
  () => getOverrideStringFor('draft', DD_KEYS.manualResolution) ?? '',
);
const draftForcedRefreshRate = computed<string>(
  () => getOverrideStringFor('draft', DD_KEYS.manualRefreshRate) ?? '',
);

const forcedHdrOptions = [
  { label: 'On', value: 'on' },
  { label: 'Off', value: 'off' },
];

const forcedHdr = computed<'on' | 'off'>(() => {
  const req = getOverrideStringFor('live', DD_KEYS.hdrRequestOverride);
  return req === 'force_off' ? 'off' : 'on';
});
const draftForcedHdr = computed<'on' | 'off'>(() => {
  const req = getOverrideStringFor('draft', DD_KEYS.hdrRequestOverride);
  return req === 'force_off' ? 'off' : 'on';
});

function setForcedResolutionFor(target: EditTarget, value: string): void {
  if (!isWindowsPlatform()) return;
  ensureDdEnabledForDisplayOverrides(target);
  setOverrideKeyFor(target, DD_KEYS.resolutionOption, 'manual');
  setOverrideKeyFor(target, DD_KEYS.manualResolution, String(value ?? ''));
}

function clearForcedResolutionFor(target: EditTarget): void {
  clearOverrideKeyFor(target, DD_KEYS.resolutionOption);
  clearOverrideKeyFor(target, DD_KEYS.manualResolution);
  cleanupDdConfigurationOptionIfUnused(target);
}

function setForcedRefreshRateFor(target: EditTarget, value: string): void {
  if (!isWindowsPlatform()) return;
  ensureDdEnabledForDisplayOverrides(target);
  setOverrideKeyFor(target, DD_KEYS.refreshRateOption, 'manual');
  setOverrideKeyFor(target, DD_KEYS.manualRefreshRate, String(value ?? ''));
}

function clearForcedRefreshRateFor(target: EditTarget): void {
  clearOverrideKeyFor(target, DD_KEYS.refreshRateOption);
  clearOverrideKeyFor(target, DD_KEYS.manualRefreshRate);
  cleanupDdConfigurationOptionIfUnused(target);
}

function setForcedHdrFor(target: EditTarget, value: string): void {
  if (!isWindowsPlatform()) return;
  ensureDdEnabledForDisplayOverrides(target);
  setOverrideKeyFor(target, DD_KEYS.hdrOption, 'auto');
  setOverrideKeyFor(target, DD_KEYS.hdrRequestOverride, value === 'off' ? 'force_off' : 'force_on');
}

function clearForcedHdrFor(target: EditTarget): void {
  clearOverrideKeyFor(target, DD_KEYS.hdrRequestOverride);
  clearOverrideKeyFor(target, DD_KEYS.hdrOption);
  cleanupDdConfigurationOptionIfUnused(target);
}

const activeSyntheticKeys = computed<string[]>(() => {
  const keys: string[] = [];
  if (isForcedResolutionActiveFor('live')) keys.push(SYN_KEYS.configureDisplayResolution);
  if (isForcedRefreshRateActiveFor('live')) keys.push(SYN_KEYS.configureDisplayRefreshRate);
  if (isForcedHdrActiveFor('live')) keys.push(SYN_KEYS.configureDisplayHdr);
  return keys;
});

const draftSyntheticKeys = computed<string[]>(() => {
  const keys: string[] = [];
  if (isForcedResolutionActiveFor('draft')) keys.push(SYN_KEYS.configureDisplayResolution);
  if (isForcedRefreshRateActiveFor('draft')) keys.push(SYN_KEYS.configureDisplayRefreshRate);
  if (isForcedHdrActiveFor('draft')) keys.push(SYN_KEYS.configureDisplayHdr);
  return keys;
});

const showResetAll = computed(
  () => overrideKeys.value.length > 0 || activeSyntheticKeys.value.length > 0,
);

function addSyntheticOverrideFor(target: EditTarget, key: string): void {
  if (!isWindowsPlatform()) return;
  if (key === SYN_KEYS.configureDisplayResolution) {
    setForcedResolutionFor(
      target,
      target === 'draft' ? draftForcedResolution.value : forcedResolution.value,
    );
  } else if (key === SYN_KEYS.configureDisplayRefreshRate) {
    setForcedRefreshRateFor(
      target,
      target === 'draft' ? draftForcedRefreshRate.value : forcedRefreshRate.value,
    );
  } else if (key === SYN_KEYS.configureDisplayHdr) {
    setForcedHdrFor(target, target === 'draft' ? draftForcedHdr.value : forcedHdr.value);
  }
}

function removeSyntheticOverrideFor(target: EditTarget, key: string): void {
  if (key === SYN_KEYS.configureDisplayResolution) {
    clearForcedResolutionFor(target);
  } else if (key === SYN_KEYS.configureDisplayRefreshRate) {
    clearForcedRefreshRateFor(target);
  } else if (key === SYN_KEYS.configureDisplayHdr) {
    clearForcedHdrFor(target);
  }
}

function setForcedResolution(value: string): void {
  setForcedResolutionFor('live', value);
}

function setDraftForcedResolution(value: string): void {
  setForcedResolutionFor('draft', value);
}

function setForcedRefreshRate(value: string): void {
  setForcedRefreshRateFor('live', value);
}

function setDraftForcedRefreshRate(value: string): void {
  setForcedRefreshRateFor('draft', value);
}

function setForcedHdr(value: string): void {
  setForcedHdrFor('live', value);
}

function setDraftForcedHdr(value: string): void {
  setForcedHdrFor('draft', value);
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
const ALL_GROUPS_ID = 'all';
const selectedGroupId = ref<string>(ALL_GROUPS_ID);

function normalizedSearchTerms(query: string): string[] {
  return String(query || '')
    .trim()
    .toLowerCase()
    .split(/\s+/)
    .filter(Boolean);
}

function scoreEntryMatch(entry: Entry, terms: string[]): number {
  if (!terms.length) return 0;
  const lv = entry.label.toLowerCase();
  const kv = entry.key.toLowerCase();
  const pv = entry.path.toLowerCase();
  const dv = (entry.desc || '').toLowerCase();
  const ov = (entry.optionsText || '').toLowerCase();
  let total = 0;
  for (const term of terms) {
    let score = 0;
    if (lv.includes(term)) {
      score += 100 - lv.indexOf(term);
      if (lv.startsWith(term)) score += 40;
    } else if (kv.includes(term)) {
      score += 85 - kv.indexOf(term);
      if (kv.startsWith(term)) score += 30;
    } else if (ov.includes(term)) {
      score += 55 - ov.indexOf(term) / 10;
    } else if (pv.includes(term)) {
      score += 40 - pv.indexOf(term) / 50;
    } else if (dv.includes(term)) {
      score += 20 - dv.indexOf(term) / 200;
    } else {
      return 0;
    }
    total += score;
  }
  total -= (pv.length + dv.length + ov.length) / 1500;
  return total;
}

const searchTerms = computed(() => normalizedSearchTerms(searchQuery.value));

const usedOverrideKeys = computed(
  () => new Set([...visibleOverrideKeys.value, ...activeSyntheticKeys.value]),
);
const browseModalOpen = ref(false);
const pendingAddKeys = ref<string[]>([]);
const modalUsedOverrideKeys = computed(
  () => new Set([...visibleDraftOverrideKeys.value, ...draftSyntheticKeys.value]),
);
const pickerReservedKeys = computed(() =>
  browseModalOpen.value ? modalUsedOverrideKeys.value : usedOverrideKeys.value,
);

const availableEntries = computed<Entry[]>(() =>
  allEntries.value.filter(
    (entry) => !pickerReservedKeys.value.has(entry.key) && !isHiddenOverrideKey(entry.key),
  ),
);

const groupOrder = computed(() => {
  const order = new Map<string, number>();
  for (const entry of allEntries.value) {
    if (!order.has(entry.groupId)) {
      order.set(entry.groupId, order.size);
    }
  }
  return order;
});

const availableGroups = computed<AvailableGroup[]>(() => {
  const groups = new Map<string, AvailableGroup>();
  for (const entry of availableEntries.value) {
    const existing = groups.get(entry.groupId);
    if (existing) {
      existing.count += 1;
    } else {
      groups.set(entry.groupId, {
        id: entry.groupId,
        name: entry.groupName,
        count: 1,
      });
    }
  }
  return Array.from(groups.values()).sort(
    (a, b) =>
      (groupOrder.value.get(a.id) ?? Number.MAX_SAFE_INTEGER) -
        (groupOrder.value.get(b.id) ?? Number.MAX_SAFE_INTEGER) || a.name.localeCompare(b.name),
  );
});

watch(
  availableGroups,
  (groups) => {
    if (selectedGroupId.value === ALL_GROUPS_ID) return;
    if (!groups.some((group) => group.id === selectedGroupId.value)) {
      selectedGroupId.value = ALL_GROUPS_ID;
    }
  },
  { immediate: true },
);

const filteredAvailableGroups = computed<FilteredGroup[]>(() => {
  const grouped = new Map<string, ScoredEntry[]>();
  for (const entry of availableEntries.value) {
    if (selectedGroupId.value !== ALL_GROUPS_ID && entry.groupId !== selectedGroupId.value) {
      continue;
    }
    const matchScore = searchTerms.value.length ? scoreEntryMatch(entry, searchTerms.value) : 1;
    if (searchTerms.value.length && matchScore <= 0) {
      continue;
    }
    const bucket = grouped.get(entry.groupId) ?? [];
    bucket.push({
      ...entry,
      matchScore,
    });
    grouped.set(entry.groupId, bucket);
  }

  return Array.from(grouped.entries())
    .map(([groupId, entries]) => ({
      id: groupId,
      name: entries[0]?.groupName ?? groupId,
      entries: entries.sort((a, b) =>
        searchTerms.value.length
          ? b.matchScore - a.matchScore || a.label.localeCompare(b.label)
          : a.label.localeCompare(b.label),
      ),
    }))
    .sort(
      (a, b) =>
        (groupOrder.value.get(a.id) ?? Number.MAX_SAFE_INTEGER) -
          (groupOrder.value.get(b.id) ?? Number.MAX_SAFE_INTEGER) || a.name.localeCompare(b.name),
    );
});

const filteredAvailableCount = computed(() =>
  filteredAvailableGroups.value.reduce((total, group) => total + group.entries.length, 0),
);

const hasFilterControls = computed(
  () => searchTerms.value.length > 0 || selectedGroupId.value !== ALL_GROUPS_ID,
);

function resetFilters() {
  searchQuery.value = '';
  selectedGroupId.value = ALL_GROUPS_ID;
}

function resetAddSettingsState() {
  pendingAddKeys.value = [];
  draftOverrides.value = {};
  draftJsonDrafts.value = {};
  draftJsonErrors.value = {};
  resetFilters();
}

function openAddSettings() {
  draftOverrides.value = cloneValue(overrides.value ?? {}) as Record<string, unknown>;
  pendingAddKeys.value = [];
  draftJsonDrafts.value = {};
  draftJsonErrors.value = {};
  resetFilters();
  browseModalOpen.value = true;
}

function cancelAddSettings() {
  browseModalOpen.value = false;
  resetAddSettingsState();
}

function addFirstFilteredEntry() {
  const first = filteredAvailableGroups.value[0]?.entries[0];
  if (first) {
    queueOverrideAddition(first.key);
  }
}

function addOverrideToDraft(key: string) {
  if (!isAllowedKey(key)) return;
  if (isHiddenOverrideKey(key)) return;
  if (isSyntheticKey(key)) {
    addSyntheticOverrideFor('draft', key);
    return;
  }
  ensureOverridesObjectFor('draft');
  if ((draftOverrides.value as any)[key] !== undefined) return;
  const current = getGlobalValue(key);
  (draftOverrides.value as any)[key] = cloneValue(current);
}

function queueOverrideAddition(key: string) {
  if (!isAllowedKey(key)) return;
  if (isHiddenOverrideKey(key)) return;
  if (modalUsedOverrideKeys.value.has(key)) return;
  addOverrideToDraft(key);
  if (!usedOverrideKeys.value.has(key) && !pendingAddKeys.value.includes(key)) {
    pendingAddKeys.value = [...pendingAddKeys.value, key];
  }
}

function savePendingAdditions() {
  commitAllJsonFor('draft');
  overrides.value = cloneValue(draftOverrides.value ?? {}) as Record<string, unknown>;
  browseModalOpen.value = false;
  resetAddSettingsState();
}

function removeOverride(key: string) {
  if (isSyntheticKey(key)) {
    removeSyntheticOverrideFor('live', key);
    return;
  }
  clearOverrideKey(key);
}

function removeDraftOverride(key: string) {
  if (isSyntheticKey(key)) {
    removeSyntheticOverrideFor('draft', key);
  } else {
    clearOverrideKeyFor('draft', key);
  }
  pendingAddKeys.value = pendingAddKeys.value.filter((value) => value !== key);
}

function clearAll() {
  overrides.value = {};
  jsonDrafts.value = {};
  jsonErrors.value = {};
}

function mapEntries(keys: string[]): Entry[] {
  const byKey = new Map(allEntries.value.map((e) => [e.key, e] as const));
  return Array.from(new Set(keys))
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
}

const overrideEntries = computed<Entry[]>(() =>
  mapEntries([...visibleOverrideKeys.value, ...activeSyntheticKeys.value]),
);

const modalOverrideEntries = computed<Entry[]>(() =>
  mapEntries([...visibleDraftOverrideKeys.value, ...draftSyntheticKeys.value]),
);

const activeOverrideCount = computed(() => overrideEntries.value.length);

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

function rawOverrideValueFor(target: EditTarget, key: string): unknown {
  return (getOverridesSource(target) as any)?.[key];
}

function rawOverrideValue(key: string): unknown {
  return rawOverrideValueFor('live', key);
}

function entryTypeLabel(key: string): string {
  if (isSyntheticKey(key)) return 'Shortcut';
  switch (editorKind(key, browseModalOpen.value ? 'draft' : 'live')) {
    case 'boolean':
      return 'Toggle';
    case 'select':
      return 'Choice';
    case 'number':
      return 'Number';
    case 'json':
      return 'JSON';
    default:
      return 'Text';
  }
}

function availableOptionsPreview(entry: Entry): string {
  if (!entry.options.length) return '';
  const preview = entry.options
    .slice(0, 4)
    .map((option) => String(option.label || option.value || '').trim())
    .filter(Boolean);
  if (!preview.length) return '';
  if (entry.options.length > preview.length) {
    return `${preview.join(', ')} +${entry.options.length - preview.length} more`;
  }
  return preview.join(', ');
}

function filterChipClass(active: boolean): string[] {
  return [
    'inline-flex shrink-0 items-center gap-2 rounded-full border px-3 py-1.5 text-[11px] font-medium transition-colors',
    active
      ? 'border-primary/35 bg-primary/10 text-primary shadow-sm'
      : 'border-dark/10 dark:border-light/10 bg-light/80 dark:bg-surface/60 hover:border-primary/25 hover:text-primary',
  ];
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

function selectOptions(key: string, target: EditTarget = 'live'): OverrideSelectOption[] {
  const cur = rawOverrideValueFor(target, key);
  const global = getGlobalValue(key);
  const currentValue = cur !== undefined ? cur : global;
  return getOverrideSelectOptions(key, {
    t,
    platform: platformKey(),
    metadata: getMetadataState(),
    currentValue,
  });
}

function editorKind(
  key: string,
  target: EditTarget = 'live',
): 'boolean' | 'select' | 'number' | 'string' | 'json' {
  const opts = selectOptions(key, target);
  if (opts && opts.length) return 'select';

  const gv = getGlobalValue(key);
  if (NUMERIC_OVERRIDE_KEYS.has(key)) return 'number';
  if (typeof gv === 'number') return 'number';
  if (boolPairFromValue(gv)) return 'boolean';
  if (typeof gv === 'string') return 'string';
  if (gv && typeof gv === 'object') return 'json';

  const ov = rawOverrideValueFor(target, key);
  if (typeof ov === 'number') return 'number';
  if (boolPairFromValue(ov)) return 'boolean';
  if (typeof ov === 'string') return 'string';
  if (ov && typeof ov === 'object') return 'json';
  return 'string';
}

function overridePlaceholder(key: string, target: EditTarget = 'live'): string {
  switch (editorKind(key, target)) {
    case 'number':
      return '(number)';
    case 'string':
      return '(value)';
    default:
      return '';
  }
}

function setRenderedOverrideValueFor(target: EditTarget, key: string, value: unknown): void {
  const kind = editorKind(key, target);
  if (value === null || value === undefined) {
    if (kind === 'number' || kind === 'select') {
      if (target === 'draft') {
        removeDraftOverride(key);
      } else {
        removeOverride(key);
      }
      return;
    }
    setOverrideKeyFor(target, key, value);
    return;
  }

  if (kind === 'number') {
    if (typeof value === 'number' && Number.isFinite(value)) {
      setOverrideKeyFor(target, key, value);
      return;
    }
    if (typeof value === 'string') {
      const parsed = Number(value);
      if (Number.isFinite(parsed)) {
        setOverrideKeyFor(target, key, parsed);
      }
    }
    return;
  }

  if (kind === 'string') {
    setOverrideKeyFor(target, key, String(value));
    return;
  }

  setOverrideKeyFor(target, key, value);
}

function setRenderedOverrideValue(key: string, value: unknown): void {
  setRenderedOverrideValueFor('live', key, value);
}

const jsonDrafts = ref<Record<string, string>>({});
const jsonErrors = ref<Record<string, string>>({});
const draftJsonDrafts = ref<Record<string, string>>({});
const draftJsonErrors = ref<Record<string, string>>({});

function clearJsonStateFor(target: EditTarget, key: string) {
  const drafts = target === 'draft' ? draftJsonDrafts : jsonDrafts;
  const errors = target === 'draft' ? draftJsonErrors : jsonErrors;
  const d = { ...drafts.value };
  const e = { ...errors.value };
  delete d[key];
  delete e[key];
  drafts.value = d;
  errors.value = e;
}

function clearJsonState(key: string) {
  clearJsonStateFor('live', key);
}

function jsonDraftFor(target: EditTarget, key: string): string {
  const drafts = target === 'draft' ? draftJsonDrafts : jsonDrafts;
  if (Object.prototype.hasOwnProperty.call(drafts.value, key)) {
    return drafts.value[key] ?? '';
  }
  const cur = rawOverrideValueFor(target, key);
  let text = '';
  try {
    text = JSON.stringify(cur, null, 2);
  } catch {
    text = String(cur ?? '');
  }
  drafts.value = { ...drafts.value, [key]: text };
  return text;
}

function jsonDraft(key: string): string {
  return jsonDraftFor('live', key);
}

function updateJsonDraftFor(target: EditTarget, key: string, value: string) {
  const drafts = target === 'draft' ? draftJsonDrafts : jsonDrafts;
  drafts.value = { ...drafts.value, [key]: String(value ?? '') };
}

function updateJsonDraft(key: string, value: string) {
  updateJsonDraftFor('live', key, value);
}

function jsonErrorFor(target: EditTarget, key: string): string {
  const errors = target === 'draft' ? draftJsonErrors : jsonErrors;
  return errors.value[key] || '';
}

function jsonError(key: string): string {
  return jsonErrorFor('live', key);
}

function commitJsonFor(target: EditTarget, key: string) {
  const drafts = target === 'draft' ? draftJsonDrafts : jsonDrafts;
  const errors = target === 'draft' ? draftJsonErrors : jsonErrors;
  const raw = (drafts.value[key] ?? '').trim();
  if (!raw) {
    if (target === 'draft') {
      removeDraftOverride(key);
    } else {
      removeOverride(key);
    }
    errors.value = { ...errors.value, [key]: '' };
    return;
  }
  try {
    const parsed = JSON.parse(raw);
    setOverrideKeyFor(target, key, parsed);
    errors.value = { ...errors.value, [key]: '' };
  } catch (e: any) {
    errors.value = {
      ...errors.value,
      [key]: e?.message ? String(e.message) : 'Invalid JSON',
    };
  }
}

function commitJson(key: string) {
  commitJsonFor('live', key);
}

function commitAllJsonFor(target: EditTarget) {
  const drafts = target === 'draft' ? draftJsonDrafts.value : jsonDrafts.value;
  for (const key of Object.keys(drafts)) {
    commitJsonFor(target, key);
  }
}
</script>
