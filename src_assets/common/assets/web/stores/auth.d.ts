export interface AuthSession {
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

interface RequireLoginOptions {
  bypassLogoutGuard?: boolean;
}

interface AuthStore {
  isAuthenticated: boolean;
  ready: boolean;
  showLoginModal: boolean;
  credentialsConfigured: boolean;
  loggingIn: boolean;
  logoutInitiated: boolean;
  sessions: AuthSession[];
  sessionsLoading: boolean;
  sessionsError: string;
  init(): Promise<void>;
  setAuthenticated(value: boolean): void;
  initiateLogout(): void;
  onLogin(callback: () => void): () => void;
  requireLogin(options?: RequireLoginOptions): void;
  hideLogin(): void;
  setCredentialsConfigured(value: boolean): void;
  waitForAuthentication(): Promise<void>;
  fetchSessions(): Promise<void>;
  revokeSession(id: string): Promise<boolean>;
  currentSessionId(): string | undefined;
}

export declare const useAuthStore: () => AuthStore;
