// Axios HTTP client with centralized auth handling
import axios, { AxiosResponse, AxiosError } from 'axios';
import { useAuthStore } from '@/stores/auth';

// Create a singleton axios instance
export const http = axios.create({
  // baseURL left relative so it works behind reverse proxies
  withCredentials: true,
  headers: {
    'X-Requested-With': 'XMLHttpRequest',
  },
});

let authInitialized = false;

function initAuthHandling(): void {
  if (authInitialized) return;
  authInitialized = true;
  const auth = useAuthStore();

  // Block outgoing requests while logged out, except auth endpoints
  http.interceptors.request.use((config) => {
    try {
      const urlRaw = String(config.url || '');
      // Extract pathname if absolute URL
      let path = urlRaw;
      try {
        // If it parses, prefer the pathname; else keep as-is for relative paths
        const u = new URL(urlRaw, window.location.origin);
        path = u.pathname;
      } catch {}
      // If user initiated logout, block all outgoing requests
      if ((auth as any).logoutInitiated) {
        const err: any = new Error('Request blocked: user logged out');
        err.code = 'ERR_CANCELED';
        return Promise.reject(err);
      }
      const allowWhenLoggedOut =
        /(\s*\/api\/auth\/(login|status)\b|\s*\/api\/password\b|\s*\/api\/configLocale\b)/.test(
          path,
        );
      if (!auth.isAuthenticated && !allowWhenLoggedOut) {
        const err: any = new Error('Request blocked: unauthenticated');
        err.code = 'ERR_CANCELED';
        return Promise.reject(err);
      }
      return config;
    } catch {
      return config;
    }
  });

  function triggerLoginModal(): void {
    if (typeof window === 'undefined') return;
    try {
      // Show login overlay; no redirect path tracking needed
      auth.requireLogin({ bypassLogoutGuard: true });
    } catch {
      /* noop */
    }
  }

  // Response interceptor to detect auth changes
  http.interceptors.response.use(
    async (response: AxiosResponse) => {
      try {
        if (typeof window !== 'undefined') {
          window.dispatchEvent(new CustomEvent('sunshine:online'));
        }
      } catch {}
      if (response?.status === 401) {
        if (auth.isAuthenticated) auth.setAuthenticated(false);
        // Suppress login modal if user explicitly logged out
        if (!(auth as any).logoutInitiated) triggerLoginModal();
      }
      return response;
    },
    async (error: AxiosError) => {
      // Network-level errors (no response) indicate possible server unavailability
      try {
        if (typeof window !== 'undefined') {
          const isCanceled = (error as any)?.code === 'ERR_CANCELED';
          const auth = useAuthStore();
          const userLoggedOut = (auth as any).logoutInitiated === true;
          if (!error?.response) {
            // Only signal offline if it's not a client-side canceled request
            // and not during user-initiated logout
            if (!isCanceled && !userLoggedOut) {
              window.dispatchEvent(new CustomEvent('sunshine:offline'));
            }
          } else {
            // Any HTTP response means the service is reachable (even 401)
            window.dispatchEvent(new CustomEvent('sunshine:online'));
          }
        }
      } catch {}
      if (error?.response?.status === 401) {
        if (auth.isAuthenticated) auth.setAuthenticated(false);
        if (!(auth as any).logoutInitiated) triggerLoginModal();
      } else if (
        error?.response?.status === 400 &&
        error?.response?.data &&
        /Credentials not configured/i.test(JSON.stringify(error.response.data))
      ) {
        // Backend indicates no credentials configured yet
        auth.setCredentialsConfigured(false);
        triggerLoginModal();
      }
      return Promise.reject(error);
    },
  );
}

// Called from main init after pinia is ready
export function initHttpLayer(): void {
  initAuthHandling();
}
