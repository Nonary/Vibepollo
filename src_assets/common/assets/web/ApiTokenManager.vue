<template>
  <div class="api-token-manager w-full">
    <n-space vertical size="large" class="token-stack w-full">
      <section class="token-hero">
        <div class="token-hero__copy">
          <h2 class="token-hero__title">
            <n-icon size="18"><i class="fas fa-key" /></n-icon>
            {{ t('auth.title') }}
          </h2>
          <p class="token-hero__subtitle">{{ t('auth.subtitle') }}</p>
        </div>
        <n-tag round type="info" size="small" class="token-hero__tag">
          <n-icon size="14"><i class="fas fa-shield-halved" /></n-icon>
          Least-privilege scopes for better security
        </n-tag>
      </section>

      <n-card size="large" class="token-panel">
      <template #header>
        <div class="token-panel__heading">
          <n-space align="center" size="small" class="token-panel__title">
            <n-icon size="18"><i class="fas fa-key" /></n-icon>
            <n-text strong>{{ t('auth.generate_new_token') }}</n-text>
          </n-space>
          <n-button
            type="primary"
            strong
            size="small"
            class="token-panel__action"
            :loading="routeCatalogLoading"
            @click="loadRouteCatalog"
          >
            <n-icon size="14"><i class="fas fa-rotate" /></n-icon>
            {{ t('auth.refresh_routes') }}
          </n-button>
        </div>
      </template>

      <n-space vertical size="large" class="token-panel__body">
        <n-alert type="info" secondary>
          {{ t('auth.generate_token_help') }}
        </n-alert>

        <n-alert v-if="routeCatalogError" type="error" closable @close="routeCatalogError = ''">
          {{ routeCatalogError }}
        </n-alert>
        <n-alert
          v-else-if="!routeCatalogLoading && routeCatalog.length === 0"
          type="warning"
          :show-icon="true"
        >
          {{ t('auth.routes_empty') }}
        </n-alert>

        <n-form :model="draft" label-placement="top" size="medium" class="token-form-shell">
          <n-grid cols="24" x-gap="12" y-gap="12" responsive="screen">
            <n-form-item-gi :span="24" :s="12" label="Route" path="path">
              <n-select
                v-model:value="draft.path"
                :options="routeSelectOptions"
                placeholder="Select a route…"
              />
            </n-form-item-gi>
            <n-form-item-gi :span="24" :s="8" label="Methods" path="selectedMethods">
              <n-checkbox-group v-model:value="draft.selectedMethods">
                <n-space wrap>
                  <n-checkbox v-for="m in draftMethods" :key="m" :value="m">
                    <n-text code>{{ m }}</n-text>
                  </n-checkbox>
                </n-space>
              </n-checkbox-group>
              <n-text v-if="draft.path && draftMethods.length === 0" size="small" depth="3">
                No methods available for this route.
              </n-text>
            </n-form-item-gi>
            <n-form-item-gi :span="24" :s="4" label=" " :show-feedback="false">
              <n-button type="primary" size="medium" class="w-full" :disabled="!canAddScope" @click="addScope">
                <n-icon size="16"><i class="fas fa-plus" /></n-icon>
                {{ t('auth.add_scope') }}
              </n-button>
            </n-form-item-gi>
          </n-grid>
        </n-form>

        <n-space v-if="scopes.length" vertical size="small" class="token-scope-list">
          <n-text depth="3" strong>{{ t('auth.selected_scopes') }}</n-text>
          <div class="token-scope-grid">
            <div v-for="(scope, idx) in scopes" :key="idx + ':' + scope.path" class="token-scope-card">
              <div class="token-scope-card__header">
                <n-text strong>{{ scope.path }}</n-text>
                <n-button type="error" strong size="small" text @click="removeScope(idx)">
                  <n-icon size="14"><i class="fas fa-times" /></n-icon>
                  Remove
                </n-button>
              </div>
              <n-space wrap size="small">
                <n-tag v-for="method in scope.methods" :key="method" type="info" size="small" round>
                  {{ method }}
                </n-tag>
              </n-space>
            </div>
          </div>
        </n-space>

        <n-space align="center" size="small" class="token-generate-row">
          <n-button
            type="success"
            size="medium"
            :disabled="!canGenerate || creating"
            :loading="creating"
            @click="createToken"
          >
            <n-icon size="16"><i class="fas fa-key" /></n-icon>
            {{ t('auth.generate_token') }}
          </n-button>
          <n-text v-if="creating" size="small" depth="3">Creating…</n-text>
        </n-space>

        <n-alert v-if="createError" type="error" closable @close="createError = ''">
          {{ createError }}
        </n-alert>
      </n-space>
      </n-card>

      <n-modal :show="showTokenModal" @update:show="(v) => (showTokenModal = v)">
      <n-card
        class="token-modal-card"
        :title="t('auth.token_created_title')"
        :bordered="false"
        style="max-width: 40rem; width: 100%"
      >
        <n-space vertical size="large">
          <n-alert type="warning" :show-icon="true">
            <n-icon class="mr-2" size="16"><i class="fas fa-triangle-exclamation" /></n-icon>
            This token is shown only once. Save or copy it now. You cannot retrieve it later.
          </n-alert>
          <n-space vertical size="small">
            <n-text depth="3" strong>Token</n-text>
            <n-code :code="createdToken" language="bash" word-wrap />
            <n-space align="center" size="small">
              <n-button size="small" type="primary" @click="copy(createdToken)">
                <n-icon size="14"><i class="fas fa-copy" /></n-icon>
                {{ t('auth.copy_token') }}
              </n-button>
              <n-tag v-if="copied" type="success" size="small" round>Copied!</n-tag>
            </n-space>
          </n-space>
        </n-space>
        <template #footer>
          <n-space justify="end">
            <n-button type="primary" @click="showTokenModal = false">{{
              $t('_common.dismiss')
            }}</n-button>
          </n-space>
        </template>
      </n-card>
      </n-modal>

      <n-card size="large" class="token-panel">
      <template #header>
        <div class="token-panel__heading">
          <n-space align="center" size="small" class="token-panel__title">
            <n-icon size="18"><i class="fas fa-lock" /></n-icon>
            <n-text strong>Active Tokens</n-text>
          </n-space>
          <n-button
            type="primary"
            strong
            size="small"
            :loading="tokensLoading"
            class="token-panel__action"
            :aria-label="t('auth.refresh')"
            @click="loadTokens"
          >
            <n-icon size="14"><i class="fas fa-rotate" /></n-icon>
            {{ t('auth.refresh') }}
          </n-button>
        </div>
      </template>

      <n-space vertical size="large" class="token-panel__body">
        <n-form :model="tableControls" inline label-placement="top" class="token-toolbar">
          <n-form-item :label="t('auth.filter')" path="filter">
            <n-input
              v-model:value="tableControls.filter"
              :placeholder="t('auth.search_tokens')"
              clearable
            />
          </n-form-item>
          <n-form-item label="Sort" path="sortBy">
            <n-select v-model:value="tableControls.sortBy" :options="sortOptions" />
          </n-form-item>
        </n-form>

        <n-alert v-if="tokensError" type="error" closable @close="tokensError = ''">
          {{ tokensError }}
        </n-alert>

        <n-empty
          v-if="filteredTokens.length === 0 && !tokensLoading"
          description="No active tokens."
        />

        <div v-else class="token-records">
          <article v-for="token in filteredTokens" :key="token.hash" class="token-record">
            <div class="token-record__header">
              <button type="button" class="token-hash-button" @click="copy(token.hash)">
                <span class="token-hash-button__label">{{ t('auth.hash') }}</span>
                <span class="token-hash-button__value">{{ shortHash(token.hash) }}</span>
              </button>
              <n-text depth="3" class="token-record__date">
                {{ t('auth.created') }}: {{ token.createdAt ? formatTime(token.createdAt) : '—' }}
              </n-text>
              <n-button
                size="small"
                type="error"
                :loading="revoking === token.hash"
                @click="promptRevoke(token)"
              >
                <n-icon size="14"><i class="fas fa-ban" /></n-icon>
                {{ t('auth.revoke') }}
              </n-button>
            </div>

            <div class="token-record__scopes">
              <div v-for="(scope, idx) in token.scopes" :key="idx" class="token-record-scope">
                <n-text strong>{{ scope.path }}</n-text>
                <n-space wrap size="small">
                  <n-tag v-for="method in scope.methods" :key="method" size="small" type="info" round>
                    {{ method }}
                  </n-tag>
                </n-space>
              </div>
            </div>
          </article>
        </div>
      </n-space>
      </n-card>

      <n-card size="large" class="token-panel">
      <template #header>
        <n-space align="center" size="small" class="token-panel__title">
          <n-icon size="18"><i class="fas fa-vial" /></n-icon>
          <n-text strong>Test Token</n-text>
        </n-space>
      </template>
      <n-space vertical size="large" class="token-panel__body">
        <n-alert type="info" secondary>
          Tester performs only safe GET requests. Select a route and send a request with your token.
        </n-alert>

        <n-form :model="test" label-placement="top" size="medium">
          <n-grid cols="24" x-gap="12" y-gap="12" responsive="screen">
            <n-form-item-gi :span="24" :s="12" label="Token" path="token">
              <n-input v-model:value="test.token" placeholder="Paste token" type="password" />
            </n-form-item-gi>
            <n-form-item-gi :span="24" :s="12" label="Route (GET only)" path="path">
              <n-select
                v-model:value="test.path"
                :options="getRouteOptions"
                placeholder="Select a GET route…"
              />
            </n-form-item-gi>
            <n-form-item-gi :span="24" :s="12" label="Header Scheme" path="scheme">
              <n-select v-model:value="test.scheme" :options="schemeOptions" />
            </n-form-item-gi>
            <n-form-item-gi :span="24" :s="12" label="Query String (optional)" path="query">
              <n-input v-model:value="test.query" placeholder="e.g. limit=50&tail=true" />
            </n-form-item-gi>
          </n-grid>
        </n-form>

        <n-alert v-if="test.scheme === 'query'" type="warning" :show-icon="true">
          Using query param (e.g., ?token=...) may expose the token in logs and referrers. Prefer
          header schemes.
        </n-alert>

        <n-space align="center" size="small">
          <n-button type="primary" :disabled="!canSendTest" :loading="testing" @click="sendTest">
            <n-icon size="16"><i class="fas fa-paper-plane" /></n-icon>
            {{ t('auth.test_token') }}
          </n-button>
          <n-text v-if="testing" size="small" depth="3">Sending…</n-text>
        </n-space>

        <n-alert v-if="testError" type="error" closable @close="testError = ''">
          {{ testError }}
        </n-alert>

        <n-space v-if="testResponse" vertical size="small">
          <n-text depth="3" strong>{{ t('auth.result') }}</n-text>
          <n-scrollbar class="token-result-shell" style="max-height: 60vh">
            <n-code :code="testResponse" language="json" word-wrap />
          </n-scrollbar>
        </n-space>
      </n-space>
      </n-card>

      <n-modal :show="showRevoke" @update:show="(v) => (showRevoke = v)">
      <n-card
        class="token-modal-card"
        :title="t('auth.confirm_revoke_title')"
        :bordered="false"
        style="max-width: 32rem; width: 100%"
      >
        <n-space vertical align="center" size="medium">
          <n-text>
            {{
              $t('auth.confirm_revoke_message_hash', { hash: shortHash(pendingRevoke?.hash || '') })
            }}
          </n-text>
        </n-space>
        <template #footer>
          <n-space justify="end" size="small">
            <n-button type="default" strong @click="showRevoke = false">{{
              $t('cancel')
            }}</n-button>
            <n-button type="error" strong @click="confirmRevoke">{{ $t('auth.revoke') }}</n-button>
          </n-space>
        </template>
      </n-card>
      </n-modal>
    </n-space>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onBeforeUnmount, reactive, ref, watch } from 'vue';
