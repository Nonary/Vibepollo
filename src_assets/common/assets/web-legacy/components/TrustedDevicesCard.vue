<template>
  <n-card class="trusted-devices-card mb-0" :segmented="{ content: true, footer: false }">
    <template #header>
      <div class="flex flex-col gap-4 md:flex-row md:items-start md:justify-between">
        <div class="trusted-devices-heading">
          <span class="trusted-devices-heading__icon">
            <i class="fas fa-shield-heart" />
          </span>
          <div class="min-w-0">
            <h2 class="text-lg font-medium">{{ t('auth.sessions_heading') }}</h2>
            <p class="text-xs opacity-70 max-w-2xl mt-1">{{ t('auth.sessions_description') }}</p>
          </div>
        </div>
        <n-button size="small" secondary :loading="loading" @click="refresh">
          <i class="fas fa-rotate" />
          <span class="ml-2">{{ t('auth.refresh') }}</span>
        </n-button>
      </div>
    </template>

    <n-spin :show="loading">
      <div v-if="errorMessage" class="trusted-devices-error">{{ errorMessage }}</div>
      <div v-else-if="!sessionsList.length" class="trusted-devices-empty">
        <i class="fas fa-shield-halved" />
        {{ t('auth.sessions_empty') }}
      </div>
      <div v-else class="trusted-session-list">
        <article v-for="session in sessionsList" :key="session.id" class="trusted-session-record">
          <div class="trusted-session-record__main">
            <span
              class="trusted-session-record__avatar"
              :class="{ 'trusted-session-record__avatar--current': session.current }"
            >
              <i class="fas fa-laptop" />
            </span>
            <div class="trusted-session-record__body">
              <div class="trusted-session-record__title-row">
                <h3 class="trusted-session-record__title">{{ primaryLabel(session) }}</h3>
                <n-tag v-if="session.current" size="small" type="success" round :bordered="false">
                  {{ t('auth.sessions_current_device') }}
                </n-tag>
              </div>
              <p v-if="secondaryLabel(session)" class="trusted-session-record__subtitle">
                {{ secondaryLabel(session) }}
              </p>
              <div class="trusted-session-record__meta">
                <span class="trusted-session-record__meta-item">
                  <i class="fas fa-clock" />
                  <span class="trusted-session-record__meta-label">
                    {{ t('auth.sessions_last_seen', { time: formatTimestamp(session.last_seen) }) }}
                  </span>
                </span>
                <span class="trusted-session-record__meta-item">
                  <i class="fas fa-hourglass-half" />
                  <span class="trusted-session-record__meta-label">
                    {{
                      t('auth.sessions_expires', { time: formatTimestamp(sessionExpiry(session)) })
                    }}
                  </span>
                </span>
              </div>
            </div>
          </div>

          <div class="trusted-session-record__side">
            <div class="trusted-session-record__tags">
              <n-tag v-if="session.remember_me" size="small" type="info" round :bordered="false">
                {{ t('auth.sessions_remember_flag') }}
              </n-tag>
              <n-tag v-else size="small" round :bordered="false">
                {{ t('auth.sessions_session_flag') }}
              </n-tag>
            </div>
            <n-button
              size="small"
              type="error"
              secondary
              :loading="revokingId === session.id"
              @click="confirmRevoke(session)"
            >
              <i class="fas fa-ban" />
              <span class="ml-2">
                {{ session.current ? t('auth.sessions_logout') : t('auth.sessions_revoke') }}
              </span>
            </n-button>
          </div>
        </article>
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
import { toIntlLocale } from '@/utils/intlLocale';

const auth = useAuthStore();
const { t, locale } = useI18n();
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

const formatter = computed(
  () =>
    new Intl.DateTimeFormat(toIntlLocale(locale.value), {
      dateStyle: 'medium',
      timeStyle: 'short',
    }),
);

function formatTimestamp(seconds?: number): string {
  if (!seconds) return t('auth.sessions_time_unknown');
  if (!Number.isFinite(seconds)) return t('auth.sessions_time_unknown');
  return formatter.value.format(new Date(seconds * 1000));
}

function sessionExpiry(session: AuthSession): number | undefined {
  const refreshExpiry = session.refresh_expires_at;
  if (Number.isFinite(refreshExpiry)) {
    return refreshExpiry;
  }
  return session.expires_at;
}

