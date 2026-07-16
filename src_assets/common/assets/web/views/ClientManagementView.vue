<template>
  <div class="clients-page space-y-5 pb-10">
    <section class="clients-hero">
      <div class="clients-hero__content">
        <div class="clients-hero__identity">
          <span class="clients-hero__icon">
            <i class="fas fa-users-cog" />
          </span>
          <div class="min-w-0">
            <h1 class="text-xl md:text-2xl font-semibold text-dark dark:text-light">
              {{ $t('clients.title') }}
            </h1>
            <p class="clients-hero__subtitle">{{ $t('clients.hero_desc') }}</p>
          </div>
        </div>
        <div class="clients-hero__actions">
          <n-button
            size="small"
            type="primary"
            secondary
            :loading="clientsRefreshing"
            @click="manualRefreshClients"
          >
            <i class="fas fa-rotate" />
            <span class="ml-2">{{ $t('clients.refresh_clients') }}</span>
          </n-button>
          <n-button size="small" tertiary @click="scrollToTokenSection">
            <i class="fas fa-key" />
            <span class="ml-2">{{ $t('clients.api_tokens_short') }}</span>
          </n-button>
          <n-button size="small" tertiary @click="goToStats">
            <i class="fas fa-chart-line" />
            <span class="ml-2">{{ $t('navbar.stats') }}</span>
          </n-button>
          <span class="clients-last-updated">{{ lastRefreshedLabel }}</span>
        </div>
      </div>
      <div class="clients-stats">
        <div class="clients-stat">
          <span class="clients-stat__value">{{ clients.length }}</span>
          <span class="clients-stat__label">{{ $t('clients.existing_title') }}</span>
        </div>
        <div class="clients-stat clients-stat--connected">
          <span class="clients-stat__value">{{ connectedClientCount }}</span>
          <span class="clients-stat__label">{{ $t('clients.connected') }}</span>
        </div>
        <div class="clients-stat">
          <span class="clients-stat__value">{{ offlineClientCount }}</span>
          <span class="clients-stat__label">{{ $t('clients.offline') }}</span>
        </div>
      </div>
    </section>

    <!-- Pair New Client -->
    <n-card class="clients-card" :segmented="{ content: true, footer: false }">
      <template #header>
        <div class="clients-section-heading">
          <span class="clients-section-icon">
            <i class="fas fa-link" />
          </span>
          <div class="min-w-0">
            <h2 class="text-lg font-medium">{{ $t('clients.pair_title') }}</h2>
            <p class="text-xs opacity-70 max-w-2xl mt-1">{{ $t('clients.pair_desc') }}</p>
          </div>
        </div>
      </template>
      <div class="space-y-4">
        <n-form
          class="grid grid-cols-1 gap-4 md:grid-cols-[minmax(8rem,12rem)_minmax(12rem,1fr)_auto] md:items-end"
          @submit.prevent="registerDevice"
        >
          <n-form-item class="flex flex-col" :label="$t('navbar.pin')" label-placement="top">
            <n-input
              :value="pin"
              :placeholder="$t('navbar.pin')"
              clearable
              maxlength="4"
              :input-props="{
                inputmode: 'numeric',
                pattern: '^[0-9]{4}$',
                autocomplete: 'one-time-code',
                required: true,
              }"
              @update:value="updatePin"
            >
              <template #prefix>
                <i class="fas fa-key" />
              </template>
            </n-input>
          </n-form-item>
          <n-form-item class="flex flex-col" :label="$t('pin.device_name')" label-placement="top">
            <n-input
              v-model:value="deviceName"
              :placeholder="$t('pin.device_name')"
              clearable
              :input-props="{ autocomplete: 'off', required: true }"
            >
              <template #prefix>
                <i class="fas fa-desktop" />
              </template>
            </n-input>
          </n-form-item>
          <n-form-item class="flex flex-col md:items-end">
            <n-button
              :disabled="pairing || !canPairClient"
              :loading="pairing"
              class="w-full md:w-auto"
              type="primary"
              strong
              attr-type="submit"
            >
              <i class="fas fa-plus" />
              <span v-if="!pairing" class="ml-2">{{ $t('pin.send') }}</span>
              <span v-else class="ml-2">{{ $t('clients.pairing') }}</span>
            </n-button>
          </n-form-item>
        </n-form>
        <div
          class="clients-pair-readiness"
          :class="{ 'clients-pair-readiness--ready': canPairClient }"
        >
          <i :class="canPairClient ? 'fas fa-circle-check' : 'fas fa-circle-info'" />
          <span>{{
            canPairClient ? $t('clients.pair_ready') : $t('clients.pair_requirements')
          }}</span>
        </div>
        <div class="space-y-2">
          <n-alert v-if="pairStatus === true" type="success">{{ $t('pin.pair_success') }}</n-alert>
          <n-alert v-if="pairStatus === false" type="error">{{ $t('pin.pair_failure') }}</n-alert>
        </div>
        <n-alert type="warning" :title="$t('_common.warning')" class="text-sm">
          {{ $t('pin.warning_msg') }}
        </n-alert>
      </div>
    </n-card>

    <!-- Existing Clients -->
    <n-card class="clients-card" :segmented="{ content: true, footer: false }">
      <template #header>
        <div class="flex flex-col gap-4 md:flex-row md:items-start md:justify-between">
          <div class="clients-section-heading">
            <span class="clients-section-icon">
              <i class="fas fa-users" />
            </span>
            <div class="min-w-0">
              <h2 class="text-lg font-medium">{{ $t('clients.existing_title') }}</h2>
              <p class="text-xs opacity-70 max-w-2xl mt-1">
                {{ $t('troubleshooting.unpair_desc') }}
              </p>
            </div>
          </div>
          <div class="clients-toolbar">
            <label class="clients-toolbar__field clients-toolbar__search">
              <span>{{ $t('clients.search_label') }}</span>
              <n-input
                v-model:value="clientSearchQuery"
                :placeholder="$t('clients.search_placeholder')"
                size="small"
                clearable
              >
                <template #prefix>
                  <i class="fas fa-magnifying-glass" />
                </template>
              </n-input>
            </label>
            <label class="clients-toolbar__field">
              <span>{{ $t('clients.status_label') }}</span>
              <n-select
                v-model:value="clientStatusFilter"
                :options="clientStatusOptions"
                size="small"
                class="min-w-[9rem]"
              />
            </label>
            <label class="clients-toolbar__field">
              <span>{{ $t('clients.sort_label') }}</span>
              <n-select
                v-model:value="clientSortMode"
                :options="clientSortOptions"
                size="small"
                class="min-w-[10rem]"
              />
            </label>
            <n-button v-if="hasClientFilters" size="small" secondary @click="clearClientFilters">
              <i class="fas fa-filter-circle-xmark" />
              <span class="ml-2">{{ $t('clients.filters_clear') }}</span>
            </n-button>
            <n-button
              size="small"
              type="error"
              strong
              secondary
              :loading="unpairAllPressed"
              :disabled="unpairAllPressed || clients.length === 0"
              @click="askConfirmUnpairAll"
            >
              <i class="fas fa-user-slash" />
              <span class="ml-2">{{ $t('troubleshooting.unpair_all') }}</span>
            </n-button>
          </div>
        </div>
      </template>

      <div class="space-y-4">
        <n-alert v-if="unpairAllStatus === true" type="success">{{
          $t('troubleshooting.unpair_all_success')
        }}</n-alert>
        <n-alert v-if="unpairAllStatus === false" type="error">{{
          $t('troubleshooting.unpair_all_error')
        }}</n-alert>

        <div v-if="clientsLoading && !clientsReady" class="client-empty client-empty--loading">
          <i class="fas fa-spinner fa-spin" />
          {{ $t('clients.loading') }}
        </div>
        <template v-else-if="visibleClients.length > 0">
          <div class="clients-list-summary">
            <span>
              {{
                $t('clients.showing_count', {
                  shown: visibleClients.length,
                  total: clients.length,
                })
              }}
            </span>
            <button v-if="hasClientFilters" type="button" @click="clearClientFilters">
              {{ $t('clients.filters_clear') }}
            </button>
          </div>
          <div class="client-list">
            <article v-for="client in visibleClients" :key="client.uuid" class="client-record">
              <div class="client-record__main">
                <div
                  class="client-avatar"
                  :class="{ 'client-avatar--connected': client.connected }"
                >
                  <i class="fas fa-display" />
                </div>
                <div class="client-record__body">
                  <div class="client-record__title-row">
                    <h3 class="client-record__title">
                      {{
                        client.name !== ''
                          ? client.name
                          : $t('troubleshooting.unpair_single_unknown')
                      }}
                    </h3>
                    <n-tag v-if="client.connected" type="success" size="small" round>
                      {{ $t('clients.connected') }}
                    </n-tag>
                    <n-tag v-else size="small" round>
                      {{ $t('clients.offline') }}
                    </n-tag>
                    <n-tag
                      size="small"
                      :type="client.perm >= highlightPermissionThreshold ? 'error' : 'default'"
                      round
                    >
                      [ {{ permToStr(client.perm) }} ]
                    </n-tag>                  </div>
                  <div class="client-record__meta">
                    <span class="client-record__meta-item">
                      <i class="fas fa-clock" />
                      <span class="client-record__meta-label">{{ lastSeenLabel(client) }}</span>
                    </span>
                    <span v-if="client.uuid" class="client-record__meta-item" :title="client.uuid">
                      <i class="fas fa-fingerprint" />
                      <span class="client-record__meta-label">{{
                        shortClientUuid(client.uuid)
                      }}</span>
                    </span>
                    <span v-if="client.displayMode" class="client-record__meta-item">
                      <i class="fas fa-tv" />
                      <span class="client-record__meta-label">{{ client.displayMode }}</span>
                    </span>
                    <span class="client-record__meta-item">
                      <i class="fas fa-route" />
                      <span class="client-record__meta-label">{{
                        displayRoutingLabel(client)
                      }}</span>
                    </span>
                    <span v-if="client.hdrProfile" class="client-record__meta-item">
                      <i class="fas fa-sun" />
                      <span class="client-record__meta-label">{{ client.hdrProfile }}</span>
                    </span>
                  </div>
                </div>
              </div>
              <div class="client-record__actions">
                <n-button
                  v-if="client.connected"
                  size="small"
                  type="warning"
                  secondary
                  :title="t('clients.disconnect')"
                  :loading="disconnecting[client.uuid] === true"
                  :disabled="disconnecting[client.uuid] === true"
                  @click="disconnectClient(client)"
                >
                  <i class="fas fa-link-slash" />
                  <span class="ml-2">{{ t('clients.disconnect') }}</span>
                </n-button>
                <n-button
                  v-if="client.editing"
                  size="small"
                  type="success"
                  strong
                  secondary
                  :loading="saving[client.uuid] === true"
                  :disabled="saving[client.uuid] === true || !isClientDisplayOverrideValid"
                  @click="saveClient(client)"
                >
                  <i class="fas fa-check" />
                  <span class="ml-2">{{ t('_common.save') }}</span>
                </n-button>
                <n-button
                  v-if="client.editing"
                  size="small"
                  secondary
                  :disabled="saving[client.uuid] === true"
                  @click="cancelEdit(client)"
                >
                  <i class="fas fa-times" />
                  <span class="ml-2">{{ t('_common.cancel') }}</span>
                </n-button>
                <n-button
                  v-if="!client.editing"
                  size="small"
                  type="primary"
                  secondary
                  @click="editClient(client)"
                >
                  <i class="fas fa-edit" />
                  <span class="ml-2">{{ t('_common.edit') }}</span>
                </n-button>
                <n-button
                  size="small"
                  type="error"
                  secondary
                  :loading="removing[client.uuid] === true"
                  :disabled="removing[client.uuid] === true"
                  @click="askConfirmUnpair(client)"
                >
                  <i class="fas fa-trash" />
                  <span class="ml-2">{{ $t('clients.remove') }}</span>
                </n-button>
              </div>

              <div v-if="client.editing" class="client-edit-panel">
                <div class="client-edit-panel__header">
                  <div>
                    <h4>{{ $t('clients.editing_title', { name: clientDisplayName(client) }) }}</h4>
                    <p>{{ $t('clients.editing_desc') }}</p>
                  </div>
                </div>
                <n-form label-placement="top" class="grid gap-4 lg:grid-cols-2" @submit.prevent>
                  <n-form-item :label="$t('pin.device_name')">
                    <n-input v-model:value="client.editName" />
                  </n-form-item>

                  <div class="space-y-3 lg:col-span-2">
                    <div class="grid gap-4 md:grid-cols-3">
                      <div v-for="group in permissionGroups" :key="group.id" class="space-y-2">
                        <div class="text-xs font-medium uppercase tracking-wide opacity-70">
                          {{ $t(group.labelKey) }}
                        </div>
                        <div class="flex flex-wrap gap-2">
                          <n-button
                            v-for="perm in group.permissions"
                            :key="perm.key"
                            size="small"
                            :type="
                              isSuppressed(client.editPerm, perm.key, perm.suppressedBy) ||
                              checkPermission(client.editPerm, perm.key)
                                ? 'primary'
                                : 'default'
                            "
                            :ghost="!checkPermission(client.editPerm, perm.key)"
                            :disabled="isSuppressed(client.editPerm, perm.key, perm.suppressedBy)"
                            @click="togglePermission(client, perm.key)"
                          >
                            {{ $t(`permissions.${perm.key}`) }}
                          </n-button>
                        </div>
                      </div>
                    </div>
                  </div>

                  <n-form-item :label="$t('pin.display_mode_override')">
                    <n-input v-model:value="client.editDisplayMode" placeholder="1920x1080x60" />
                    <template #feedback>
                      <span class="text-xs opacity-70">{{
                        $t('pin.display_mode_override_desc')
                      }}</span>
                    </template>
                  </n-form-item>

                  <n-form-item>
                    <n-checkbox v-model:checked="client.editAllowClientCommands" size="small">
                      <div class="flex flex-col">
                        <span>Allow Client Commands</span>
                        <span class="text-[11px] opacity-60">
                          Allow this client to run connect and disconnect commands.
                        </span>
                      </div>
                    </n-checkbox>
                  </n-form-item>

                  <div v-if="client.editAllowClientCommands" class="space-y-4 lg:col-span-2">
                    <div
                      class="space-y-3 rounded-xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-dark/40 p-4"
                    >
                      <div class="flex items-center justify-between gap-3">
                        <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
                          Connect Commands
                        </div>
                        <n-button
                          size="tiny"
                          tertiary
                          @click="addClientCommand(client.editDoCommands)"
                        >
                          <i class="fas fa-plus" /> {{ $t('_common.add') }}
                        </n-button>
                      </div>
                      <div v-if="client.editDoCommands.length === 0" class="text-xs opacity-70">
                        No commands configured.
                      </div>
                      <div v-else class="space-y-2">
                        <div
                          v-for="(command, index) in client.editDoCommands"
                          :key="`do-${client.uuid}-${index}`"
                          class="rounded-md border border-dark/10 dark:border-light/10 p-3"
                        >
                          <div class="grid gap-3 md:grid-cols-[1fr_auto_auto]">
                            <n-input
                              v-model:value="command.cmd"
                              class="font-mono"
                              :placeholder="$t('_common.cmd')"
                            />
                            <n-checkbox
                              v-if="isWindows"
                              v-model:checked="command.elevated"
                              size="small"
                            >
                              {{ $t('_common.elevated') }}
                            </n-checkbox>
                            <n-button
                              size="small"
                              type="error"
                              secondary
                              @click="removeClientCommand(client.editDoCommands, index)"
                            >
                              <i class="fas fa-trash" />
                            </n-button>
                          </div>
                        </div>
                      </div>
                    </div>

                    <div
                      class="space-y-3 rounded-xl border border-dark/10 dark:border-light/10 bg-light/60 dark:bg-dark/40 p-4"
                    >
                      <div class="flex items-center justify-between gap-3">
                        <div class="text-xs font-semibold uppercase tracking-wide opacity-70">
                          Disconnect Commands
                        </div>
                        <n-button
                          size="tiny"
                          tertiary
                          @click="addClientCommand(client.editUndoCommands)"
                        >
                          <i class="fas fa-plus" /> {{ $t('_common.add') }}
                        </n-button>
                      </div>
                      <div v-if="client.editUndoCommands.length === 0" class="text-xs opacity-70">
                        No commands configured.
                      </div>
                      <div v-else class="space-y-2">
                        <div
                          v-for="(command, index) in client.editUndoCommands"
                          :key="`undo-${client.uuid}-${index}`"
                          class="rounded-md border border-dark/10 dark:border-light/10 p-3"
                        >
                          <div class="grid gap-3 md:grid-cols-[1fr_auto_auto]">
                            <n-input
                              v-model:value="command.cmd"
                              class="font-mono"
                              :placeholder="$t('_common.cmd')"
                            />
                            <n-checkbox
                              v-if="isWindows"
                              v-model:checked="command.elevated"
                              size="small"
                            >
                              {{ $t('_common.elevated') }}
                            </n-checkbox>
                            <n-button
                              size="small"
                              type="error"
                              secondary
                              @click="removeClientCommand(client.editUndoCommands, index)"
                            >
                              <i class="fas fa-trash" />
                            </n-button>
                          </div>
                        </div>
                      </div>
                    </div>
                  </div>
                  <div v-if="isWindows" class="space-y-3 lg:col-span-2">
                    <n-checkbox
                      v-model:checked="client.editDisplayOverrideEnabled"
                      size="small"
                      @update:checked="(v: boolean) => applyClientDisplayOverrideEnabled(client, v)"
                    >
                      <div class="flex flex-col">
                        <span>{{ t('config.client_display_override_label') }}</span>
                        <span class="text-[11px] opacity-60">
                          {{ t('config.client_display_override_hint') }}
                        </span>
                      </div>
                    </n-checkbox>

                    <div v-if="client.editDisplayOverrideEnabled" class="client-advanced-panel">
                      <div class="space-y-2">
                        <div class="flex items-center justify-between gap-3">
                          <span class="text-xs font-semibold uppercase opacity-70">
                            {{ t('config.client_display_override_label') }}
                          </span>
                        </div>
                        <p class="text-[11px] opacity-70">
                          {{ t('config.client_display_override_hint') }}
                        </p>
                      </div>

                      <div class="space-y-2">
                        <n-radio-group
                          :value="client.editDisplaySelection"
                          @update:value="
                            (v: string) =>
                              applyClientDisplaySelection(client, v as ClientDisplaySelection)
                          "
                          class="grid gap-3 sm:grid-cols-2"
                        >
                          <n-radio value="virtual" class="app-radio-card cursor-pointer">
                            <span class="app-radio-card-title">{{
                              t('config.app_display_override_virtual')
                            }}</span>
                          </n-radio>
                          <n-radio value="physical" class="app-radio-card cursor-pointer">
                            <span class="app-radio-card-title">{{
                              t('config.app_display_override_physical')
                            }}</span>
                          </n-radio>
                        </n-radio-group>
                      </div>

                      <div v-if="client.editDisplaySelection === 'physical'" class="space-y-2">
                        <div class="flex items-center justify-between gap-3">
                          <span class="text-xs font-semibold uppercase opacity-70">
                            {{ t('config.app_display_physical_label') }}
                          </span>
                          <n-button
                            size="tiny"
                            tertiary
                            :loading="displayDevicesLoading"
                            @click="loadDisplayDevices"
                          >
                            {{ t('_common.refresh') }}
                          </n-button>
                        </div>
                        <p class="text-[11px] opacity-70">
                          {{ t('config.app_display_physical_hint') }}
                        </p>
                        <n-select
                          v-model:value="client.editPhysicalOutputOverride"
                          :options="displayDeviceOptions"
                          :loading="displayDevicesLoading"
                          :placeholder="t('config.app_display_physical_placeholder')"
                          filterable
                          clearable
                          :fallback-option="
                            (value) => ({
                              label: value as string,
                              value: value as string,
                              displayName: value as string,
                              id: value as string,
                              active: null,
                            })
                          "
                          @focus="ensureDisplayDevicesLoaded"
                        >
                        </n-select>
                        <div class="text-[11px] opacity-70">
                          <span v-if="displayDevicesError" class="text-red-500">{{
                            displayDevicesError
                          }}</span>
                          <span v-else>{{ t('config.app_display_physical_status_hint') }}</span>
                        </div>
                      </div>

                      <div v-else class="space-y-5">
                        <div class="space-y-2">
                          <div class="flex items-center justify-between gap-3">
                            <span class="text-xs font-semibold uppercase opacity-70">
                              {{ t('config.virtual_display_mode_label') }}
                            </span>
                          </div>
                          <p class="text-[11px] opacity-70">
                            {{ t('config.virtual_display_mode_step_hint') }}
                          </p>
                          <n-radio-group
                            v-model:value="client.editVirtualDisplayMode"
                            class="grid gap-3 sm:grid-cols-2"
                          >
                            <n-radio
                              v-for="option in virtualDisplayModeOptions"
                              :key="String(option.value)"
                              :value="option.value"
                              class="app-radio-card cursor-pointer"
                            >
                              <span class="app-radio-card-title">{{ option.label }}</span>
                            </n-radio>
                          </n-radio-group>
                          <div
                            v-if="client.editVirtualDisplayMode === 'global'"
                            class="text-[11px] opacity-70"
                          >
                            {{ t('config.app_virtual_display_mode_follow_global') }}
                          </div>
                        </div>

                        <div class="space-y-2">
                          <div class="flex items-center justify-between gap-3">
                            <span class="text-xs font-semibold uppercase opacity-70">
                              {{ t('config.virtual_display_layout_label') }}
                            </span>
                            <n-button
                              v-if="client.editVirtualDisplayLayout !== null"
                              size="tiny"
                              tertiary
                              @click="client.editVirtualDisplayLayout = null"
                            >
                              {{ t('config.app_virtual_display_layout_reset') }}
                            </n-button>
                          </div>
                          <p class="text-[11px] opacity-70">
                            {{ t('config.virtual_display_layout_hint') }}
                          </p>
                          <n-radio-group
                            :value="
                              client.editVirtualDisplayLayout ??
                              globalVirtualDisplayLayout ??
                              'exclusive'
                            "
                            @update:value="
                              (v: string) =>
                                (client.editVirtualDisplayLayout =
                                  v === globalVirtualDisplayLayout
                                    ? null
                                    : (v as ClientVirtualDisplayLayout))
                            "
                            class="space-y-4"
                          >
                            <div
                              v-for="option in virtualDisplayLayoutOptions"
                              :key="option.value"
                              class="flex flex-col cursor-pointer py-2 px-2 rounded-md hover:bg-surface/10"
                              @click="
                                client.editVirtualDisplayLayout =
                                  option.value === globalVirtualDisplayLayout ? null : option.value
                              "
                              @keydown.enter.prevent="
                                client.editVirtualDisplayLayout =
                                  option.value === globalVirtualDisplayLayout ? null : option.value
                              "
                              @keydown.space.prevent="
                                client.editVirtualDisplayLayout =
                                  option.value === globalVirtualDisplayLayout ? null : option.value
                              "
                              tabindex="0"
                            >
                              <div class="flex items-center gap-3">
                                <n-radio :value="option.value" />
                                <span class="text-sm font-semibold">{{ option.label }}</span>
                              </div>
                              <span class="text-[11px] opacity-70 leading-snug ml-6">
                                {{ t(`config.virtual_display_layout_${option.value}_desc`) }}
                              </span>
                            </div>
                          </n-radio-group>
                          <div
                            v-if="client.editVirtualDisplayLayout === null"
                            class="text-[11px] opacity-70"
                          >
                            {{ t('config.app_virtual_display_layout_follow_global') }}
                          </div>
                        </div>
                      </div>
                    </div>
                  </div>

                  <div
                    v-if="isWindows && clientUsesSharedVirtualDisplay(client)"
                    class="rounded-lg border border-amber-200 bg-amber-50 p-3 dark:border-amber-800 dark:bg-amber-950/30 lg:col-span-2"
                  >
                    <p class="text-[11px] leading-snug text-amber-900 dark:text-amber-100">
                      <span class="flex items-start gap-2">
                        <i
                          class="fas fa-exclamation-triangle mt-0.5 flex-shrink-0 text-amber-600 dark:text-amber-400"
                        />
                        <span class="block">{{ t('clients.shared_display_hdr_warning') }}</span>
                      </span>
                    </p>
                  </div>

                  <n-form-item v-if="isWindows" :label="t('clients.hdr_profile_label')">
                    <n-select
                      v-model:value="client.editHdrProfile"
                      :options="hdrProfileOptions"
                      :loading="hdrProfilesLoading"
                      :placeholder="t('clients.hdr_profile_placeholder')"
                      filterable
                      clearable
                      @focus="ensureHdrProfilesLoaded"
                    />
                    <template #feedback>
                      <span class="text-xs opacity-70">{{ t('clients.hdr_profile_desc') }}</span>
                      <span v-if="hdrProfilesError" class="text-xs text-red-500 block">{{
                        hdrProfilesError
                      }}</span>
                    </template>
                  </n-form-item>

                  <n-form-item v-if="isWindows" :label="t('clients.hdr_peak_nits_label')">
                    <n-input-number
                      :value="clientNumericOverride(client, 'rtx_hdr_peak_brightness', 400, 2000)"
                      :min="400"
                      :max="2000"
                      :step="50"
                      clearable
                      :placeholder="t('clients.hdr_peak_nits_placeholder')"
                      @update:value="
                        (value) =>
                          setClientNumericOverride(
                            client,
                            'rtx_hdr_peak_brightness',
                            value,
                            400,
                            2000,
                          )
                      "
                    >
                      <template #suffix>nits</template>
                    </n-input-number>
                    <template #feedback>
                      <span class="text-xs opacity-70">{{ t('clients.hdr_peak_nits_desc') }}</span>
                    </template>
                  </n-form-item>

                  <n-form-item
                    v-if="isWindows && clientUsesVirtualDisplay(client)"
                    :label="t('clients.virtual_display_scale_label')"
                  >
                    <n-select
                      :value="clientNumericOverride(client, 'dd_virtual_display_scale', 0, 500)"
                      :options="clientVirtualDisplayScaleOptions"
                      clearable
                      :placeholder="
                        globalVirtualDisplayScale > 0
                          ? t('clients.virtual_display_scale_placeholder', {
                              scale: globalVirtualDisplayScale,
                            })
                          : t('config.virtual_display_scale_auto')
                      "
                      @update:value="
                        (value) =>
                          setClientNumericOverride(
                            client,
                            'dd_virtual_display_scale',
                            value,
                            0,
                            500,
                          )
                      "
                    />
                    <template #feedback>
                      <span class="text-xs opacity-70">{{
                        t('clients.virtual_display_scale_desc')
                      }}</span>
                    </template>
                  </n-form-item>

                  <n-form-item :label="t('config.prefer_10bit_sdr')">
                    <n-select
                      v-model:value="client.editPrefer10BitSdr"
                      :options="prefer10BitSdrOptions"
                      clearable
                      :placeholder="t('config.prefer_10bit_sdr_follow_global')"
                    />
                    <template #feedback>
                      <span class="text-xs opacity-70">{{
                        t('config.prefer_10bit_sdr_desc')
                      }}</span>
                      <span
                        v-if="client.editPrefer10BitSdr === null"
                        class="text-xs opacity-70 block"
                      >
                        {{ t('config.prefer_10bit_sdr_follow_global') }}
                        ({{ globalPrefer10BitSdr ? t('_common.enabled') : t('_common.disabled') }})
                      </span>
                    </template>
                  </n-form-item>

                  <AppEditConfigOverridesSection
                    class="lg:col-span-2"
                    v-model:overrides="client.editConfigOverrides"
                    scope-label="client"
                  />
                </n-form>
              </div>
            </article>
          </div>
        </template>
        <div v-else class="client-empty">
          <i class="fas fa-users-slash" />
          <span>
            {{
              clients.length > 0
                ? $t('clients.empty_filtered')
                : $t('troubleshooting.unpair_single_no_devices')
            }}
          </span>
          <small>{{
            clients.length > 0 ? $t('clients.empty_filtered_desc') : $t('clients.empty_pair_hint')
          }}</small>
          <n-button v-if="hasClientFilters" size="small" tertiary @click="clearClientFilters">
            {{ $t('clients.filters_clear') }}
          </n-button>
        </div>
      </div>
    </n-card>

    <TrustedDevicesCard />
    <section ref="apiTokensSectionRef" id="clients-api-tokens" class="clients-api-section">
      <ApiTokenManager />
    </section>

    <!-- Confirm remove single client -->
    <n-modal :show="showConfirmRemove" @update:show="setConfirmRemoveVisible">
      <n-card
        class="clients-modal-card"
        :title="
          $t('clients.confirm_remove_title_named', {
            name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
          })
        "
        style="max-width: 32rem; width: 100%"
        :bordered="false"
      >
        <div class="text-sm text-center">
          {{
            $t('clients.confirm_remove_message_named', {
              name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
            })
          }}
        </div>
        <template #footer>
          <div class="flex justify-end gap-2">
            <n-button :disabled="pendingRemoveBusy" @click="setConfirmRemoveVisible(false)">
              {{ $t('_common.cancel') }}
            </n-button>
            <n-button
              type="error"
              strong
              secondary
              :loading="pendingRemoveBusy"
              :disabled="pendingRemoveBusy"
              @click="confirmRemove"
            >
              {{ $t('clients.remove') }}
            </n-button>
          </div>
        </template>
      </n-card>
    </n-modal>

    <!-- Confirm unpair all -->
    <n-modal :show="showConfirmUnpairAll" @update:show="setConfirmUnpairAllVisible">
      <n-card
        class="clients-modal-card"
        :title="$t('clients.confirm_unpair_all_title')"
        style="max-width: 32rem; width: 100%"
        :bordered="false"
      >
        <div class="text-sm text-center">
          {{
            $t('clients.confirm_unpair_all_message_count', {
              count: clients.length,
            })
          }}
        </div>
        <template #footer>
          <div class="flex justify-end gap-2">
            <n-button :disabled="unpairAllPressed" @click="setConfirmUnpairAllVisible(false)">
              {{ $t('_common.cancel') }}
            </n-button>
            <n-button
              type="error"
              strong
              secondary
              :loading="unpairAllPressed"
              :disabled="unpairAllPressed"
              @click="confirmUnpairAll"
            >
              {{ $t('troubleshooting.unpair_all') }}
            </n-button>
          </div>
        </template>
      </n-card>
    </n-modal>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute, useRouter } from 'vue-router';