import {
  NAlert,
  NButton,
  NCard,
  NCheckbox,
  NCheckboxGroup,
  NCode,
  NEmpty,
  NForm,
  NFormItem,
  NFormItemGi,
  NGrid,
  NIcon,
  NInput,
  NModal,
  NScrollbar,
  NSelect,
  NSpace,
  NTag,
  NText,
  useMessage,
} from 'naive-ui';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';

type RouteDef = { path: string; methods: string[] };
type Scope = { path: string; methods: string[] };
type TokenRecord = { hash: string; scopes: Scope[]; createdAt?: string | number | null };

// Available API routes and methods
const DEFAULT_ROUTE_OPTIONS: RouteDef[] = [
  { path: '/api/pin', methods: ['POST'] },
  { path: '/api/otp', methods: ['POST'] },
  { path: '/api/apps', methods: ['GET', 'POST'] },
  { path: '/api/apps/reorder', methods: ['POST'] },
  { path: '/api/apps/delete', methods: ['POST'] },
  { path: '/api/apps/launch', methods: ['POST'] },
  { path: '/api/apps/close', methods: ['POST'] },
  { path: '/api/apps/purge_autosync', methods: ['POST'] },
  { path: '/api/apps/([0-9]+)', methods: ['DELETE'] },
  { path: '/api/apps/([^/]+)/cover', methods: ['GET'] },
  { path: '/api/logs', methods: ['GET'] },
  { path: '/api/logs/export', methods: ['GET'] },
  { path: '/api/config', methods: ['GET', 'POST', 'PATCH'] },
  { path: '/api/configLocale', methods: ['GET'] },
  { path: '/api/metadata', methods: ['GET'] },
  { path: '/api/restart', methods: ['POST'] },
  { path: '/api/quit', methods: ['POST'] },
  { path: '/api/password', methods: ['POST'] },
  { path: '/api/session/status', methods: ['GET'] },
  { path: '/api/display-devices', methods: ['GET'] },
  { path: '/api/display/export_golden', methods: ['POST'] },
  { path: '/api/display/golden_status', methods: ['GET'] },
  { path: '/api/display/golden', methods: ['DELETE'] },
  { path: '/api/health/vigem', methods: ['GET'] },
  { path: '/api/covers/upload', methods: ['POST'] },
  { path: '/api/playnite/status', methods: ['GET'] },
  { path: '/api/playnite/install', methods: ['POST'] },
  { path: '/api/playnite/uninstall', methods: ['POST'] },
  { path: '/api/playnite/games', methods: ['GET'] },
  { path: '/api/playnite/categories', methods: ['GET'] },
  { path: '/api/playnite/force_sync', methods: ['POST'] },
  { path: '/api/playnite/launch', methods: ['POST'] },
  { path: '/api/rtss/status', methods: ['GET'] },
  { path: '/api/lossless_scaling/status', methods: ['GET'] },
  { path: '/api/auth/login', methods: ['POST'] },
  { path: '/api/auth/logout', methods: ['POST'] },
  { path: '/api/auth/status', methods: ['GET'] },
  { path: '/api/auth/sessions', methods: ['GET'] },
  { path: '/api/auth/sessions/([A-Fa-f0-9]+)', methods: ['DELETE'] },
  { path: '/api/clients/list', methods: ['GET'] },
  { path: '/api/clients/hdr-profiles', methods: ['GET'] },
  { path: '/api/clients/unpair', methods: ['POST'] },
  { path: '/api/clients/unpair-all', methods: ['POST'] },
  { path: '/api/apps/close', methods: ['POST'] },
  { path: '/api/covers/upload', methods: ['POST'] },
  { path: '/api/clients/update', methods: ['POST'] },
  { path: '/api/clients/disconnect', methods: ['POST'] },
  { path: '/api/token', methods: ['POST'] },
  { path: '/api/tokens', methods: ['GET'] },
  { path: '/api/token/([a-fA-F0-9]+)', methods: ['DELETE'] },
];

