<script setup lang="ts">
import { computed, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { UiIcon } from '@/components/ui';
import { useSystemStore } from '@/stores/system';

const system = useSystemStore();
const { t } = useI18n();
const username = ref('');
const password = ref('');
const confirmPassword = ref('');
const rememberMe = ref(true);
const submitting = ref(false);
const error = ref('');

const isSetup = computed(() => system.needsSetup);

async function submit(): Promise<void> {
  error.value = '';
  if (!username.value.trim() || !password.value) {
    error.value = t('ui.auth.credentials_required');
    return;
  }
  if (isSetup.value && password.value !== confirmPassword.value) {
    error.value = t('auth.password_mismatch');
    return;
  }

  submitting.value = true;
  try {
    if (isSetup.value) {
      await system.createCredentials(username.value.trim(), password.value, confirmPassword.value);
      await system.login(username.value.trim(), password.value, true);
    } else {
      await system.login(username.value.trim(), password.value, rememberMe.value);
    }
  } catch {
    error.value = t(isSetup.value ? 'auth.create_user_failed' : 'auth.login_failed');
  } finally {
    submitting.value = false;
  }
}
</script>

<template>
  <main id="main-content" class="auth-page">
    <section class="auth-panel" aria-labelledby="auth-title">
      <div class="auth-brand">
        <img src="/images/logo-apollo-45.png" alt="" width="45" height="45" />
        <span>Vibepollo</span>
      </div>

      <div class="auth-heading">
        <span class="auth-heading__icon" aria-hidden="true"><UiIcon name="user" :size="20" /></span>
        <div>
          <h1 id="auth-title">{{ t(isSetup ? 'auth.create_first_user' : 'auth.login_title') }}</h1>
          <p>
            {{
              isSetup
                ? t('ui.auth.setup_description')
                : t('ui.auth.login_description')
            }}
          </p>
        </div>
      </div>

      <form class="auth-form" novalidate @submit.prevent="submit">
        <div v-if="error" class="form-error" role="alert">
          <UiIcon name="alert-triangle" />
          <span>{{ error }}</span>
        </div>

        <label class="vs-field">
          <span class="vs-field__label">{{ t('_common.username') }}</span>
          <input
            class="vs-input"
            v-model="username"
            name="username"
            autocomplete="username"
            inputmode="text"
            required
            autofocus
          />
        </label>

        <label class="vs-field">
          <span class="vs-field__label">{{ t('auth.password') }}</span>
          <input
            class="vs-input"
            v-model="password"
            name="password"
            :autocomplete="isSetup ? 'new-password' : 'current-password'"
            type="password"
            required
          />
        </label>

        <label v-if="isSetup" class="vs-field">
          <span class="vs-field__label">{{ t('welcome.confirm_password') }}</span>
          <input
            class="vs-input"
            v-model="confirmPassword"
            name="confirm-password"
            autocomplete="new-password"
            type="password"
            required
          />
        </label>

        <label v-else class="vs-checkbox">
          <input v-model="rememberMe" type="checkbox" />
          <span>{{ t('auth.remember_me_label') }}</span>
        </label>

        <button class="button button--primary auth-submit" type="submit" :disabled="submitting">
          <span>{{ submitting ? t('_common.loading') : t(isSetup ? 'auth.create_user' : 'auth.login_sign_in') }}</span>
          <UiIcon name="chevron-right" />
        </button>
      </form>

      <p class="auth-security-note">
        {{ t('ui.auth.security_note') }}
      </p>
    </section>
  </main>
</template>

<style scoped>
.auth-page {
  min-height: 100vh;
  display: grid;
  place-items: center;
  padding: var(--vs-space-24);
  background: var(--vs-color-bg-canvas);
}

.auth-panel {
  width: min(100%, 440px);
  padding: var(--vs-space-32);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-dialog);
  background: var(--vs-color-bg-surface);
}

.auth-brand,
.auth-heading,
.auth-heading__icon,
.form-error,
.button {
  display: flex;
  align-items: center;
}

.auth-brand {
  gap: var(--vs-space-12);
  margin-bottom: var(--vs-space-32);
  color: var(--vs-color-text-primary);
  font-size: 18px;
  font-weight: 650;
}

.auth-heading {
  align-items: flex-start;
  gap: var(--vs-space-12);
  margin-bottom: var(--vs-space-24);
}

.auth-heading__icon {
  flex: 0 0 40px;
  justify-content: center;
  height: 40px;
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-accent-default);
}

h1 {
  margin: 0;
  font-size: 22px;
  line-height: 28px;
}

.auth-heading p,
.auth-security-note {
  color: var(--vs-color-text-secondary);
}

.auth-heading p {
  margin: var(--vs-space-4) 0 0;
}

.auth-form {
  display: grid;
  gap: var(--vs-space-16);
}

.form-error {
  gap: var(--vs-space-8);
  padding: var(--vs-space-12);
  border: 1px solid var(--vs-color-status-danger);
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-text-primary);
  background: var(--vs-color-bg-subtle);
}

.auth-submit {
  justify-content: space-between;
  width: 100%;
  margin-top: var(--vs-space-8);
}

.auth-security-note {
  margin: var(--vs-space-24) 0 0;
  font-size: 12px;
  line-height: 16px;
  text-align: center;
}

@media (max-width: 479px) {
  .auth-page {
    align-items: start;
    padding: var(--vs-space-16);
  }

  .auth-panel {
    padding: var(--vs-space-24) 0;
    border: 0;
    background: transparent;
  }
}
</style>
