<template>
  <div
    class="min-h-screen bg-gradient-to-br from-light/40 via-surface/40 to-light/60 dark:from-dark/50 dark:via-dark/60 dark:to-dark/80 flex items-center justify-center px-4 py-12"
  >
    <n-card class="w-full max-w-md" :bordered="false">
      <n-space vertical size="large" class="w-full">
        <n-space vertical align="center" size="small">
          <n-image src="/images/logo-sunshine-45.png" width="45" preview-disabled alt="Sunshine" />
          <n-h2>{{ $t('auth.login_title') }}</n-h2>
        </n-space>

        <template v-if="!isLoggedIn">
          <n-form :model="credentials" label-placement="top" size="large" class="w-full">
            <n-form-item :label="$t('_common.username')" path="username">
              <n-input
                id="username"
                v-model:value="credentials.username"
                type="text"
                name="username"
                autocomplete="username"
                placeholder="Username"
              />
            </n-form-item>
            <n-form-item :label="$t('_common.password')" path="password">
              <n-input
                id="password"
                v-model:value="credentials.password"
                type="password"
                name="password"
                autocomplete="current-password"
                show-password-on="mousedown"
                placeholder="••••••••"
              />
            </n-form-item>
          </n-form>
          <n-button type="primary" size="large" block :loading="loading" @click="login">
            {{ $t('auth.login_sign_in') }}
          </n-button>
          <n-alert v-if="error" type="error" closable @close="error = ''">
            {{ error }}
          </n-alert>
        </template>

        <n-space v-else vertical align="center" size="medium">
          <n-alert type="success" :show-icon="true">{{ $t('auth.login_success') }}</n-alert>
          <n-spin :show="true">
            <span class="sr-only">{{ $t('auth.login_loading') }}</span>
          </n-spin>
        </n-space>
      </n-space>
    </n-card>
  </div>
</template>

<script setup>
import { reactive, ref, onMounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import {
  NForm,
  NFormItem,
  NInput,
  NButton,
  NAlert,
  NSpin,
  NCard,
  NSpace,
  NH2,
  NImage,
} from 'naive-ui';

const auth = useAuthStore();
const credentials = reactive({ username: '', password: '' });
const loading = ref(false);
const error = ref('');
const isLoggedIn = ref(false);
const requestedRedirect = ref('/');
const safeRedirect = ref('/');
const { t } = useI18n ? useI18n() : { t: (k, d) => d || k };

function sanitizeRedirect(raw) {
  try {
    if (!raw || typeof raw !== 'string') return '/';
    // decode then re-encode for validation of % sequences
    try {
      raw = decodeURIComponent(raw);
    } catch {
      /* ignore */
    }
    // Must start with single slash, no protocol, no double slash at start, limit length
    if (!raw.startsWith('/')) return '/';
    if (raw.startsWith('//')) return '/';
    if (raw.includes('://')) return '/';
    if (raw.length > 512) return '/';
    // Strip any /login recursion to avoid loop
    if (raw.startsWith('/login')) return '/';
    return raw;
  } catch {
    return '/';
  }
}
function redirectNowIfAuthenticated() {
  // Rely on shared Pinia auth state (initialized during app startup)
  if (auth.isAuthenticated) {
    const target = sanitizeRedirect(
      sessionStorage.getItem('pending_redirect') || safeRedirect.value || '/',
    );
    window.location.replace(target);
  }
}

onMounted(() => {
  document.title = `Sunshine - ${t('auth.login_title')}`;
  const urlParams = new URLSearchParams(window.location.search);
  const redirectParam = urlParams.get('redirect');
  if (redirectParam) {
    const sanitized = sanitizeRedirect(redirectParam);
    sessionStorage.setItem('pending_redirect', sanitized);
    urlParams.delete('redirect');
    const cleanUrl =
      window.location.pathname + (urlParams.toString() ? '?' + urlParams.toString() : '');
    window.history.replaceState({}, document.title, cleanUrl);
  }
  requestedRedirect.value = sessionStorage.getItem('pending_redirect') || '/';
  safeRedirect.value = sanitizeRedirect(requestedRedirect.value);

  // The auth store is initialized during app init; just perform redirect check and listen for login events
  redirectNowIfAuthenticated();
  auth.onLogin(() => {
    redirectNowIfAuthenticated();
  });
});

/**
 * Attempt login with credentials.
 * @returns {Promise<void>}
 */
async function login() {
  loading.value = true;
  error.value = '';
  try {
    const response = await http.post(
      '/api/auth/login',
      {
        username: credentials.username,
        password: credentials.password,
        redirect: requestedRedirect.value,
      },
      { validateStatus: () => true },
    );
    const data = response.data || {};
    if (response.status === 200 && data.status) {
      // Login endpoint created the session cookie; rely on shared auth detection for global state
      isLoggedIn.value = true;
      safeRedirect.value = sanitizeRedirect(data.redirect) || '/';
      sessionStorage.removeItem('pending_redirect');
      // Attempt immediate redirect; auth store will also detect the cookie and notify listeners
      setTimeout(() => {
        redirectToApp();
      }, 250);
    } else {
      error.value = data.error || t('auth.login_failed');
    }
  } catch (e) {
    console.error('Login error:', e);
    error.value = t('auth.login_network_error');
  } finally {
    loading.value = false;
  }
}

/**
 * Redirects the user to the application after successful login.
 * @returns {void}
 */
function redirectToApp() {
  window.location.replace(safeRedirect.value);
}
</script>