const props = defineProps<{ routes?: RouteDef[] }>();

const routeOptions = computed<RouteDef[]>(() =>
  props.routes && props.routes.length > 0 ? props.routes : DEFAULT_ROUTE_OPTIONS,
);
const getOnlyRoutes = computed(() => routeOptions.value.filter((r) => r.methods.includes('GET')));
const routeSelectOptions = computed(() =>
  routeOptions.value.map((r) => ({ label: r.path, value: r.path })),
);
const getRouteOptions = computed(() =>
  getOnlyRoutes.value.map((r) => ({ label: r.path, value: r.path })),
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
  () => routeOptions.value.find((r) => r.path === draft.path)?.methods || [],
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
        await loadTokens({ waitForAuth: true });
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
const tableControls = reactive<{ filter: string; sortBy: 'created' | 'path' }>({
  filter: '',
  sortBy: 'created',
});
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

async function loadTokens(options: { waitForAuth?: boolean } = {}): Promise<void> {
  const auth = useAuthStore();
  if (!auth.isAuthenticated) {
    if (!options.waitForAuth) {
      tokensLoading.value = false;
      return;
    }
    await auth.waitForAuthentication();
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
  const q = (tableControls.filter || '').toLowerCase();
  let out = tokens.value.filter((t) => {
    if (!q) return true;
    if (t.hash.toLowerCase().includes(q)) return true;
    return t.scopes.some((s) => s.path.toLowerCase().includes(q));
  });
  if (tableControls.sortBy === 'created') {
    out = out.slice().sort((a, b) => {
      const ta = a.createdAt ? Date.parse(String(a.createdAt)) : 0;
      const tb = b.createdAt ? Date.parse(String(b.createdAt)) : 0;
      return tb - ta; // newest first
    });
  } else if (tableControls.sortBy === 'path') {
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
    loadTokens({ waitForAuth: true });
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
.token-stack {
  gap: 1.5rem !important;
}

.token-hero {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
  border-radius: 1rem;
  padding: 1.1rem 1.25rem;
  border: 1px solid rgb(var(--color-dark) / 0.1);
  background:
    radial-gradient(circle at top right, rgb(var(--color-primary) / 0.12), transparent 42%),
    linear-gradient(135deg, rgb(var(--color-light) / 0.88), rgb(var(--color-surface) / 0.82));
}

.dark .token-hero {
  border-color: rgb(var(--color-light) / 0.16);
  background:
    radial-gradient(circle at top right, rgb(var(--color-primary) / 0.2), transparent 42%),
    linear-gradient(135deg, rgb(var(--color-surface) / 0.9), rgb(var(--color-dark) / 0.88));
}

.token-hero__copy {
  min-width: 16rem;
  flex: 1;
}

.token-hero__title {
  display: inline-flex;
  align-items: center;
  gap: 0.625rem;
  margin: 0;
  font-size: 1.1rem;
  font-weight: 700;
}

.token-hero__subtitle {
  margin: 0.45rem 0 0;
  opacity: 0.78;
  max-width: 52rem;
}

.token-hero__tag {
  margin-top: 0.1rem;
}

.token-panel {
  border-radius: 1rem;
  border: 1px solid rgb(var(--color-dark) / 0.1);
  background: rgb(var(--color-light) / 0.82);
  backdrop-filter: blur(6px);
}

.dark .token-panel {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-surface) / 0.74);
}

.token-panel__heading {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem;
}

.token-panel__title {
  margin-right: auto;
}

.token-panel__action {
  min-width: 8.5rem;
}

.token-panel__body {
  gap: 1rem !important;
}

.token-form-shell {
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.9rem;
  background: rgb(var(--color-light) / 0.46);
  padding: 0.9rem;
}

.dark .token-form-shell {
  border-color: rgb(var(--color-light) / 0.12);
  background: rgb(var(--color-dark) / 0.3);
}

.token-scope-list {
  gap: 0.55rem !important;
}

.token-scope-grid {
  display: grid;
  gap: 0.65rem;
}

.token-scope-card {
  border: 1px solid rgb(var(--color-dark) / 0.1);
  border-radius: 0.8rem;
  background: rgb(var(--color-light) / 0.62);
  padding: 0.65rem 0.75rem;
}

.dark .token-scope-card {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-dark) / 0.22);
}