import { http } from '@/http';
import {
  NAlert,
  NButton,
  NCard,
  NCheckbox,
  NForm,
  NFormItem,
  NInput,
  NInputNumber,
  NModal,
  NRadio,
  NRadioGroup,
  NSelect,
  NTag,
  useMessage,
} from 'naive-ui';
import ApiTokenManager from '@/ApiTokenManager.vue';
import TrustedDevicesCard from '@/components/TrustedDevicesCard.vue';
import AppEditConfigOverridesSection from '@/components/app-edit/AppEditConfigOverridesSection.vue';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';

type ClientDisplaySelection = 'physical' | 'virtual';
type ClientVirtualDisplayMode = 'disabled' | 'per_client' | 'shared' | 'global' | null;
type ClientVirtualDisplayLayout =
  | 'exclusive'
  | 'extended'
  | 'extended_primary'
  | 'extended_isolated'
  | 'extended_primary_isolated'
  | null;
type ClientPrefer10BitSdrOverride = 'enabled' | 'disabled' | null;
type ClientSortMode = 'recent' | 'name' | 'status';
type ClientStatusFilter = 'all' | 'connected' | 'offline';

type PermissionToggleKey =
  | 'list'
  | 'view'
  | 'launch'
  | 'clipboard_set'
  | 'clipboard_read'
  | 'server_cmd'
  | 'input_controller'
  | 'input_touch'
  | 'input_pen'
  | 'input_mouse'
  | 'input_kbd';

