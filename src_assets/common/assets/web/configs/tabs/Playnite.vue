<template>
  <div class="space-y-8 playnite-tab">
    <n-alert v-if="platform && platform !== 'windows'" type="info" :show-icon="true">
      {{ $t('playnite.only_windows') }}
    </n-alert>

    <section v-if="platform === 'windows'" class="space-y-3">
      <h3 class="text-sm font-semibold uppercase tracking-wider">
        {{ $t('playnite.status_title') }}
      </h3>
      <div
        class="bg-light/70 dark:bg-surface/70 border border-dark/10 dark:border-light/10 rounded-lg p-4 space-y-4 playnite-card"
      >
        <!-- Integration is always on; no enable/disable toggle -->
        <div class="text-sm grid md:grid-cols-3 gap-3">
          <div class="flex items-center gap-2">
            <b>{{ $t('playnite.status_overall') }}</b>
            <n-tooltip v-if="statusKind === 'waiting'" trigger="hover">
              <template #trigger>
                <n-tag size="small" :type="statusType">{{ statusText }}</n-tag>
              </template>
              <span>{{ $t('playnite.limited_tooltip') }}</span>
            </n-tooltip>
            <n-tag v-else size="small" :type="statusType">{{ statusText }}</n-tag>
          </div>
        </div>
        <n-alert v-if="pluginOutdated" type="warning" :show-icon="true">
          {{
            $t('playnite.plugin_outdated', {
              installed: status.plugin_version || '?',
              latest: status.plugin_latest || '?',
            })
          }}
        </n-alert>
        <div class="text-xs opacity-80" v-if="diagnosticText">
          {{ diagnosticText }}
        </div>
        <div class="flex items-center gap-2">
          <n-button
            v-if="canLaunch"
            size="small"
            type="primary"
            strong
            :loading="launching"
            @click="launchPlaynite"
          >
            <i class="fas fa-rocket" />
            <span class="ml-2">{{ $t('playnite.launch_button') || 'Launch Playnite' }}</span>
          </n-button>
          <n-button size="small" type="primary" strong @click="refreshStatus">
            <i class="fas fa-sync" />
            <span class="ml-2">{{ $t('playnite.refresh_status') || 'Refresh Status' }}</span>
          </n-button>
        </div>

        <!-- Merged maintenance details -->
        <div class="text-sm flex items-center gap-2 inline-info" v-if="status.extensions_dir">
          <b class="shrink-0">{{ $t('playnite.extensions_dir') }}:</b>
          <code
            class="text-xs whitespace-nowrap overflow-x-auto px-1 rounded bg-black/5 dark:bg-white/5"
            >{{ status.extensions_dir }}</code
          >
          <n-button size="tiny" type="default" strong @click="copyExtensionsPath">
            <i class="fas fa-copy" />
            <span class="ml-1">{{ $t('playnite.copy_path') || 'Copy' }}</span>
          </n-button>
        </div>
        <div class="text-sm flex items-center gap-2 inline-info" v-if="status.plugin_version">
          <b>{{ $t('playnite.plugin_version') || 'Plugin' }}:</b>
          <n-tag size="small" type="default">v{{ status.plugin_version }}</n-tag>
          <template v-if="status.plugin_latest">
            <span class="opacity-70">→</span>
            <n-tag size="small" :type="pluginOutdated ? 'warning' : 'success'"
              >v{{ status.plugin_latest }}</n-tag
            >
          </template>
        </div>
        <div class="pt-2 border-t border-dark/10 dark:border-light/10 mt-2">
          <div class="flex items-center justify-end gap-2 playnite-actions">
            <PlayniteReinstallButton
              v-if="status.extensions_dir"
              size="small"
              :strong="true"
              :restart="true"
              :label="
                status.installed
                  ? pluginOutdated
                    ? ($t('playnite.upgrade_button') as any) || 'Upgrade Plugin'
                    : ($t('playnite.reinstall_button') as any) ||
                      ($t('playnite.repair_button') as any) ||
                      'Reinstall Plugin'
                  : ($t('playnite.install_button') as any) || 'Install Plugin'
              "
              @done="onReinstallDone"
            />
            <n-button
              v-if="status.extensions_dir && status.installed"
              size="small"
              type="error"
              strong
              :loading="uninstalling"
              @click="openUninstallConfirm"
            >
              <i class="fas fa-trash" />
              <span class="ml-2">{{ $t('playnite.uninstall_button') || 'Uninstall Plugin' }}</span>
            </n-button>
          </div>
        </div>
      </div>
    </section>
    <section v-if="platform === 'windows'" class="space-y-6">
      <h3 class="text-sm font-semibold uppercase tracking-wider">
        {{ $t('playnite.settings_title') }}
      </h3>

      <!-- Auto-sync card -->
      <div
        class="bg-light/70 dark:bg-surface/70 border border-dark/10 dark:border-light/10 rounded-lg section-card"
      >
        <div class="px-4 pt-3 pb-2 flex items-baseline justify-between section-header">
          <h4 class="text-sm font-semibold">
            {{ $t('playnite.section_auto_sync') || 'Auto-sync' }}
          </h4>
          <n-button size="tiny" type="default" strong @click="resetAutoSyncSection">
            <i class="fas fa-undo" />
            <span class="ml-1">{{ $t('playnite.reset_defaults') || 'Reset to defaults' }}</span>
          </n-button>
        </div>
        <div class="px-4 pb-4 section-body">
          <div class="grid grid-cols-1 md:grid-cols-2 gap-x-6 gap-y-3 items-start">
            <div>
              <Checkbox
                v-model="config.playnite_auto_sync"
                id="playnite_auto_sync"
                :default="store.defaults.playnite_auto_sync"
                :localePrefix="'playnite'"
                label="playnite.auto_sync"
                :desc="''"
              />
              <div v-if="!autoSyncEnabled" class="form-text">
                {{
                  $t('playnite.enable_autosync_hint') || 'Enable Auto-sync to edit these settings.'
                }}
              </div>
            </div>
            <div>
              <label for="playnite_recent_games" class="form-label">{{
                $t('playnite.recent_games')
              }}</label>
              <n-input-number
                id="playnite_recent_games"
                v-model:value="config.playnite_recent_games"
                :min="0"
                :max="50"
                :show-button="true"
                class="w-32"
                :disabled="!autoSyncEnabled"
              />
              <div class="form-text">
                {{ $t('playnite.recent_games_desc') }} (0 =
                {{ $t('_common.disabled') || 'disabled' }})
              </div>
            </div>
            <div>
              <label for="playnite_recent_max_age_days" class="form-label">{{
                $t('playnite.recent_max_age_days')
              }}</label>
              <n-input-number
                id="playnite_recent_max_age_days"
                v-model:value="config.playnite_recent_max_age_days"
                :min="0"
                :max="3650"
                :show-button="true"
                class="w-32"
                :disabled="!autoSyncEnabled"
              />
              <div class="form-text">
                {{ $t('playnite.recent_max_age_days_desc') }} (0 =
                {{ $t('_common.disabled') || 'disabled' }})
              </div>
            </div>
            <div class="md:col-span-1">
              <label for="playnite_sync_categories" class="form-label">{{
                $t('playnite.sync_categories')
              }}</label>
              <n-tooltip :disabled="!disablePlayniteSelection && autoSyncEnabled" trigger="hover">
                <template #trigger>
                  <n-select
                    id="playnite_sync_categories"
                    v-model:value="selectedCategories"
                    multiple
                    :options="categoryOptions"
                    filterable
                    tag
                    clearable
                    :placeholder="
                      $t('playnite.categories_placeholder') || 'All categories (default)'
                    "
                    :loading="categoriesLoading"
                    :disabled="disablePlayniteSelection || !autoSyncEnabled"
                    @focus="() => loadCategories()"
                    class="w-full"
                  />
                </template>
                <span>{{
                  !autoSyncEnabled
                    ? $t('playnite.enable_autosync_hint') ||
                      'Enable Auto-sync to edit these settings.'
                    : disabledHint
                }}</span>
              </n-tooltip>
              <div class="form-text">{{ $t('playnite.sync_categories_help') }}</div>
            </div>
            <div class="md:col-span-2 grid grid-cols-1 md:grid-cols-2 gap-x-6 gap-y-3">
              <div>
                <label for="playnite_autosync_delete_after_days" class="form-label">{{
                  $t('playnite.delete_after_days')
                }}</label>
                <n-input-number
                  id="playnite_autosync_delete_after_days"
                  v-model:value="config.playnite_autosync_delete_after_days"
                  :min="0"
                  :max="3650"
                  :show-button="true"
                  class="w-32"
                  :disabled="!autoSyncEnabled"
                />
                <div class="form-text">
                  {{ $t('playnite.delete_after_days_desc') }} (0 =
                  {{ $t('_common.disabled') || 'disabled' }})
                </div>
              </div>
              <div>
                <label class="form-label" for="playnite_cleanup_policy">{{
                  $t('playnite.cleanup_policy') || 'Cleanup policy'
                }}</label>
                <n-radio-group
                  id="playnite_cleanup_policy"
                  v-model:value="config.playnite_autosync_require_replacement"
                  :disabled="!autoSyncEnabled"
                >
                  <div class="flex flex-col gap-1 text-sm">
                    <label class="flex items-center gap-2">
                      <n-radio :value="true" />
                      <span>{{
                        $t('playnite.policy_keep_until_replaced') || 'Keep until replaced (default)'
                      }}</span>
                    </label>
                    <label class="flex items-center gap-2">
                      <n-radio :value="false" />
                      <span>{{
                        $t('playnite.policy_prune_immediately') ||
                        'Always prune games that no longer qualify'
                      }}</span>
                    </label>
                  </div>
                </n-radio-group>
                <div class="form-text">
                  {{
                    $t('playnite.policy_explainer') ||
                    'Choose how Sunshine removes old auto-synced games.'
                  }}
                </div>
              </div>
              <div class="md:col-span-2 form-text" v-if="autoSyncEnabled">{{ policySummary }}</div>
            </div>
          </div>
        </div>
      </div>

      <!-- Launch Behavior card -->
      <div
        class="bg-light/70 dark:bg-surface/70 border border-dark/10 dark:border-light/10 rounded-lg section-card"
      >
        <div class="px-4 pt-3 pb-2 flex items-baseline justify-between section-header">
          <h4 class="text-sm font-semibold">
            {{ $t('playnite.section_launch_behavior') || 'Launch Behavior' }}
          </h4>
          <n-button size="tiny" type="default" strong @click="resetLaunchSection">
            <i class="fas fa-undo" />
            <span class="ml-1">{{ $t('playnite.reset_defaults') || 'Reset to defaults' }}</span>
          </n-button>
        </div>
        <div class="px-4 pb-4 section-body">
          <div class="grid grid-cols-1 md:grid-cols-2 gap-x-6 gap-y-3 items-start">
            <div class="md:col-span-2">
              <Checkbox
                v-model="config.playnite_fullscreen_entry_enabled"
                id="playnite_fullscreen_entry_enabled"
                :default="store.defaults.playnite_fullscreen_entry_enabled"
                :localePrefix="'playnite'"
                label="Add 'Playnite (Fullscreen)' to Applications"
                desc="When enabled, Sunshine adds a launcher entry that opens Playnite in fullscreen desktop mode."
              />
            </div>
            <div>
              <label for="playnite_focus_attempts" class="form-label">{{
                $t('playnite.focus_attempts') || 'Auto-focus attempts'
              }}</label>
              <n-input-number
                id="playnite_focus_attempts"
                v-model:value="config.playnite_focus_attempts"
                :min="0"
                :max="30"
                :show-button="true"
                class="w-32"
              />
              <div class="form-text">
                {{
                  $t('playnite.focus_attempts_help') ||
                  'Number of times to try to bring Playnite windows to the foreground when launching.'
                }}
              </div>
            </div>
            <div>
              <label for="playnite_focus_timeout_secs" class="form-label">{{
                $t('playnite.focus_timeout_secs') || 'Auto-focus timeout window (seconds)'
              }}</label>
              <n-input-number
                id="playnite_focus_timeout_secs"
                v-model:value="config.playnite_focus_timeout_secs"
                :min="0"
                :max="120"
                :show-button="true"
                class="w-32"
              />
              <div class="form-text">
                {{
                  $t('playnite.focus_timeout_secs_help') ||
                  'How long auto-focus runs while re-applying focus (0 to disable).'
                }}
              </div>
            </div>
            <div class="md:col-span-2">
              <Checkbox
                v-model="config.playnite_focus_exit_on_first"
                id="playnite_focus_exit_on_first"
                :default="store.defaults.playnite_focus_exit_on_first"
                :localePrefix="'playnite'"
                label="playnite.focus_exit_on_first"
                desc="playnite.focus_exit_on_first_help"
              />
            </div>
          </div>
        </div>
      </div>

      <!-- Exclusions & Filters card -->
      <div
        class="bg-light/70 dark:bg-surface/70 border border-dark/10 dark:border-light/10 rounded-lg"
      >
        <div class="px-4 pt-3 pb-2 flex items-baseline justify-between">
          <h4 class="text-sm font-semibold">
            {{ $t('playnite.section_exclusions_filters') || 'Exclusions & Filters' }}
          </h4>
          <n-button size="tiny" type="default" strong @click="resetFiltersSection">
            <i class="fas fa-undo" />
            <span class="ml-1">{{ $t('playnite.reset_defaults') || 'Reset to defaults' }}</span>
          </n-button>
        </div>
        <div class="px-4 pb-4">
          <div class="grid grid-cols-1 md:grid-cols-2 gap-x-6 gap-y-3 items-start">
            <div>
              <label for="playnite_exclude_categories" class="form-label">{{
                $t('playnite.exclude_categories') || 'Exclude categories'
              }}</label>
              <n-tooltip :disabled="!disablePlayniteSelection && autoSyncEnabled" trigger="hover">
                <template #trigger>
                  <n-select
                    id="playnite_exclude_categories"
                    v-model:value="excludedCategories"
                    multiple
                    :options="categoryOptions"
                    filterable
                    tag
                    clearable
                    :placeholder="
                      $t('playnite.categories_placeholder') || 'All categories (default)'
                    "
                    :loading="categoriesLoading"
                    :disabled="disablePlayniteSelection || !autoSyncEnabled"
                    @focus="() => loadCategories()"
                    class="w-full"
                  />
                </template>
                <span>{
                  !autoSyncEnabled
                    ? $t('playnite.enable_autosync_hint') ||
                      'Enable Auto-sync to edit these settings.'
                    : disabledHint
                }}</span>
              </n-tooltip>
              <div class="form-text">
                {{
                  $t('playnite.exclude_categories_help') ||
                    'Games tagged with these categories will never be auto-synced.'
                }}
              </div>
            </div>
            <div>
              <label for="playnite_exclude_plugins" class="form-label">{{
                $t('playnite.exclude_plugins') || 'Exclude library plugins'
              }}</label>
              <n-tooltip :disabled="!disablePlayniteSelection && autoSyncEnabled" trigger="hover">
                <template #trigger>
                  <n-select
                    id="playnite_exclude_plugins"
                    v-model:value="excludedPlugins"
                    multiple
                    :options="pluginOptions"
                    filterable
                    clearable
                    :placeholder="
                      $t('playnite.plugins_placeholder') || 'All library plugins (default)'
                    "
                    :loading="pluginsLoading"
                    :disabled="disablePlayniteSelection || !autoSyncEnabled"
                    @focus="() => loadPlugins()"
                    class="w-full"
                  />
                </template>
                <span>{
                  !autoSyncEnabled
                    ? $t('playnite.enable_autosync_hint') ||
                      'Enable Auto-sync to edit these settings.'
                    : disabledHint
                }}</span>
              </n-tooltip>
              <div class="form-text">
                {{
                  $t('playnite.exclude_plugins_help') ||
                    'Games imported from these plugins will never be auto-synced.'
                }}
              </div>
            </div>
            <div class="md:col-span-2">
              <div class="flex flex-col gap-2">
                <label class="form-label">{{
                  $t('playnite.exclude_games') || 'Exclude games from auto-sync'
                }}</label>
                <div class="text-[12px]">
                  <n-alert v-if="limitedNoCache" type="warning" :show-icon="true">
                    {{
                      $t('playnite.games_unavailable_indicator') ||
                      'Cannot retrieve Playnite games right now. Start Playnite to load games.'
                    }}
                  </n-alert>
                  <n-alert v-else-if="limitedWithCache" type="info" :show-icon="true">
                    {{
                      $t('playnite.games_cached_indicator') ||
                      'Showing cached Playnite games due to limited connectivity.'
                    }}
                  </n-alert>
                </div>
                <div class="playnite-exclusions">
                  <div class="flex items-center justify-between mb-2">
                    <div class="text-sm font-medium">
                      {{ $t('playnite.exclude_games_table_title') || 'Excluded Games' }}
                    </div>
                    <div class="flex items-center gap-2">
                      <n-button
                        size="small"
                        type="default"
                        strong
                        @click="openAddExclusions"
                        :disabled="disablePlayniteSelection"
                      >
                        <i class="fas fa-plus" />
                        <span class="ml-1">{{ $t('playnite.add_exclusions') || 'Add' }}</span>
                      </n-button>
                      <n-button
                        size="small"
                        type="default"
                        strong
                        @click="clearAllExclusions"
                        :disabled="!excludedIds.length"
                      >
                        <i class="fas fa-times" />
                        <span class="ml-1">{{ $t('_common.clear_all') || 'Clear All' }}</span>
                      </n-button>
                    </div>
                  </div>
                  <n-data-table
                    :columns="exclusionsColumns"
                    :data="excludedDisplayList"
                    :bordered="true"
                    :single-line="false"
                    :pagination="false"
                    size="small"
                  />
                </div>
                <div class="text-[11px] opacity-60">
                  <span v-if="gamesSource === 'live'">{{
                    $t('playnite.games_loaded_live') || 'Loaded from Playnite'
                  }}</span>
                  <span v-else-if="gamesSource === 'cache'"
                    >{{ $t('playnite.games_loaded_cache') || 'Loaded from cache'
                    }}<template v-if="gamesCacheTime">
                      — {{ new Date(gamesCacheTime).toLocaleString() }}</template
                    ></span
                  >
                  <span v-else>{{
                    $t('playnite.games_not_available') ||
                    'No games available. Start Playnite to fetch games.'
                  }}</span>
                </div>
                <div class="form-text">
                  {{
                    $t('playnite.exclude_games_desc') ||
                    'Selected games will not be auto-synced from Playnite.'
                  }}
                </div>
                <div class="form-text">
                  {{ $t('playnite.exclusions_override_note') || 'Exclusions override categories.' }}
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>
  </div>
  <!-- Uninstall confirmation -->
  <n-modal :show="showUninstallConfirm" @update:show="(v) => (showUninstallConfirm = v)">
    <n-card :bordered="false" style="max-width: 32rem; width: 100%">
      <template #header>
        <div class="flex items-center gap-2">
          <i class="fas fa-trash" />
          <span>{{ $t('playnite.uninstall_button') || 'Uninstall Plugin' }}</span>
        </div>
      </template>
      <div class="text-sm">
        {{
          $t('playnite.uninstall_requires_restart') ||
          'Uninstalling the Playnite plugin may require restarting Playnite. Continue?'
        }}
      </div>
      <template #footer>
        <div class="w-full flex items-center justify-center gap-3">
          <n-button type="default" strong @click="showUninstallConfirm = false">{{
            $t('_common.cancel')
          }}</n-button>
          <n-button type="error" strong :loading="uninstalling" @click="confirmUninstall">{{
            $t('_common.continue') || 'Continue'
          }}</n-button>
        </div>
      </template>
    </n-card>
  </n-modal>

  <!-- Add Exclusions modal -->
  <n-modal :show="showAddModal" @update:show="(v) => (showAddModal = v)">
    <n-card
      :bordered="false"
      style="max-width: 40rem; width: 100%; height: auto; max-height: calc(100dvh - 2rem)"
    >
      <template #header>
        <div class="flex items-center gap-2">
          <i class="fas fa-list-check" />
          <span>{{ $t('playnite.add_exclusions') || 'Add Exclusions' }}</span>
        </div>
      </template>
      <div class="space-y-2">
        <n-select
          v-model:value="addSelection"
          :options="addOptions"
          multiple
          filterable
          clearable
          :loading="gamesLoading"
          :disabled="disablePlayniteSelection"
          :placeholder="$t('playnite.add_exclusions_placeholder') || 'Search and select games'"
          class="w-full"
          @focus="() => loadGames()"
        />
        <div class="text-[12px] opacity-70">
          {{
            $t('playnite.add_exclusions_hint') ||
            'Pick one or more games to exclude from auto-sync.'
          }}
        </div>
      </div>
      <template #footer>
        <div class="w-full flex items-center justify-center gap-3">
          <n-button type="default" strong @click="showAddModal = false">{{
            $t('_common.cancel')
          }}</n-button>
          <n-button type="primary" :disabled="!addSelection.length" @click="confirmAddExclusions">{{
            $t('_common.add') || 'Add'
          }}</n-button>
        </div>
      </template>
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref, onUnmounted, watch, h } from 'vue';
import {
  NInputNumber,
  NSelect,
  NButton,
  NAlert,
  NTag,
  NTooltip,
  NModal,
  NCard,
  NRadioGroup,
  NRadio,
  NDataTable,
  useNotification,
} from 'naive-ui';
import { useI18n } from 'vue-i18n';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { http } from '@/http';
import PlayniteReinstallButton from '@/components/PlayniteReinstallButton.vue';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const platform = computed(() =>
  (metadata.value?.platform || config.value?.platform || '').toLowerCase(),
);
const { t } = useI18n();

