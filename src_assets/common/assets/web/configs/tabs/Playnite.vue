<template>
  <n-space vertical size="large" class="playnite-tab w-full">
    <n-alert v-if="platform && platform !== 'windows'" type="info" :show-icon="true">
      {{ $t('playnite.only_windows') }}
    </n-alert>

    <section v-if="platform === 'windows'" class="playnite-section">
      <n-card
        class="playnite-card playnite-main-card"
        :segmented="{ content: true, footer: 'soft' }"
      >
        <template #header>
          <div class="playnite-card-heading">
            <n-icon size="18"><i class="fas fa-plug" /></n-icon>
            <n-text strong>{{ $t('playnite.status_title') }}</n-text>
          </div>
        </template>
        <n-space vertical size="large">
          <!-- Integration is always on; no enable/disable toggle -->
          <div class="playnite-status-line">
            <div class="playnite-status-value">
              <n-text strong>{{ $t('playnite.status_overall') }}</n-text>
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
          <n-text v-if="diagnosticText" depth="3" class="playnite-help">
            {{ diagnosticText }}
          </n-text>
          <div class="playnite-primary-actions">
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
          <n-card
            v-if="status.extensions_dir || status.plugin_version"
            size="small"
            embedded
            class="playnite-status-details"
          >
            <div v-if="status.extensions_dir" class="playnite-detail-row">
              <n-text depth="3" strong class="playnite-detail-label">
                {{ $t('playnite.extensions_dir') }}
              </n-text>
              <div class="playnite-path-row">
                <n-text code class="playnite-path">{{ status.extensions_dir }}</n-text>
                <n-button size="tiny" secondary @click="copyExtensionsPath">
                  <i class="fas fa-copy" />
                  <span class="ml-1">{{ $t('playnite.copy_path') || 'Copy' }}</span>
                </n-button>
              </div>
            </div>
            <n-divider
              v-if="status.extensions_dir && status.plugin_version"
              class="playnite-detail-divider"
            />
            <div v-if="status.plugin_version" class="playnite-detail-row">
              <n-text depth="3" strong class="playnite-detail-label">
                {{ $t('playnite.plugin_version') || 'Plugin' }}
              </n-text>
              <n-space align="center" size="small">
                <n-tag size="small" type="default">v{{ status.plugin_version }}</n-tag>
                <template v-if="status.plugin_latest">
                  <n-text depth="3">→</n-text>
                  <n-tag size="small" :type="pluginOutdated ? 'warning' : 'success'">
                    v{{ status.plugin_latest }}
                  </n-tag>
                </template>
              </n-space>
            </div>
          </n-card>
        </n-space>
        <template #footer>
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
        </template>
      </n-card>
    </section>
    <section v-if="platform === 'windows'" class="playnite-section">
      <n-divider title-placement="left" class="playnite-section-divider">
        <n-text depth="3" strong>{{ $t('playnite.settings_title') }}</n-text>
      </n-divider>

      <!-- Auto-sync card -->
      <n-card class="playnite-card playnite-main-card" :segmented="{ content: true }">
        <template #header>
          <div class="playnite-card-heading">
            <n-icon size="18"><i class="fas fa-arrows-rotate" /></n-icon>
            <n-text strong>
              {{ $t('playnite.section_auto_sync') || 'Auto-sync' }}
            </n-text>
          </div>
        </template>
        <template #header-extra>
          <n-space wrap size="small">
            <n-button size="tiny" type="default" strong @click="resetAutoSyncSection">
              <i class="fas fa-undo" />
              <span class="ml-1">{{ $t('playnite.reset_defaults') || 'Reset to defaults' }}</span>
            </n-button>
            <n-button
              size="tiny"
              type="error"
              strong
              :loading="deletingAutosync"
              @click="openDeleteAutosyncConfirm"
            >
              <i class="fas fa-trash" />
              <span class="ml-1">
                {{ $t('playnite.delete_all_autosync') || 'Delete Auto-sync Games' }}
              </span>
            </n-button>
          </n-space>
        </template>
        <n-form label-placement="top" :show-feedback="false">
          <div class="settings-grid">
            <div>
              <Checkbox
                v-model="config.playnite_auto_sync"
                id="playnite_auto_sync"
                :default="store.defaults.playnite_auto_sync"
                :localePrefix="'playnite'"
                label="playnite.auto_sync"
                :desc="''"
              />
              <n-text v-if="!autoSyncEnabled" depth="3" class="playnite-help">
                {{
                  $t('playnite.enable_autosync_hint') || 'Enable Auto-sync to edit these settings.'
                }}
              </n-text>
            </div>
            <div>
              <Checkbox
                v-model="config.playnite_sync_all_installed"
                id="playnite_sync_all_installed"
                :default="store.defaults.playnite_sync_all_installed"
                :localePrefix="'playnite'"
                label="playnite.sync_all_installed"
                desc="playnite.sync_all_installed_desc"
                :disabled="!autoSyncEnabled"
              />
            </div>
            <div>
              <n-form-item :label="$t('playnite.recent_games')">
                <n-input-number
                  id="playnite_recent_games"
                  v-model:value="config.playnite_recent_games"
                  :min="0"
                  :max="50"
                  :show-button="true"
                  class="playnite-number-input"
                  :disabled="!autoSyncEnabled"
                />
              </n-form-item>
              <n-text depth="3" class="playnite-help">
                {{ $t('playnite.recent_games_desc') }} (0 =
                {{ $t('_common.disabled') || 'disabled' }})
              </n-text>
            </div>
            <div>
              <n-form-item :label="$t('playnite.recent_max_age_days')">
                <n-input-number
                  id="playnite_recent_max_age_days"
                  v-model:value="config.playnite_recent_max_age_days"
                  :min="0"
                  :max="3650"
                  :show-button="true"
                  class="playnite-number-input"
                  :disabled="!autoSyncEnabled"
                />
              </n-form-item>
              <n-text depth="3" class="playnite-help">
                {{ $t('playnite.recent_max_age_days_desc') }} (0 =
                {{ $t('_common.disabled') || 'disabled' }})
              </n-text>
            </div>
            <div class="md:col-span-1">
              <n-form-item :label="$t('playnite.sync_categories')">
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
              </n-form-item>
              <n-text depth="3" class="playnite-help">
                {{ $t('playnite.sync_categories_help') }}
              </n-text>
            </div>
            <div>
              <n-form-item :label="$t('playnite.sync_plugins') || 'Include library plugins'">
                <n-tooltip :disabled="!disablePlayniteSelection && autoSyncEnabled" trigger="hover">
                  <template #trigger>
                    <n-select
                      id="playnite_sync_plugins"
                      v-model:value="includedPlugins"
                      multiple
                      :options="pluginOptions"
                      filterable
                      clearable
                      :placeholder="
                        $t('playnite.plugins_include_placeholder') || 'Include all games from...'
                      "
                      :loading="pluginsLoading"
                      :disabled="disablePlayniteSelection || !autoSyncEnabled"
                      @focus="() => loadPlugins()"
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
              </n-form-item>
              <n-text depth="3" class="playnite-help">
                {{ $t('playnite.sync_plugins_help') }}
              </n-text>
            </div>
            <div class="md:col-span-2 settings-grid">
              <div>
                <n-form-item :label="$t('playnite.delete_after_days')">
                  <n-input-number
                    id="playnite_autosync_delete_after_days"
                    v-model:value="config.playnite_autosync_delete_after_days"
                    :min="0"
                    :max="3650"
                    :show-button="true"
                    class="playnite-number-input"
                    :disabled="!autoSyncEnabled"
                  />
                </n-form-item>
                <n-text depth="3" class="playnite-help">
                  {{ $t('playnite.delete_after_days_desc') }} (0 =
                  {{ $t('_common.disabled') || 'disabled' }})
                </n-text>
              </div>
              <div>
                <n-form-item :label="$t('playnite.cleanup_policy') || 'Cleanup policy'">
                  <n-radio-group
                    id="playnite_cleanup_policy"
                    v-model:value="config.playnite_autosync_require_replacement"
                    :disabled="!autoSyncEnabled"
                  >
                    <n-space vertical size="small">
                      <n-radio :value="true">
                        {{
                          $t('playnite.policy_keep_until_replaced') ||
                          'Keep until replaced (default)'
                        }}
                      </n-radio>
                      <n-radio :value="false">
                        {{
                          $t('playnite.policy_prune_immediately') ||
                          'Always prune games that no longer qualify'
                        }}
                      </n-radio>
                    </n-space>
                  </n-radio-group>
                </n-form-item>
                <n-text depth="3" class="playnite-help">
                  {{
                    $t('playnite.policy_explainer') ||
                    'Choose how Vibepollo removes old auto-synced games.'
                  }}
                </n-text>
              </div>
              <div class="md:col-span-2">
                <Checkbox
                  v-model="config.playnite_autosync_remove_uninstalled"
                  id="playnite_autosync_remove_uninstalled"
                  :default="store.defaults.playnite_autosync_remove_uninstalled"
                  :localePrefix="'playnite'"
                  label="playnite.remove_uninstalled"
                  desc="playnite.remove_uninstalled_desc"
                  :disabled="!autoSyncEnabled"
                />
              </div>
              <n-text v-if="autoSyncEnabled" depth="3" class="md:col-span-2 playnite-help">
                {{ policySummary }}
              </n-text>
            </div>
          </div>
        </n-form>
      </n-card>

      <!-- Launch Behavior card -->
      <n-card class="playnite-card playnite-main-card" :segmented="{ content: true }">
        <template #header>
          <div class="playnite-card-heading">
            <n-icon size="18"><i class="fas fa-gamepad" /></n-icon>
            <n-text strong>
              {{ $t('playnite.section_launch_behavior') || 'Launch Behavior' }}
            </n-text>
          </div>
        </template>
        <template #header-extra>
          <n-button size="tiny" type="default" strong @click="resetLaunchSection">
            <i class="fas fa-undo" />
            <span class="ml-1">{{ $t('playnite.reset_defaults') || 'Reset to defaults' }}</span>
          </n-button>
        </template>
        <n-form label-placement="top" :show-feedback="false">
          <div class="settings-grid">
            <div class="md:col-span-2">
              <Checkbox
                v-model="config.playnite_fullscreen_entry_enabled"
                id="playnite_fullscreen_entry_enabled"
                :default="store.defaults.playnite_fullscreen_entry_enabled"
                :localePrefix="'playnite'"
                label="Add 'Playnite (Fullscreen)' to Applications"
                desc="When enabled, Vibepollo adds a launcher entry that opens Playnite in fullscreen desktop mode."
              />
            </div>
            <div>
              <n-form-item :label="$t('playnite.focus_attempts') || 'Auto-focus attempts'">
                <n-input-number
                  id="playnite_focus_attempts"
                  v-model:value="config.playnite_focus_attempts"
                  :min="0"
                  :max="30"
                  :show-button="true"
                  class="playnite-number-input"
                />
              </n-form-item>
              <n-text depth="3" class="playnite-help">
                {{
                  $t('playnite.focus_attempts_help') ||
                  'Number of times to try to bring Playnite windows to the foreground when launching.'
                }}
              </n-text>
            </div>
            <div>
              <ConfigDurationField
                id="playnite_focus_timeout_secs"
                v-model="config.playnite_focus_timeout_secs"
                :label="
                  String($t('playnite.focus_timeout_secs') || 'Auto-focus timeout window (seconds)')
                "
                :desc="
                  String(
                    $t('playnite.focus_timeout_secs_help') ||
                      'How long auto-focus runs while re-applying focus (0 to disable).',
                  )
                "
                :min="0"
                :max="120"
                size="small"
              />
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
        </n-form>
      </n-card>

      <!-- Exclusions & Filters card -->
      <n-card class="playnite-card playnite-main-card" :segmented="{ content: true }">
        <template #header>
          <div class="playnite-card-heading">
            <n-icon size="18"><i class="fas fa-filter" /></n-icon>
            <n-text strong>
              {{ $t('playnite.section_exclusions_filters') || 'Exclusions & Filters' }}
            </n-text>
          </div>
        </template>
        <template #header-extra>
          <n-button size="tiny" type="default" strong @click="resetFiltersSection">
            <i class="fas fa-undo" />
            <span class="ml-1">{{ $t('playnite.reset_defaults') || 'Reset to defaults' }}</span>
          </n-button>
        </template>
        <n-form label-placement="top" :show-feedback="false">
          <div class="settings-grid">
            <div>
              <n-form-item :label="$t('playnite.exclude_categories') || 'Exclude categories'">
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
                  <span>{{
                    !autoSyncEnabled
                      ? $t('playnite.enable_autosync_hint') ||
                        'Enable Auto-sync to edit these settings.'
                      : disabledHint
                  }}</span>
                </n-tooltip>
              </n-form-item>
              <n-text depth="3" class="playnite-help">
                {{
                  $t('playnite.exclude_categories_help') ||
                  'Games tagged with these categories will never be auto-synced.'
                }}
              </n-text>
            </div>
            <div>
              <n-form-item :label="$t('playnite.exclude_plugins') || 'Exclude library plugins'">
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
                  <span>{{
                    !autoSyncEnabled
                      ? $t('playnite.enable_autosync_hint') ||
                        'Enable Auto-sync to edit these settings.'
                      : disabledHint
                  }}</span>
                </n-tooltip>
              </n-form-item>
              <n-text depth="3" class="playnite-help">
                {{
                  $t('playnite.exclude_plugins_help') ||
                  'Games imported from these plugins will never be auto-synced.'
                }}
              </n-text>
            </div>
            <div class="md:col-span-2">
              <div class="flex flex-col gap-2">
                <n-text strong>
                  {{ $t('playnite.exclude_games') || 'Exclude games from auto-sync' }}
                </n-text>
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
                <n-card size="small" embedded class="playnite-exclusions">
                  <template #header>
                    <n-text strong>
                      {{ $t('playnite.exclude_games_table_title') || 'Excluded Games' }}
                    </n-text>
                  </template>
                  <div class="playnite-exclusions-toolbar">
                    <n-button
                      size="small"
                      type="default"
                      strong
                      :disabled="disablePlayniteSelection"
                      @click="openAddExclusions"
                    >
                      <i class="fas fa-plus" />
                      <span class="ml-1">{{ $t('playnite.add_exclusions') || 'Add' }}</span>
                    </n-button>
                    <n-button
                      size="small"
                      type="default"
                      strong
                      :disabled="!excludedIds.length"
                      @click="clearAllExclusions"
                    >
                      <i class="fas fa-times" />
                      <span class="ml-1">{{ $t('_common.clear_all') || 'Clear All' }}</span>
                    </n-button>
                  </div>
                  <n-data-table
                    class="playnite-exclusions-table"
                    :columns="exclusionsColumns"
                    :data="excludedDisplayList"
                    :bordered="false"
                    :single-line="false"
                    :pagination="false"
                    size="small"
                  />
                  <n-list
                    v-if="excludedDisplayList.length"
                    class="playnite-exclusions-list"
                    show-divider
                  >
                    <n-list-item v-for="game in excludedDisplayList" :key="game.id">
                      <n-text class="playnite-excluded-game-name">{{ game.name }}</n-text>
                      <template #suffix>
                        <n-button
                          circle
                          tertiary
                          type="error"
                          size="small"
                          :aria-label="$t('_common.remove') || 'Remove'"
                          @click="removeExclusion(game.id)"
                        >
                          <i class="fas fa-trash" />
                        </n-button>
                      </template>
                    </n-list-item>
                  </n-list>
                  <n-empty
                    v-else
                    class="playnite-exclusions-empty"
                    size="small"
                    :description="$t('_common.no_data') || 'No excluded games'"
                  />
                </n-card>
                <n-text depth="3" class="playnite-source">
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
                </n-text>
                <n-text depth="3" class="playnite-help">
                  {{
                    $t('playnite.exclude_games_desc') ||
                    'Selected games will not be auto-synced from Playnite.'
                  }}
                </n-text>
                <n-text depth="3" class="playnite-help">
                  {{ $t('playnite.exclusions_override_note') || 'Exclusions override categories.' }}
                </n-text>
              </div>
            </div>
          </div>
        </n-form>
      </n-card>
    </section>
  </n-space>
  <!-- Uninstall confirmation -->
  <n-modal :show="showDeleteAutosyncConfirm" @update:show="(v) => (showDeleteAutosyncConfirm = v)">
    <n-card :bordered="false" style="max-width: 32rem; width: 100%">
      <template #header>
        <div class="flex items-center gap-2">
          <i class="fas fa-trash" />
          <span>{{ $t('playnite.delete_autosync_title') || 'Delete auto-synced games?' }}</span>
        </div>
      </template>
      <div class="space-y-2 text-sm">
        <p>
          {{
            $t('playnite.delete_autosync_body') ||
            'This removes every Playnite-managed auto-sync entry from the Applications list. Apps added manually are not affected.'
          }}
        </p>
      </div>
      <template #footer>
        <div class="w-full flex items-center justify-center gap-3">
          <n-button type="default" strong @click="showDeleteAutosyncConfirm = false">{{
            $t('_common.cancel') || 'Cancel'
          }}</n-button>
          <n-button
            type="error"
            strong
            :loading="deletingAutosync"
            @click="confirmDeleteAutosync"
            >{{ $t('_common.continue') || 'Continue' }}</n-button
          >
        </div>
      </template>
    </n-card>
  </n-modal>

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
  NDivider,
  NForm,
  NFormItem,
  NIcon,
  NEmpty,
  NList,
  NListItem,
  NRadioGroup,
  NRadio,
  NSpace,
  NText,
  NDataTable,
  useNotification,
} from 'naive-ui';
import { useI18n } from 'vue-i18n';
import Checkbox from '@/Checkbox.vue';
import ConfigDurationField from '@/ConfigDurationField.vue';
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
  installed: boolean | null;
  installed_unknown?: boolean;
  active: boolean;
  enabled?: boolean;
  playnite_running?: boolean;
  extensions_dir: string;
  plugin_version?: string;
  plugin_latest?: string;
}>({ installed: null, active: false, extensions_dir: '' });
const launching = ref(false);
const uninstalling = ref(false);
const deletingAutosync = ref(false);
const showUninstallConfirm = ref(false);
const showDeleteAutosyncConfirm = ref(false);
// Naive UI notifications for transient messages
const notification = useNotification();
function notify(type: 'success' | 'error' | 'info' | 'warning', content: string) {
  notification.create({ type, content, duration: 5000 });
}

