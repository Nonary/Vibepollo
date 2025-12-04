<script setup lang="ts">
import Checkbox from '@/Checkbox.vue';
import { ref, computed, onMounted } from 'vue';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { NSelect, NInput, NButton, NInputNumber, NCheckbox } from 'naive-ui';
import { useI18n } from 'vue-i18n';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const platform = computed(() => metadata.value?.platform || '');

// Select options - Apollo includes Hungarian and Vietnamese
const localeOptions = [
  { label: 'Български (Bulgarian)', value: 'bg' },
  { label: 'Čeština (Czech)', value: 'cs' },
  { label: 'Deutsch (German)', value: 'de' },
  { label: 'English', value: 'en' },
  { label: 'English, UK', value: 'en_GB' },
  { label: 'English, US', value: 'en_US' },
  { label: 'Español (Spanish)', value: 'es' },
  { label: 'Français (French)', value: 'fr' },
  { label: 'Magyar (Hungarian)', value: 'hu' },
  { label: 'Italiano (Italian)', value: 'it' },
  { label: '日本語 (Japanese)', value: 'ja' },
  { label: '한국어 (Korean)', value: 'ko' },
  { label: 'Polski (Polish)', value: 'pl' },
  { label: 'Português (Portuguese)', value: 'pt' },
  { label: 'Português, Brasileiro (Portuguese, Brazilian)', value: 'pt_BR' },
  { label: 'Русский (Russian)', value: 'ru' },
  { label: 'svenska (Swedish)', value: 'sv' },
  { label: 'Türkçe (Turkish)', value: 'tr' },
  { label: 'Українська (Ukranian)', value: 'uk' },
  { label: 'Tiếng Việt (Vietnamese)', value: 'vi' },
  { label: '简体中文 (Chinese Simplified)', value: 'zh' },
  { label: '繁體中文 (Chinese Traditional)', value: 'zh_TW' },
];

const { t } = useI18n();
const logLevelOptions = computed(() =>
  [0, 1, 2, 3, 4, 5, 6].map((v) => ({ label: t(`config.min_log_level_${v}`), value: v })),
);

const serverCmdTemplate = {
  name: '',
  cmd: '',
};

const serverCmd = computed({
  get() {
    return Array.isArray(config.value?.server_cmd) ? config.value.server_cmd : [];
  },
  set(val) {
    if (config.value) {
      store.updateOption('server_cmd', val);
      if (store.markManualDirty) store.markManualDirty('server_cmd');
    }
  },
});

// Global prep commands
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
  if (store.markManualDirty) store.markManualDirty('global_prep_cmd');
}

function removeCmd(index: number) {
  if (!config.value) return;
  const current = Array.isArray(config.value.global_prep_cmd)
    ? [...config.value.global_prep_cmd]
    : [];
  if (index < 0 || index >= current.length) return;
  current.splice(index, 1);
  store.updateOption('global_prep_cmd', current);
  if (store.markManualDirty) store.markManualDirty('global_prep_cmd');
}

// Server commands
function addServerCmd(idx?: number) {
  const _tpl = Object.assign({}, serverCmdTemplate);
  if (platform.value === 'windows') {
    (_tpl as any).elevated = false;
  }
  const current = [...serverCmd.value];
  if (idx === undefined || idx < 0) {
    current.push(_tpl);
  } else {
    current.splice(idx, 0, _tpl);
  }
  serverCmd.value = current;
}

function removeServerCmd(index: number) {
  const current = [...serverCmd.value];
  current.splice(index, 1);
  serverCmd.value = current;
}

onMounted(() => {
  // Set default value for enable_pairing if not present
  if (config.value && config.value.enable_pairing === undefined) {
    config.value.enable_pairing = 'enabled';
  }
});
</script>

