<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiDelete, apiGet, apiPost } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  EmptyState,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
  UiIcon,
  type StatusTone,
} from '@/components/ui';
import { useSystemStore, type HostMetadata } from '@/stores/system';
import { formatBytes } from '@/utils/format';

interface CrashDumpStatus {
  available?: boolean;
  filename?: string;
  path?: string;
  process?: string;
  size_bytes?: number;
  captured_at?: string;
  age_seconds?: number;
  dismissed?: boolean;
  dismissed_at?: string;
}

interface GoldenStatus {
  exists?: boolean;
  snapshot_version?: number | null;
  latest_snapshot_version?: number;
  has_layout?: boolean;
  needs_layout_upgrade?: boolean;
  out_of_date?: boolean;
  out_of_date_reason?: string;
  current_mismatch_reason?: string;
  restore_failure_count?: number;
  restore_failure_threshold?: number;
  restore_failure_window_hours?: number;
  restore_status_reason?: string;
  restore_last_failure_reason?: string;
  restore_latest_failure_unix_ms?: number | null;
  restore_status_updated_at_unix_ms?: number | null;
}

interface BrowserSession {
  id: string;
  username: string;
  created_at: number;
  expires_at: number;
  refresh_expires_at?: number;
  last_seen: number;
  remember_me: boolean;
  current: boolean;
  user_agent?: string;
  remote_address?: string;
  device_label?: string;
}

interface SessionsResponse {
  sessions?: BrowserSession[];
}

interface MutationResponse {
  status?: boolean;
  deleted?: boolean;
  error?: string;
  message?: string;
}

type PendingAction =
  | { kind: 'golden-export' }
  | { kind: 'golden-delete' }
  | { kind: 'revoke-session'; session: BrowserSession }
  | { kind: 'restart' };

const { locale, t } = useI18n();
const system = useSystemStore();
const metadata = ref<HostMetadata | null>(system.metadata);
const crashDump = ref<CrashDumpStatus | null>(null);
const golden = ref<GoldenStatus | null>(null);
const browserSessions = ref<BrowserSession[]>([]);
const loading = ref(true);
const refreshing = ref(false);
const actionBusy = ref(false);
const passwordBusy = ref(false);
const dismissingCrash = ref(false);
const loadErrors = ref<string[]>([]);
const notice = ref('');
const actionError = ref('');
const confirmOpen = ref(false);
const pendingAction = ref<PendingAction | null>(null);

const credentials = reactive({
  username: '',
  currentPassword: '',
  newPassword: '',
  confirmPassword: '',
});

const isWindows = computed(() =>
  String(metadata.value?.platform ?? system.metadata?.platform ?? '')
    .toLocaleLowerCase()
    .includes('windows'),
);

function message(cause: unknown, fallback: string): string {
  return cause instanceof ApiError ? fallback : cause instanceof Error ? cause.message : fallback;
}

function reconcileSessions(current: BrowserSession[], incoming: BrowserSession[]): BrowserSession[] {
  const byId = new Map(incoming.map((session) => [session.id, session]));
  const stable = current.flatMap((session) => {
    const replacement = byId.get(session.id);
    if (!replacement) return [];
    byId.delete(session.id);
    return [replacement];
  });
  return [...stable, ...byId.values()];
}

