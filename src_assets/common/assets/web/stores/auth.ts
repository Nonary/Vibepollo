import { defineStore } from 'pinia';
import { ref, Ref } from 'vue';
import { http } from '@/http';

interface AuthStatusResponse {
  credentials_configured?: boolean;
  authenticated?: boolean;
  login_required?: boolean;
}

type AuthListener = () => void;

// Auth store now driven by axios/http interceptor layer instead of polling.
// Provides subscription for login events and a setter used by http.js.
export const useAuthStore = defineStore('auth', () => {
  const isAuthenticated: Ref<boolean> = ref(false);
  const ready: Ref<boolean> = ref(false);
  const _listeners: AuthListener[] = [];
  const showLoginModal: Ref<boolean> = ref(false);
  const credentialsConfigured: Ref<boolean> = ref(true);
  const loggingIn: Ref<boolean> = ref(false);
  const logoutInitiated: Ref<boolean> = ref(false);

  function setAuthenticated(v: boolean): void {
    if (v === isAuthenticated.value) return;
    const becameAuthed = !isAuthenticated.value && v;
    isAuthenticated.value = v;
    if (becameAuthed) {
      for (const cb of _listeners) {
        try {
          cb();
        } catch (e) {
          console.error('auth listener error', e);
        }
      }
    }
  }

  function initiateLogout(): void {
    // Mark that this logout was user-initiated so we suppress login prompts
    logoutInitiated.value = true;
    // Immediately reflect unauthenticated state and hide login modal
    setAuthenticated(false);
    showLoginModal.value = false;
  }

  // Single init call invoked during app bootstrap after http layer validation.
  async function init(): Promise<void> {
    if (ready.value) return;
    try {
      const res = await http.get<AuthStatusResponse>('/api/auth/status', {
        validateStatus: () => true,
      });
      if (res && res.status === 200 && res.data) {
        if (typeof res.data.credentials_configured === 'boolean') {
          credentialsConfigured.value = res.data.credentials_configured;
        }
        if (res.data.authenticated) {
          setAuthenticated(true);
        }
        // Show login only if explicitly required and not already authenticated
        if (res.data.login_required && !res.data.authenticated) {
          showLoginModal.value = true;
        }
      }
    } catch (e) {
      // Network issues; defer to interceptor behavior
    } finally {
      ready.value = true;
    }
  }

  function onLogin(cb: AuthListener): () => void {
    if (typeof cb !== 'function') return () => {};
    _listeners.push(cb);
    if (isAuthenticated.value)
      setTimeout(() => {
        try {
          cb();
        } catch {}
      }, 0);
    return () => {
      const idx = _listeners.indexOf(cb);
      if (idx !== -1) _listeners.splice(idx, 1);
    };
  }

  function requireLogin(): void {
    // If user intentionally logged out, do not show the login modal
    if (logoutInitiated.value) return;
    showLoginModal.value = true;
  }

  function hideLogin(): void {
    showLoginModal.value = false;
  }

  function setCredentialsConfigured(v: boolean): void {
    credentialsConfigured.value = !!v;
  }

  async function waitForAuthentication(): Promise<void> {
    while (!isAuthenticated.value) {
      await new Promise<void>((resolve) => setTimeout(resolve, 20));
    }
  }

  return {
    isAuthenticated,
    ready,
    init,
    setAuthenticated,
    initiateLogout,
    onLogin,
    showLoginModal,
    requireLogin,
    hideLogin,
    credentialsConfigured,
    setCredentialsConfigured,
    waitForAuthentication,
    loggingIn,
    logoutInitiated,
  };
});