interface PermissionGroup {
  id: string;
  labelKey: string;
  permissions: Array<{ key: PermissionToggleKey; suppressedBy: PermissionToggleKey[] }>;
}

const permissionMapping = {
  input_controller: 0x00000100,
  input_touch: 0x00000200,
  input_pen: 0x00000400,
  input_mouse: 0x00000800,
  input_kbd: 0x00001000,
  _all_inputs: 0x00001f00,
  clipboard_set: 0x00010000,
  clipboard_read: 0x00020000,
  file_upload: 0x00040000,
  file_dwnload: 0x00080000,
  server_cmd: 0x00100000,
  _all_operations: 0x001f0000,
  list: 0x01000000,
  view: 0x02000000,
  launch: 0x04000000,
  _allow_view: 0x06000000,
  _all_actions: 0x07000000,
  _default: 0x03000000,
  _no: 0x00000000,
  _all: 0x071f1f00,
} as const;

const permissionGroups: PermissionGroup[] = [
  {
    id: 'actions',
    labelKey: 'permissions.group_action',
    permissions: [
      { key: 'list', suppressedBy: ['view', 'launch'] },
      { key: 'view', suppressedBy: ['launch'] },
      { key: 'launch', suppressedBy: [] },
    ],
  },
  {
    id: 'operations',
    labelKey: 'permissions.group_operation',
    permissions: [
      { key: 'clipboard_set', suppressedBy: [] },
      { key: 'clipboard_read', suppressedBy: [] },
      { key: 'server_cmd', suppressedBy: [] },
    ],
  },
  {
    id: 'inputs',
    labelKey: 'permissions.group_input',
    permissions: [
      { key: 'input_controller', suppressedBy: [] },
      { key: 'input_touch', suppressedBy: [] },
      { key: 'input_pen', suppressedBy: [] },
      { key: 'input_mouse', suppressedBy: [] },
      { key: 'input_kbd', suppressedBy: [] },
    ],
  },
];