async function load(): Promise<void> {
  if (refreshing.value) return;
  refreshing.value = true;
  actionError.value = '';
  const errors: string[] = [];

  const primary = await Promise.allSettled([
    apiGet<HostMetadata>('/api/metadata'),
    apiGet<SessionsResponse>('/api/auth/sessions'),
  ]);
  const [metadataResult, sessionsResult] = primary;

  if (metadataResult.status === 'fulfilled') {
    metadata.value = metadataResult.value;
    system.metadata = metadataResult.value;
  } else {
    errors.push(message(metadataResult.reason, t('ui.maintenance.errors.metadata')));
  }

  if (sessionsResult.status === 'fulfilled') {
    const incoming = Array.isArray(sessionsResult.value.sessions) ? sessionsResult.value.sessions : [];
    browserSessions.value = reconcileSessions(browserSessions.value, incoming);
    const current = incoming.find((session) => session.current);
    if (current && !credentials.username) credentials.username = current.username;
  } else {
    errors.push(message(sessionsResult.reason, t('ui.maintenance.errors.sessions')));
  }

  if (isWindows.value) {
    const windowsResults = await Promise.allSettled([
      apiGet<CrashDumpStatus>('/api/health/crashdump'),
      apiGet<GoldenStatus>('/api/display/golden_status'),
    ]);
    const [crashResult, goldenResult] = windowsResults;
    if (crashResult.status === 'fulfilled') crashDump.value = crashResult.value;
    else errors.push(message(crashResult.reason, t('ui.maintenance.errors.crashStatus')));
    if (goldenResult.status === 'fulfilled') golden.value = goldenResult.value;
    else errors.push(message(goldenResult.reason, t('ui.maintenance.errors.recoveryStatus')));
  } else {
    crashDump.value = null;
    golden.value = null;
  }

  loadErrors.value = [...new Set(errors)];
  loading.value = false;
  refreshing.value = false;
}

const goldenState = computed<{ label: string; tone: StatusTone; detail: string }>(() => {
  if (!isWindows.value) {
    return {
      label: t('ui.maintenance.status.windowsOnly'),
      tone: 'neutral',
      detail: t('ui.maintenance.recovery.windowsOnly'),
    };
  }
  if (!golden.value) {
    return {
      label: t('ui.maintenance.status.unavailable'),
      tone: 'danger',
      detail: t('ui.maintenance.recovery.unavailable'),
    };
  }
  if (!golden.value.exists) {
    return {
      label: t('ui.maintenance.recovery.notCaptured'),
      tone: 'neutral',
      detail: t('ui.maintenance.recovery.notCapturedDetail'),
    };
  }
  if (golden.value.out_of_date || golden.value.needs_layout_upgrade) {
    return {
      label: t('ui.maintenance.recovery.refreshRecommended'),
      tone: 'warning',
      detail: t('ui.maintenance.recovery.refreshRecommendedDetail'),
    };
  }
  return {
    label: t('ui.maintenance.recovery.available'),
    tone: 'success',
    detail: t('ui.maintenance.recovery.availableDetail'),
  };
});

const crashState = computed<{ label: string; tone: StatusTone }>(() => {
  if (!isWindows.value) return { label: t('ui.maintenance.status.windowsOnly'), tone: 'neutral' };
  if (!crashDump.value) return { label: t('ui.maintenance.status.unavailable'), tone: 'danger' };
  if (!crashDump.value.available) return { label: t('ui.maintenance.crash.noneRecent'), tone: 'success' };
  if (crashDump.value.dismissed) return { label: t('ui.maintenance.crash.acknowledged'), tone: 'neutral' };
  return { label: t('ui.maintenance.crash.detected'), tone: 'warning' };
});

function versionLabel(): string {
  if (!metadata.value?.version) return t('ui.maintenance.version.unknown');
  const prerelease = typeof metadata.value.prerelease === 'string' ? metadata.value.prerelease : '';
  return `${metadata.value.version}${prerelease ? `-${prerelease}` : ''}`;
}

function formatTimestamp(value: number | string | null | undefined): string {
  if (value === null || value === undefined || value === '') {
    return t('ui.maintenance.status.unavailable');
  }
  const numeric = typeof value === 'number' ? value : Number(value);
  const date = Number.isFinite(numeric)
    ? new Date(numeric < 10_000_000_000 ? numeric * 1000 : numeric)
    : new Date(value);
  return Number.isFinite(date.getTime())
    ? new Intl.DateTimeFormat(locale.value || undefined, {
        dateStyle: 'medium',
        timeStyle: 'short',
      }).format(date)
    : t('ui.maintenance.status.unavailable');
}

function formatDate(value: string | null | undefined): string {
  if (!value) return t('_common.unknown');
  const date = new Date(value);
  return Number.isFinite(date.getTime())
    ? new Intl.DateTimeFormat(locale.value || undefined, { dateStyle: 'medium' }).format(date)
    : value;
}

function sessionName(session: BrowserSession): string {
  return session.device_label || session.remote_address || session.user_agent || t('ui.maintenance.sessions.trustedBrowser');
}