const NULL_GUID = '00000000-0000-0000-0000-000000000000';
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

type IdNameEntry = { id?: string; name?: string };

function normalizeIdNameEntries(value: unknown): IdNameEntry[] {
  if (Array.isArray(value)) return value as IdNameEntry[];
  if (value && typeof value === 'object') return [value as IdNameEntry];
  return [];
}

const selectedCategories = computed<string[]>({
  get() {
    const arr = normalizeIdNameEntries(config.value?.playnite_sync_categories);
    return arr.map((o) => o.id || o.name || '').filter(Boolean);
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
    const arr = normalizeIdNameEntries(config.value?.playnite_exclude_categories);
    return arr.map((o) => o.id || o.name || '').filter(Boolean);
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
    const arr = normalizeIdNameEntries(config.value?.playnite_exclude_plugins);
    return arr.map((o) => o.id || o.name || '').filter(Boolean);
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

const includedPlugins = computed<string[]>({
  get() {
    const arr = normalizeIdNameEntries(config.value?.playnite_sync_plugins);
    return arr.map((o) => o.id || o.name || '').filter(Boolean);
  },
  set(v: string[]) {
    const mapByVal = new Map(pluginOptions.value.map((o) => [o.value, o.label] as const));
    const next = (v || []).map((val) => ({
      id: val && mapByVal.has(val) ? val : '',
      name: mapByVal.get(val) || val,
    }));
    store.updateOption('playnite_sync_plugins', next);
  },
});

const excludedIds = computed<string[]>({
  get() {
    const arr = normalizeIdNameEntries(config.value?.playnite_exclude_games);
    return arr.map((o) => o.id || '').filter(Boolean);
  },
  set(v: string[]) {
    const nameById = new Map(gamesList.value.map((g) => [g.id, g.name] as const));
    const next = (v || []).map((id) => ({ id, name: nameById.get(id) || '' }));
    store.updateOption('playnite_exclude_games', next);
  },
});

// Build the display list of current exclusions, resolving names from cache if missing
const excludedDisplayList = computed<Array<{ id: string; name: string }>>(() => {
  const arr = normalizeIdNameEntries(config.value?.playnite_exclude_games);
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
      status.installed = typeof d.installed === 'boolean' ? d.installed : null;
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
  const selected = [
    ...((config.value?.playnite_exclude_plugins || []) as Array<{ id: string; name: string }>),
    ...((config.value?.playnite_sync_plugins || []) as Array<{ id: string; name: string }>),
  ];
  let changed = false;
  for (const entry of selected || []) {
    const value = entry?.id || entry?.name || '';
    if (!value || value === NULL_GUID) continue;
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
    const map = new Map<string, string>();
    const ingestGames = (rows: GameRow[]) => {
      for (const g of rows) {
        if (!g) continue;
        const pid = g.pluginId ? String(g.pluginId) : '';
        const pname = g.pluginName ? String(g.pluginName) : '';
        if (!pid || pid === NULL_GUID) continue;
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
            })),
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

function openDeleteAutosyncConfirm() {
  showDeleteAutosyncConfirm.value = true;
}

async function confirmDeleteAutosync() {
  deletingAutosync.value = true;
  try {
    const r = await http.post('/api/apps/purge_autosync', {}, { validateStatus: () => true });
    let body: any = null;
    try {
      body = r.data;
    } catch {}
    const ok = r.status >= 200 && r.status < 300 && body && body.status === true;
    if (ok) {
      notify(
        'success',
        (t('playnite.delete_autosync_success') as any) || 'Removed auto-synced Playnite games.',
      );
      showDeleteAutosyncConfirm.value = false;
    } else {
      const msg =
        ((t('playnite.delete_autosync_error') as any) ||
          'Failed to delete auto-synced Playnite games.') + (body?.error ? `: ${body.error}` : '');
      notify('error', msg);
    }
  } catch (e: any) {
    const msg =
      ((t('playnite.delete_autosync_error') as any) ||
        'Failed to delete auto-synced Playnite games.') + (e?.message ? `: ${e.message}` : '');
    notify('error', msg);
  }
  deletingAutosync.value = false;
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
    { deep: true },
  );
  watch(
    () => config.value?.playnite_sync_plugins,
    () => {
      ensurePluginOptionsIncludeSelection();
    },
    { deep: true },
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
  if (status.active) return 'active';
  if (!status.extensions_dir) return 'unknown';
  if (status.installed === false) return 'uninstalled';
  if (status.installed === true) return 'waiting';
  return 'unknown';
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
  if (status.installed !== true) return false;
  if (!status.plugin_version || !status.plugin_latest) return false;
  return cmpSemver(status.plugin_version, status.plugin_latest) < 0;
});
const canLaunch = computed(() => {
  return !!(status.extensions_dir && status.installed === true && !status.active);
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
  const syncAll = !!config.value?.playnite_sync_all_installed;
  const includePluginCount = normalizeIdNameEntries(config.value?.playnite_sync_plugins).length;
  const removeUninstalled = config.value?.playnite_autosync_remove_uninstalled !== false;
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
  if (syncAll) {
    parts.push(
      (t('playnite.summary_all_installed') as any) ||
        'All installed Playnite games will be kept in Vibepollo.',
    );
  } else if (includePluginCount > 0) {
    parts.push(
      (t('playnite.summary_plugin_include', { count: includePluginCount }) as any) ||
        `Includes every game from ${includePluginCount} selected library plugins.`,
    );
  }
  if (pruneDays > 0) {
    parts.push(
      (t('playnite.summary_delete_after', { days: pruneDays }) as any) ||
        `Also remove games never launched after ${pruneDays} days.`,
    );
  }
  parts.push(
    removeUninstalled
      ? (t('playnite.summary_remove_uninstalled_on') as any) ||
          'Uninstalled games are removed automatically.'
      : (t('playnite.summary_remove_uninstalled_off') as any) ||
          'Uninstalled games remain until removed manually.',
  );
  const excluded = (
    (config.value?.playnite_exclude_categories || []) as Array<{
      id: string;
      name: string;
    }>
  )
    .map((o) => (o?.name || o?.id || '').toString().trim())
    .filter(Boolean);
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
  store.updateOption('playnite_sync_all_installed', d.playnite_sync_all_installed);
  store.updateOption('playnite_recent_games', d.playnite_recent_games);
  store.updateOption('playnite_recent_max_age_days', d.playnite_recent_max_age_days);
  store.updateOption('playnite_autosync_delete_after_days', d.playnite_autosync_delete_after_days);
  store.updateOption(
    'playnite_autosync_require_replacement',
    d.playnite_autosync_require_replacement,
  );
  store.updateOption(
    'playnite_autosync_remove_uninstalled',
    d.playnite_autosync_remove_uninstalled,
  );
  store.updateOption('playnite_sync_categories', d.playnite_sync_categories);
  store.updateOption('playnite_sync_plugins', d.playnite_sync_plugins);
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
    width: 104,
    render: (row: ExcludedRow) =>
      h('div', { class: 'flex items-center gap-2 justify-end' }, [
        h(
          NButton as any,
          { type: 'error', size: 'tiny', tertiary: true, onClick: () => removeExclusion(row.id) },
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
.playnite-tab {
  min-width: 0;
}

.playnite-tab :deep(> .n-space-item) {
  width: 100%;
  min-width: 0;
}

.playnite-section {
  display: flex;
  min-width: 0;
  width: 100%;
  flex-direction: column;
  gap: 1rem;
}

.playnite-card {
  min-width: 0;
  width: 100%;
}

.playnite-card-heading {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: 0.625rem;
  line-height: 1.25rem;
}

.playnite-card-heading :deep(.n-icon) {
  display: inline-flex;
  flex: 0 0 1.125rem;
  align-items: center;
  align-self: center;
  justify-content: center;
  line-height: 1;
}

.playnite-card-heading :deep(.n-icon i) {
  display: block;
  line-height: 1;
}

.playnite-status-line {
  display: flex;
  min-width: 0;
  align-items: center;
  font-size: 0.875rem;
}

.playnite-status-value {
  display: inline-flex;
  min-width: 0;
  flex-wrap: nowrap;
  align-items: center;
  gap: 0.5rem;
  white-space: nowrap;
}

.playnite-status-value :deep(.n-tag) {
  flex: 0 0 auto;
}

.playnite-primary-actions,
.playnite-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
}

.playnite-path {
  display: block;
  min-width: 0;
  max-width: 100%;
  flex: 1 1 auto;
  overflow-x: auto;
  white-space: nowrap;
}

.playnite-path-row {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: 0.5rem;
}

.playnite-path-row :deep(.n-button) {
  flex: 0 0 auto;
}

.playnite-status-details {
  min-width: 0;
  width: 100%;
}

.playnite-detail-row {
  display: grid;
  min-width: 0;
  grid-template-columns: minmax(9rem, auto) minmax(0, 1fr);
  align-items: center;
  gap: 0.75rem 1rem;
}

.playnite-detail-label {
  min-width: 0;
}

.playnite-detail-divider {
  margin: 0.75rem 0;
}

.settings-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  align-items: start;
  gap: 1.5rem 2rem;
}

.settings-grid > * {
  min-width: 0;
}

.playnite-number-input {
  width: 100%;
  max-width: 10rem;
}

.playnite-exclusions {
  min-width: 0;
  width: 100%;
}

.playnite-exclusions-toolbar {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 0.5rem;
  margin-bottom: 0.75rem;
}

.playnite-exclusions-list,
.playnite-exclusions-empty {
  display: none;
}

.playnite-excluded-game-name {
  display: block;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.playnite-exclusions-list :deep(.n-list-item__main) {
  min-width: 0;
}

.playnite-exclusions-list :deep(.n-list-item__suffix) {
  flex: 0 0 auto;
  margin-left: 0.75rem;
}

.playnite-help,
.playnite-source {
  display: block;
  font-size: 0.75rem;
  line-height: 1.45;
}

.playnite-help {
  margin-top: 0.375rem;
}

.playnite-source {
  font-size: 0.6875rem;
}

@media (max-width: 767px) {
  .settings-grid {
    grid-template-columns: minmax(0, 1fr);
  }
}

@media (max-width: 640px) {
  .settings-grid {
    gap: 1.25rem;
  }

  :deep(.playnite-main-card > .n-card-header) {
    flex-direction: column;
    align-items: stretch;
    gap: 0.75rem;
    padding: 1rem;
  }

  :deep(.playnite-main-card > .n-card-header .n-card-header__extra) {
    width: 100%;
    margin-left: 0;
  }

  :deep(.playnite-main-card > .n-card__content),
  :deep(.playnite-main-card > .n-card__footer) {
    padding-right: 1rem;
    padding-left: 1rem;
  }

  .playnite-primary-actions :deep(.n-button) {
    flex: 1 1 auto;
  }

  .playnite-detail-row {
    grid-template-columns: minmax(0, 1fr);
    gap: 0.5rem;
  }

  .playnite-detail-label {
    white-space: nowrap;
  }

  .playnite-exclusions-table {
    display: none;
  }

  .playnite-exclusions-list {
    display: block;
  }

  .playnite-exclusions-empty {
    display: flex;
    padding: 0.75rem 0;
  }

  .playnite-exclusions-toolbar {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .playnite-exclusions-toolbar :deep(.n-button) {
    width: 100%;
  }

  .playnite-actions {
    width: 100%;
    flex-direction: column;
    align-items: stretch;
  }

  .playnite-actions :deep(.n-button) {
    width: 100%;
  }

  .playnite-actions :deep(.n-button .n-button__content) {
    white-space: normal;
    line-height: 1.2;
    text-align: center;
  }
}
</style>
