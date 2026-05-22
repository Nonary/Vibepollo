import type { AxiosResponse, InternalAxiosRequestConfig } from 'axios';
import { beforeEach, describe, expect, test, vi } from 'vitest';

async function setupHttp() {
  vi.resetModules();
  const pinia = await import('pinia');
  pinia.setActivePinia(pinia.createPinia());
  const httpModule = await import('@web/http');
  httpModule.initHttpLayer();
  return httpModule.http;
}

function response(
  config: InternalAxiosRequestConfig,
  status: number,
  data: Record<string, unknown>,
): AxiosResponse {
  return {
    config,
    data,
    headers: {},
    status,
    statusText: String(status),
  };
}

describe('http CSRF handling', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
  });

  test('adds a CSRF token to unauthenticated login requests', async () => {
    const http = await setupHttp();
    const calls: InternalAxiosRequestConfig[] = [];

    http.defaults.adapter = vi.fn(async (config: InternalAxiosRequestConfig) => {
      calls.push(config);
      if (config.url === '/api/csrf-token') {
        return response(config, 200, { csrf_token: 'token-1' });
      }
      return response(config, 200, { status: true });
    });

    const res = await http.post('/api/auth/login', {}, { validateStatus: () => true });

    expect(res.status).toBe(200);
    expect(calls.map((call) => `${call.method?.toUpperCase()} ${call.url}`)).toEqual([
      'GET /api/csrf-token',
      'POST /api/auth/login',
    ]);
    expect(calls[1]?.headers?.['X-CSRF-Token']).toBe('token-1');
  });

  test('refreshes and retries once when the cached CSRF token is rejected', async () => {
    const http = await setupHttp();
    const calls: InternalAxiosRequestConfig[] = [];
    let tokenRequestCount = 0;

    http.defaults.adapter = vi.fn(async (config: InternalAxiosRequestConfig) => {
      calls.push(config);
      if (config.url === '/api/csrf-token') {
        tokenRequestCount += 1;
        return response(config, 200, {
          csrf_token: tokenRequestCount === 1 ? 'stale-token' : 'fresh-token',
        });
      }
      if (config.headers?.['X-CSRF-Token'] === 'stale-token') {
        return response(config, 400, { error: 'Invalid CSRF token' });
      }
      return response(config, 200, { status: true });
    });

    const res = await http.post('/api/auth/login', {}, { validateStatus: () => true });

    expect(res.status).toBe(200);
    expect(calls.map((call) => `${call.method?.toUpperCase()} ${call.url}`)).toEqual([
      'GET /api/csrf-token',
      'POST /api/auth/login',
      'GET /api/csrf-token',
      'POST /api/auth/login',
    ]);
    expect(calls[1]?.headers?.['X-CSRF-Token']).toBe('stale-token');
    expect(calls[3]?.headers?.['X-CSRF-Token']).toBe('fresh-token');
  });
});