function sessionLastSeen(session: BrowserSession): string {
  const date = new Date(session.last_seen * 1000);
  const delta = date.getTime() - Date.now();
  if (!Number.isFinite(delta)) return t('_common.unknown');

  const formatter = new Intl.RelativeTimeFormat(locale.value || undefined, { numeric: 'auto' });
  const ranges: Array<[Intl.RelativeTimeFormatUnit, number]> = [
    ['year', 365 * 24 * 60 * 60 * 1000],
    ['month', 30 * 24 * 60 * 60 * 1000],
    ['day', 24 * 60 * 60 * 1000],
    ['hour', 60 * 60 * 1000],
    ['minute', 60 * 1000],
    ['second', 1000],
  ];
  for (const [unit, size] of ranges) {
    if (Math.abs(delta) >= size || unit === 'second') {
      return formatter.format(Math.round(delta / size), unit);
    }
  }
  return t('ui.maintenance.sessions.now');
}

function requestAction(action: PendingAction): void {
  pendingAction.value = action;
  confirmOpen.value = true;
  actionError.value = '';
}

const dialogCopy = computed(() => {
  const action = pendingAction.value;
  if (action?.kind === 'golden-export') {
    return {
      title: golden.value?.exists
        ? t('ui.maintenance.confirm.replaceSnapshotTitle')
        : t('ui.maintenance.confirm.captureSnapshotTitle'),
      description: t('ui.maintenance.confirm.captureSnapshotDescription'),
      confirm: golden.value?.exists
        ? t('ui.maintenance.actions.replaceSnapshot')
        : t('ui.maintenance.actions.captureSnapshot'),
      tone: 'default' as const,
    };
  }
  if (action?.kind === 'golden-delete') {
    return {
      title: t('ui.maintenance.confirm.deleteSnapshotTitle'),
      description: t('ui.maintenance.confirm.deleteSnapshotDescription'),
      confirm: t('ui.maintenance.actions.deleteSnapshot'),
      tone: 'danger' as const,
    };
  }
  if (action?.kind === 'revoke-session') {
    return {
      title: t('ui.maintenance.confirm.revokeSessionTitle', {
        browser: sessionName(action.session),
      }),
      description: t('ui.maintenance.confirm.revokeSessionDescription'),
      confirm: t('ui.maintenance.actions.revokeSession'),
      tone: 'danger' as const,
    };
  }
  if (action?.kind === 'restart') {
    return {
      title: t('ui.maintenance.confirm.restartTitle'),
      description: t('ui.maintenance.confirm.restartDescription'),
      confirm: t('ui.maintenance.actions.restartService'),
      tone: 'danger' as const,
    };
  }
  return {
    title: t('ui.maintenance.confirm.defaultTitle'),
    description: '',
    confirm: t('_common.continue'),
    tone: 'default' as const,
  };
});

function updateConfirmOpen(value: boolean): void {
  confirmOpen.value = value;
  if (!value && !actionBusy.value) pendingAction.value = null;
}

async function runConfirmedAction(): Promise<void> {
  const action = pendingAction.value;
  if (!action || actionBusy.value) return;
  actionBusy.value = true;
  notice.value = '';
  actionError.value = '';

  try {
    if (action.kind === 'golden-export') {
      const result = await apiPost<MutationResponse>('/api/display/export_golden', {});
      if (result.status === false) {
        throw new Error(result.error || t('ui.maintenance.errors.snapshotCapture'));
      }
      notice.value = t('ui.maintenance.notices.snapshotCaptured');
      await load();
    } else if (action.kind === 'golden-delete') {
      const result = await apiDelete<MutationResponse>('/api/display/golden', {});
      if (golden.value?.exists && result.deleted !== true) {
        throw new Error(t('ui.maintenance.errors.snapshotNotDeleted'));
      }
      notice.value = t('ui.maintenance.notices.snapshotDeleted');
      await load();
    } else if (action.kind === 'revoke-session') {
      await apiDelete<MutationResponse>(`/api/auth/sessions/${encodeURIComponent(action.session.id)}`, {});
      notice.value = t('ui.maintenance.notices.sessionRevoked', {
        browser: sessionName(action.session),
      });
      await load();
    } else {
      notice.value = t('ui.maintenance.notices.restartRequested');
      try {
        await apiPost('/api/restart', {});
      } catch (cause) {
        if (cause instanceof ApiError) throw cause;
        // A network disconnect is the expected success path when the service exits promptly.
      }
    }
  } catch (cause) {
    notice.value = '';
    actionError.value = message(cause, t('ui.maintenance.errors.actionFailed'));
  } finally {
    actionBusy.value = false;
    pendingAction.value = null;
  }
}

