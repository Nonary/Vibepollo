<template>
  <n-card class="mb-8" :segmented="{ content: true, footer: false }">
    <template #header>
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-lg font-medium flex items-center gap-2">
            <i class="fas fa-shield-heart" /> {{ t('auth.sessions_heading') }}
          </h2>
          <p class="text-xs opacity-70 max-w-2xl">{{ t('auth.sessions_description') }}</p>
        </div>
        <n-button size="small" :loading="loading" @click="refresh">
          <i class="fas fa-rotate" />
          <span class="ml-2">{{ t('auth.refresh') }}</span>
        </n-button>
      </div>
    </template>

    <n-spin :show="loading">
      <div v-if="errorMessage" class="text-xs text-danger">{{ errorMessage }}</div>
      <div v-else-if="!sessionsList.length" class="text-xs opacity-60">
        {{ t('auth.sessions_empty') }}
      </div>
      <div v-else class="overflow-x-auto">
        <table class="min-w-full text-sm">
          <thead class="text-left text-xs uppercase tracking-wide opacity-70 border-b border-dark/10 dark:border-light/10">
            <tr>
              <th class="py-2 pr-4 font-semibold">{{ t('auth.sessions_device') }}</th>
              <th class="py-2 pr-4 font-semibold">{{ t('auth.sessions_activity') }}</th>
              <th class="py-2 pr-4 font-semibold">{{ t('auth.sessions_status') }}</th>
              <th class="py-2 text-right font-semibold">{{ t('auth.sessions_actions') }}</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-dark/10 dark:divide-light/10">
            <tr v-for="session in sessionsList" :key="session.id" class="align-top">
              <td class="py-3 pr-4">
                <div class="flex flex-col gap-1">
                  <span class="font-medium break-words">{{ primaryLabel(session) }}</span>
                  <span class="text-xs opacity-70 break-words">
                    {{ secondaryLabel(session) }}
                  </span>
                </div>
              </td>
              <td class="py-3 pr-4">
                <div class="flex flex-col gap-1 text-xs">
                  <span>{{ formatTimestamp(session.created_at) }}</span>
                  <span class="opacity-70">{{ t('auth.sessions_last_seen', { time: formatTimestamp(session.last_seen) }) }}</span>
                  <span class="opacity-70">{{ t('auth.sessions_expires', { time: formatTimestamp(session.expires_at) }) }}</span>
                </div>
              </td>
              <td class="py-3 pr-4">
                <div class="flex flex-wrap items-center gap-2 text-xs">
                  <n-tag v-if="session.remember_me" size="small" type="info" :bordered="false">
                    {{ t('auth.sessions_remember_flag') }}
                  </n-tag>
                  <n-tag v-else size="small" :bordered="false">
                    {{ t('auth.sessions_session_flag') }}
                  </n-tag>
                  <n-tag v-if="session.current" size="small" type="success" :bordered="false">
                    {{ t('auth.sessions_current_device') }}
                  </n-tag>
                </div>
              </td>
              <td class="py-3 text-right">
                <n-button
                  size="tiny"
                  type="error"
                  strong
                  :loading="revokingId === session.id"
                  @click="confirmRevoke(session)"
                >
                  {{ session.current ? t('auth.sessions_logout') : t('auth.sessions_revoke') }}
                </n-button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </n-spin>
  </n-card>
</template>

<script setup lang="ts">
import { computed, ref, onMounted } from 'vue';
import { storeToRefs } from 'pinia';
import { useI18n } from 'vue-i18n';
import { useDialog, useMessage, NCard, NButton, NSpin, NTag } from 'naive-ui';
import { useAuthStore, type AuthSession } from '@/stores/auth';

const auth = useAuthStore();
const { t } = useI18n();
const dialog = useDialog();
const message = useMessage();

const { sessions, sessionsLoading, sessionsError } = storeToRefs(auth);
const revokingId = ref('');

const sessionsList = computed(() => sessions.value || []);
const loading = computed(() => sessionsLoading.value);

const errorMessage = computed(() => {
  if (!sessionsError.value) return '';
  if (sessionsError.value === 'error') return t('auth.sessions_load_failed');
  return sessionsError.value;
});

const formatter = new Intl.DateTimeFormat(undefined, {
  dateStyle: 'medium',
  timeStyle: 'short',
});

function formatTimestamp(seconds?: number): string {
  if (!seconds) return t('auth.sessions_time_unknown');
  if (!Number.isFinite(seconds)) return t('auth.sessions_time_unknown');
  return formatter.format(new Date(seconds * 1000));
}

function primaryLabel(session: AuthSession): string {
  return session.device_label || fallbackAgent(session.user_agent) || t('auth.sessions_unknown_device');
}

function secondaryLabel(session: AuthSession): string {
  const parts: string[] = [];
  if (session.remote_address) {
    parts.push(session.remote_address);
  }
  const agentSummary = fallbackAgent(session.user_agent, true);
  if (agentSummary) {
    parts.push(agentSummary);
  }
  return parts.join(' • ');
}

function fallbackAgent(agent?: string, compact = false): string {
  if (!agent) return '';
  const limit = compact ? 48 : 80;
  if (agent.length <= limit) return agent;
  return `${agent.slice(0, limit - 1)}…`;
}

async function refresh(): Promise<void> {
  await auth.fetchSessions();
}

function confirmRevoke(session: AuthSession): void {
  const isCurrent = session.current;
  dialog.warning({
    title: t('auth.sessions_revoke_title'),
    content: t('auth.sessions_revoke_message', {
      device: primaryLabel(session),
    }),
    positiveText: isCurrent ? t('auth.sessions_logout') : t('auth.sessions_revoke'),
    negativeText: t('auth.sessions_cancel'),
    onPositiveClick: async () => {
      revokingId.value = session.id;
      const ok = await auth.revokeSession(session.id);
      revokingId.value = '';
      if (ok) {
        message.success(t('auth.sessions_revoke_success'));
      } else {
        message.error(t('auth.sessions_revoke_failed'));
      }
    },
  });
}

onMounted(() => {
  auth.fetchSessions().catch(() => {});
});
</script>
