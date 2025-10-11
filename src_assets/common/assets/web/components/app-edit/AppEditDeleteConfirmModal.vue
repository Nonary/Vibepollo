<template>
  <n-modal
    :show="visible"
    :z-index="3300"
    :mask-style="{ backgroundColor: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }"
    @update:show="(v) => emit('update:visible', v)"
  >
    <n-card
      :title="
        isPlayniteAuto
          ? 'Remove and Exclude from Auto‑Sync?'
          : ($t('apps.confirm_delete_title_named', { name: name }) as any)
      "
      :bordered="false"
      style="max-width: 32rem; width: 100%"
    >
      <div class="text-sm text-center space-y-2">
        <template v-if="isPlayniteAuto">
          <div>
            This application is managed by Playnite. Removing it will also add it to the Excluded
            Games list so it won’t be auto‑synced back.
          </div>
          <div class="opacity-80">
            You can restore it later by manually adding it, or by removing the exclusion under
            Settings → Playnite.
          </div>
          <div class="opacity-70">Do you want to continue?</div>
        </template>
        <template v-else>
          {{ $t('apps.confirm_delete_message_named', { name }) }}
        </template>
      </div>
      <template #footer>
        <div class="w-full flex items-center justify-center gap-3">
          <n-button type="default" strong @click="emit('cancel')">{{ $t('_common.cancel') }}</n-button>
          <n-button type="error" strong @click="emit('confirm')">{{ $t('apps.delete') }}</n-button>
        </div>
      </template>
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { toRefs } from 'vue';
import { NModal, NCard, NButton } from 'naive-ui';

const rawProps = defineProps<{
  visible: boolean;
  isPlayniteAuto: boolean;
  name: string;
}>();

const { visible, isPlayniteAuto, name } = toRefs(rawProps);

const emit = defineEmits<{
  (e: 'update:visible', value: boolean): void;
  (e: 'cancel'): void;
  (e: 'confirm'): void;
}>();
</script>
