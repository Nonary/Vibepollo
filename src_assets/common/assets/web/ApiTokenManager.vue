<template>
  <div class="max-w-6xl mx-auto px-4 py-6 space-y-8">
    <header class="flex items-center justify-between gap-4">
      <div>
        <h1 class="text-2xl font-semibold tracking-tight">API Token Management</h1>
        <p class="text-sm opacity-70">Create scoped tokens, manage active ones, and test safely.</p>
      </div>
      <div class="text-xs opacity-75">
        <span class="inline-flex items-center gap-2">
          <i class="fas fa-shield-halved"></i>
          Least-privilege scopes for better security
        </span>
      </div>
    </header>

    <!-- Create Token -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <header class="flex items-center gap-3 mb-4">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-key icon" /> Create Token
        </h2>
      </header>
      <div class="space-y-5">
        <div class="text-sm opacity-75">
          Choose one or more route scopes. Each scope grants specific HTTP methods for a path.
        </div>

        <!-- Add scope row -->
        <n-grid cols="24" x-gap="12" y-gap="8" responsive="screen" class="items-start">
          <n-gi :span="24" :s="12">
            <label class="form-label">Route</label>
            <n-select
              v-model:value="draft.path"
              :options="routeSelectOptions"
              placeholder="Select a route…"
            />
          </n-gi>
          <n-gi :span="24" :s="8">
            <label class="form-label">Methods</label>
            <n-checkbox-group v-model:value="draft.selectedMethods">
              <n-space wrap>
                <n-checkbox v-for="m in draftMethods" :key="m" :value="m">
                  <span class="uppercase text-[11px] tracking-wide font-semibold">{{ m }}</span>
                </n-checkbox>
              </n-space>
            </n-checkbox-group>
            <div v-if="draft.path && draftMethods.length === 0" class="form-text">
              No methods available for this route.
            </div>
          </n-gi>
          <n-gi :span="24" :s="4" class="flex items-center">
            <n-button type="primary" size="medium" :disabled="!canAddScope" @click="addScope">
              <i class="fas fa-plus icon" /> Add Scope
            </n-button>
          </n-gi>
        </n-grid>

        <!-- Current scopes summary -->
        <div v-if="scopes.length" class="space-y-2">
          <div class="text-xs uppercase tracking-wider opacity-70">Scopes</div>
          <div class="flex flex-col gap-2">
            <div
              v-for="(s, idx) in scopes"
              :key="idx + ':' + s.path"
              class="flex items-start gap-2"
            >
              <div
                class="flex flex-wrap items-center gap-1 bg-dark/5 dark:bg-light/10 rounded px-2 py-1"
              >
                <span class="font-semibold">{{ s.path }}</span>
                <span class="flex gap-1 flex-wrap">
                  <span
                    v-for="m in s.methods"
                    :key="m"
                    class="uppercase text-[11px] tracking-wide bg-primary/20 text-brand rounded px-1 py-0.5"
                    >{{ m }}</span
                  >
                </span>
              </div>
              <n-button tertiary size="small" title="Remove" @click="removeScope(idx)">
                <i class="fas fa-times icon"></i>
              </n-button>
            </div>
          </div>
        </div>

        <div class="flex items-center gap-3">
          <n-button
            type="success"
            size="medium"
            :disabled="!canGenerate || creating"
            :loading="creating"
            @click="createToken"
          >
            <i class="fas fa-key icon" /> Generate Token
          </n-button>
          <span v-if="creating" class="text-xs opacity-70">Creating…</span>
        </div>

        <!-- Created token output moved to modal -->

        <n-alert v-if="createError" type="error" closable @close="createError = ''">
          {{ createError }}
        </n-alert>
      </div>
    </section>

    <!-- Token Modal -->
    <n-modal :show="showTokenModal" @update:show="(v) => (showTokenModal = v)">
      <n-card title="API Token Created" :bordered="false" style="max-width: 40rem; width: 100%">
        <div class="space-y-3">
          <n-alert type="warning" :show-icon="true">
            <i class="fas fa-triangle-exclamation" /> This token is shown only once. Save or copy it
            now. You cannot retrieve it later.
          </n-alert>
          <div
            class="rounded-md border border-dark/10 dark:border-light/10 p-3 bg-dark/5 dark:bg-light/10"
          >
            <div class="text-xs opacity-80">Token</div>
            <code class="block text-sm font-mono break-words my-1">{{ createdToken }}</code>
            <div class="flex items-center gap-3">
              <n-button size="small" type="primary" @click="copy(createdToken)">
                <i class="fas fa-copy icon" /> Copy
              </n-button>
              <span v-if="copied" class="text-xs text-success">Copied!</span>
            </div>
          </div>
        </div>
        <template #footer>
          <div class="flex items-center justify-end">
            <n-button type="primary" @click="showTokenModal = false">{{
              $t('_common.dismiss')
            }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>

    <!-- Active Tokens -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <div class="flex items-center gap-3 mb-4">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-lock icon" /> Active Tokens
        </h2>
        <n-button
          class="ml-auto"
          type="default"
          tertiary
          size="small"
          :loading="tokensLoading"
          aria-label="Refresh tokens"
          @click="loadTokens"
        >
          <i class="fas fa-rotate icon" />
          <span class="hidden sm:inline">Refresh</span>
        </n-button>
      </div>
      <div class="space-y-3">
        <n-grid cols="24" x-gap="12" y-gap="8" responsive="screen" class="items-end">
          <n-gi :span="24" :s="18">
            <n-input v-model:value="filter" placeholder="Filter by hash or path…" />
          </n-gi>
          <n-gi :span="24" :s="6">
            <label class="opacity-70 text-xs block mb-1">Sort</label>
            <n-select v-model:value="sortBy" :options="sortOptions" />
          </n-gi>
        </n-grid>

        <div v-if="tokensError" class="text-sm text-danger">{{ tokensError }}</div>

        <div v-if="filteredTokens.length === 0 && !tokensLoading" class="text-sm opacity-70">
          No active tokens.
        </div>

        <div v-else class="overflow-auto">
          <n-table :bordered="false" :single-line="false" class="min-w-[640px]">
            <thead>
              <tr>
                <th class="w-40">Hash</th>
                <th>Scopes</th>
                <th class="w-28">Created</th>
                <th class="w-24"></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="t in filteredTokens" :key="t.hash">
                <td class="align-middle">
                  <span
                    class="font-mono text-xs inline-block min-w-[70px] cursor-pointer"
                    tabindex="0"
                    title="Click to copy"
                    @click="copy(t.hash)"
                    @keydown.enter.prevent="copy(t.hash)"
                  >
                    {{ shortHash(t.hash) }}
                  </span>
                </td>
                <td class="align-middle">
                  <div class="flex flex-wrap gap-1">
                    <div
                      v-for="(s, idx) in t.scopes"
                      :key="idx"
                      class="rounded bg-dark/5 dark:bg-light/10 px-2 py-1"
                    >
                      <span class="font-semibold">{{ s.path }}</span>
                      <span class="ml-1 space-x-1">
                        <span
                          v-for="m in s.methods"
                          :key="m"
                          class="uppercase text-[11px] tracking-wide bg-primary/20 text-brand rounded px-1 py-0.5"
                          >{{ m }}</span
                        >
                      </span>
                    </div>
                  </div>
                </td>
                <td class="text-xs opacity-70 align-middle">
                  {{ t.createdAt ? formatTime(t.createdAt) : '—' }}
                </td>
                <td class="align-middle">
                  <n-button
                    size="small"
                    type="error"
                    :loading="revoking === t.hash"
                    @click="promptRevoke(t)"
                  >
                    <i class="fas fa-ban icon" /> Revoke
                  </n-button>
                </td>
              </tr>
            </tbody>
          </n-table>
        </div>
      </div>
    </section>

    <!-- Tester (GET only) -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <header class="flex items-center gap-3 mb-4">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-vial icon" /> Test Token
        </h2>
      </header>
      <div class="space-y-4">
        <n-alert type="info">
          <span class="text-xs">
            Tester performs only safe GET requests. Select a route and send a request with your
            token.
          </span>
        </n-alert>
        <n-grid cols="24" x-gap="12" y-gap="12" responsive="screen">
          <n-gi :span="24" :s="12">
            <label class="form-label">Token</label>
            <n-input v-model:value="test.token" placeholder="Paste token" type="password" />
          </n-gi>
          <n-gi :span="24" :s="12">
            <label class="form-label">Route (GET only)</label>
            <n-select
              v-model:value="test.path"
              :options="getRouteOptions"
              placeholder="Select a GET route…"
            />
          </n-gi>
          <n-gi :span="24" :s="12">
            <label class="form-label">Header Scheme</label>
            <n-select v-model:value="test.scheme" :options="schemeOptions" />
          </n-gi>
          <n-gi :span="24" :s="12">
            <label class="form-label">Query String (optional)</label>
            <n-input v-model:value="test.query" placeholder="e.g. limit=50&tail=true" />
          </n-gi>
        </n-grid>
        <n-alert v-if="test.scheme === 'query'" type="warning" :show-icon="true" class="text-xs">
          Using query param (e.g., ?token=...) may expose the token in logs and referers. Prefer
          header schemes.
        </n-alert>
        <div class="flex items-center gap-3">
          <n-button type="primary" :disabled="!canSendTest" :loading="testing" @click="sendTest">
            <i class="fas fa-paper-plane icon" /> Test Token
          </n-button>
          <span v-if="testing" class="text-xs opacity-70">Sending…</span>
        </div>

        <div v-if="testError" class="alert alert-danger text-sm">{{ testError }}</div>

        <div v-if="testResponse" class="space-y-2">
          <div class="text-xs opacity-70">Response</div>
          <pre
            class="p-3 rounded-md bg-dark/5 dark:bg-light/10 text-dark dark:text-light text-xs overflow-auto max-h-[60vh]"
          ><code class="whitespace-pre-wrap">{{ testResponse }}</code></pre>
        </div>
      </div>
    </section>

    <!-- Revoke confirm modal -->
    <n-modal :show="showRevoke" @update:show="(v) => (showRevoke = v)">
      <n-card
        :title="$t('auth.confirm_revoke_title')"
        :bordered="false"
        style="max-width: 32rem; width: 100%"
      >
        <div class="text-sm text-center">
          {{
            $t('auth.confirm_revoke_message_hash', { hash: shortHash(pendingRevoke?.hash || '') })
          }}
        </div>
        <template #footer>
          <div class="w-full flex items-center justify-center gap-3">
            <n-button tertiary @click="showRevoke = false">{{ $t('cancel') }}</n-button>
            <n-button secondary @click="confirmRevoke">{{ $t('auth.revoke') }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onBeforeUnmount, reactive, ref, watch } from 'vue';
import {
  NButton,
  NAlert,
  NModal,
  NCard,
  NGrid,
  NGi,
  NSelect,
  NInput,
  NCheckboxGroup,
  NCheckbox,
  NTable,
  NSpace,
  useMessage,
} from 'naive-ui';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';

type RouteDef = { path: string; methods: string[] };
type Scope = { path: string; methods: string[] };
type TokenRecord = { hash: string; scopes: Scope[]; createdAt?: string | number | null };

// Available API routes and methods
const ROUTE_OPTIONS: RouteDef[] = [
  { path: '/api/pin', methods: ['POST'] },
  { path: '/api/apps', methods: ['GET', 'POST'] },
  { path: '/api/logs', methods: ['GET'] },
  { path: '/api/config', methods: ['GET', 'POST'] },
  { path: '/api/configLocale', methods: ['GET'] },
  { path: '/api/restart', methods: ['POST'] },
  { path: '/api/password', methods: ['POST'] },
  { path: '/api/apps/([0-9]+)', methods: ['DELETE'] },
  { path: '/api/clients/unpair-all', methods: ['POST'] },
  { path: '/api/clients/list', methods: ['GET'] },
  { path: '/api/clients/unpair', methods: ['POST'] },
  { path: '/api/apps/close', methods: ['POST'] },
  { path: '/api/covers/upload', methods: ['POST'] },
  { path: '/api/token', methods: ['POST'] },
  { path: '/api/tokens', methods: ['GET'] },
  { path: '/api/token/([a-fA-F0-9]+)', methods: ['DELETE'] },
];

const GET_OPTIONS = computed(() => ROUTE_OPTIONS.filter((r) => r.methods.includes('GET')));
const routeSelectOptions = computed(() =>
  ROUTE_OPTIONS.map((r) => ({ label: r.path, value: r.path })),
);
const getRouteOptions = computed(() =>
  GET_OPTIONS.value.map((r) => ({ label: r.path, value: r.path })),
);
const sortOptions = [
  { label: 'Newest', value: 'created' },
  { label: 'Path', value: 'path' },
];
const schemeOptions = [
  { label: 'Authorization: Bearer <token>', value: 'bearer' },
  { label: 'X-Api-Token: <token>', value: 'x-api-token' },
  { label: 'X-Token: <token>', value: 'x-token' },
  { label: 'Query param ?token=<token>', value: 'query' },
];

// Create token state
const draft = reactive<{ path: string; selectedMethods: string[] }>({
  path: '',
  selectedMethods: [],
});
const scopes = ref<Scope[]>([]);
const creating = ref(false);
const createdToken = ref('');
const createError = ref('');
const copied = ref(false);
const showTokenModal = ref(false);

// Cleanup trackers for in-flight HTTP
const _aborts = new Set<AbortController>();
function makeAbortController() {
  const ac = new AbortController();
  _aborts.add(ac);
  return ac;
}
function releaseAbortController(ac: AbortController) {
  _aborts.delete(ac);
}

const draftMethods = computed<string[]>(
  () => ROUTE_OPTIONS.find((r) => r.path === draft.path)?.methods || [],
);
const canAddScope = computed(() => !!draft.path && draft.selectedMethods.length > 0);
const canGenerate = computed(
  () => scopes.value.length > 0 || (draft.path && draft.selectedMethods.length > 0),
);

function addScope(): void {
  if (!canAddScope.value) return;
  const methods = Array.from(new Set(draft.selectedMethods.map((m) => m.toUpperCase())));
  const existingIdx = scopes.value.findIndex((s) => s.path === draft.path);
  if (existingIdx !== -1) {
    // Merge and de-duplicate (guard against undefined index access)
    const cur = scopes.value[existingIdx];
    const merged = Array.from(new Set([...(cur?.methods ?? []), ...methods]));
    scopes.value[existingIdx] = { path: draft.path, methods: merged };
  } else {
    scopes.value.push({ path: draft.path, methods });
  }
  draft.path = '';
  draft.selectedMethods = [];
}

function removeScope(idx: number): void {
  scopes.value.splice(idx, 1);
}

function getEffectiveScopes(): Scope[] {
  const s = scopes.value.slice();
  if (draft.path && draft.selectedMethods.length > 0) {
    const methods = Array.from(new Set(draft.selectedMethods.map((m) => m.toUpperCase())));
    const idx = s.findIndex((x) => x.path === draft.path);
    if (idx !== -1) {
      const cur = s[idx];
      s[idx] = {
        path: draft.path,
        methods: Array.from(new Set([...(cur?.methods ?? []), ...methods])),
      };
    } else {
      s.push({ path: draft.path, methods });
    }
  }
  return s;
}

async function createToken(): Promise<void> {
  createError.value = '';
  createdToken.value = '';
  copied.value = false;
  creating.value = true;
  let ac: AbortController | null = null;
  try {
    // Tentative payload; backend can map to expected format
    const newScopes = getEffectiveScopes();
    lastCreatedScopes.value = newScopes.slice();
    const payload = { scopes: newScopes };
    ac = makeAbortController();
    const res = await http.post('/api/token', payload, {
      validateStatus: () => true,
      signal: ac.signal,
    });
    if (res.status >= 200 && res.status < 300) {
      const token = (res.data && (res.data.token || res.data.value || res.data)) as string;
      if (typeof token === 'string' && token.length > 0) {
        createdToken.value = token;
        // refresh active list
        await loadTokens();
        showTokenModal.value = true;
      } else {
        createError.value = 'Token created, but server returned no token string.';
      }
    } else {
      const msg = (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`;
      createError.value = `Failed to create token: ${msg}`;
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED')
      createError.value = e?.message || 'Network error creating token.';
  } finally {
    if (ac) releaseAbortController(ac);
    creating.value = false;
  }
}

async function copy(text: string): Promise<void> {
  copied.value = false;
  try {
    await navigator.clipboard.writeText(text);
    copied.value = true;
    setTimeout(() => (copied.value = false), 1500);
  } catch {
    // fallback
    try {
      const ta = document.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.position = 'absolute';
      ta.style.left = '-9999px';
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
      copied.value = true;
      setTimeout(() => (copied.value = false), 1500);
    } catch {
      /* noop */
    }
  }
}

// Active tokens state
const tokens = ref<TokenRecord[]>([]);
const tokensLoading = ref(false);
const tokensError = ref('');
const filter = ref('');
const sortBy = ref<'created' | 'path'>('created');
const revoking = ref('');
const showRevoke = ref(false);
const pendingRevoke = ref<TokenRecord | null>(null);

function normalizeToken(rec: any): TokenRecord | null {
  if (!rec) return null;
  const scopes: Scope[] = Array.isArray(rec.scopes)
    ? rec.scopes.map((s: any) => ({
        path: s.path || s.route || '',
        methods: (s.methods || s.verbs || []).map((v: any) => String(v).toUpperCase()),
      }))
    : [];
  const hash: string = rec.hash ?? rec.id ?? rec.token_hash ?? '';
  const createdAt = rec.createdAt ?? rec.created_at ?? rec.created ?? null;
  if (!hash) return null;
  return { hash, scopes, createdAt };
}

async function loadTokens(): Promise<void> {
  const auth = useAuthStore();
  if (!auth.isAuthenticated) {
    // Not authenticated; do not poll or load
    tokensLoading.value = false;
    return;
  }
  tokensLoading.value = true;
  tokensError.value = '';
  let ac: AbortController | null = null;
  try {
    ac = makeAbortController();
    const res = await http.get('/api/tokens', { validateStatus: () => true, signal: ac.signal });
    if (res.status >= 200 && res.status < 300) {
      const list = Array.isArray(res.data) ? res.data : res.data?.tokens || [];
      tokens.value = (list as any[]).map((x) => normalizeToken(x)).filter(Boolean) as TokenRecord[];
    } else {
      const msg = (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`;
      tokensError.value = `Failed to load tokens: ${msg}`;
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED')
      tokensError.value = e?.message || 'Network error loading tokens.';
  } finally {
    if (ac) releaseAbortController(ac);
    tokensLoading.value = false;
  }
}

function promptRevoke(t: TokenRecord): void {
  pendingRevoke.value = t;
  showRevoke.value = true;
}

async function confirmRevoke(): Promise<void> {
  const t = pendingRevoke.value;
  if (!t?.hash) return;
  revoking.value = t.hash;
  let ac: AbortController | null = null;
  try {
    const url = `/api/token/${encodeURIComponent(t.hash)}`;
    ac = makeAbortController();
    const res = await http.delete(url, { validateStatus: () => true, signal: ac.signal });
    if (res.status >= 200 && res.status < 300) {
      tokens.value = tokens.value.filter((x) => x.hash !== t.hash);
      showRevoke.value = false;
      pendingRevoke.value = null;
    } else {
      const msg = (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`;
      message.error(`Failed to revoke: ${msg}`);
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED')
      message.error(`Failed to revoke: ${e?.message || 'Network error'}`);
  } finally {
    if (ac) releaseAbortController(ac);
    revoking.value = '';
  }
}

const filteredTokens = computed<TokenRecord[]>(() => {
  const q = (filter.value || '').toLowerCase();
  let out = tokens.value.filter((t) => {
    if (!q) return true;
    if (t.hash.toLowerCase().includes(q)) return true;
    return t.scopes.some((s) => s.path.toLowerCase().includes(q));
  });
  if (sortBy.value === 'created') {
    out = out.slice().sort((a, b) => {
      const ta = a.createdAt ? Date.parse(String(a.createdAt)) : 0;
      const tb = b.createdAt ? Date.parse(String(b.createdAt)) : 0;
      return tb - ta; // newest first
    });
  } else if (sortBy.value === 'path') {
    const firstPath = (t: TokenRecord) => t.scopes.map((s) => s.path).sort()[0] || '';
    out = out.slice().sort((a, b) => firstPath(a).localeCompare(firstPath(b)));
  }
  return out;
});

function shortHash(h: string): string {
  if (!h) return '';
  if (h.length <= 10) return h;
  return `${h.slice(0, 6)}…${h.slice(-4)}`;
}

function formatTime(v: any): string {
  try {
    const d = typeof v === 'number' ? new Date(v) : new Date(String(v));
    if (isNaN(d.getTime())) return '—';
    return d.toLocaleString();
  } catch {
    return '—';
  }
}

// Tester state
const test = reactive<{
  token: string;
  path: string;
  query: string;
  scheme: 'bearer' | 'x-api-token' | 'x-token' | 'query';
}>({ token: '', path: '', query: '', scheme: 'bearer' });
const testing = ref(false);
const testResponse = ref('');
const testError = ref('');
const canSendTest = computed(() => !!test.token && !!test.path);

// Store last created token scopes to assist tester auto-selection
const lastCreatedScopes = ref<Scope[]>([]);

function firstGetScopePath(scopesIn: Scope[]): string {
  try {
    for (const s of scopesIn || []) {
      const methods = (s.methods || []).map((m) => String(m).toUpperCase());
      if (methods.includes('GET')) return s.path;
    }
    return '';
  } catch {
    return '';
  }
}

async function sendTest(): Promise<void> {
  testError.value = '';
  testResponse.value = '';
  testing.value = true;
  let ac: AbortController | null = null;
  try {
    const urlBase = test.path;
    const qs = (test.query || '').trim();
    const fullUrl = qs ? `${urlBase}?${qs}` : urlBase;
    const headers: Record<string, string> = { 'X-Requested-With': 'XMLHttpRequest' };
    if (test.scheme === 'bearer') headers['Authorization'] = `Bearer ${test.token}`;
    if (test.scheme === 'x-api-token') headers['X-Api-Token'] = test.token;
    if (test.scheme === 'x-token') headers['X-Token'] = test.token;
    const url =
      test.scheme === 'query'
        ? `${fullUrl}${fullUrl.includes('?') ? '&' : '?'}token=${encodeURIComponent(test.token)}`
        : fullUrl;
    ac = makeAbortController();
    const res = await http.get(url, { headers, validateStatus: () => true, signal: ac.signal });
    const pretty = prettyPrint(res.data);
    testResponse.value = `${res.status} ${res.statusText || ''}\n\n${pretty}`;
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED') testError.value = e?.message || 'Test request failed.';
  } finally {
    if (ac) releaseAbortController(ac);
    testing.value = false;
  }
}

function prettyPrint(data: any): string {
  try {
    if (typeof data === 'string') {
      // try parse as JSON, else return as-is
      try {
        const obj = JSON.parse(data);
        return JSON.stringify(obj, null, 2);
      } catch {
        return data;
      }
    }
    return JSON.stringify(data, null, 2);
  } catch {
    return String(data);
  }
}

onMounted(() => {
  const auth = useAuthStore();
  const start = () => {
    loadTokens();
  };
  if (auth.isAuthenticated) start();
  else {
    // Wait for auth only; do not init here
    const off = auth.onLogin(() => {
      try {
        start();
      } finally {
        // unsubscribe after first run
        off?.();
      }
    });
  }
});

onBeforeUnmount(() => {
  // Abort pending HTTP
  _aborts.forEach((ac) => {
    try {
      ac.abort();
    } catch {}
  });
  _aborts.clear();
});

// Message API for consistent feedback
const message = useMessage();

// React to newly-created token to auto-fill the tester
watch(
  () => createdToken.value,
  (val) => {
    if (!val) return;
    test.token = val;
    if (!test.path) {
      const first = firstGetScopePath(lastCreatedScopes.value);
      if (first) test.path = first;
    }
  },
);
</script>

<style scoped>
.icon {
  font-size: 0.95em;
  line-height: 1;
}

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.2s ease;
}
.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>