const status = reactive<{
  installed: boolean;
  installed_unknown?: boolean;
  active: boolean;
  enabled?: boolean;
  playnite_running?: boolean;
  extensions_dir: string;
  plugin_version?: string;
  plugin_latest?: string;
}>({ installed: false, active: false, extensions_dir: '' });
const launching = ref(false);
const uninstalling = ref(false);
const showUninstallConfirm = ref(false);
// Naive UI notifications for transient messages
const notification = useNotification();
function notify(type: 'success' | 'error' | 'info' | 'warning', content: string) {
  notification.create({ type, content, duration: 5000 });
}

const categoriesLoading = ref(false);
const pluginsLoading = ref(false);
const gamesLoading = ref(false);
const categoryOptions = ref<{ label: string; value: string }[]>([]);
const pluginOptions = ref<{ label: string; value: string }[]>([]);
type GameRow = {
  id: string;
  name: string;
  installed?: boolean;
  categories?: string[];
  pluginId?: string;
  pluginName?: string;
};
const gamesList = ref<GameRow[]>([]);
const gamesSource = ref<'live' | 'cache' | 'none'>('none');
const gamesCacheTime = ref<number | null>(null);
// Dual-list transfer value mirrors the excluded IDs
const transferValue = ref<string[]>([]);

const selectedCategories = computed<string[]>({
  get() {
    const arr = (config.value?.playnite_sync_categories || []) as Array<{
      id: string;
      name: string;
    }>;
    return (arr || []).map((o) => o.id || o.name || '').filter(Boolean);
  },
  set(v: string[]) {
    const mapByVal = new Map(categoryOptions.value.map((o) => [o.value, o.label] as const));
    const next = (v || []).map((val) => ({
      id: val && mapByVal.has(val) ? val : '',
      name: mapByVal.get(val) || val,
    }));
    store.updateOption('playnite_sync_categories', next);
  },
});

