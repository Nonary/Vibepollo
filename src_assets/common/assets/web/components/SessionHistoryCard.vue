<template>
  <n-card class="mb-8" :segmented="{ content: true, footer: false }">
    <template #header>
      <div class="flex flex-wrap items-center justify-between gap-3">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-history" /> {{ t('sessions.history_title') }}
        </h2>
        <n-button size="small" :loading="loading" @click="loadPage(1)">
          <i class="fas fa-rotate" />
          <span class="ml-2">{{ t('sessions.refresh') }}</span>
        </n-button>
      </div>
    </template>

    <n-spin :show="loading && sessions.length === 0">
      <n-empty
        v-if="!loading && sessions.length === 0"
        :description="t('sessions.history_empty')"
      />
      <n-data-table
        v-else
        :columns="columns"
        :data="sessions"
        :row-props="rowProps"
        :bordered="false"
        size="small"
      />
    </n-spin>

    <template v-if="sessions.length > 0" #action>
      <div class="flex items-center justify-center">
        <n-pagination
          v-model:page="currentPage"
          :page-count="pageCount"
          :page-size="PAGE_SIZE"
          size="small"
          @update:page="loadPage"
        />
      </div>
    </template>

    <SessionHistoryDetail v-model:visible="showDetail" :uuid="selectedUuid" />
  </n-card>
</template>

<script setup lang="ts">
import { h, onMounted, ref, computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { NButton, NCard, NDataTable, NEmpty, NPagination, NSpin, NTag } from 'naive-ui';
import type { DataTableColumns } from 'naive-ui';
import { fetchSessionHistory } from '@/services/sessionsApi';
import type { SessionSummary } from '@/types/sessions';
import { useAuthStore } from '@/stores/auth';
import SessionHistoryDetail from './SessionHistoryDetail.vue';

const { t } = useI18n();
const auth = useAuthStore();

const PAGE_SIZE = 25;
const sessions = ref<SessionSummary[]>([]);
const loading = ref(false);
const currentPage = ref(1);
const totalCount = ref(0);
const showDetail = ref(false);
const selectedUuid = ref('');

const pageCount = computed(() => Math.max(1, Math.ceil(totalCount.value / PAGE_SIZE)));

function formatDuration(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function formatStartTime(unixTime: number): string {
  const date = new Date(unixTime * 1000);
  return date.toLocaleString(undefined, {
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  });
}

function verdictType(verdict?: string): 'success' | 'warning' | 'error' | 'default' {
  switch (verdict) {
    case 'healthy':
      return 'success';
    case 'degraded':
      return 'warning';
    case 'failed':
      return 'error';
    default:
      return 'default';
  }
}

function verdictLabel(verdict?: string): string {
  switch (verdict) {
    case 'healthy':
      return t('sessions.history_verdict_healthy');
    case 'degraded':
      return t('sessions.history_verdict_degraded');
    case 'failed':
      return t('sessions.history_verdict_failed');
    default:
      return t('sessions.history_verdict_unknown');
  }
}

function verdictIcon(verdict?: string): string {
  switch (verdict) {
    case 'healthy':
      return '✅';
    case 'degraded':
      return '⚠️';
    case 'failed':
      return '❌';
    default:
      return '❔';
  }
}

const columns = computed<DataTableColumns<SessionSummary>>(() => [
  {
    title: t('sessions.history_protocol'),
    key: 'protocol',
    width: 90,
    render(row) {
      return h(
        NTag,
        { size: 'small', bordered: false, type: row.protocol === 'rtsp' ? 'info' : 'warning' },
        () => row.protocol.toUpperCase(),
      );
    },
  },
  {
    title: t('sessions.history_client'),
    key: 'client_name',
    ellipsis: { tooltip: true },
    render(row) {
      const primary = row.client_name || row.device_name || row.uuid.substring(0, 8);
      const secondary = row.client_name && row.device_name ? row.device_name : '';
      return h('div', {}, [
        h('div', { class: 'font-medium text-sm' }, primary),
        secondary ? h('div', { class: 'text-xs opacity-60' }, secondary) : null,
      ]);
    },
  },
  {
    title: t('sessions.history_app'),
    key: 'app_name',
    ellipsis: { tooltip: true },
    render(row) {
      return h('span', { class: 'text-sm' }, row.app_name || '—');
    },
  },
  {
    title: t('sessions.history_resolution'),
    key: 'width',
    width: 140,
    render(row) {
      return h(
        'span',
        { class: 'text-sm font-mono' },
        `${row.width}×${row.height}@${row.target_fps}`,
      );
    },
  },
  {
    title: t('sessions.history_duration'),
    key: 'duration_seconds',
    width: 120,
    render(row) {
      return h('div', {}, [
        h('div', { class: 'text-sm font-mono' }, formatDuration(row.duration_seconds)),
        h('div', { class: 'text-xs opacity-60' }, formatStartTime(row.start_time_unix)),
      ]);
    },
  },
  {
    title: t('sessions.history_verdict'),
    key: 'verdict',
    width: 130,
    render(row) {
      return h(
        NTag,
        { size: 'small', bordered: false, type: verdictType(row.verdict) },
        () => `${verdictIcon(row.verdict)} ${verdictLabel(row.verdict)}`,
      );
    },
  },
]);

function rowProps(row: SessionSummary) {
  return {
    style: 'cursor: pointer;',
    onClick: () => {
      selectedUuid.value = row.uuid;
      showDetail.value = true;
    },
  };
}

async function loadPage(page: number): Promise<void> {
  loading.value = true;
  try {
    const offset = (page - 1) * PAGE_SIZE;
    const data = await fetchSessionHistory(PAGE_SIZE, offset);
    sessions.value = data;
    currentPage.value = page;
    // Estimate total: if we got a full page, there might be more
    if (data.length === PAGE_SIZE) {
      totalCount.value = Math.max(totalCount.value, offset + PAGE_SIZE + 1);
    } else {
      totalCount.value = offset + data.length;
    }
  } catch {
    // Silently ignore
  } finally {
    loading.value = false;
  }
}

onMounted(async () => {
  await auth.waitForAuthentication();
  await loadPage(1);
});
</script>
