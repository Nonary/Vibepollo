export type ApiPayload = Record<string, unknown> | unknown[] | string | number | boolean | null;

export type ApiErrorCode =
  | 'invalid-json'
  | 'csrf-request-failed'
  | 'csrf-token-missing'
  | 'request-failed';

export class ApiError extends Error {
  readonly code: ApiErrorCode;
  readonly status: number;
  readonly payload: unknown;

  constructor(code: ApiErrorCode, status: number, payload?: unknown) {
    super(code);
    this.name = 'ApiError';
    this.code = code;
    this.status = status;
    this.payload = payload;
  }
}

interface ApiRequestOptions extends Omit<RequestInit, 'body'> {
  body?: BodyInit | null;
  json?: ApiPayload;
}

let csrfToken: string | null = null;
let csrfRequest: Promise<string> | null = null;

function isMutation(method: string): boolean {
  return ['POST', 'PUT', 'PATCH', 'DELETE'].includes(method);
}

function isCsrfFailure(payload: unknown): boolean {
  if (!payload || typeof payload !== 'object') return false;
  const error = (payload as { error?: unknown }).error;
  return typeof error === 'string' && error.toLocaleLowerCase().includes('csrf');
}

async function parseResponse(response: Response): Promise<unknown> {
  if (response.status === 204) return null;

  const text = await response.text();
  if (!text) return null;

  const contentType = response.headers.get('content-type') ?? '';
  if (contentType.includes('json')) {
    try {
      return JSON.parse(text) as unknown;
    } catch {
      throw new ApiError('invalid-json', response.status, text);
    }
  }
  return text;
}

async function requestCsrfToken(): Promise<string> {
  if (csrfToken) return csrfToken;
  if (csrfRequest) return csrfRequest;

  csrfRequest = (async () => {
    const response = await fetch('/api/csrf-token', {
      credentials: 'same-origin',
      headers: { Accept: 'application/json' },
    });
    const payload = await parseResponse(response);
    if (!response.ok) {
      throw new ApiError('csrf-request-failed', response.status, payload);
    }
    const token = (payload as { csrf_token?: unknown } | null)?.csrf_token;
    if (typeof token !== 'string' || !token) {
      throw new ApiError('csrf-token-missing', response.status, payload);
    }
    csrfToken = token;
    return token;
  })();

  try {
    return await csrfRequest;
  } finally {
    csrfRequest = null;
  }
}

export function clearCsrfToken(): void {
  csrfToken = null;
  csrfRequest = null;
}

export async function apiRequest<T>(
  path: string,
  options: ApiRequestOptions = {},
  retryCsrf = true,
): Promise<T> {
  const method = (options.method ?? 'GET').toUpperCase();
  const headers = new Headers(options.headers);
  headers.set('Accept', 'application/json');

  let body = options.body;
  if (options.json !== undefined) {
    headers.set('Content-Type', 'application/json');
    body = JSON.stringify(options.json);
  }

  if (isMutation(method)) {
    headers.set('X-CSRF-Token', await requestCsrfToken());
  }

  const response = await fetch(path, {
    ...options,
    body,
    credentials: options.credentials ?? 'same-origin',
    headers,
    method,
  });
  const payload = await parseResponse(response);

  if (!response.ok) {
    if (
      retryCsrf &&
      isMutation(method) &&
      response.status === 400 &&
      isCsrfFailure(payload)
    ) {
      clearCsrfToken();
      return apiRequest<T>(path, options, false);
    }
    if (response.status === 401 || response.status === 403) clearCsrfToken();
    throw new ApiError('request-failed', response.status, payload);
  }

  return payload as T;
}

export function apiGet<T>(path: string, options: ApiRequestOptions = {}): Promise<T> {
  return apiRequest<T>(path, { ...options, method: 'GET' });
}

export function apiPost<T>(path: string, json?: ApiPayload): Promise<T> {
  return apiRequest<T>(path, { method: 'POST', json: json ?? {} });
}

export function apiPatch<T>(path: string, json: ApiPayload): Promise<T> {
  return apiRequest<T>(path, { method: 'PATCH', json });
}

export function apiDelete<T>(path: string, json?: ApiPayload): Promise<T> {
  return apiRequest<T>(path, { method: 'DELETE', json });
}