const excludedCategories = computed<string[]>({
  get() {
    const arr = (config.value?.playnite_exclude_categories || []) as Array<{
      id: string;
      name: string;
    }>;
    return (arr || []).map((o) => o.id || o.name || '').filter(Boolean);
  },
  set(v: string[]) {
    const mapByVal = new Map(categoryOptions.value.map((o) => [o.value, o.label] as const));
    const next = (v || []).map((val) => ({
      id: val && mapByVal.has(val) ? val : '',
      name: mapByVal.get(val) || val,
    }));
    store.updateOption('playnite_exclude_categories', next);
  },
});

const excludedPlugins = computed<string[]>({
  get() {
    const arr = (config.value?.playnite_exclude_plugins || []) as Array<{
      id: string;
      name: string;
    }>;
    return (arr || []).map((o) => o.id || o.name || '').filter(Boolean);
  },
  set(v: string[]) {
    const mapByVal = new Map(pluginOptions.value.map((o) => [o.value, o.label] as const));
    const next = (v || []).map((val) => ({
      id: val && mapByVal.has(val) ? val : '',
      name: mapByVal.get(val) || val,
    }));
    store.updateOption('playnite_exclude_plugins', next);
  },
});

const excludedIds = computed<string[]>({
  get() {
    const arr = (config.value?.playnite_exclude_games || []) as Array<{ id: string; name: string }>;
    return (arr || []).map((o) => o.id).filter(Boolean);
  },
  set(v: string[]) {
    const nameById = new Map(gamesList.value.map((g) => [g.id, g.name] as const));
    const next = (v || []).map((id) => ({ id, name: nameById.get(id) || '' }));
    store.updateOption('playnite_exclude_games', next);
  },
});

