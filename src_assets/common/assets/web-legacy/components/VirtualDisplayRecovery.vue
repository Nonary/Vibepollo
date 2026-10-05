<script setup lang="ts">
import { ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { NAlert, NButton, NCard, useDialog } from 'naive-ui';

import { http } from '@/http';
import {
  dismissDisplayRecoveryIntroduction,
  displayRecoveryNotice,
  showDisplayRecoveryIntroduction,
  type DisplayRecoveryResult,
} from '../../web/utils/displayRecovery';

const { t } = useI18n();
const dialog = useDialog();
const introduction = ref(showDisplayRecoveryIntroduction());
const busy = ref(false);
const notice = ref('');
const failed = ref(false);
const recoveryUnconfirmed = ref(false);

function dismissIntroduction(): void {
  introduction.value = false;
  dismissDisplayRecoveryIntroduction();
}

async function terminate(): Promise<void> {
  if (busy.value) return;
  busy.value = true;
  notice.value = '';
  try {
    const response = await http.post<DisplayRecoveryResult>('/api/display/terminate_virtual', {});
    const key = displayRecoveryNotice(response.data ?? {});
    failed.value = key === 'index.display_recovery.failed';
    recoveryUnconfirmed.value = key === 'index.display_recovery.without_restore';
    notice.value = t(key);
  } catch {
    failed.value = true;
    notice.value = t('index.display_recovery.failed');
  } finally {
    busy.value = false;
  }
}

function confirmTermination(): void {
  if (busy.value) return;
  dialog.warning({
    title: t('index.display_recovery.confirm_title'),
    content: t('index.display_recovery.confirm_description'),
    positiveText: t('index.display_recovery.action'),
    negativeText: t('_common.cancel'),
    onPositiveClick: terminate,
    closeOnEsc: false,
    maskClosable: false,
  });
}
</script>

<template>
  <n-card :title="t('index.display_recovery.title')">
    <div class="space-y-4">
      <n-alert
        v-if="introduction"
        type="info"
        :title="t('index.display_recovery.introduction_title')"
      >
        <p>{{ t('index.display_recovery.introduction') }}</p>
        <n-button size="small" @click="dismissIntroduction">
          {{ t('index.display_recovery.dismiss') }}
        </n-button>
      </n-alert>
      <div class="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-4">
        <p class="text-sm opacity-80">{{ t('index.display_recovery.description') }}</p>
        <n-button type="error" :loading="busy" :disabled="busy" @click="confirmTermination">
          <i class="fas fa-stop" aria-hidden="true" />
          {{ t(busy ? 'index.display_recovery.working' : 'index.display_recovery.action') }}
        </n-button>
      </div>
      <n-alert
        v-if="notice"
        :type="failed ? 'error' : recoveryUnconfirmed ? 'warning' : 'info'"
        role="status"
      >
        {{ notice }}
      </n-alert>
    </div>
  </n-card>
</template>
