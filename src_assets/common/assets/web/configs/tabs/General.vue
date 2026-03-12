<script setup lang="ts">
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import Checkbox from '@/Checkbox.vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import ConfigInputField from '@/ConfigInputField.vue';
import { useConfigStore } from '@/stores/config';
import { NButton } from 'naive-ui';

type PrepCommandKey = 'global_prep_cmd' | 'global_state_cmd';
type PrepCommand = { do: string; undo: string; elevated?: boolean };
type ServerCommand = { name: string; cmd: string; elevated?: boolean };

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const platform = computed(() => metadata.value?.platform || '');
const prepCommandSections: ReadonlyArray<{
  key: PrepCommandKey;
  labelKey: string;
  descKey: string;
}> = [
  {
    key: 'global_prep_cmd',
    labelKey: 'config.global_prep_cmd',
    descKey: 'config.global_prep_cmd_desc',
  },
  {
    key: 'global_state_cmd',
    labelKey: 'config.global_state_cmd',
    descKey: 'config.global_state_cmd_desc',
  },
];

function prepCommands(key: PrepCommandKey): PrepCommand[] {
  const current = config.value?.[key];
  return Array.isArray(current) ? (current as PrepCommand[]) : [];
}

function serverCommands(): ServerCommand[] {
  const current = config.value?.server_cmd;
  return Array.isArray(current) ? (current as ServerCommand[]) : [];
}

function markManualDirty() {
  store.markManualDirty?.();
}

function addPrepCommand(key: PrepCommandKey) {
  const template = {
    do: '',
    undo: '',
    ...(platform.value === 'windows' ? { elevated: false } : {}),
  };
  const current = prepCommands(key);
  const next = [...current, template];
  store.updateOption(key, next);
  markManualDirty();
}

function removePrepCommand(key: PrepCommandKey, index: number) {
  const current = [...prepCommands(key)];
  if (index < 0 || index >= current.length) return;
  current.splice(index, 1);
  store.updateOption(key, current);
  markManualDirty();
}

function addServerCommand() {
  const template = {
    name: '',
    cmd: '',
    ...(platform.value === 'windows' ? { elevated: false } : {}),
  };
  const next = [...serverCommands(), template];
  store.updateOption('server_cmd', next);
  markManualDirty();
}

function removeServerCommand(index: number) {
  const current = [...serverCommands()];
  if (index < 0 || index >= current.length) return;
  current.splice(index, 1);
  store.updateOption('server_cmd', current);
  markManualDirty();
}
</script>