const highlightPermissionThreshold = 0x04000000;
interface ClientApiEntry {
  uuid?: string;
  name?: string;
  connected?: boolean;
  last_seen?: number | string | null;
  perm?: number | string;
  hdr_profile?: string;
  display_mode?: string;
  output_name_override?: string;
  always_use_virtual_display?: boolean | string | number;
  virtual_display_mode?: string;
  virtual_display_layout?: string;
  prefer_10bit_sdr?: boolean | string | number | null;
  config_overrides?: Record<string, unknown> | null;
  allow_client_commands?: boolean | string | number;
  do?: unknown;
  undo?: unknown;
}

interface ClientsListResponse {
  status: boolean;
  named_certs: ClientApiEntry[];
  platform?: string;
}

interface HdrProfileEntry {
  filename?: string;
  added_ms?: number;
}

interface HdrProfilesResponse {
  status?: boolean;
  profiles?: HdrProfileEntry[];
  error?: string;
}

interface ClientViewModel {
  uuid: string;
  name: string;
  connected: boolean;
  lastSeen: number | null;
  perm: number;
  hdrProfile: string;
  displayMode: string;
  outputOverride: string;
  alwaysUseVirtualDisplay: boolean;
  prefer10BitSdr: ClientPrefer10BitSdrOverride;
  virtualDisplayMode: ClientVirtualDisplayMode;
  virtualDisplayLayout: ClientVirtualDisplayLayout;
  configOverrides: Record<string, unknown>;
  allowClientCommands: boolean;
  doCommands: ClientCommandEntry[];
  undoCommands: ClientCommandEntry[];

  editing: boolean;
  editHdrProfile: string | null;
  editName: string;
  editDisplayMode: string;
  editPerm: number;
  editDisplayOverrideEnabled: boolean;
  editDisplaySelection: ClientDisplaySelection;
  editPhysicalOutputOverride: string | null;
  editVirtualDisplayMode: ClientVirtualDisplayMode;
  editVirtualDisplayLayout: ClientVirtualDisplayLayout;
  editPrefer10BitSdr: ClientPrefer10BitSdrOverride;
  editConfigOverrides: Record<string, unknown>;
  editAllowClientCommands: boolean;
  editDoCommands: ClientCommandEntry[];
  editUndoCommands: ClientCommandEntry[];
}

interface ClientCommandEntry {
  cmd: string;
  elevated: boolean;
}

interface DisplayDevice {
  device_id?: string;
  display_name?: string;
  friendly_name?: string;
  info?: unknown;
}

interface DisplayDeviceInfo {
  active?: unknown;
}

interface ClientUpdatePayload {
  uuid: string;
  name: string;
  hdr_profile: string;
  display_mode: string;
  perm: number;
  allow_client_commands: boolean;
  do: ClientCommandEntry[];
  undo: ClientCommandEntry[];  output_name_override: string;
  always_use_virtual_display: boolean;
  virtual_display_mode: string | null;
  virtual_display_layout: string | null;
  config_overrides?: Record<string, unknown>;
  prefer_10bit_sdr?: boolean;
}

type UnknownRecord = Record<string, unknown>;

const { t } = useI18n();
const route = useRoute();
const router = useRouter();
const message = useMessage();
const configStore = useConfigStore();
const apiTokensSectionRef = ref<HTMLElement | null>(null);
const globalPrefer10BitSdr = computed<boolean>(() =>
  toBool(configValue('prefer_10bit_sdr'), false),
);
const prefer10BitSdrOptions = computed(() => [
  { label: t('_common.enabled'), value: 'enabled' },
  { label: t('_common.disabled'), value: 'disabled' },
]);

const clients = ref<ClientViewModel[]>([]);
const clientsLoading = ref<boolean>(true);
const clientsReady = ref<boolean>(false);
const clientsRefreshing = ref<boolean>(false);
const lastRefreshedAt = ref<number | null>(null);
const platform = ref<string>('');
const clientSearchQuery = ref<string>('');
const clientStatusFilter = ref<ClientStatusFilter>('all');
const clientSortMode = ref<ClientSortMode>('recent');
const connectedClientCount = computed(
  () => clients.value.filter((client) => client.connected).length,
);
const offlineClientCount = computed(() =>
  Math.max(0, clients.value.length - connectedClientCount.value),
);
const hasClientFilters = computed(
  () => clientSearchQuery.value.trim().length > 0 || clientStatusFilter.value !== 'all',
);

const pin = ref<string>('');
const deviceName = ref<string>('');
const canPairClient = computed(
  () => /^[0-9]{4}$/.test(pin.value.trim()) && deviceName.value.trim().length > 0,
);

function updatePin(value: string): void {
  pin.value = String(value || '')
    .replace(/\D/g, '')
    .slice(0, 4);
}
const pairing = ref<boolean>(false);
const pairStatus = ref<boolean | null>(null);

const unpairAllPressed = ref<boolean>(false);
const unpairAllStatus = ref<boolean | null>(null);
const removing = ref<Record<string, boolean>>({});
const saving = ref<Record<string, boolean>>({});
const disconnecting = ref<Record<string, boolean>>({});
let refreshIntervalId: ReturnType<typeof setInterval> | null = null;

const showConfirmRemove = ref<boolean>(false);
const pendingRemoveUuid = ref<string>('');
const pendingRemoveName = ref<string>('');
const pendingRemoveBusy = computed(
  () => !!pendingRemoveUuid.value && removing.value[pendingRemoveUuid.value] === true,
);
const showConfirmUnpairAll = ref<boolean>(false);

const isWindows = computed(() => {
  const p = (platform.value || '').toLowerCase();
  if (p) return p.startsWith('win') || p === 'windows';
  const metadata = configStore.metadata as UnknownRecord | null | undefined;
  const meta = String(metadata?.['platform'] || '').toLowerCase();
  return meta === 'windows' || meta.startsWith('win');
});

function configValue(key: string): unknown {
  const config = configStore.config as UnknownRecord | null | undefined;
  return config?.[key];
}

function toBool(value: unknown, fallback = false): boolean {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return value !== 0;
  if (typeof value === 'string') {
    const v = value.trim().toLowerCase();
    if (['1', 'true', 'yes', 'on', 'enabled'].includes(v)) return true;
    if (['0', 'false', 'no', 'off', 'disabled', ''].includes(v)) return false;
  }
  return fallback;
}

function permToStr(perm: number): string {
  const segments = [];
  segments.push((perm >> 24) & 0xff);
  segments.push((perm >> 16) & 0xff);
  segments.push((perm >> 8) & 0xff);
  return segments.map((seg) => seg.toString(16).toUpperCase().padStart(2, '0')).join(' ');
}

function checkPermission(perm: number, permission: PermissionToggleKey): boolean {
  return (perm & permissionMapping[permission]) !== 0;
}

function isSuppressed(
  perm: number,
  permission: PermissionToggleKey,
  suppressedBy: PermissionToggleKey[],
): boolean {
  return suppressedBy.some((suppressed) => checkPermission(perm, suppressed));
}

function togglePermission(client: ClientViewModel, permission: PermissionToggleKey): void {
  client.editPerm ^= permissionMapping[permission];
}

function parseClientVirtualDisplayMode(value: unknown): ClientVirtualDisplayMode {
  const v = String(value ?? '')
    .trim()
    .toLowerCase();
  if (!v) return null;
  if (v === 'disabled' || v === 'per_client' || v === 'shared' || v === 'global')
    return v as ClientVirtualDisplayMode;
  return null;
}

function parseClientVirtualDisplayLayout(value: unknown): ClientVirtualDisplayLayout {
  const v = String(value ?? '')
    .trim()
    .toLowerCase();
  if (!v) return null;
  if (
    v === 'exclusive' ||
    v === 'extended' ||
    v === 'extended_primary' ||
    v === 'extended_isolated' ||
    v === 'extended_primary_isolated'
  )
    return v as ClientVirtualDisplayLayout;
  return null;
}

function parseLastSeen(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value) && value > 0) return value;
  if (typeof value === 'string') {
    const n = Number(value);
    if (Number.isFinite(n) && n > 0) return n;
  }
  return null;
}