// Build the display list of current exclusions, resolving names from cache if missing
const excludedDisplayList = computed<Array<{ id: string; name: string }>>(() => {
  const arr = (config.value?.playnite_exclude_games || []) as Array<{ id: string; name: string }>;
  const nameById = new Map(gamesList.value.map((g) => [g.id, g.name] as const));
  return (arr || []).map(({ id, name }) => ({ id, name: name || nameById.get(id) || '' }));
});

// Connectivity indicator helpers for transfer UI
const limitedConnectivity = computed(() => statusKind.value !== 'active');
const hasCachedGames = computed(
  () =>
    gamesSource.value === 'cache' || (Array.isArray(gamesList.value) && gamesList.value.length > 0),
);
const limitedNoCache = computed(() => limitedConnectivity.value && !hasCachedGames.value);
const limitedWithCache = computed(() => limitedConnectivity.value && hasCachedGames.value);

// Transfer options include all games and any excluded items not in games (so right list always shows)
const transferOptions = computed(() => {
  const map = new Map<string, string>();
  for (const g of gamesList.value)
    map.set(g.id, g.name || (t('playnite.unknown_game') as any) || 'Unknown');
  for (const g of excludedDisplayList.value)
    if (!map.has(g.id)) map.set(g.id, g.name || (t('playnite.unknown_game') as any) || 'Unknown');
  const arr = Array.from(map.entries()).map(([value, label]) => ({ value, label }));
  return arr.sort((a, b) => a.label.localeCompare(b.label));
});