<template>
  <div id="general" class="config-page">
    <ConfigFieldRenderer setting-key="locale" v-model="config.locale" class="mb-6" />

    <ConfigFieldRenderer
      setting-key="sunshine_name"
      v-model="config.sunshine_name"
      class="mb-6"
      placeholder="Vibeshine"
    />

    <ConfigFieldRenderer setting-key="min_log_level" v-model="config.min_log_level" class="mb-6" />

    <div
      v-for="section in prepCommandSections"
      :id="section.key"
      :key="section.key"
      class="mb-6 flex flex-col"
    >
      <label class="block text-sm font-medium mb-1 text-dark dark:text-light">
        {{ $t(section.labelKey) }}
      </label>
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t(section.descKey) }}
      </div>
      <div v-if="prepCommands(section.key).length > 0" class="mt-3 space-y-3">
        <div
          v-for="(command, index) in prepCommands(section.key)"
          :key="index"
          class="rounded-md border border-dark/10 dark:border-light/10 p-3 space-y-3"
        >
          <div class="flex items-center justify-between gap-2">
            <div class="text-xs opacity-70">Step {{ index + 1 }}</div>
            <div class="flex items-center gap-2">
              <Checkbox
                v-if="platform === 'windows'"
                :id="`${section.key}_elevated_${index}`"
                v-model="command.elevated"
                :label="$t('_common.elevated')"
                desc=""
                class="mb-0"
                @update:model-value="markManualDirty()"
              />
              <n-button secondary size="small" @click="removePrepCommand(section.key, index)">
                <i class="fas fa-trash" />
              </n-button>
              <n-button primary size="small" @click="addPrepCommand(section.key)">
                <i class="fas fa-plus" />
              </n-button>
            </div>
          </div>

          <div class="grid grid-cols-1 gap-3">
            <ConfigInputField
              :id="`${section.key}_do_${index}`"
              v-model="command.do"
              :label="$t('_common.do_cmd')"
              desc=""
              type="textarea"
              monospace
              :autosize="{ minRows: 1, maxRows: 3 }"
              @update:model-value="markManualDirty()"
            />

            <ConfigInputField
              :id="`${section.key}_undo_${index}`"
              v-model="command.undo"
              :label="$t('_common.undo_cmd')"
              desc=""
              type="textarea"
              monospace
              :autosize="{ minRows: 1, maxRows: 3 }"
              @update:model-value="markManualDirty()"
            />
          </div>
        </div>
      </div>

      <div class="mt-4">
        <n-button primary class="mx-auto block" @click="addPrepCommand(section.key)">
          &plus; {{ $t('config.add') }}
        </n-button>
      </div>
    </div>

    <div id="server_cmd" class="mb-6 flex flex-col">
      <label class="block text-sm font-medium mb-1 text-dark dark:text-light">
        {{ $t('config.server_cmd') }}
      </label>
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.server_cmd_desc') }}
      </div>
      <div v-if="serverCommands().length > 0" class="mt-3 space-y-3">
        <div
          v-for="(command, index) in serverCommands()"
          :key="index"
          class="rounded-md border border-dark/10 dark:border-light/10 p-3 space-y-3"
        >
          <div class="flex items-center justify-between gap-2">
            <div class="text-xs opacity-70">Command {{ index + 1 }}</div>
            <div class="flex items-center gap-2">
              <Checkbox
                v-if="platform === 'windows'"
                :id="`server_cmd_elevated_${index}`"
                v-model="command.elevated"
                :label="$t('_common.elevated')"
                desc=""
                class="mb-0"
                @update:model-value="markManualDirty()"
              />
              <n-button secondary size="small" @click="removeServerCommand(index)">
                <i class="fas fa-trash" />
              </n-button>
              <n-button primary size="small" @click="addServerCommand">
                <i class="fas fa-plus" />
              </n-button>
            </div>
          </div>

          <div class="grid grid-cols-1 gap-3">
            <ConfigInputField
              :id="`server_cmd_name_${index}`"
              v-model="command.name"
              :label="$t('_common.name')"
              desc=""
              @update:model-value="markManualDirty()"
            />

            <ConfigInputField
              :id="`server_cmd_cmd_${index}`"
              v-model="command.cmd"
              :label="$t('_common.cmd')"
              desc=""
              type="textarea"
              monospace
              :autosize="{ minRows: 1, maxRows: 3 }"
              @update:model-value="markManualDirty()"
            />
          </div>
        </div>
      </div>

      <div class="mt-4">
        <n-button primary class="mx-auto block" @click="addServerCommand">
          &plus; {{ $t('config.add') }}
        </n-button>
      </div>
    </div>

    <ConfigFieldRenderer
      setting-key="enable_pairing"
      v-model="config.enable_pairing"
      class="mb-3"
    />

    <ConfigFieldRenderer
      setting-key="enable_discovery"
      v-model="config.enable_discovery"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="session_token_ttl_seconds"
      v-model="config.session_token_ttl_seconds"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="remember_me_refresh_token_ttl_seconds"
      v-model="config.remember_me_refresh_token_ttl_seconds"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="update_check_interval"
      v-model="config.update_check_interval"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="notify_pre_releases"
      v-model="config.notify_pre_releases"
      class="mb-3"
    />

    <ConfigFieldRenderer setting-key="system_tray" v-model="config.system_tray" class="mb-3" />

    <ConfigFieldRenderer
      v-if="config.system_tray"
      setting-key="hide_tray_controls"
      v-model="config.hide_tray_controls"
      class="mb-3"
    />
  </div>
</template>

<style scoped></style>