.token-scope-card__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem;
  margin-bottom: 0.45rem;
}

.token-generate-row {
  justify-content: flex-start;
}

.token-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 1rem;
}

.token-toolbar :deep(.n-form-item) {
  margin-bottom: 0;
}

.token-records {
  display: grid;
  gap: 0.75rem;
}

.token-record {
  border: 1px solid rgb(var(--color-dark) / 0.1);
  border-radius: 0.9rem;
  background: rgb(var(--color-light) / 0.55);
  padding: 0.85rem;
}

.dark .token-record {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-dark) / 0.26);
}

.token-record__header {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.75rem;
  margin-bottom: 0.75rem;
}

.token-record__date {
  margin-left: auto;
}

.token-hash-button {
  border: 1px solid rgb(var(--color-dark) / 0.16);
  border-radius: 0.7rem;
  background: rgb(var(--color-light) / 0.72);
  color: inherit;
  padding: 0.4rem 0.6rem;
  text-align: left;
  cursor: pointer;
}

.dark .token-hash-button {
  border-color: rgb(var(--color-light) / 0.18);
  background: rgb(var(--color-dark) / 0.4);
}

.token-hash-button:hover {
  border-color: rgb(var(--color-primary) / 0.55);
}

.token-hash-button__label {
  display: block;
  font-size: 0.67rem;
  line-height: 1;
  opacity: 0.68;
  text-transform: uppercase;
  letter-spacing: 0.04em;
  margin-bottom: 0.2rem;
}