<template>
  <div id="general" class="config-page">
    <!-- Locale -->
    <div class="mb-6">
      <label for="locale" class="form-label">{{ t('config.locale') }}</label>
      <n-select
        id="locale"
        v-model:value="config.locale"
        :options="localeOptions"
        :data-search-options="localeOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ t('config.locale_desc') }}
      </div>
    </div>

    <!-- Apollo Name -->
    <div class="mb-6">
      <label for="sunshine_name" class="form-label">{{ t('config.sunshine_name') }}</label>
      <n-input
        id="sunshine_name"
        v-model:value="config.sunshine_name"
        type="text"
        placeholder="Apollo"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ t('config.sunshine_name_desc') }}
      </div>
    </div>

    <!-- Log Level -->
    <div class="mb-6">
      <label for="min_log_level" class="form-label">{{ $t('config.min_log_level') }}</label>
      <n-select
        id="min_log_level"
        v-model:value="config.min_log_level"
        :options="
          logLevelOptions.map((o) => ({ ...o, label: $t(`config.min_log_level_${o.value}`) }))
        "
        :data-search-options="logLevelOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.min_log_level_desc') }}
      </div>
    </div>

    <!-- Global Prep Commands -->
    <div id="global_prep_cmd" class="mb-6 flex flex-col">
      <label class="block text-sm font-medium mb-1 text-dark dark:text-light">{{
        t('config.global_prep_cmd')
      }}</label>
      <div class="text-[11px] opacity-60 mt-1">
        {{ t('config.global_prep_cmd_desc') }}
      </div>
      <div
        v-if="config.global_prep_cmd && config.global_prep_cmd.length > 0"
        class="mt-3 space-y-3"
      >
        <div
          v-for="(c, i) in config.global_prep_cmd"
          :key="i"
          class="rounded-md border border-dark/10 dark:border-light/10 p-2"
        >
          <div class="flex items-center justify-between gap-2 mb-2">
            <div class="text-xs opacity-70">Step {{ i + 1 }}</div>
            <div class="flex items-center gap-2">
              <n-checkbox
                v-if="platform === 'windows'"
                v-model:checked="c.elevated"
                size="small"
                @update:checked="store.markManualDirty()"
              >
                {{ t('_common.elevated') }}
              </n-checkbox>
              <n-button secondary size="small" @click="removeCmd(i)">
                <i class="fas fa-trash" />
              </n-button>
              <n-button primary size="small" @click="addCmd">
                <i class="fas fa-plus" />
              </n-button>
            </div>
          </div>
          <div class="grid grid-cols-1 gap-2">
            <div>
              <label class="text-[11px] opacity-60">{{ t('_common.do_cmd') }}</label>
              <n-input
                v-model:value="c.do"
                type="textarea"
                :autosize="{ minRows: 1, maxRows: 3 }"
                class="monospace"
                @update:value="store.markManualDirty()"
              />
            </div>
            <div>
              <label class="text-[11px] opacity-60">{{ t('_common.undo_cmd') }}</label>
              <n-input
                v-model:value="c.undo"
                type="textarea"
                :autosize="{ minRows: 1, maxRows: 3 }"
                class="monospace"
                @update:value="store.markManualDirty()"
              />
            </div>
          </div>
        </div>
      </div>
      <div class="mt-4">
        <n-button primary class="mx-auto block" @click="addCmd">
          &plus; {{ t('config.add') }}
        </n-button>
      </div>
    </div>

    <!-- Session Token TTL -->
    <div class="mb-6">
      <label
        for="session_token_ttl_seconds"
        class="block text-sm font-medium mb-1 text-dark dark:text-light"
        >{{ t('config.session_token_ttl_seconds') }}</label
      >
      <n-input-number
        id="session_token_ttl_seconds"
        v-model:value="config.session_token_ttl_seconds"
        :min="60"
        :step="60"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ t('config.session_token_ttl_seconds_desc') }}
      </div>
    </div>

    <!-- Remember-me Refresh Token TTL -->
    <div class="mb-6">
      <label
        for="remember_me_refresh_token_ttl_seconds"
        class="block text-sm font-medium mb-1 text-dark dark:text-light"
        >{{ $t('config.remember_me_refresh_token_ttl_seconds') }}</label
      >
      <n-input-number
        id="remember_me_refresh_token_ttl_seconds"
        v-model:value="config.remember_me_refresh_token_ttl_seconds"
        :min="3600"
        :step="3600"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.remember_me_refresh_token_ttl_seconds_desc') }}
      </div>
    </div>

    <!-- Update Check Interval (seconds) -->
    <div class="mb-6">
      <label for="update_check_interval" class="form-label">{{
        t('config.update_check_interval')
      }}</label>
      <n-input-number
        id="update_check_interval"
        v-model:value="config.update_check_interval"
        :min="0"
        :step="60"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ t('config.update_check_interval_desc') }}
      </div>
    </div>

    <!-- Server Commands -->
    <div id="server_cmd" class="mb-6 flex flex-col">
      <label class="block text-sm font-medium mb-1 text-dark dark:text-light">{{
        t('config.server_cmd')
      }}</label>
      <div class="text-[11px] opacity-60 mt-1">
        {{ t('config.server_cmd_desc') }}
      </div>
      <div class="text-[11px] opacity-60 mt-1">
        <a
          href="https://github.com/ClassicOldSong/Apollo/wiki/Server-Commands"
          target="_blank"
          class="underline"
          >{{ t('_common.learn_more') }}</a
        >
      </div>
      <div v-if="serverCmd.length > 0" class="mt-3 space-y-3">
        <div
          v-for="(c, i) in serverCmd"
          :key="i"
          class="rounded-md border border-dark/10 dark:border-light/10 p-2"
        >
          <div class="flex items-center justify-between gap-2 mb-2">
            <div class="text-xs opacity-70">Command {{ i + 1 }}</div>
            <div class="flex items-center gap-2">
              <n-checkbox
                v-if="platform === 'windows'"
                v-model:checked="c.elevated"
                size="small"
                @update:checked="store.markManualDirty()"
              >
                {{ t('_common.elevated') }}
              </n-checkbox>
              <n-button secondary size="small" @click="removeServerCmd(i)">
                <i class="fas fa-trash" />
              </n-button>
              <n-button primary size="small" @click="addServerCmd(i)">
                <i class="fas fa-plus" />
              </n-button>
            </div>
          </div>
          <div class="grid grid-cols-1 gap-2">
            <div>
              <label class="text-[11px] opacity-60">{{ t('_common.cmd_name') }}</label>
              <n-input v-model:value="c.name" type="text" @update:value="store.markManualDirty()" />
            </div>
            <div>
              <label class="text-[11px] opacity-60">{{ t('_common.cmd_val') }}</label>
              <n-input
                v-model:value="c.cmd"
                type="text"
                class="monospace"
                @update:value="store.markManualDirty()"
              />
            </div>
          </div>
        </div>
      </div>
      <div class="mt-4">
        <n-button primary class="mx-auto block" @click="addServerCmd()">
          &plus; {{ t('config.add') }}
        </n-button>
      </div>
    </div>

    <!-- Enable Pairing -->
    <Checkbox
      class="mb-3"
      id="enable_pairing"
      locale-prefix="config"
      v-model="config.enable_pairing"
      default="true"
    ></Checkbox>

    <!-- Enable Discovery -->
    <Checkbox
      class="mb-3"
      id="enable_discovery"
      locale-prefix="config"
      v-model="config.enable_discovery"
      default="true"
    ></Checkbox>

    <!-- Notify Pre-Releases -->
    <Checkbox
      id="notify_pre_releases"
      v-model="config.notify_pre_releases"
      class="mb-3"
      locale-prefix="config"
      default="false"
    />

    <!-- Enable system tray -->
    <Checkbox
      id="system_tray"
      v-model="config.system_tray"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Hide Tray Controls -->
    <Checkbox
      id="hide_tray_controls"
      v-model="config.hide_tray_controls"
      class="mb-3"
      locale-prefix="config"
      default="false"
    />
  </div>
</template>

<style scoped></style>
