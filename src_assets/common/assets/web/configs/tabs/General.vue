<script setup lang="ts">
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import Checkbox from '@/Checkbox.vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import ConfigInputField from '@/ConfigInputField.vue';
import { useConfigStore } from '@/stores/config';
import { NButton } from 'naive-ui';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const platform = computed(() => metadata.value?.platform || '');

function addCmd() {
  const template = {
    do: '',
    undo: '',
    ...(platform.value === 'windows' ? { elevated: false } : {}),
  };
  if (!config.value) return;
  const current = Array.isArray(config.value.global_prep_cmd) ? config.value.global_prep_cmd : [];
  const next = [...current, template];
  store.updateOption('global_prep_cmd', next);
  store.markManualDirty?.('global_prep_cmd');
}

function removeCmd(index: number) {
  if (!config.value) return;
  const current = Array.isArray(config.value.global_prep_cmd)
    ? [...config.value.global_prep_cmd]
    : [];
  if (index < 0 || index >= current.length) return;
  current.splice(index, 1);
  store.updateOption('global_prep_cmd', current);
  store.markManualDirty?.('global_prep_cmd');
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

    <div id="global_prep_cmd" class="mb-6 flex flex-col">
      <label class="block text-sm font-medium mb-1 text-dark dark:text-light">
        {{ $t('config.global_prep_cmd') }}
      </label>
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.global_prep_cmd_desc') }}
      </div>
      <div v-if="config.global_prep_cmd.length > 0" class="mt-3 space-y-3">
        <div
          v-for="(command, index) in config.global_prep_cmd"
          :key="index"
          class="rounded-md border border-dark/10 dark:border-light/10 p-3 space-y-3"
        >
          <div class="flex items-center justify-between gap-2">
            <div class="text-xs opacity-70">Step {{ index + 1 }}</div>
            <div class="flex items-center gap-2">
              <Checkbox
                v-if="platform === 'windows'"
                :id="`global_prep_cmd_elevated_${index}`"
                v-model="command.elevated"
                :label="$t('_common.elevated')"
                desc=""
                class="mb-0"
                @update:model-value="store.markManualDirty()"
              />
              <n-button secondary size="small" @click="removeCmd(index)">
                <i class="fas fa-trash" />
              </n-button>
              <n-button primary size="small" @click="addCmd">
                <i class="fas fa-plus" />
              </n-button>
            </div>
          </div>

          <div class="grid grid-cols-1 gap-3">
            <ConfigInputField
              :id="`global_prep_cmd_do_${index}`"
              v-model="command.do"
              :label="$t('_common.do_cmd')"
              desc=""
              type="textarea"
              monospace
              :autosize="{ minRows: 1, maxRows: 3 }"
              @update:model-value="store.markManualDirty()"
            />

            <ConfigInputField
              :id="`global_prep_cmd_undo_${index}`"
              v-model="command.undo"
              :label="$t('_common.undo_cmd')"
              desc=""
              type="textarea"
              monospace
              :autosize="{ minRows: 1, maxRows: 3 }"
              @update:model-value="store.markManualDirty()"
            />
          </div>
        </div>
      </div>

      <div class="mt-4">
        <n-button primary class="mx-auto block" @click="addCmd">
          &plus; {{ $t('config.add') }}
        </n-button>
      </div>
    </div>

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
  </div>
</template>

<style scoped></style>
