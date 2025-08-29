<script setup lang="ts">
import Checkbox from '@/Checkbox.vue';
import { ref, computed } from 'vue';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { NSelect, NInput, NButton, NInputNumber, NCheckbox } from 'naive-ui';
import { useI18n } from 'vue-i18n';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const platform = computed(() => metadata.value?.platform || '');

// Select options
const localeOptions = [
  { label: 'Български (Bulgarian)', value: 'bg' },
  { label: 'Čeština (Czech)', value: 'cs' },
  { label: 'Deutsch (German)', value: 'de' },
  { label: 'English', value: 'en' },
  { label: 'English, UK', value: 'en_GB' },
  { label: 'English, US', value: 'en_US' },
  { label: 'Español (Spanish)', value: 'es' },
  { label: 'Français (French)', value: 'fr' },
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
  { label: '简体中文 (Chinese Simplified)', value: 'zh' },
  { label: '繁體中文 (Chinese Traditional)', value: 'zh_TW' },
];

const { t } = useI18n();
const logLevelOptions = computed(() =>
  [0, 1, 2, 3, 4, 5, 6].map((v) => ({ label: t(`config.log_level_${v}`), value: v })),
);

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
</script>

<template>
  <div id="general" class="config-page">
    <!-- Locale -->
    <div class="mb-6">
      <label for="locale" class="form-label">{{ $t('config.locale') }}</label>
      <n-select
        id="locale"
        v-model:value="config.locale"
        :options="localeOptions"
        :data-search-options="localeOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.locale_desc') }}
      </div>
    </div>

    <!-- Sunshine Name -->
    <div class="mb-6">
      <label for="sunshine_name" class="form-label">{{ $t('config.sunshine_name') }}</label>
      <n-input
        id="sunshine_name"
        v-model:value="config.sunshine_name"
        type="text"
        placeholder="Sunshine"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.sunshine_name_desc') }}
      </div>
    </div>

    <!-- Log Level -->
    <div class="mb-6">
      <label for="min_log_level" class="form-label">{{ $t('config.log_level') }}</label>
      <n-select
        id="min_log_level"
        v-model:value="config.min_log_level"
        :options="logLevelOptions"
        :data-search-options="logLevelOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.log_level_desc') }}
      </div>
    </div>

    <!-- Global Prep Commands -->
    <div id="global_prep_cmd" class="mb-6 flex flex-col">
      <label class="block text-sm font-medium mb-1 text-dark dark:text-light">{{
        $t('config.global_prep_cmd')
      }}</label>
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.global_prep_cmd_desc') }}
      </div>
      <div v-if="config.global_prep_cmd.length > 0" class="mt-3 space-y-3">
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
                {{ $t('_common.elevated') }}
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
              <label class="text-[11px] opacity-60">{{ $t('_common.do_cmd') }}</label>
              <n-input
                v-model:value="c.do"
                type="textarea"
                :autosize="{ minRows: 1, maxRows: 3 }"
                class="monospace"
                @update:value="store.markManualDirty()"
              />
            </div>
            <div>
              <label class="text-[11px] opacity-60">{{ $t('_common.undo_cmd') }}</label>
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
        <n-button primary @click="addCmd" class="mx-auto block">
          &plus; {{ $t('config.add') }}
        </n-button>
      </div>
    </div>

    <!-- Session Token TTL -->
    <div class="mb-6">
      <label
        for="session_token_ttl_seconds"
        class="block text-sm font-medium mb-1 text-dark dark:text-light"
        >{{ $t('config.session_token_ttl_seconds') }}</label
      >
      <n-input-number
        id="session_token_ttl_seconds"
        v-model:value="config.session_token_ttl_seconds"
        :min="60"
        :step="60"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.session_token_ttl_seconds_desc') }}
      </div>
    </div>

    <!-- Update Check Interval (seconds) -->
    <div class="mb-6">
      <label for="update_check_interval" class="form-label">{{
        $t('config.update_check_interval')
      }}</label>
      <n-input-number
        id="update_check_interval"
        v-model:value="config.update_check_interval"
        :min="0"
        :step="60"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.update_check_interval_desc') }}
      </div>
    </div>

    <!-- Notify Pre-Releases -->
    <Checkbox
      id="notify_pre_releases"
      v-model="config.notify_pre_releases"
      class="mb-3"
      locale-prefix="config"
      default-value="false"
    />
  </div>
</template>

<style scoped></style>