function normalizeClientCommandEntry(value: unknown): ClientCommandEntry | null {
  if (typeof value === 'string') {
    return { cmd: value, elevated: false };
  }
  if (!value || typeof value !== 'object') return null;
  const obj = value as Record<string, unknown>;
  const cmd = String(obj['cmd'] ?? '').trim();
  if (!cmd) return null;
  return {
    cmd,
    elevated: toBool(obj['elevated'], false),
  };
}

function normalizeClientCommandList(value: unknown): ClientCommandEntry[] {
  if (!Array.isArray(value)) return [];
  return value
    .map((entry) => normalizeClientCommandEntry(entry))
    .filter((entry): entry is ClientCommandEntry => !!entry);
}

function createClientViewModel(entry: ClientApiEntry): ClientViewModel {
  const name = entry.name ?? '';
  const displayMode = entry.display_mode ?? '';
  const outputOverride = entry.output_name_override ?? '';
  const alwaysVirtual = toBool(entry.always_use_virtual_display, false);
  const hdrProfile = String(entry.hdr_profile ?? '').trim();
  const lastSeen = parseLastSeen(entry.last_seen);
  const perm =
    typeof entry.perm === 'number'
      ? entry.perm
      : Number.parseInt(String(entry.perm ?? '0'), 10) || 0;
  const configOverrides =
    entry.config_overrides &&
    typeof entry.config_overrides === 'object' &&
    !Array.isArray(entry.config_overrides)
      ? JSON.parse(JSON.stringify(entry.config_overrides))
      : {};
  const prefer10: ClientPrefer10BitSdrOverride =
    entry.prefer_10bit_sdr === undefined || entry.prefer_10bit_sdr === null
      ? null
      : toBool(entry.prefer_10bit_sdr, false)
        ? 'enabled'
        : 'disabled';
  const virtualMode = parseClientVirtualDisplayMode(entry.virtual_display_mode ?? '');
  const virtualLayout = parseClientVirtualDisplayLayout(entry.virtual_display_layout ?? '');
  const allowClientCommands = toBool(entry.allow_client_commands, true);
  const doCommands = normalizeClientCommandList(entry.do);
  const undoCommands = normalizeClientCommandList(entry.undo);
  const overrideEnabled =
    alwaysVirtual || !!outputOverride.trim() || virtualMode !== null || virtualLayout !== null;
  const selection: ClientDisplaySelection =
    alwaysVirtual || (virtualMode !== null && virtualMode !== 'disabled') ? 'virtual' : 'physical';
  const client: ClientViewModel = {
    uuid: entry.uuid ?? '',
    name,
    connected: !!entry.connected,
    lastSeen,
    perm,
    hdrProfile,
    displayMode,
    outputOverride,
    alwaysUseVirtualDisplay: alwaysVirtual,
    prefer10BitSdr: prefer10,
    virtualDisplayMode: virtualMode,
    virtualDisplayLayout: virtualLayout,
    configOverrides,
    allowClientCommands,
    doCommands,
    undoCommands,
    editing: false,
    editHdrProfile: hdrProfile || null,
    editName: name,
    editDisplayMode: displayMode,
    editPerm: perm,
    editDisplayOverrideEnabled: overrideEnabled,
    editDisplaySelection: selection,
    editPhysicalOutputOverride: outputOverride || null,
    editVirtualDisplayMode: virtualMode,
    editVirtualDisplayLayout: virtualLayout,
    editPrefer10BitSdr: prefer10,
    editConfigOverrides: JSON.parse(JSON.stringify(configOverrides)),
    editAllowClientCommands: allowClientCommands,
    editDoCommands: JSON.parse(JSON.stringify(doCommands)),
    editUndoCommands: JSON.parse(JSON.stringify(undoCommands)),
  };

  if (client.editDisplayOverrideEnabled) {
    applyClientDisplaySelection(client, client.editDisplaySelection);
  }

  return client;
}

function resetClientEdits(client: ClientViewModel): void {
  client.editName = client.name;
  client.editHdrProfile = (client.hdrProfile || '').trim() || null;
  client.editDisplayMode = client.displayMode;
  client.editPerm = client.perm;
  client.editDisplayOverrideEnabled =
    client.alwaysUseVirtualDisplay ||
    !!(client.outputOverride || '').trim() ||
    client.virtualDisplayMode !== null ||
    client.virtualDisplayLayout !== null;
  client.editDisplaySelection =
    client.alwaysUseVirtualDisplay ||
    (client.virtualDisplayMode !== null && client.virtualDisplayMode !== 'disabled')
      ? 'virtual'
      : 'physical';
  client.editPhysicalOutputOverride = client.outputOverride || null;
  client.editVirtualDisplayMode = client.virtualDisplayMode;
  client.editVirtualDisplayLayout = client.virtualDisplayLayout;
  client.editPrefer10BitSdr = client.prefer10BitSdr;
  client.editConfigOverrides = JSON.parse(JSON.stringify(client.configOverrides || {}));
  client.editAllowClientCommands = client.allowClientCommands;
  client.editDoCommands = JSON.parse(JSON.stringify(client.doCommands || []));
  client.editUndoCommands = JSON.parse(JSON.stringify(client.undoCommands || []));

  if (client.editDisplayOverrideEnabled) {
    applyClientDisplaySelection(client, client.editDisplaySelection);
  }
}

function addClientCommand(commands: ClientCommandEntry[], index = -1): void {
  const next: ClientCommandEntry = {
    cmd: '',
    elevated: false,
  };
  if (index < 0 || index >= commands.length) {
    commands.push(next);
    return;
  }
  commands.splice(index + 1, 0, next);
}

function removeClientCommand(commands: ClientCommandEntry[], index: number): void {
  if (index < 0 || index >= commands.length) return;
  commands.splice(index, 1);
}

const virtualDisplayModeOptions = computed(() => [
  { label: t('config.app_virtual_display_mode_follow_global'), value: 'global' },
  { label: t('config.virtual_display_mode_per_client'), value: 'per_client' },
  { label: t('config.virtual_display_mode_shared'), value: 'shared' },
]);

const globalVirtualDisplayMode = computed<'disabled' | 'per_client' | 'shared'>(() => {
  const mode = parseClientVirtualDisplayMode(configValue('virtual_display_mode'));
  return mode === 'disabled' || mode === 'shared' ? mode : 'per_client';
});

const globalVirtualDisplayScale = computed<number>(() => {
  const value = Number(configValue('dd_virtual_display_scale'));
  return Number.isFinite(value) ? value : 250;
});

const clientVirtualDisplayScaleOptions = computed(() => [
  { label: t('config.virtual_display_scale_auto'), value: 0 },
  ...[100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500].map((value) => ({
    label: `${value}%`,
    value,
  })),
]);

function clientUsesVirtualDisplay(client: ClientViewModel): boolean {
  if (!client.editDisplayOverrideEnabled) {
    return globalVirtualDisplayMode.value !== 'disabled';
  }
  return client.editDisplaySelection === 'virtual';
}

function clientUsesSharedVirtualDisplay(client: ClientViewModel): boolean {
  if (!clientUsesVirtualDisplay(client)) return false;
  if (!client.editDisplayOverrideEnabled) {
    return globalVirtualDisplayMode.value === 'shared';
  }
  return (
    client.editVirtualDisplayMode === 'shared' ||
    ((client.editVirtualDisplayMode === 'global' || client.editVirtualDisplayMode === null) &&
      globalVirtualDisplayMode.value === 'shared')
  );
}

function clientNumericOverride(
  client: ClientViewModel,
  key: string,
  min: number,
  max: number,
): number | null {
  const rawValue = client.editConfigOverrides?.[key];
  if (rawValue === null || rawValue === undefined || rawValue === '') return null;
  const value = Number(rawValue);
  return Number.isFinite(value) && value >= min && value <= max ? value : null;
}

function setClientNumericOverride(
  client: ClientViewModel,
  key: string,
  value: number | null,
  min: number,
  max: number,
): void {
  if (value === null || !Number.isFinite(value)) {
    delete client.editConfigOverrides[key];
    return;
  }
  client.editConfigOverrides[key] = Math.min(max, Math.max(min, Number(value)));
}

const globalVirtualDisplayLayout = computed<ClientVirtualDisplayLayout>(() =>
  parseClientVirtualDisplayLayout(configValue('virtual_display_layout')),
);

const virtualDisplayLayoutOptions = computed(() => {
  const values: Array<Exclude<ClientVirtualDisplayLayout, null>> = [
    'exclusive',
    'extended',
    'extended_primary',
    'extended_isolated',
    'extended_primary_isolated',
  ];
  return values.map((value) => ({ label: t(`config.virtual_display_layout_${value}`), value }));
});

const hdrProfiles = ref<HdrProfileEntry[]>([]);
const hdrProfilesLoading = ref(false);
const hdrProfilesError = ref('');

const hdrProfileOptions = computed<any[]>(() => {
  const list = Array.isArray(hdrProfiles.value) ? [...hdrProfiles.value] : [];
  list.sort((a, b) => (Number(b.added_ms || 0) || 0) - (Number(a.added_ms || 0) || 0));
  const options: any[] = [{ label: t('clients.hdr_profile_auto'), value: null }];
  for (const p of list) {
    const filename = String(p?.filename || '').trim();
    if (!filename) continue;
    options.push({ label: filename, value: filename });
  }
  return options;
});

async function loadHdrProfiles(): Promise<void> {
  if (!isWindows.value) return;
  hdrProfilesLoading.value = true;
  hdrProfilesError.value = '';
  try {
    const r = await http.get<HdrProfilesResponse>('./api/clients/hdr-profiles', {
      validateStatus: () => true,
    });
    const response = r.data || ({} as HdrProfilesResponse);
    const ok =
      r.status >= 200 &&
      r.status < 300 &&
      response.status === true &&
      Array.isArray(response.profiles);
    if (!ok) {
      hdrProfiles.value = [];
      hdrProfilesError.value = response.error || t('clients.hdr_profile_load_failed');
      return;
    }
    hdrProfiles.value = response.profiles || [];
  } catch (e: any) {
    hdrProfiles.value = [];
    hdrProfilesError.value = e?.message || t('clients.hdr_profile_load_failed');
  } finally {
    hdrProfilesLoading.value = false;
  }
}

function ensureHdrProfilesLoaded(): void {
  if (!isWindows.value) return;
  if (!hdrProfilesLoading.value && hdrProfiles.value.length === 0) {
    void loadHdrProfiles();
  }
}