async function refreshStatus() {
  if (platform.value !== 'windows') return;
  try {
    const r = await http.get('/api/playnite/status');
    if (r.status === 200 && r.data) {
      const d = r.data as any;
      status.installed = !!d.installed;
      status.active = !!d.active;
      // 'enabled' is no longer a config; presence is indicated by 'installed'
      if (typeof d.playnite_running === 'boolean') status.playnite_running = !!d.playnite_running;
      status.extensions_dir = d.extensions_dir || '';
      status.plugin_version =
        d.installed_version || d.plugin_version || d.version || status.plugin_version;
      status.plugin_latest =
        d.packaged_version || d.plugin_latest || d.latest_version || status.plugin_latest;
    }
  } catch (_) {}
}

const diagnosticText = computed<string | ''>(() => {
  switch (statusKind.value) {
    case 'uninstalled':
      return (
        (t('playnite.diagnostic_not_installed') as any) ||
        'Playnite plugin is not installed in the Extensions directory.'
      );
    case 'waiting':
      return (
        (t('playnite.diagnostic_not_running') as any) ||
        'Playnite is not running. Launch it to resume syncing.'
      );
    case 'active':
      return '';
    default:
      return '';
  }
});

async function loadCategories() {
  if (platform.value !== 'windows') return;
  if (categoriesLoading.value || categoryOptions.value.length) return;
  categoriesLoading.value = true;
  try {
    // Prefer categories endpoint if available
    try {
      const rc = await http.get('/api/playnite/categories', { validateStatus: () => true });
      if (rc.status >= 200 && rc.status < 300 && Array.isArray(rc.data) && rc.data.length) {
        const cats = (rc.data as any[])
          .map((c) => {
            if (c && typeof c === 'object') {
              const id = String((c as any).id || '');
              const name = String((c as any).name || id);
              return { label: name, value: id || name };
            }
            const s = String(c || '');
            return s ? { label: s, value: s } : null;
          })
          .filter((x): x is { label: string; value: string } => !!x)
          .sort((a, b) => a.label.localeCompare(b.label));
        categoryOptions.value = cats as { label: string; value: string }[];
        categoriesLoading.value = false;
        return;
      }
    } catch {}
    // Fallback: derive from games list
    const rg = await http.get('/api/playnite/games');
    const games: any[] = Array.isArray(rg.data) ? rg.data : [];
    const set = new Set<string>();
    for (const g of games)
      for (const c of g?.categories || []) if (c && typeof c === 'string') set.add(c);
    categoryOptions.value = Array.from(set)
      .sort((a, b) => a.localeCompare(b))
      .map((c) => ({ label: c, value: c }));
  } catch (_) {}
  categoriesLoading.value = false;
}

function ensurePluginOptionsIncludeSelection() {
  const current = pluginOptions.value.slice();
  const byValue = new Map(current.map((o) => [o.value, o] as const));
  const selected = (config.value?.playnite_exclude_plugins || []) as Array<{
    id: string;
    name: string;
  }>;
  let changed = false;
  for (const entry of selected || []) {
    const value = entry?.id || entry?.name || '';
    if (!value) continue;
    const label = entry?.name || entry?.id || value;
    if (!byValue.has(value)) {
      current.push({ value, label });
      byValue.set(value, current[current.length - 1]);
      changed = true;
    } else {
      const existing = byValue.get(value);
      if (existing && !existing.label && label) {
        existing.label = label;
        changed = true;
      }
    }
  }
  if (changed) {
    pluginOptions.value = current.sort((a, b) => a.label.localeCompare(b.label));
  }
}

