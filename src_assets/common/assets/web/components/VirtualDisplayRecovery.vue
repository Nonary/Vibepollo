<script setup lang="ts">
import { ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiPost } from '@/api/client';
import { AppButton, ConfirmDialog, InlineAlert } from '@/components/ui';
import {
  dismissDisplayRecoveryIntroduction,
  displayRecoveryNotice,
  showDisplayRecoveryIntroduction,
  type DisplayRecoveryResult,
} from '@/utils/displayRecovery';

const emit = defineEmits<{ recovered: [] }>();
const { t } = useI18n();
const introduction = ref(showDisplayRecoveryIntroduction());
const confirming = ref(false);
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
    const result = await apiPost<DisplayRecoveryResult>('/api/display/terminate_virtual', {});
    const key = displayRecoveryNotice(result ?? {});
    failed.value = key === 'index.display_recovery.failed';
    recoveryUnconfirmed.value = key === 'index.display_recovery.without_restore';
    notice.value = t(key);
    emit('recovered');
  } catch {
    failed.value = true;
    notice.value = t('index.display_recovery.failed');
  } finally {
    busy.value = false;
    confirming.value = false;
  }
}
</script>

<template>
  <section class="display-recovery" aria-labelledby="display-recovery-title">
    <InlineAlert
      v-if="introduction"
      tone="info"
      :title="t('index.display_recovery.introduction_title')"
    >
      {{ t('index.display_recovery.introduction') }}
      <template #actions>
        <AppButton
          variant="secondary"
          :label="t('index.display_recovery.dismiss')"
          @click="dismissIntroduction"
        />
      </template>
    </InlineAlert>
    <div class="display-recovery__action">
      <div>
        <h2 id="display-recovery-title">{{ t('index.display_recovery.title') }}</h2>
        <p>{{ t('index.display_recovery.description') }}</p>
      </div>
      <AppButton
        icon="stop"
        variant="danger"
        :label="t('index.display_recovery.action')"
        :busy="busy"
        :busy-label="t('index.display_recovery.working')"
        @click="confirming = true"
      />
    </div>
    <InlineAlert
      v-if="notice"
      :tone="failed ? 'danger' : recoveryUnconfirmed ? 'warning' : 'info'"
      announce="polite"
    >
      {{ notice }}
    </InlineAlert>
    <ConfirmDialog
      v-model:open="confirming"
      tone="danger"
      :title="t('index.display_recovery.confirm_title')"
      :description="t('index.display_recovery.confirm_description')"
      :confirm-label="t('index.display_recovery.action')"
      :busy="busy"
      :busy-label="t('index.display_recovery.working')"
      :close-on-confirm="false"
      @confirm="terminate"
    />
  </section>
</template>

<style scoped>
.display-recovery {
  display: grid;
  gap: var(--vs-space-16);
  padding: var(--vs-space-24);
  background: var(--vs-color-bg-surface);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
}
.display-recovery__action {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-24);
}
.display-recovery h2 {
  margin: 0 0 var(--vs-space-8);
  font-size: 1.125rem;
}
.display-recovery p {
  margin: 0;
  color: var(--vs-color-text-secondary);
}
@media (max-width: 640px) {
  .display-recovery__action {
    align-items: stretch;
    flex-direction: column;
  }
}
</style>