function applyClientDisplayOverrideEnabled(client: ClientViewModel, enabled: boolean): void {
  client.editDisplayOverrideEnabled = enabled;
  if (!enabled) {
    client.editDisplaySelection = 'physical';
    client.editPhysicalOutputOverride = null;
    client.editVirtualDisplayMode = null;
    client.editVirtualDisplayLayout = null;
    return;
  }

  applyClientDisplaySelection(client, client.editDisplaySelection);
}

function applyClientDisplaySelection(
  client: ClientViewModel,
  selection: ClientDisplaySelection,
): void {
  client.editDisplaySelection = selection;
  if (selection === 'physical') {
    client.editVirtualDisplayMode = 'disabled';
    client.editVirtualDisplayLayout = null;
    return;
  }

  client.editPhysicalOutputOverride = null;
  if (client.editVirtualDisplayMode === null || client.editVirtualDisplayMode === 'disabled') {
    client.editVirtualDisplayMode = 'global';
  }
}

const isClientDisplayOverrideValid = computed(() => {
  for (const client of clients.value) {
    if (!client.editing) continue;
    if (!client.editDisplayOverrideEnabled) continue;

    if (client.editDisplaySelection === 'virtual') {
      if (
        client.editVirtualDisplayMode !== 'global' &&
        client.editVirtualDisplayMode !== 'per_client' &&
        client.editVirtualDisplayMode !== 'shared'
      ) {
        return false;
      }
    }
  }
  return true;
});

async function refreshClients(options: { manual?: boolean } = {}): Promise<void> {
  const auth = useAuthStore();
  if (!auth.isAuthenticated) {
    clientsReady.value = true;
    clientsLoading.value = false;
    clientsRefreshing.value = false;
    return;
  }
  if (options.manual && clientsRefreshing.value) return;
  if (options.manual) {
    clientsRefreshing.value = true;
  }
  if (!clientsReady.value) {
    clientsLoading.value = true;
  }
  try {
    const r = await http.get<ClientsListResponse>('./api/clients/list', {
      validateStatus: () => true,
    });
    const response = r.data || ({} as ClientsListResponse);
    if (typeof response.platform === 'string') {
      platform.value = response.platform;
    }
    if (response.status === true && Array.isArray(response.named_certs)) {
      const prior = new Map(clients.value.map((client) => [client.uuid, client] as const));
      const mapped = response.named_certs.map((entry) => {
        const uuid = entry.uuid ?? '';
        const existing = uuid ? prior.get(uuid) : undefined;
        if (existing?.editing) {
          existing.connected = !!entry.connected;
          existing.lastSeen = parseLastSeen(entry.last_seen);
          return existing;
        }
        return createClientViewModel(entry);
      });
      clients.value = mapped;
      lastRefreshedAt.value = Date.now();
      ensureDisplayDevicesLoaded();
    } else {
      clients.value = [];
      lastRefreshedAt.value = Date.now();
    }
  } catch {
    clients.value = [];
  } finally {
    clientsReady.value = true;
    clientsLoading.value = false;
    clientsRefreshing.value = false;
  }
}

async function manualRefreshClients(): Promise<void> {
  await refreshClients({ manual: true });
}

const clientStatusOptions = computed(() => [
  { label: t('clients.status_all'), value: 'all' },
  { label: t('clients.status_connected'), value: 'connected' },
  { label: t('clients.status_offline'), value: 'offline' },
]);

const clientSortOptions = computed(() => [
  { label: t('clients.sort_recent'), value: 'recent' },
  { label: t('clients.sort_name'), value: 'name' },
  { label: t('clients.sort_status'), value: 'status' },
]);

function compareByName(a: ClientViewModel, b: ClientViewModel): number {
  const nameA = (a.name || '').toLowerCase();
  const nameB = (b.name || '').toLowerCase();
  if (nameA === nameB) return a.uuid.localeCompare(b.uuid);
  if (nameA === '') return 1;
  if (nameB === '') return -1;
  return nameA.localeCompare(nameB);
}

const clientTimeFormatter = new Intl.DateTimeFormat(undefined, {
  dateStyle: 'medium',
  timeStyle: 'short',
});

const clientRefreshTimeFormatter = new Intl.DateTimeFormat(undefined, {
  timeStyle: 'short',
});

function formatClientTimestamp(seconds: number): string {
  return clientTimeFormatter.format(new Date(seconds * 1000));
}

const lastRefreshedLabel = computed(() => {
  if (!lastRefreshedAt.value) return t('clients.last_updated_never');
  return t('clients.last_updated', {
    time: clientRefreshTimeFormatter.format(new Date(lastRefreshedAt.value)),
  });
});

function lastSeenLabel(client: ClientViewModel): string {
  if (!client.lastSeen || !Number.isFinite(client.lastSeen)) {
    return t('clients.last_seen_unknown');
  }
  return t('clients.last_seen', { time: formatClientTimestamp(client.lastSeen) });
}

function clientDisplayName(client: ClientViewModel): string {
  return client.name || t('troubleshooting.unpair_single_unknown');
}

function shortClientUuid(uuid: string): string {
  if (uuid.length <= 8) return uuid;
  return uuid.slice(0, 8);
}

function displayRoutingLabel(client: ClientViewModel): string {
  if (client.outputOverride) {
    return t('clients.display_route_physical', { display: client.outputOverride });
  }
  if (
    client.alwaysUseVirtualDisplay ||
    (client.virtualDisplayMode !== null && client.virtualDisplayMode !== 'disabled')
  ) {
    const mode =
      client.virtualDisplayMode === 'shared'
        ? t('config.virtual_display_mode_shared')
        : client.virtualDisplayMode === 'per_client'
          ? t('config.virtual_display_mode_per_client')
          : t('config.app_virtual_display_mode_follow_global');
    return t('clients.display_route_virtual', { mode });
  }
  return t('clients.display_route_global');
}

function searchableClientText(client: ClientViewModel): string {
  return [
    client.name,
    client.uuid,
    client.displayMode,
    client.hdrProfile,
    client.outputOverride,
    displayRoutingLabel(client),
    client.connected ? t('clients.connected') : t('clients.offline'),
  ]
    .filter(Boolean)
    .join(' ')
    .toLowerCase();
}

function clearClientFilters(): void {
  clientSearchQuery.value = '';
  clientStatusFilter.value = 'all';
}

const filteredClients = computed<ClientViewModel[]>(() => {
  const query = clientSearchQuery.value.trim().toLowerCase();
  return clients.value.filter((client) => {
    if (clientStatusFilter.value === 'connected' && !client.connected) return false;
    if (clientStatusFilter.value === 'offline' && client.connected) return false;
    if (!query) return true;
    return searchableClientText(client).includes(query);
  });
});

const visibleClients = computed<ClientViewModel[]>(() => {
  const list = [...filteredClients.value];
  if (clientSortMode.value === 'recent') {
    list.sort((a, b) => {
      if (a.connected !== b.connected) return a.connected ? -1 : 1;
      const lastA = a.lastSeen ?? 0;
      const lastB = b.lastSeen ?? 0;
      if (lastA !== lastB) return lastB - lastA;
      return compareByName(a, b);
    });
    return list;
  }
  if (clientSortMode.value === 'status') {
    list.sort((a, b) => {
      if (a.connected !== b.connected) return a.connected ? -1 : 1;
      return compareByName(a, b);
    });
    return list;
  }
  list.sort(compareByName);
  return list;
});

async function registerDevice(): Promise<void> {
  if (pairing.value) return;
  pairStatus.value = null;
  pairing.value = true;
  try {
    const trimmedName = deviceName.value.trim();
    const body = { pin: pin.value.trim(), name: trimmedName };
    const r = await http.post('./api/pin', body, { validateStatus: () => true });
    const ok =
      r &&
      r.status >= 200 &&
      r.status < 300 &&
      (r.data?.status === true || r.data?.status === 'true' || r.data?.status === 1);
    pairStatus.value = !!ok;
    if (ok) {
      const prevCount = clients.value?.length || 0;
      await refreshClients({ manual: true });
      const deadline = Date.now() + 5000;
      const target = trimmedName.toLowerCase();
      while (Date.now() < deadline) {
        const found = clients.value?.some((c) => (c.name || '').toLowerCase() === target);
        if (found || (clients.value?.length || 0) > prevCount) break;
        await new Promise((res) => setTimeout(res, 400));
        await refreshClients();
      }
      pin.value = '';
      deviceName.value = '';
    }
  } catch {
    pairStatus.value = false;
  } finally {
    pairing.value = false;
    setTimeout(() => {
      pairStatus.value = null;
    }, 5000);
  }
}

function askConfirmUnpair(client: ClientViewModel): void {
  pendingRemoveUuid.value = client.uuid;
  pendingRemoveName.value = client && client.name ? client.name : '';
  showConfirmRemove.value = true;
}

function setConfirmRemoveVisible(value: boolean): void {
  if (!value && pendingRemoveBusy.value) return;
  showConfirmRemove.value = value;
}

async function confirmRemove(): Promise<void> {
  const uuid = pendingRemoveUuid.value;
  if (!uuid) return;
  const removed = await unpairSingle(uuid);
  if (removed) {
    showConfirmRemove.value = false;
    pendingRemoveUuid.value = '';
    pendingRemoveName.value = '';
  }
}

async function unpairSingle(uuid: string): Promise<boolean> {
  if (removing.value[uuid]) return false;
  removing.value = { ...removing.value, [uuid]: true };
  let removed = false;
  try {
    const r = await http.post('./api/clients/unpair', { uuid }, { validateStatus: () => true });
    if (r.status >= 200 && r.status < 300 && r.data?.status === true) {
      removed = true;
      message.success(t('clients.remove_success'));
    } else {
      message.error(t('clients.remove_failed'));
    }
  } catch {
    message.error(t('clients.remove_failed'));
  } finally {
    delete removing.value[uuid];
    removing.value = { ...removing.value };
    refreshClients();
  }
  return removed;
}

function askConfirmUnpairAll(): void {
  showConfirmUnpairAll.value = true;
}

function setConfirmUnpairAllVisible(value: boolean): void {
  if (!value && unpairAllPressed.value) return;
  showConfirmUnpairAll.value = value;
}

async function confirmUnpairAll(): Promise<void> {
  const removed = await unpairAll();
  if (removed) {
    showConfirmUnpairAll.value = false;
  }
}