async function dismissCrash(): Promise<void> {
  if (!crashDump.value?.filename || dismissingCrash.value) return;
  dismissingCrash.value = true;
  actionError.value = '';
  try {
    const result = await apiPost<MutationResponse>('/api/health/crashdump/dismiss', {
      captured_at: crashDump.value.captured_at || '',
      filename: crashDump.value.filename,
    });
    if (result.status === false) {
      throw new Error(result.error || t('ui.maintenance.errors.crashDismiss'));
    }
    crashDump.value = { ...crashDump.value, dismissed: true };
    notice.value = t('ui.maintenance.notices.crashAcknowledged');
  } catch (cause) {
    actionError.value = message(cause, t('ui.maintenance.errors.crashDismiss'));
  } finally {
    dismissingCrash.value = false;
  }
}

async function changePassword(): Promise<void> {
  if (passwordBusy.value) return;
  actionError.value = '';
  notice.value = '';
  if (!credentials.username.trim()) {
    actionError.value = t('ui.maintenance.validation.username');
    return;
  }
  if (!credentials.currentPassword) {
    actionError.value = t('ui.maintenance.validation.currentPassword');
    return;
  }
  if (!credentials.newPassword) {
    actionError.value = t('ui.maintenance.validation.newPassword');
    return;
  }
  if (credentials.newPassword !== credentials.confirmPassword) {
    actionError.value = t('ui.maintenance.validation.passwordMismatch');
    return;
  }

  passwordBusy.value = true;
  try {
    const result = await apiPost<MutationResponse>('/api/password', {
      confirmNewPassword: credentials.confirmPassword,
      currentPassword: credentials.currentPassword,
      currentUsername: credentials.username.trim(),
      newPassword: credentials.newPassword,
      newUsername: credentials.username.trim(),
    });
    if (result.status === false) {
      throw new Error(result.error || t('ui.maintenance.errors.passwordUnchanged'));
    }
    credentials.currentPassword = '';
    credentials.newPassword = '';
    credentials.confirmPassword = '';
    notice.value = t('ui.maintenance.notices.credentialsUpdated');
    await system.fetchAuthStatus();
  } catch (cause) {
    actionError.value = message(cause, t('ui.maintenance.errors.passwordChange'));
  } finally {
    passwordBusy.value = false;
  }
}

onMounted(() => void load());
</script>