async function loadPlugins() {
  if (platform.value !== 'windows') return;
  if (pluginsLoading.value || pluginOptions.value.length) {
    ensurePluginOptionsIncludeSelection();
    return;
  }
  pluginsLoading.value = true;
  try {
    try {
      const rp = await http.get('/api/playnite/plugins', { validateStatus: () => true });
      if (rp.status >= 200 && rp.status < 300 && Array.isArray(rp.data) && rp.data.length) {
        const opts = (rp.data as any[])
          .map((p) => {
            if (p && typeof p === 'object') {
              const id = String((p as any).id || '');
              const name = String((p as any).name || '');
              const value = id || name;
              if (!value) return null;
              return { value, label: name || value };
            }
            const s = String(p || '');
            return s ? { value: s, label: s } : null;
          })
          .filter((x): x is { label: string; value: string } => !!x)
          .sort((a, b) => a.label.localeCompare(b.label));
        pluginOptions.value = opts;
        ensurePluginOptionsIncludeSelection();
        return;
      }
    } catch {}

    const map = new Map<string, string>();
    const ingestGames = (rows: GameRow[]) => {
      for (const g of rows) {
        if (!g) continue;
        const pid = g.pluginId ? String(g.pluginId) : '';
        const pname = g.pluginName ? String(g.pluginName) : '';
        if (!pid) continue;
        if (!map.has(pid)) {
          map.set(pid, pname || pid);
        } else if (!map.get(pid) && pname) {
          map.set(pid, pname);
        }
      }
    };
    if (gamesList.value.length) {
      ingestGames(gamesList.value);
    }
    if (!map.size) {
      try {
        const rg = await http.get('/api/playnite/games', { validateStatus: () => true });
        if (rg.status >= 200 && rg.status < 300 && Array.isArray(rg.data)) {
          ingestGames(
            (rg.data as any[]).map((g) => ({
              id: String(g?.id || ''),
              name: String(g?.name || g?.id || ''),
              pluginId: g?.pluginId ? String(g.pluginId) : '',
              pluginName: g?.pluginName ? String(g.pluginName) : '',
            }))
          );
        }
      } catch {}
    }
    const opts = Array.from(map.entries())
      .map(([value, label]) => ({ value, label: label || value }))
      .sort((a, b) => a.label.localeCompare(b.label));
    pluginOptions.value = opts;
    ensurePluginOptionsIncludeSelection();
  } finally {
    pluginsLoading.value = false;
  }
}