async function unpairAll(): Promise<boolean> {
  unpairAllPressed.value = true;
  let removed = false;
  try {
    const r = await http.post('./api/clients/unpair-all', {}, { validateStatus: () => true });
    removed = r.data?.status === true;
    unpairAllStatus.value = removed;
    if (removed) {
      message.success(t('troubleshooting.unpair_all_success'));
    } else {
      message.error(t('troubleshooting.unpair_all_error'));
    }
  } catch {
    unpairAllStatus.value = false;
    message.error(t('troubleshooting.unpair_all_error'));
  } finally {
    unpairAllPressed.value = false;
    setTimeout(() => {
      unpairAllStatus.value = null;
    }, 5000);
    refreshClients();
  }
  return removed;
}

function editClient(client: ClientViewModel): void {
  for (const c of clients.value) {
    if (c.uuid !== client.uuid && c.editing) {
      c.editing = false;
      resetClientEdits(c);
    }
  }
  resetClientEdits(client);
  client.editing = true;
  ensureDisplayDevicesLoaded();
  ensureHdrProfilesLoaded();
}

function cancelEdit(client: ClientViewModel): void {
  resetClientEdits(client);
  client.editing = false;
}

async function saveClient(client: ClientViewModel): Promise<void> {
  if (saving.value[client.uuid]) return;
  saving.value = { ...saving.value, [client.uuid]: true };
  try {
    const payload: ClientUpdatePayload = {
      uuid: client.uuid,
      name: (client.editName || '').trim(),
      hdr_profile: String(client.editHdrProfile ?? '').trim(),
      display_mode: (client.editDisplayMode || '').trim(),
      perm: client.editPerm & permissionMapping._all,
      allow_client_commands: !!client.editAllowClientCommands,
      do: client.editDoCommands.reduce((result: ClientCommandEntry[], entry) => {
        const cmd = String(entry?.cmd ?? '').trim();
        if (!cmd) return result;
        result.push({
          cmd,
          elevated: !!entry?.elevated,
        });
        return result;
      }, []),
      undo: client.editUndoCommands.reduce((result: ClientCommandEntry[], entry) => {
        const cmd = String(entry?.cmd ?? '').trim();
        if (!cmd) return result;
        result.push({
          cmd,
          elevated: !!entry?.elevated,
        });
        return result;
      }, []),      output_name_override: '',
      always_use_virtual_display: false,
      virtual_display_mode: '',
      virtual_display_layout: '',
    };

    if (!client.editDisplayOverrideEnabled) {
      payload.output_name_override = '';
      payload.always_use_virtual_display = false;
      payload.virtual_display_mode = '';
      payload.virtual_display_layout = '';
    } else if (client.editDisplaySelection === 'physical') {
      payload.output_name_override = String(client.editPhysicalOutputOverride || '').trim();
      payload.always_use_virtual_display = false;
      payload.virtual_display_mode = 'disabled';
      payload.virtual_display_layout = '';
    } else {
      payload.output_name_override = '';
      if (client.editVirtualDisplayMode === 'global' || client.editVirtualDisplayMode === null) {
        payload.always_use_virtual_display = false;
        payload.virtual_display_mode = 'global';
      } else {
        payload.always_use_virtual_display = true;
        payload.virtual_display_mode = client.editVirtualDisplayMode;
      }
      payload.virtual_display_layout = client.editVirtualDisplayLayout ?? '';
    }

    if (!isClientDisplayOverrideValid.value) {
      message.error(t('clients.update_failed'));
      return;
    }

    payload.config_overrides =
      client.editConfigOverrides &&
      typeof client.editConfigOverrides === 'object' &&
      !Array.isArray(client.editConfigOverrides)
        ? Object.fromEntries(
            Object.entries(client.editConfigOverrides).filter(
              ([k, v]) => typeof k === 'string' && k.length > 0 && v !== undefined && v !== null,
            ),
          )
        : {};
    if (client.editPrefer10BitSdr !== null) {
      payload.prefer_10bit_sdr = client.editPrefer10BitSdr === 'enabled';
    }
    payload.hdr_profile = String(client.editHdrProfile ?? '').trim();

    const r = await http.post('./api/clients/update', payload, { validateStatus: () => true });
    const ok = r && r.status >= 200 && r.status < 300 && r.data?.status === true;
    if (!ok) {
      message.error(t('clients.update_failed'));
      return;
    }

    client.name = payload.name;
    client.perm = payload.perm;
    client.hdrProfile = payload.hdr_profile;
    client.displayMode = payload.display_mode;
    client.outputOverride = payload.output_name_override;
    client.alwaysUseVirtualDisplay = payload.always_use_virtual_display;
    client.virtualDisplayMode = parseClientVirtualDisplayMode(payload.virtual_display_mode);
    client.virtualDisplayLayout = parseClientVirtualDisplayLayout(payload.virtual_display_layout);
    client.hdrProfile = payload.hdr_profile || '';
    client.allowClientCommands = payload.allow_client_commands;
    client.doCommands = JSON.parse(JSON.stringify(payload.do || []));
    client.undoCommands = JSON.parse(JSON.stringify(payload.undo || []));
    client.prefer10BitSdr =
      payload.prefer_10bit_sdr === undefined
        ? null
        : payload.prefer_10bit_sdr
          ? 'enabled'
          : 'disabled';
    client.configOverrides =
      payload.config_overrides &&
      typeof payload.config_overrides === 'object' &&
      !Array.isArray(payload.config_overrides)
        ? JSON.parse(JSON.stringify(payload.config_overrides))
        : {};

    resetClientEdits(client);
    client.editing = false;
    message.success(t('clients.update_success'));
  } catch (e: any) {
    message.error(e?.message || t('clients.update_failed'));
  } finally {
    delete saving.value[client.uuid];
    saving.value = { ...saving.value };
    refreshClients();
  }
}

async function disconnectClient(client: ClientViewModel): Promise<void> {
  if (disconnecting.value[client.uuid]) return;
  disconnecting.value = { ...disconnecting.value, [client.uuid]: true };
  try {
    const r = await http.post(
      './api/clients/disconnect',
      { uuid: client.uuid },
      { validateStatus: () => true },
    );
    const ok = r && r.status >= 200 && r.status < 300 && r.data?.status === true;
    if (!ok) {
      message.error(t('clients.disconnect_failed'));
      return;
    }
    message.success(t('clients.disconnect_success'));
  } catch (e: any) {
    message.error(e?.message || t('clients.disconnect_failed'));
  } finally {
    delete disconnecting.value[client.uuid];
    disconnecting.value = { ...disconnecting.value };
    refreshClients();
  }
}

const displayDevices = ref<DisplayDevice[]>([]);
const displayDevicesLoading = ref(false);
const displayDevicesError = ref('');

async function loadDisplayDevices(): Promise<void> {
  if (!isWindows.value) return;
  displayDevicesLoading.value = true;
  displayDevicesError.value = '';
  try {
    const res = await http.get<DisplayDevice[]>('/api/display-devices', {
      params: { detail: 'full' },
    });
    displayDevices.value = Array.isArray(res.data) ? res.data : [];
  } catch (e: any) {
    displayDevicesError.value = e?.message || 'Failed to load display devices';
    displayDevices.value = [];
  } finally {
    displayDevicesLoading.value = false;
  }
}

function ensureDisplayDevicesLoaded(): void {
  if (!isWindows.value) return;
  if (!displayDevicesLoading.value && displayDevices.value.length === 0) {
    void loadDisplayDevices();
  }
}

const displayDeviceOptions = computed(() => {
  const opts: Array<{
    label: string;
    value: string;
    displayName: string;
    id: string;
    active: boolean | null;
  }> = [];
  const seen = new Set<string>();
  for (const d of displayDevices.value) {
    const value = d.device_id || d.display_name || '';
    if (!value || seen.has(value)) continue;
    const displayName = d.friendly_name || d.display_name || 'Display';
    const active = displayDeviceActiveState(d.info);
    const suffix =
      active === null
        ? ''
        : active
          ? ` (${t('config.app_display_status_active')})`
          : ` (${t('config.app_display_status_inactive')})`;
    opts.push({
      label: `${displayName} - ${value}${suffix}`,
      value,
      displayName,
      id: value,
      active,
    });
    seen.add(value);
  }
  return opts;
});

function displayDeviceActiveState(info: unknown): boolean | null {
  if (info && typeof info === 'object' && 'active' in info) {
    return Boolean((info as DisplayDeviceInfo).active);
  }
  return info ? true : null;
}

function scrollToTokenSection(): void {
  apiTokensSectionRef.value?.scrollIntoView({ behavior: 'smooth', block: 'start' });
}

function goToStats(): void {
  void router.push('/stats');
}

onMounted(async () => {
  const auth = useAuthStore();
  await configStore.fetchConfig().catch(() => {});
  await auth.waitForAuthentication();
  await refreshClients();
  if (route.query['sec'] === 'tokens') {
    await nextTick();
    scrollToTokenSection();
  }
  if (refreshIntervalId === null) {
    refreshIntervalId = setInterval(() => {
      void refreshClients();
    }, 5000);
  }
});

watch(
  () => route.query['sec'],
  async (section) => {
    if (section !== 'tokens') {
      return;
    }
    await nextTick();
    scrollToTokenSection();
  },
);

onBeforeUnmount(() => {
  if (refreshIntervalId !== null) {
    clearInterval(refreshIntervalId);
    refreshIntervalId = null;
  }
});
</script>

<style scoped>
.clients-hero {
  display: flex;
  flex-direction: column;
  gap: 1rem;
  border: 1px solid rgb(var(--color-dark) / 0.1);
  border-radius: 0.5rem;
  background: rgb(var(--color-light) / 0.72);
  padding: 1rem;
  box-shadow: 0 1px 2px rgb(var(--color-dark) / 0.04);
  backdrop-filter: blur(6px);
}

.dark .clients-hero {
  border-color: rgb(var(--color-light) / 0.12);
  background: rgb(var(--color-surface) / 0.68);
  box-shadow: none;
}

.clients-hero__content {
  display: grid;
  min-width: 0;
  gap: 0.85rem;
}

.clients-hero__subtitle {
  margin: 0.25rem 0 0;
  max-width: 48rem;
  color: rgb(var(--color-dark) / 0.72);
  font-size: 0.84rem;
  line-height: 1.45;
}

.dark .clients-hero__subtitle {
  color: rgb(var(--color-light) / 0.72);
}

.clients-hero__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
}

.clients-last-updated {
  display: inline-flex;
  align-items: center;
  min-height: 1.75rem;
  border-radius: 999px;
  background: rgb(var(--color-dark) / 0.06);
  padding: 0.25rem 0.65rem;
  color: rgb(var(--color-dark) / 0.62);
  font-size: 0.72rem;
  font-weight: 600;
}

.dark .clients-last-updated {
  background: rgb(var(--color-light) / 0.08);
  color: rgb(var(--color-light) / 0.66);
}