<template>
  <div class="page page--narrow maintenance-page">
    <PageHeader
      :title="t('ui.maintenance.title')"
      :description="t('ui.maintenance.description')"
    >
      <template #actions>
        <AppButton
          icon="refresh"
          :label="t('_common.refresh')"
          variant="secondary"
          :busy="refreshing"
          :busy-label="t('ui.maintenance.refreshing')"
          @click="load"
        />
      </template>
    </PageHeader>

    <InlineAlert
      v-if="notice"
      tone="success"
      :title="t('ui.maintenance.actionCompleted')"
      announce="polite"
    >
      {{ notice }}
    </InlineAlert>
    <InlineAlert
      v-if="actionError"
      tone="danger"
      :title="t('ui.maintenance.actionFailed')"
      announce="assertive"
    >
      {{ actionError }}
    </InlineAlert>
    <InlineAlert
      v-if="loadErrors.length"
      tone="warning"
      :title="t('ui.maintenance.dataUnavailable')"
    >
      {{ loadErrors.join(' ') }}
    </InlineAlert>

    <template v-if="loading">
      <LoadingSkeleton v-for="index in 4" :key="index" variant="block" height="152px" />
    </template>

    <template v-else>
      <section class="maintenance-section" aria-labelledby="installed-version-title">
        <div class="maintenance-section__heading">
          <div>
            <h2 id="installed-version-title">{{ t('ui.maintenance.version.title') }}</h2>
            <p>{{ t('ui.maintenance.version.description') }}</p>
          </div>
          <StatusBadge
            :label="metadata?.status === false ? t('ui.maintenance.version.metadataIncomplete') : t('changelog.installed')"
            :tone="metadata?.status === false ? 'warning' : 'info'"
          />
        </div>
        <dl class="metadata-grid">
          <div><dt>{{ t('ui.maintenance.version.version') }}</dt><dd>{{ versionLabel() }}</dd></div>
          <div><dt>{{ t('ui.maintenance.version.platform') }}</dt><dd>{{ metadata?.platform || t('_common.unknown') }}</dd></div>
          <div><dt>{{ t('ui.maintenance.version.branch') }}</dt><dd>{{ metadata?.branch || t('_common.unknown') }}</dd></div>
          <div><dt>{{ t('ui.maintenance.version.commit') }}</dt><dd class="monospace">{{ metadata?.commit || t('_common.unknown') }}</dd></div>
          <div><dt>{{ t('ui.maintenance.version.releaseDate') }}</dt><dd>{{ formatDate(metadata?.release_date) }}</dd></div>
        </dl>
      </section>

      <section class="maintenance-section" aria-labelledby="support-title">
        <div class="maintenance-section__heading">
          <div>
            <h2 id="support-title">{{ t('ui.maintenance.support.title') }}</h2>
            <p>{{ t('ui.maintenance.support.description') }}</p>
          </div>
          <StatusBadge :label="crashState.label" :tone="crashState.tone" />
        </div>
        <div v-if="isWindows" class="support-actions">
          <a class="button button--secondary" href="/api/logs/export" download>
            <UiIcon name="download" aria-hidden="true" />
            {{ t('ui.maintenance.support.downloadLogs') }}
          </a>
          <a
            v-if="crashDump?.available"
            class="button button--secondary"
            href="/api/logs/export_crash"
            download
          >
            <UiIcon name="download" aria-hidden="true" />
            {{ t('ui.maintenance.support.downloadCrash') }}
          </a>
        </div>
        <div v-if="crashDump?.available" class="crash-summary">
          <div>
            <strong>{{ crashDump.filename || t('ui.maintenance.crash.recentDump') }}</strong>
            <span>
              {{ crashDump.process || t('config.virtual_display_driver_vibeshine_name') }} &middot;
              {{ formatBytes(crashDump.size_bytes || 0, locale) }} &middot;
              {{ formatTimestamp(crashDump.captured_at) }}
            </span>
          </div>
          <AppButton
            v-if="!crashDump.dismissed"
            :label="t('ui.maintenance.actions.acknowledge')"
            variant="tertiary"
            size="compact"
            :busy="dismissingCrash"
            :busy-label="t('ui.maintenance.saving')"
            @click="dismissCrash"
          />
        </div>
        <p v-else-if="isWindows" class="maintenance-muted">
          {{ t('ui.maintenance.crash.noneFound') }}
        </p>
        <p v-else class="maintenance-muted">
          {{ t('ui.maintenance.support.windowsUnavailable') }}
        </p>
      </section>

      <section class="maintenance-section" aria-labelledby="display-recovery-title">
        <div class="maintenance-section__heading">
          <div>
            <h2 id="display-recovery-title">{{ t('ui.maintenance.recovery.title') }}</h2>
            <p>{{ goldenState.detail }}</p>
          </div>
          <StatusBadge :label="goldenState.label" :tone="goldenState.tone" />
        </div>
        <dl v-if="golden?.exists" class="recovery-facts">
          <div>
            <dt>{{ t('ui.maintenance.recovery.snapshotSchema') }}</dt>
            <dd>{{ golden.snapshot_version ?? t('_common.unknown') }} / {{ golden.latest_snapshot_version ?? t('_common.unknown') }}</dd>
          </div>
          <div><dt>{{ t('ui.maintenance.recovery.unresolvedRestores') }}</dt><dd>{{ golden.restore_failure_count ?? 0 }}</dd></div>
          <div><dt>{{ t('ui.maintenance.recovery.lastFailure') }}</dt><dd>{{ golden.restore_last_failure_reason || t('ui.maintenance.recovery.noneRecorded') }}</dd></div>
          <div><dt>{{ t('ui.maintenance.recovery.statusUpdated') }}</dt><dd>{{ formatTimestamp(golden.restore_status_updated_at_unix_ms) }}</dd></div>
        </dl>
        <div v-if="isWindows" class="maintenance-actions">
          <AppButton
            :label="golden?.exists ? t('ui.maintenance.actions.replaceSnapshot') : t('ui.maintenance.actions.captureSnapshot')"
            variant="secondary"
            @click="requestAction({ kind: 'golden-export' })"
          />
          <AppButton
            v-if="golden?.exists"
            class="maintenance-danger-text"
            :label="t('ui.maintenance.actions.deleteSnapshot')"
            variant="tertiary"
            @click="requestAction({ kind: 'golden-delete' })"
          />
        </div>
      </section>

      <section class="maintenance-section" aria-labelledby="trusted-sessions-title">
        <div class="maintenance-section__heading">
          <div>
            <h2 id="trusted-sessions-title">{{ t('ui.maintenance.sessions.title') }}</h2>
            <p>{{ t('ui.maintenance.sessions.description') }}</p>
          </div>
          <StatusBadge
            :label="t('ui.maintenance.sessions.activeCount', { count: browserSessions.length })"
            tone="neutral"
          />
        </div>
        <div v-if="browserSessions.length" class="vs-table-wrap">
          <table class="vs-table vs-table--responsive">
            <thead>
              <tr>
                <th>{{ t('ui.maintenance.sessions.browser') }}</th>
                <th>{{ t('ui.maintenance.sessions.lastSeen') }}</th>
                <th>{{ t('ui.maintenance.sessions.expires') }}</th>
                <th><span class="visually-hidden">{{ t('auth.sessions_actions') }}</span></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="sessionItem in browserSessions" :key="sessionItem.id">
                <td :data-label="t('ui.maintenance.sessions.browser')">
                  <div class="session-identity">
                    <div class="session-identity__title vs-cluster">
                      <strong>{{ sessionName(sessionItem) }}</strong>
                      <StatusBadge
                        v-if="sessionItem.current"
                        :label="t('ui.maintenance.sessions.thisBrowser')"
                        tone="info"
                        compact
                      />
                    </div>
                    <span v-if="!sessionItem.current && sessionItem.remember_me">
                      {{ t('ui.maintenance.sessions.rememberedDevice') }}
                    </span>
                  </div>
                </td>
                <td :data-label="t('ui.maintenance.sessions.lastSeen')">{{ sessionLastSeen(sessionItem) }}</td>
                <td :data-label="t('ui.maintenance.sessions.expires')">{{ formatTimestamp(sessionItem.refresh_expires_at || sessionItem.expires_at) }}</td>
                <td :data-label="t('auth.sessions_actions')" class="vs-table__actions">
                  <AppButton
                    v-if="!sessionItem.current"
                    class="maintenance-danger-text"
                    :label="t('auth.sessions_revoke')"
                    variant="tertiary"
                    size="compact"
                    @click="requestAction({ kind: 'revoke-session', session: sessionItem })"
                  />
                  <span v-else class="maintenance-muted">{{ t('ui.maintenance.sessions.current') }}</span>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
        <EmptyState
          v-else
          compact
          icon="user"
          :title="t('ui.maintenance.sessions.emptyTitle')"
          :description="t('ui.maintenance.sessions.emptyDescription')"
        />
      </section>

      <section class="maintenance-section" aria-labelledby="credentials-title">
        <div class="maintenance-section__heading">
          <div>
            <h2 id="credentials-title">{{ t('ui.maintenance.credentials.title') }}</h2>
            <p>{{ t('ui.maintenance.credentials.description') }}</p>
          </div>
        </div>
        <form class="credentials-form" @submit.prevent="changePassword">
          <label class="vs-field">
            <span class="vs-field__label">{{ t('_common.username') }}</span>
            <input v-model.trim="credentials.username" class="vs-input" autocomplete="username" required />
          </label>
          <label class="vs-field">
            <span class="vs-field__label">{{ t('ui.maintenance.credentials.currentPassword') }}</span>
            <input v-model="credentials.currentPassword" class="vs-input" type="password" autocomplete="current-password" required />
          </label>
          <div class="credentials-form__new-password">
            <label class="vs-field">
              <span class="vs-field__label">{{ t('auth.new_password') }}</span>
              <input v-model="credentials.newPassword" class="vs-input" type="password" autocomplete="new-password" required />
            </label>
            <label class="vs-field">
              <span class="vs-field__label">{{ t('auth.confirm_new_password') }}</span>
              <input v-model="credentials.confirmPassword" class="vs-input" type="password" autocomplete="new-password" required />
            </label>
          </div>
          <div class="maintenance-actions">
            <AppButton
              type="submit"
              :label="t('navbar.password')"
              variant="primary"
              :busy="passwordBusy"
              :busy-label="t('ui.maintenance.updating')"
            />
          </div>
        </form>
      </section>

      <section class="danger-zone" aria-labelledby="restart-title">
        <div>
          <h2 id="restart-title">{{ t('troubleshooting.restart_sunshine') }}</h2>
          <p>{{ t('ui.maintenance.restart.description') }}</p>
        </div>
        <AppButton
          icon="refresh"
          :label="t('ui.maintenance.actions.restartService')"
          variant="danger"
          @click="requestAction({ kind: 'restart' })"
        />
      </section>
    </template>

    <ConfirmDialog
      :open="confirmOpen"
      :title="dialogCopy.title"
      :description="dialogCopy.description"
      :confirm-label="dialogCopy.confirm"
      :tone="dialogCopy.tone"
      :busy="actionBusy"
      :busy-label="t('ui.maintenance.applying')"
      @update:open="updateConfirmOpen"
      @confirm="runConfirmedAction"
    />
  </div>