const GAMES_CACHE_KEY = 'playnite_games_cache_v2';
function saveGamesCache(list: GameRow[]) {
  try {
    const payload = {
      t: Date.now(),
      games: list.map((g) => ({
        id: g.id,
        name: g.name,
        installed: !!g.installed,
        categories: Array.isArray(g.categories) ? g.categories : [],
        pluginId: g.pluginId || '',
        pluginName: g.pluginName || '',
      })),
    };
    localStorage.setItem(GAMES_CACHE_KEY, JSON.stringify(payload));
  } catch {}
}
function loadGamesCache(): { t: number; games: GameRow[] } | null {
  try {
    const raw = localStorage.getItem(GAMES_CACHE_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (!parsed || !Array.isArray(parsed.games)) return null;
    return {
      t: Number(parsed.t) || Date.now(),
      games: (parsed.games as any[]).map((g) => ({
        id: String(g?.id || ''),
        name: String(g?.name || g?.id || ''),
        installed: !!g?.installed,
        categories: Array.isArray(g?.categories) ? g.categories : [],
        pluginId: g?.pluginId ? String(g.pluginId) : '',
        pluginName: g?.pluginName ? String(g.pluginName) : '',
      })) as GameRow[],
    };
  } catch {
    return null;
  }
}

async function loadGames(useCacheFirst = true) {
  if (platform.value !== 'windows') return;
  if (gamesLoading.value) return;
  gamesLoading.value = true;
  if (useCacheFirst) {
    const cached = loadGamesCache();
    if (cached && cached.games.length) {
      gamesList.value = cached.games.slice().sort((a, b) => a.name.localeCompare(b.name));
      gamesSource.value = 'cache';
      gamesCacheTime.value = cached.t;
    }
  }
  try {
    const r = await http.get('/api/playnite/games', { validateStatus: () => true });
    if (r.status >= 200 && r.status < 300 && Array.isArray(r.data)) {
      const games: any[] = r.data as any[];
      const list: GameRow[] = games
        .filter((g) => !!g.installed)
        .map((g) => ({
          id: String(g.id),
          name: String(g.name || g.id),
          installed: !!g.installed,
          categories: Array.isArray(g.categories) ? g.categories : [],
          pluginId: g.pluginId ? String(g.pluginId) : '',
          pluginName: g.pluginName ? String(g.pluginName) : '',
        }))
        .sort((a, b) => a.name.localeCompare(b.name));
      gamesList.value = list;
      gamesSource.value = 'live';
      gamesCacheTime.value = Date.now();
      saveGamesCache(list);
    } else if (gamesSource.value === 'none') {
      const cached = loadGamesCache();
      if (cached && cached.games.length) {
        gamesList.value = cached.games.slice().sort((a, b) => a.name.localeCompare(b.name));
        gamesSource.value = 'cache';
        gamesCacheTime.value = cached.t;
      } else {
        gamesSource.value = 'none';
      }
    }
  } catch (_) {
    if (gamesSource.value === 'none') {
      const cached = loadGamesCache();
      if (cached && cached.games.length) {
        gamesList.value = cached.games.slice().sort((a, b) => a.name.localeCompare(b.name));
        gamesSource.value = 'cache';
        gamesCacheTime.value = cached.t;
      }
    }
  }
  gamesLoading.value = false;
}

async function onReinstallDone(res: { ok: boolean; error?: string }) {
  if (res.ok) {
    notify('success', (t('playnite.install_success') as any) || 'Plugin installed successfully.');
    await refreshStatus();
  } else {
    const msg =
      ((t('playnite.install_error') as any) || 'Failed to install plugin.') +
      (res.error ? `: ${res.error}` : '');
    notify('error', msg);
  }
}

function openUninstallConfirm() {
  showUninstallConfirm.value = true;
}

async function confirmUninstall() {
  uninstalling.value = true;
  showUninstallConfirm.value = false;
  try {
    const r = await http.post(
      '/api/playnite/uninstall',
      { restart: true },
      { validateStatus: () => true },
    );
    let ok = false;
    let body: any = null;
    try {
      body = r.data;
    } catch {}
    ok = r.status >= 200 && r.status < 300 && body && body.status === true;
    if (ok) {
      notify(
        'success',
        (t('playnite.uninstall_success') as any) || 'Plugin uninstalled successfully.',
      );
      await refreshStatus();
    } else {
      const msg =
        ((t('playnite.uninstall_error') as any) || 'Failed to uninstall plugin.') +
        (body && body.error ? `: ${body.error}` : '');
      notify('error', msg);
    }
  } catch (e: any) {
    const msg =
      ((t('playnite.uninstall_error') as any) || 'Failed to uninstall plugin.') +
      (e?.message ? `: ${e.message}` : '');
    notify('error', msg);
  }
  uninstalling.value = false;
}

onMounted(async () => {
  // ensure config is loaded so platform/keys available
  if (!config.value) await store.fetchConfig();
  await refreshStatus();
  // Preload lists so existing selections display with names immediately
  loadGames();
  loadCategories();
  loadPlugins();
  ensurePluginOptionsIncludeSelection();
  // Periodically refresh Playnite status while on Windows
  if (platform.value === 'windows') {
    statusTimer.value = window.setInterval(() => {
      refreshStatus();
    }, 3000);
  }
  // Initialize transfer value from current exclusions and stay in sync
  transferValue.value = excludedIds.value.slice();
  watch(excludedIds, (v) => {
    transferValue.value = (v || []).slice();
  });
  watch(
    () => config.value?.playnite_exclude_plugins,
    () => {
      ensurePluginOptionsIncludeSelection();
    },
    { deep: true }
  );
  // no screen-size watchers needed for exclusions table
});
onUnmounted(() => {
  if (statusTimer.value) {
    window.clearInterval(statusTimer.value);
    statusTimer.value = undefined;
  }
  // nothing to unbind
});

const statusKind = computed<'active' | 'waiting' | 'uninstalled' | 'unknown'>(() => {
  if (!status.extensions_dir) return 'unknown';
  if (!status.installed) return 'uninstalled';
  return status.active ? 'active' : 'waiting';
});
const statusType = computed<'success' | 'warning' | 'error' | 'default'>(() => {
  switch (statusKind.value) {
    case 'active':
      return 'success';
    case 'waiting':
      return 'warning';
    case 'uninstalled':
      return 'error';
    case 'unknown':
      return 'default';
    default:
      return 'default';
  }
});
const statusText = computed<string>(() => {
  switch (statusKind.value) {
    case 'active':
      return t('playnite.status_connected');
    case 'waiting':
      return t('playnite.status_waiting');
    case 'uninstalled':
      return t('playnite.status_uninstalled');
    case 'unknown':
      return (t('playnite.status_unknown') as any) || t('playnite.status_not_running_unknown');
    default:
      return '';
  }
});

function cmpSemver(a?: string, b?: string): number {
  if (!a || !b) return 0;
  const na = String(a)
    .replace(/^v/i, '')
    .split('.')
    .map((x) => parseInt(x, 10));
  const nb = String(b)
    .replace(/^v/i, '')
    .split('.')
    .map((x) => parseInt(x, 10));
  const len = Math.max(na.length, nb.length);
  for (let i = 0; i < len; i++) {
    const va = Number.isFinite(na[i]) ? na[i] : 0;
    const vb = Number.isFinite(nb[i]) ? nb[i] : 0;
    if (va < vb) return -1;
    if (va > vb) return 1;
  }
  return 0;
}

const pluginOutdated = computed(() => {
  if (!status.installed) return false;
  if (!status.plugin_version || !status.plugin_latest) return false;
  return cmpSemver(status.plugin_version, status.plugin_latest) < 0;
});
const canLaunch = computed(() => {
  return !!(status.extensions_dir && status.installed && !status.active);
});

const statusTimer = ref<number | undefined>();

const autoSyncEnabled = computed<boolean>(() => !!config.value?.playnite_auto_sync);

// Disable category/game selection when Playnite is not fully connected
const disablePlayniteSelection = computed<boolean>(() => statusKind.value !== 'active');
const disabledHint = computed<string>(() => {
  return (
    (t('playnite.selects_disabled_hint') as any) ||
    'Cannot modify without Playnite connectivity. Start Playnite to continue.'
  );
});

function copyExtensionsPath() {
  try {
    if (status.extensions_dir) navigator.clipboard?.writeText(status.extensions_dir);
    notify('success', (t('playnite.copied_path') as any) || 'Copied path to clipboard.');
  } catch {}
}

// old table-based exclusion code removed in favor of action list UI

const policySummary = computed<string>(() => {
  if (!autoSyncEnabled.value) return '';
  const n = Number(config.value?.playnite_recent_games ?? 0);
  const days = Number(config.value?.playnite_recent_max_age_days ?? 0);
  const pruneDays = Number(config.value?.playnite_autosync_delete_after_days ?? 0);
  const keepUntilReplaced = !!config.value?.playnite_autosync_require_replacement;
  const parts: string[] = [];
  parts.push(
    (t('playnite.summary_recent_limit', { n }) as any) ||
      `Up to ${n} most-recently played games will be auto-synced.`,
  );
  parts.push(
    days > 0
      ? (t('playnite.summary_activity_window', { days }) as any) ||
          `Activity window: last ${days} days.`
      : (t('playnite.summary_activity_ignored') as any) || 'Activity window is ignored.',
  );
  parts.push(
    keepUntilReplaced
      ? (t('playnite.summary_keep_until_replaced') as any) ||
          'Games stay until a newer game replaces them.'
      : (t('playnite.summary_prune_immediately') as any) ||
          'Games are pruned when they no longer qualify.',
  );
  if (pruneDays > 0) {
    parts.push(
      (t('playnite.summary_delete_after', { days: pruneDays }) as any) ||
        `Also remove games never launched after ${pruneDays} days.`,
    );
  }
  const excluded = ((config.value?.playnite_exclude_categories || []) as Array<{
    id: string;
    name: string;
  }>).map((o) => (o?.name || o?.id || '').toString().trim()).filter(Boolean);
  if (excluded.length) {
    const shown = excluded.slice(0, 3);
    const sample = shown.join(', ');
    const more = excluded.length > shown.length ? excluded.length - shown.length : 0;
    if (more > 0) {
      parts.push(
        (t('playnite.summary_excluded_categories_more', {
          categories: sample,
          count: more,
        }) as any) || `Excluded categories: ${sample} (+${more} more).`,
      );
    } else {
      parts.push(
        (t('playnite.summary_excluded_categories', { categories: sample }) as any) ||
          `Excluded categories: ${sample}.`,
      );
    }
  }
  return parts.join(' ');
});

async function launchPlaynite() {
  if (platform.value !== 'windows' || !canLaunch.value) return;
  launching.value = true;
  try {
    await http.post('/api/playnite/launch', {}, { validateStatus: () => true });
    window.setTimeout(() => refreshStatus(), 1000);
  } catch (_) {}
  launching.value = false;
}

// --- Exclusions update helpers --------------------------------------------
function handleTransferUpdate(next: string[]) {
  const prev = new Set(excludedIds.value);
  const nextSet = new Set(next);
  const added: string[] = [];
  const removed: string[] = [];
  for (const v of nextSet) if (!prev.has(v)) added.push(v);
  for (const v of prev) if (!nextSet.has(v)) removed.push(v);

  const final = Array.from(nextSet);
  excludedIds.value = final;
  transferValue.value = final;
  // No toast for additions per design
  // No toast for removals per design
}

// --- Reset to defaults (per section) ---------------------------------------
function resetAutoSyncSection() {
  const d = store.defaults as any;
  store.updateOption('playnite_auto_sync', d.playnite_auto_sync);
  store.updateOption('playnite_recent_games', d.playnite_recent_games);
  store.updateOption('playnite_recent_max_age_days', d.playnite_recent_max_age_days);
  store.updateOption('playnite_autosync_delete_after_days', d.playnite_autosync_delete_after_days);
  store.updateOption(
    'playnite_autosync_require_replacement',
    d.playnite_autosync_require_replacement,
  );
  store.updateOption('playnite_sync_categories', d.playnite_sync_categories);
  notify('success', (t('playnite.reset_done') as any) || 'Section reset to defaults.');
}

function resetLaunchSection() {
  const d = store.defaults as any;
  store.updateOption('playnite_focus_attempts', d.playnite_focus_attempts);
  store.updateOption('playnite_focus_timeout_secs', d.playnite_focus_timeout_secs);
  store.updateOption('playnite_focus_exit_on_first', d.playnite_focus_exit_on_first);
  notify('success', (t('playnite.reset_done') as any) || 'Section reset to defaults.');
}

function resetFiltersSection() {
  const d = store.defaults as any;
  store.updateOption('playnite_exclude_categories', d.playnite_exclude_categories);
  store.updateOption('playnite_exclude_plugins', d.playnite_exclude_plugins);
  store.updateOption('playnite_exclude_games', d.playnite_exclude_games);
  notify('success', (t('playnite.reset_done') as any) || 'Section reset to defaults.');
}

// Data table columns and actions
type ExcludedRow = { id: string; name: string };
const exclusionsColumns = computed(() => [
  { title: (t('playnite.table_game') as any) || 'Game', key: 'name' },
  {
    title: (t('playnite.table_actions') as any) || 'Actions',
    key: 'actions',
    width: 120,
    render: (row: ExcludedRow) =>
      h('div', { class: 'flex items-center gap-2 justify-end' }, [
        h(
          NButton as any,
          { type: 'error', size: 'tiny', strong: true, onClick: () => removeExclusion(row.id) },
          {
            default: () => [
              h('i', { class: 'fas fa-trash' }),
              h('span', { class: 'ml-1' }, (t('_common.remove') as any) || 'Remove'),
            ],
          },
        ),
      ]),
  },
]);

function removeExclusion(id: string) {
  const next = transferValue.value.filter((x) => x !== id);
  handleTransferUpdate(next);
}

function clearAllExclusions() {
  handleTransferUpdate([]);
}

// Add modal state and actions
const showAddModal = ref(false);
const addSelection = ref<string[]>([]);
const addOptions = computed(() => {
  const excluded = new Set(excludedIds.value);
  return gamesList.value
    .filter((g) => !excluded.has(g.id))
    .map((g) => ({
      label: g.name || (t('playnite.unknown_game') as any) || 'Unknown',
      value: g.id,
    }))
    .sort((a, b) => a.label.localeCompare(b.label));
});

function openAddExclusions() {
  addSelection.value = [];
  showAddModal.value = true;
  loadGames();
}
function confirmAddExclusions() {
  const merged = Array.from(new Set([...transferValue.value, ...addSelection.value]));
  handleTransferUpdate(merged);
  showAddModal.value = false;
}
</script>

<style scoped>
/* No global heights for transfer lists; only adjust on mobile below */
/* Compact nested cards and responsive actions on small screens */
@media (max-width: 640px) {
  .playnite-tab .playnite-card {
    padding: 0.75rem !important; /* reduce p-4 */
  }
  .playnite-tab .section-card .section-header {
    padding-left: 0.75rem !important;
    padding-right: 0.75rem !important;
    padding-top: 0.5rem !important;
    padding-bottom: 0.25rem !important;
  }
  .playnite-tab .section-card .section-body {
    padding-left: 0.75rem !important;
    padding-right: 0.75rem !important;
    padding-bottom: 0.75rem !important;
  }
  /* Stack info rows to avoid cramping */
  .playnite-tab .inline-info {
    flex-direction: column;
    align-items: stretch;
    gap: 0.25rem;
  }
  .playnite-tab .inline-info code {
    max-width: 100%;
  }
  /* Plugin actions: stack and let text wrap to prevent overflow */
  .playnite-tab .playnite-actions {
    flex-direction: column;
    align-items: stretch;
  }
  .playnite-tab .playnite-actions :deep(.n-button) {
    width: 100%;
  }
  .playnite-tab .playnite-actions :deep(.n-button .n-button__content) {
    white-space: normal; /* allow wrapping */
    line-height: 1.2;
    text-align: center;
  }

  /* No special mobile styles needed for exclusions table */
}
</style>