@media (min-width: 768px) {
  .clients-hero {
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
    padding: 1.25rem 1.5rem;
  }

  .clients-hero__content {
    flex: 1 1 auto;
  }
}

.clients-stats {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 0.5rem;
  min-width: min(100%, 24rem);
}

@media (max-width: 360px) {
  .clients-stats {
    grid-template-columns: 1fr;
  }
}

.clients-hero__identity {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: 0.75rem;
}

.clients-hero__icon {
  display: inline-flex;
  width: 2.35rem;
  height: 2.35rem;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  border-radius: 0.5rem;
  background: rgb(var(--color-primary) / 0.16);
  color: rgb(var(--color-primary));
}

.clients-stat {
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.5rem;
  background: rgb(var(--color-light) / 0.56);
  padding: 0.625rem 0.75rem;
}

.dark .clients-stat {
  border-color: rgb(var(--color-light) / 0.1);
  background: rgb(var(--color-dark) / 0.22);
}

.clients-stat--connected {
  border-color: rgb(var(--color-success) / 0.28);
  background: rgb(var(--color-success) / 0.1);
}

.dark .clients-stat--connected {
  border-color: rgb(var(--color-success) / 0.36);
  background: rgb(var(--color-success) / 0.14);
}

.clients-stat__value,
.clients-stat__label {
  display: block;
}

.clients-stat__value {
  font-size: 1.35rem;
  font-weight: 700;
  line-height: 1;
}

.clients-stat__label {
  margin-top: 0.25rem;
  font-size: 0.68rem;
  font-weight: 600;
  line-height: 1.15;
  opacity: 0.68;
  overflow-wrap: anywhere;
  text-transform: uppercase;
}

.clients-section-heading {
  display: flex;
  min-width: 0;
  align-items: flex-start;
  gap: 0.75rem;
}

.clients-section-icon {
  display: inline-flex;
  width: 2rem;
  height: 2rem;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  border-radius: 0.5rem;
  background: rgb(var(--color-primary) / 0.16);
  color: rgb(var(--color-primary));
}

.clients-toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-end;
  justify-content: flex-start;
  gap: 0.75rem;
}

.clients-toolbar__field {
  display: grid;
  gap: 0.25rem;
  font-size: 0.72rem;
  font-weight: 600;
  opacity: 0.82;
}

.clients-toolbar__search {
  min-width: min(100%, 14rem);
}

@media (max-width: 640px) {
  .clients-toolbar,
  .clients-toolbar__field,
  .clients-toolbar__search,
  .clients-toolbar :deep(.n-button) {
    width: 100%;
  }
}

.clients-page :deep(.n-card) {
  border-radius: 0.5rem;
  overflow: hidden;
  border: 1px solid rgb(var(--color-dark) / 0.1);
  background: rgb(var(--color-light) / 0.8);
  backdrop-filter: blur(6px);
}

.dark .clients-page :deep(.n-card) {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-surface) / 0.74);
}

.clients-page :deep(.n-card .n-card__header),
.clients-page :deep(.n-card .n-card-header),
.clients-page :deep(.n-card .n-card__footer),
.clients-page :deep(.n-card .n-card-footer) {
  border-radius: 0.5rem;
}

.clients-modal-card {
  border-radius: 0.5rem;
  border: 1px solid rgb(var(--color-dark) / 0.12);
}

.dark .clients-modal-card {
  border-color: rgb(var(--color-light) / 0.14);
}

.clients-page :deep(.n-alert),
.clients-page :deep(.n-empty),
.clients-page :deep(.n-input .n-input-wrapper),
.clients-page :deep(.n-base-selection),
.clients-page :deep(.n-base-selection .n-base-selection-label),
.clients-page :deep(.n-data-table-wrapper),
.clients-page :deep(.n-table-wrapper) {
  border-radius: 0.5rem !important;
}

.clients-pair-readiness {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.5rem;
  background: rgb(var(--color-dark) / 0.04);
  padding: 0.65rem 0.75rem;
  color: rgb(var(--color-dark) / 0.68);
  font-size: 0.82rem;
  line-height: 1.35;
}

.clients-pair-readiness--ready {
  border-color: rgb(var(--color-success) / 0.28);
  background: rgb(var(--color-success) / 0.1);
  color: rgb(var(--color-success));
}

.dark .clients-pair-readiness {
  border-color: rgb(var(--color-light) / 0.1);
  background: rgb(var(--color-light) / 0.06);
  color: rgb(var(--color-light) / 0.72);
}

.dark .clients-pair-readiness--ready {
  border-color: rgb(var(--color-success) / 0.36);
  background: rgb(var(--color-success) / 0.14);
  color: rgb(var(--color-success));
}

.clients-list-summary {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: 0.5rem;
  border-radius: 0.5rem;
  background: rgb(var(--color-dark) / 0.04);
  padding: 0.55rem 0.7rem;
  color: rgb(var(--color-dark) / 0.64);
  font-size: 0.76rem;
  font-weight: 600;
}

.clients-list-summary button {
  border: 0;
  background: transparent;
  color: rgb(var(--color-primary));
  cursor: pointer;
  font: inherit;
  padding: 0;
}

.dark .clients-list-summary {
  background: rgb(var(--color-light) / 0.06);
  color: rgb(var(--color-light) / 0.66);
}

.clients-api-section {
  scroll-margin-top: 5rem;
}

.client-list {
  display: grid;
  gap: 0.75rem;
}

.client-record {
  display: grid;
  gap: 0.875rem;
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.5rem;
  background: rgb(var(--color-light) / 0.5);
  padding: 0.875rem 1rem;
  transition:
    border-color 0.15s ease,
    background 0.15s ease;
}

.client-record:hover {
  border-color: rgb(var(--color-primary) / 0.2);
  background: rgb(var(--color-light) / 0.62);
}

.dark .client-record {
  border-color: rgb(var(--color-light) / 0.12);
  background: rgb(var(--color-dark) / 0.22);
}

.dark .client-record:hover {
  border-color: rgb(var(--color-primary) / 0.34);
  background: rgb(var(--color-light) / 0.08);
}

@media (min-width: 900px) {
  .client-record {
    grid-template-columns: minmax(0, 1fr) auto;
    align-items: center;
  }
}

.client-record__main {
  display: grid;
  min-width: 0;
  grid-template-columns: 2rem minmax(0, 1fr);
  align-items: start;
  gap: 0.75rem;
}

.client-record__body {
  display: grid;
  min-width: 0;
  gap: 0.5rem;
}

.client-avatar {
  display: inline-flex;
  width: 2rem;
  height: 2rem;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.5rem;
  background: rgb(var(--color-surface) / 0.65);
  color: rgb(var(--color-dark) / 0.74);
  font-size: 0.95rem;
}

.dark .client-avatar {
  border-color: rgb(var(--color-light) / 0.12);
  background: rgb(var(--color-light) / 0.06);
  color: rgb(var(--color-light) / 0.76);
}

.client-avatar--connected {
  border-color: rgb(var(--color-success) / 0.36);
  background: rgb(var(--color-success) / 0.14);
  color: rgb(var(--color-success));
}

.client-record__title-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.375rem 0.625rem;
}

.client-record__title {
  min-width: 0;
  margin: 0;
  overflow-wrap: anywhere;
  font-size: 1rem;
  font-weight: 650;
  line-height: 1.35;
}

.client-record__meta {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 10.5rem), 1fr));
  gap: 0.4rem 1rem;
  font-size: 0.73rem;
  line-height: 1.35;
  opacity: 0.68;
}

.client-record__meta-item {
  display: grid;
  min-width: 0;
  grid-template-columns: 0.875rem minmax(0, 1fr);
  align-items: start;
  column-gap: 0.4rem;
}

.client-record__meta-item i {
  width: 0.875rem;
  line-height: 1.35;
  text-align: center;
}

.client-record__meta-label {
  min-width: 0;
  overflow-wrap: anywhere;
}

.client-record__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-start;
  gap: 0.5rem;
}

@media (max-width: 520px) {
  .client-record__actions {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(8rem, 1fr));
    width: 100%;
  }

  .client-record__actions :deep(.n-button) {
    justify-content: center;
  }
}

@media (min-width: 900px) {
  .client-record__actions {
    justify-content: flex-end;
  }
}

.client-edit-panel {
  border-top: 1px solid rgb(var(--color-dark) / 0.08);
  padding-top: 0.875rem;
}

.dark .client-edit-panel {
  border-top-color: rgb(var(--color-light) / 0.1);
}

.client-edit-panel__header {
  margin-bottom: 0.85rem;
  border: 1px solid rgb(var(--color-primary) / 0.18);
  border-radius: 0.5rem;
  background: rgb(var(--color-primary) / 0.08);
  padding: 0.75rem 0.85rem;
}

.client-edit-panel__header h4 {
  margin: 0;
  font-size: 0.92rem;
  font-weight: 700;
}

.client-edit-panel__header p {
  margin: 0.25rem 0 0;
  color: rgb(var(--color-dark) / 0.66);
  font-size: 0.75rem;
  line-height: 1.4;
}

.dark .client-edit-panel__header {
  border-color: rgb(var(--color-primary) / 0.28);
  background: rgb(var(--color-primary) / 0.12);
}

.dark .client-edit-panel__header p {
  color: rgb(var(--color-light) / 0.68);
}

@media (min-width: 900px) {
  .client-edit-panel {
    grid-column: 1 / -1;
  }
}

.client-advanced-panel {
  display: grid;
  gap: 1.25rem;
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.5rem;
  background: rgb(var(--color-light) / 0.42);
  padding: 1rem;
}

.dark .client-advanced-panel {
  border-color: rgb(var(--color-light) / 0.12);
  background: rgb(var(--color-dark) / 0.24);
}

.client-empty {
  display: flex;
  min-height: 8rem;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 0.75rem;
  border: 1px dashed rgb(var(--color-dark) / 0.14);
  border-radius: 0.5rem;
  background: rgb(var(--color-light) / 0.38);
  padding: 1.5rem;
  text-align: center;
  font-size: 0.9rem;
  opacity: 0.78;
}

.client-empty i {
  font-size: 1.3rem;
  opacity: 0.72;
}

.client-empty span {
  font-weight: 650;
}

.client-empty small {
  max-width: 32rem;
  color: rgb(var(--color-dark) / 0.62);
  font-size: 0.78rem;
  line-height: 1.45;
}

.client-empty--loading {
  opacity: 0.88;
}

.dark .client-empty {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-dark) / 0.2);
}

.dark .client-empty small {
  color: rgb(var(--color-light) / 0.64);
}
</style>