</template>

<style scoped>
.maintenance-page {
  display: grid;
  gap: var(--vs-space-24);
}

.maintenance-section {
  display: grid;
  gap: var(--vs-space-20);
  padding: var(--vs-space-20);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.maintenance-section__heading {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.maintenance-section h2,
.danger-zone h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.maintenance-section__heading p,
.danger-zone p,
.maintenance-muted {
  margin-top: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.metadata-grid,
.recovery-facts {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12) var(--vs-space-24);
}

.metadata-grid > div,
.recovery-facts > div {
  min-width: 0;
  padding-bottom: var(--vs-space-12);
  border-bottom: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.metadata-grid dt,
.recovery-facts dt {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.metadata-grid dd,
.recovery-facts dd {
  margin: var(--vs-space-4) 0 0;
  overflow-wrap: anywhere;
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
}

.support-actions,
.maintenance-actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.crash-summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-16);
  padding: var(--vs-space-12);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.crash-summary strong,
.crash-summary span {
  display: block;
}

.crash-summary > span,
.session-identity > span {
  margin-top: var(--vs-space-2);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.session-identity {
  min-width: 13rem;
}

.session-identity__title {
  --vs-cluster-gap: var(--vs-space-8);
}

.credentials-form,
.credentials-form__new-password {
  display: grid;
  gap: var(--vs-space-16);
}

.credentials-form {
  max-width: 42rem;
}

.credentials-form__new-password {
  grid-template-columns: repeat(2, minmax(0, 1fr));
}

.maintenance-danger-text {
  color: var(--vs-color-status-danger);
}

.danger-zone {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-20);
  padding: var(--vs-space-20);
  border: var(--vs-border-emphasis-width) solid color-mix(in srgb, var(--vs-color-status-danger) 58%, var(--vs-color-border-subtle));
  border-radius: var(--vs-radius-card);
  background: color-mix(in srgb, var(--vs-color-status-danger) 6%, var(--vs-color-bg-surface));
}

@media (max-width: 47.999rem) {
  .maintenance-section__heading,
  .danger-zone,
  .crash-summary {
    align-items: stretch;
    flex-direction: column;
  }

  .metadata-grid,
  .recovery-facts,
  .credentials-form__new-password {
    grid-template-columns: minmax(0, 1fr);
  }

  .support-actions > .button,
  .danger-zone > .vs-button {
    width: 100%;
  }
}

@media (forced-colors: active) {
  .maintenance-section,
  .crash-summary,
  .danger-zone {
    border: var(--vs-border-width) solid CanvasText;
  }
}
</style>