function primaryLabel(session: AuthSession): string {
  return (
    session.device_label || fallbackAgent(session.user_agent) || t('auth.sessions_unknown_device')
  );
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

<style scoped>
.trusted-devices-heading {
  display: flex;
  min-width: 0;
  align-items: flex-start;
  gap: 0.75rem;
}

.trusted-devices-heading__icon {
  display: inline-flex;
  width: 2rem;
  height: 2rem;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  border-radius: 0.5rem;
  background: rgb(var(--color-primary) / 0.16);
  color: rgb(var(--color-primary));
}

.trusted-devices-error {
  border: 1px solid rgb(var(--color-danger) / 0.2);
  border-radius: 0.5rem;
  background: rgb(var(--color-danger) / 0.08);
  padding: 0.75rem;
  font-size: 0.75rem;
  color: rgb(var(--color-danger));
}

.trusted-devices-empty {
  display: flex;
  min-height: 7rem;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 0.65rem;
  border: 1px dashed rgb(var(--color-dark) / 0.14);
  border-radius: 0.5rem;
  background: rgb(var(--color-light) / 0.38);
  padding: 1.25rem;
  text-align: center;
  font-size: 0.85rem;
  opacity: 0.78;
}

.dark .trusted-devices-empty {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-dark) / 0.2);
}

.trusted-session-list {
  display: grid;
  gap: 0.75rem;
}

.trusted-session-record {
  display: grid;
  gap: 0.875rem;
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.5rem;
  background: rgb(var(--color-light) / 0.5);
  padding: 0.875rem 1rem;
  transition:
    border-color 0.15s ease,
    background 0.15s ease;
}

.trusted-session-record:hover {
  border-color: rgb(var(--color-primary) / 0.2);
  background: rgb(var(--color-light) / 0.62);
}

.dark .trusted-session-record {
  border-color: rgb(var(--color-light) / 0.12);
  background: rgb(var(--color-dark) / 0.22);
}

.dark .trusted-session-record:hover {
  border-color: rgb(var(--color-primary) / 0.34);
  background: rgb(var(--color-light) / 0.08);
}

@media (min-width: 900px) {
  .trusted-session-record {
    grid-template-columns: minmax(0, 1fr) auto;
    align-items: center;
  }
}

.trusted-session-record__main {
  display: grid;
  min-width: 0;
  grid-template-columns: 2rem minmax(0, 1fr);
  align-items: start;
  gap: 0.75rem;
}

.trusted-session-record__body {
  display: grid;
  min-width: 0;
  gap: 0.45rem;
}

.trusted-session-record__avatar {
  display: inline-flex;
  width: 2rem;
  height: 2rem;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.5rem;
  background: rgb(var(--color-surface) / 0.65);
  color: rgb(var(--color-dark) / 0.74);
  font-size: 0.95rem;
}

.dark .trusted-session-record__avatar {
  border-color: rgb(var(--color-light) / 0.12);
  background: rgb(var(--color-light) / 0.06);
  color: rgb(var(--color-light) / 0.76);
}

.trusted-session-record__avatar--current {
  border-color: rgb(var(--color-success) / 0.36);
  background: rgb(var(--color-success) / 0.14);
  color: rgb(var(--color-success));
}

.trusted-session-record__title-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
}

.trusted-session-record__title {
  min-width: 0;
  margin: 0;
  overflow-wrap: anywhere;
  font-size: 1rem;
  font-weight: 650;
  line-height: 1.35;
}

.trusted-session-record__subtitle {
  margin: 0;
  overflow-wrap: anywhere;
  font-size: 0.75rem;
  line-height: 1.35;
  opacity: 0.68;
}

.trusted-session-record__meta {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 10.5rem), 1fr));
  gap: 0.4rem 1rem;
  font-size: 0.73rem;
  line-height: 1.35;
  opacity: 0.68;
}

.trusted-session-record__meta-item {
  display: grid;
  min-width: 0;
  grid-template-columns: 0.875rem minmax(0, 1fr);
  align-items: start;
  column-gap: 0.4rem;
}

.trusted-session-record__meta-item i {
  width: 0.875rem;
  line-height: 1.35;
  text-align: center;
}

.trusted-session-record__meta-label {
  min-width: 0;
  overflow-wrap: anywhere;
}

.trusted-session-record__side {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-start;
  gap: 0.5rem;
}

.trusted-session-record__tags {
  display: inline-flex;
  flex-wrap: wrap;
  gap: 0.35rem;
}

@media (max-width: 520px) {
  .trusted-session-record__side {
    display: grid;
    grid-template-columns: 1fr;
    width: 100%;
  }

  .trusted-session-record__side :deep(.n-button) {
    justify-content: center;
  }
}

@media (min-width: 900px) {
  .trusted-session-record__side {
    justify-content: flex-end;
  }
}
</style>