.token-hash-button__value {
  display: block;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 0.79rem;
}

.token-record__scopes {
  display: grid;
  gap: 0.55rem;
}

.token-record-scope {
  border: 1px solid rgb(var(--color-dark) / 0.08);
  border-radius: 0.75rem;
  background: rgb(var(--color-light) / 0.5);
  padding: 0.55rem 0.65rem;
}

.dark .token-record-scope {
  border-color: rgb(var(--color-light) / 0.12);
  background: rgb(var(--color-dark) / 0.28);
}

.token-result-shell {
  border: 1px solid rgb(var(--color-dark) / 0.11);
  border-radius: 0.8rem;
  padding: 0.35rem;
  background: rgb(var(--color-light) / 0.58);
}

.dark .token-result-shell {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-dark) / 0.35);
}

.token-modal-card {
  border-radius: 1rem;
  border: 1px solid rgb(var(--color-dark) / 0.12);
}

.dark .token-modal-card {
  border-color: rgb(var(--color-light) / 0.14);
}

.api-token-manager :deep(.n-button .n-button__content) {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 0.5rem;
}

.api-token-manager :deep(.n-button .n-icon) {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  line-height: 1;
}

.api-token-manager :deep(.n-button .n-icon .icon),
.api-token-manager :deep(.n-button .n-icon .fa),
.api-token-manager :deep(.n-button .n-icon .fas),
.api-token-manager :deep(.n-button .n-icon .far),
.api-token-manager :deep(.n-button .n-icon .fal),
.api-token-manager :deep(.n-button .n-icon .fab) {
  margin: 0 !important;
  vertical-align: middle !important;
  line-height: 1 !important;
}

