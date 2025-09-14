<template>
  <div class="inline-flex">
    <n-button
      :type="type"
      :size="size"
      :strong="strong"
      :loading="loading"
      :disabled="loading"
      @click="open"
    >
      <template #icon>
        <i :class="loading ? 'fas fa-spinner animate-spin' : icon" />
      </template>
      <span>{{ label }}</span>
    </n-button>

    <n-modal :show="show" @update:show="(v) => (show = v)">
      <n-card :bordered="false" style="max-width: 32rem; width: 100%">
        <template #header>
          <div class="flex items-center gap-2">
            <i class="fas fa-plug" />
            <span>{{ confirmTitle }}</span>
          </div>
        </template>
        <div class="text-sm">
          {{ confirmMessage }}
        </div>
        <template #footer>
          <div class="w-full flex items-center justify-center gap-3">
            <n-button type="default" strong @click="show = false">{{ cancelText }}</n-button>
            <n-button type="primary" :loading="loading" @click="confirm">{{
              continueText
            }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { NButton, NModal, NCard } from 'naive-ui';
import { http } from '@/http';

const props = defineProps<{
  label?: string;
  icon?: string;
  confirmTitle?: string;
  confirmMessage?: string;
  cancelText?: string;
  continueText?: string;
  size?: 'tiny' | 'small' | 'medium' | 'large';
  type?: 'default' | 'primary' | 'info' | 'success' | 'warning' | 'error';
  strong?: boolean;
  restart?: boolean;
}>();

const emit = defineEmits<{
  (e: 'done', payload: { ok: boolean; data?: any; error?: string }): void;
}>();

const show = ref(false);
const loading = ref(false);

const label = props.label ?? 'Install/Update Playnite Extension';
const icon = props.icon ?? 'fas fa-plug';
const confirmTitle = props.confirmTitle ?? 'Install/Update Playnite Extension';
const confirmMessage =
  props.confirmMessage ??
  'This will (re)install the Sunshine Playnite extension and restart Playnite if needed. Continue?';
const cancelText = props.cancelText ?? 'Cancel';
const continueText = props.continueText ?? 'Continue';
const size = props.size ?? 'small';
const type = props.type ?? 'primary';
const strong = props.strong ?? true;
const restart = props.restart ?? true;

function open() {
  show.value = true;
}

async function confirm() {
  if (loading.value) return;
  loading.value = true;
  show.value = false;
  let ok = false;
  let body: any = null;
  let error = '';
  try {
    const r = await http.post('/api/playnite/install', { restart }, { validateStatus: () => true });
    try {
      body = r.data;
    } catch {}
    ok = r.status >= 200 && r.status < 300 && body && body.status === true;
    if (!ok) {
      error = (body && (body.error || body.message)) || `HTTP ${r.status}`;
    }
  } catch (e: any) {
    error = e?.message || 'Request failed';
  }
  loading.value = false;
  emit('done', { ok, data: body, error: ok ? undefined : error });
}
</script>

<style scoped></style>