.token-hero__title :deep(.n-icon),
.token-panel__title :deep(.n-icon),
.token-hero__tag :deep(.n-icon) {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  line-height: 1;
}

.api-token-manager :deep(.n-alert) {
  border-radius: 0.8rem;
  border: 1px solid rgb(var(--color-dark) / 0.12);
}

.dark .api-token-manager :deep(.n-alert) {
  border-color: rgb(var(--color-light) / 0.14);
}

.api-token-manager :deep(.n-alert.n-alert--info-type.n-alert--secondary),
.api-token-manager :deep(.n-alert.n-alert--warning-type.n-alert--secondary) {
  background: rgb(var(--color-light) / 0.52);
}

.dark .api-token-manager :deep(.n-alert.n-alert--info-type.n-alert--secondary),
.dark .api-token-manager :deep(.n-alert.n-alert--warning-type.n-alert--secondary) {
  background: rgb(var(--color-dark) / 0.32);
}

.api-token-manager :deep(.n-input .n-input-wrapper),
.api-token-manager :deep(.n-base-selection) {
  border-radius: 0.75rem !important;
  border-color: rgb(var(--color-dark) / 0.18) !important;
  background: rgb(var(--color-light) / 0.62) !important;
}

.dark .api-token-manager :deep(.n-input .n-input-wrapper),
.dark .api-token-manager :deep(.n-base-selection) {
  border-color: rgb(var(--color-light) / 0.18) !important;
  background: rgb(var(--color-dark) / 0.36) !important;
}

.api-token-manager :deep(.n-base-selection .n-base-selection-label) {
  border-radius: 0.75rem !important;
  background: transparent !important;
}

.api-token-manager :deep(.n-code) {
  border-radius: 0.75rem;
  border: 1px solid rgb(var(--color-dark) / 0.12);
  background: rgb(var(--color-light) / 0.62);
}

.dark .api-token-manager :deep(.n-code) {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-dark) / 0.36);
}

.api-token-manager :deep(.n-empty) {
  border-radius: 0.85rem;
  border: 1px solid rgb(var(--color-dark) / 0.1);
  background: rgb(var(--color-light) / 0.45);
  padding: 1rem;
}

.dark .api-token-manager :deep(.n-empty) {
  border-color: rgb(var(--color-light) / 0.14);
  background: rgb(var(--color-dark) / 0.25);
}

@media (min-width: 960px) {
  .token-scope-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .token-record__scopes {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (max-width: 640px) {
  .token-hero {
    padding: 1rem;
  }

  .token-record__date {
    margin-left: 0;
    width: 100%;
  }

  .token-panel__action {
    width: 100%;
  }
}
</style>
