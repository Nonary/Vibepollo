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
          {{ t('auth.least_privilege_hint') }}
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
              <n-form-item-gi :span="24" :s="12" :label="t('auth.select_api_path')" path="path">
                <n-select
                  v-model:value="draft.path"
                  :loading="routeCatalogLoading"
                  :options="routeSelectOptions"
                  :placeholder="t('auth.select_api_path')"
                />
              </n-form-item-gi>
              <n-form-item-gi :span="24" :s="8" :label="t('auth.methods')" path="selectedMethods">
                <n-checkbox-group v-model:value="draft.selectedMethods">
                  <n-space wrap>
                    <n-checkbox v-for="m in draftMethods" :key="m" :value="m">
                      <n-text code>{{ m }}</n-text>
                    </n-checkbox>
                  </n-space>
                </n-checkbox-group>
                <n-text v-if="draft.path && draftMethods.length === 0" size="small" depth="3">
                  {{ t('auth.no_methods_for_route') }}
                </n-text>
              </n-form-item-gi>
              <n-form-item-gi :span="24" :s="4" label=" " :show-feedback="false">
                <n-button
                  type="primary"
                  size="medium"
                  class="w-full"
                  :disabled="!canAddScope"
                  @click="addScope"
                >
                  <n-icon size="16"><i class="fas fa-plus" /></n-icon>
                  {{ t('auth.add_scope') }}
                </n-button>
              </n-form-item-gi>
            </n-grid>
          </n-form>

          <n-space v-if="scopes.length" vertical size="small" class="token-scope-list">
            <n-text depth="3" strong>{{ t('auth.selected_scopes') }}</n-text>
            <div class="token-scope-grid">
              <div
                v-for="(scope, idx) in scopes"
                :key="idx + ':' + scope.path"
                class="token-scope-card"
              >
                <div class="token-scope-card__header">
                  <n-text strong>{{ scope.path }}</n-text>
                  <n-button type="error" strong size="small" text @click="removeScope(idx)">
                    <n-icon size="14"><i class="fas fa-times" /></n-icon>
                    {{ t('auth.remove') }}
                  </n-button>
                </div>
                <n-space wrap size="small">
                  <n-tag
                    v-for="method in scope.methods"
                    :key="method"
                    type="info"
                    size="small"
                    round
                  >
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
            <n-text v-if="creating" size="small" depth="3">{{ t('auth.creating') }}</n-text>
          </n-space>

          <n-text v-if="!canGenerate" size="small" depth="3">{{
            t('auth.generate_disabled_hint')
          }}</n-text>

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
              {{ t('auth.token_modal_warning') }}
            </n-alert>
            <n-space vertical size="small">
              <n-text depth="3" strong>{{ t('auth.token') }}</n-text>
              <n-code :code="createdToken" language="bash" word-wrap />
              <n-space align="center" size="small">
                <n-button size="small" type="primary" @click="copy(createdToken)">
                  <n-icon size="14"><i class="fas fa-copy" /></n-icon>
                  {{ t('auth.copy_token') }}
                </n-button>
                <n-tag v-if="copied" type="success" size="small" round>{{
                  t('auth.token_copied')
                }}</n-tag>
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
              <n-text strong>{{ t('auth.active_tokens') }}</n-text>
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
            <n-form-item :label="t('auth.sort_field')" path="sortBy">
              <n-select v-model:value="tableControls.sortBy" :options="sortOptions" />
            </n-form-item>
          </n-form>

          <n-alert v-if="tokensError" type="error" closable @close="tokensError = ''">
            {{ tokensError }}
          </n-alert>

          <n-empty
            v-if="filteredTokens.length === 0 && !tokensLoading"
            :description="t('auth.no_active_tokens')"
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
                    <n-tag
                      v-for="method in scope.methods"
                      :key="method"
                      size="small"
                      type="info"
                      round
                    >
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
            <n-text strong>{{ t('auth.test_api_token') }}</n-text>
          </n-space>
        </template>
        <n-space vertical size="large" class="token-panel__body">
          <n-alert type="info" secondary>
            {{ t('auth.testing_help') }}
          </n-alert>

          <n-form :model="test" label-placement="top" size="medium">
            <n-grid cols="24" x-gap="12" y-gap="12" responsive="screen">
              <n-form-item-gi :span="24" :s="12" :label="t('auth.token')" path="token">
                <n-input
                  v-model:value="test.token"
                  :placeholder="t('auth.paste_token_here')"
                  type="password"
                />
              </n-form-item-gi>
              <n-form-item-gi :span="24" :s="12" :label="t('auth.api_path_get_only')" path="path">
                <n-select
                  v-model:value="test.path"
                  :options="getRouteOptions"
                  :placeholder="t('auth.select_api_path_to_test')"
                />
              </n-form-item-gi>
              <n-form-item-gi :span="24" :s="12" :label="t('auth.test_query')" path="query">
                <n-input
                  v-model:value="test.query"
                  :placeholder="t('auth.test_query_placeholder')"
                />
              </n-form-item-gi>
            </n-grid>
          </n-form>

          <n-space align="center" size="small">
            <n-button type="primary" :disabled="!canSendTest" :loading="testing" @click="sendTest">
              <n-icon size="16"><i class="fas fa-paper-plane" /></n-icon>
              {{ t('auth.test_token') }}
            </n-button>
            <n-text v-if="testing" size="small" depth="3">{{ t('auth.sending') }}</n-text>
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
                t('auth.confirm_revoke_message_hash', {
                  hash: shortHash(pendingRevoke?.hash || ''),
                })
              }}
            </n-text>
          </n-space>
          <template #footer>
            <n-space justify="end" size="small">
              <n-button type="default" strong @click="showRevoke = false">{{
                t('_common.cancel')
              }}</n-button>
              <n-button type="error" strong @click="confirmRevoke">{{ t('auth.revoke') }}</n-button>
            </n-space>
          </template>
        </n-card>
      </n-modal>
    </n-space>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
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

const METHOD_ORDER = ['GET', 'POST', 'PUT', 'PATCH', 'DELETE'] as const;

const { t } = useI18n();
const message = useMessage();
const authStore = useAuthStore();

const routeCatalog = ref<RouteDef[]>([]);
const routeCatalogLoading = ref(false);
const routeCatalogError = ref('');

const routeSelectOptions = computed(() =>
  routeCatalog.value.map((route) => ({ label: route.path, value: route.path })),
);
const getRouteOptions = computed(() =>
  routeCatalog.value
    .filter((route) => route.methods.includes('GET'))
    .map((route) => ({ label: route.path, value: route.path })),
);

const sortOptions = computed(() => [
  { label: t('auth.sort_newest'), value: 'created' },
  { label: t('auth.sort_path'), value: 'path' },
]);

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

const _aborts = new Set<AbortController>();
function makeAbortController() {
  const ac = new AbortController();
  _aborts.add(ac);
  return ac;
}
function releaseAbortController(ac: AbortController) {
  _aborts.delete(ac);
}

function orderMethods(methods: string[]): string[] {
  const normalized = Array.from(
    new Set(
      methods
        .map((method) =>
          String(method || '')
            .trim()
            .toUpperCase(),
        )
        .filter((method) => method.length > 0),
    ),
  );

  const preferred: string[] = [];
  for (const method of METHOD_ORDER) {
    if (normalized.includes(method)) {
      preferred.push(method);
    }
  }

  const extra = normalized
    .filter((method) => !METHOD_ORDER.includes(method as (typeof METHOD_ORDER)[number]))
    .sort((a, b) => a.localeCompare(b));

  return [...preferred, ...extra];
}

function normalizeRouteDef(route: any): RouteDef | null {
  const path = typeof route?.path === 'string' ? route.path.trim() : '';
  if (!path) {
    return null;
  }
  const methods = Array.isArray(route?.methods) ? orderMethods(route.methods) : [];
  return { path, methods };
}

function withDetail(prefix: string, detail: string): string {
  if (!detail) {
    return prefix;
  }
  return `${prefix}: ${detail}`;
}

async function loadRouteCatalog(): Promise<void> {
  if (!authStore.isAuthenticated) {
    routeCatalogLoading.value = false;
    return;
  }

  routeCatalogLoading.value = true;
  routeCatalogError.value = '';
  let ac: AbortController | null = null;

  try {
    ac = makeAbortController();
    const res = await http.get('/api/token/routes', {
      validateStatus: () => true,
      signal: ac.signal,
    });

    if (res.status >= 200 && res.status < 300) {
      const raw = Array.isArray(res.data) ? res.data : res.data?.routes;
      if (!Array.isArray(raw)) {
        routeCatalog.value = [];
        return;
      }

      routeCatalog.value = raw
        .map((entry: any) => normalizeRouteDef(entry))
        .filter((entry): entry is RouteDef => !!entry)
        .sort((a, b) => a.path.localeCompare(b.path));
    } else {
      const msg = String(
        (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`,
      );
      routeCatalogError.value = withDetail(t('auth.routes_load_failed'), msg);
      routeCatalog.value = [];
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED') {
      routeCatalogError.value = withDetail(
        t('auth.routes_load_failed'),
        e?.message || t('auth.request_failed'),
      );
      routeCatalog.value = [];
    }
  } finally {
    if (ac) {
      releaseAbortController(ac);
    }
    routeCatalogLoading.value = false;
  }
}

const draftMethods = computed<string[]>(
  () => routeCatalog.value.find((route) => route.path === draft.path)?.methods || [],
);

const canAddScope = computed(
  () => !routeCatalogLoading.value && !!draft.path && draft.selectedMethods.length > 0,
);

const canGenerate = computed(
  () => getEffectiveScopes().length > 0 && routeCatalog.value.length > 0,
);

function addScope(): void {
  if (!canAddScope.value) {
    return;
  }

  const methods = orderMethods(draft.selectedMethods);
  const existingIdx = scopes.value.findIndex((scope) => scope.path === draft.path);

  if (existingIdx !== -1) {
    const current = scopes.value[existingIdx];
    scopes.value[existingIdx] = {
      path: draft.path,
      methods: orderMethods([...(current?.methods ?? []), ...methods]),
    };
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
  const effective = scopes.value.slice();

  if (draft.path && draft.selectedMethods.length > 0) {
    const methods = orderMethods(draft.selectedMethods);
    const idx = effective.findIndex((scope) => scope.path === draft.path);

    if (idx !== -1) {
      const current = effective[idx];
      effective[idx] = {
        path: draft.path,
        methods: orderMethods([...(current?.methods ?? []), ...methods]),
      };
    } else {
      effective.push({ path: draft.path, methods });
    }
  }

  return effective;
}

const lastCreatedScopes = ref<Scope[]>([]);

async function createToken(): Promise<void> {
  createError.value = '';
  createdToken.value = '';
  copied.value = false;

  const nextScopes = getEffectiveScopes();
  if (nextScopes.length === 0) {
    createError.value = t('auth.please_specify_scope');
    return;
  }

  creating.value = true;
  let ac: AbortController | null = null;

  try {
    lastCreatedScopes.value = nextScopes.slice();
    ac = makeAbortController();

    const res = await http.post(
      '/api/token',
      { scopes: nextScopes },
      { validateStatus: () => true, signal: ac.signal },
    );

    if (res.status >= 200 && res.status < 300) {
      const token = (res.data && (res.data.token || res.data.value || res.data)) as string;
      if (typeof token === 'string' && token.length > 0) {
        createdToken.value = token;
        await loadTokens();
        showTokenModal.value = true;
      } else {
        createError.value = withDetail(
          t('auth.failed_to_generate_token'),
          t('auth.request_failed'),
        );
      }
    } else {
      const msg = String(
        (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`,
      );
      createError.value = withDetail(t('auth.failed_to_generate_token'), msg);
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED') {
      createError.value = withDetail(
        t('auth.failed_to_generate_token'),
        e?.message || t('auth.request_failed'),
      );
    }
  } finally {
    if (ac) {
      releaseAbortController(ac);
    }
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
      // noop
    }
  }
}

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
  if (!rec) {
    return null;
  }

  const scopes: Scope[] = Array.isArray(rec.scopes)
    ? rec.scopes.map((scope: any) => ({
        path: scope.path || scope.route || '',
        methods: orderMethods(scope.methods || scope.verbs || []),
      }))
    : [];

  const hash: string = rec.hash ?? rec.id ?? rec.token_hash ?? '';
  const createdAt = rec.createdAt ?? rec.created_at ?? rec.created ?? null;
  if (!hash) {
    return null;
  }

  return { hash, scopes, createdAt };
}

async function loadTokens(): Promise<void> {
  if (!authStore.isAuthenticated) {
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
      tokens.value = (list as any[])
        .map((entry) => normalizeToken(entry))
        .filter(Boolean) as TokenRecord[];
    } else {
      const msg = String(
        (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`,
      );
      tokensError.value = withDetail(t('auth.request_failed'), msg);
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED') {
      tokensError.value = withDetail(
        t('auth.request_failed'),
        e?.message || t('auth.request_failed'),
      );
    }
  } finally {
    if (ac) {
      releaseAbortController(ac);
    }
    tokensLoading.value = false;
  }
}

function promptRevoke(token: TokenRecord): void {
  pendingRevoke.value = token;
  showRevoke.value = true;
}

async function confirmRevoke(): Promise<void> {
  const token = pendingRevoke.value;
  if (!token?.hash) {
    return;
  }

  revoking.value = token.hash;
  let ac: AbortController | null = null;

  try {
    const url = `/api/token/${encodeURIComponent(token.hash)}`;
    ac = makeAbortController();

    const res = await http.delete(url, { validateStatus: () => true, signal: ac.signal });
    if (res.status >= 200 && res.status < 300) {
      tokens.value = tokens.value.filter((entry) => entry.hash !== token.hash);
      showRevoke.value = false;
      pendingRevoke.value = null;
    } else {
      const msg = String(
        (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`,
      );
      message.error(withDetail(t('auth.failed_to_revoke_token'), msg));
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED') {
      message.error(
        withDetail(t('auth.failed_to_revoke_token'), e?.message || t('auth.request_failed')),
      );
    }
  } finally {
    if (ac) {
      releaseAbortController(ac);
    }
    revoking.value = '';
  }
}

const filteredTokens = computed<TokenRecord[]>(() => {
  const query = (tableControls.filter || '').toLowerCase();

  let out = tokens.value.filter((token) => {
    if (!query) {
      return true;
    }
    if (token.hash.toLowerCase().includes(query)) {
      return true;
    }
    return token.scopes.some((scope) => scope.path.toLowerCase().includes(query));
  });

  if (tableControls.sortBy === 'created') {
    out = out.slice().sort((a, b) => {
      const ta = a.createdAt ? Date.parse(String(a.createdAt)) : 0;
      const tb = b.createdAt ? Date.parse(String(b.createdAt)) : 0;
      return tb - ta;
    });
  } else {
    const firstPath = (token: TokenRecord) =>
      token.scopes.map((scope) => scope.path).sort()[0] || '';
    out = out.slice().sort((a, b) => firstPath(a).localeCompare(firstPath(b)));
  }

  return out;
});

function shortHash(hash: string): string {
  if (!hash) {
    return '';
  }
  if (hash.length <= 10) {
    return hash;
  }
  return `${hash.slice(0, 6)}…${hash.slice(-4)}`;
}

function formatTime(rawValue: any): string {
  try {
    let value = rawValue;
    if (typeof rawValue === 'string' && /^\d+$/.test(rawValue)) {
      value = Number(rawValue);
    }

    let date: Date;
    if (typeof value === 'number') {
      const millis = value > 0 && value < 1e12 ? value * 1000 : value;
      date = new Date(millis);
    } else {
      date = new Date(String(value));
    }

    if (isNaN(date.getTime())) {
      return '—';
    }

    return date.toLocaleString();
  } catch {
    return '—';
  }
}

const test = reactive<{
  token: string;
  path: string;
  query: string;
}>({ token: '', path: '', query: '' });

const testing = ref(false);
const testResponse = ref('');
const testError = ref('');
const canSendTest = computed(() => !!test.token && !!test.path);

function firstGetScopePath(inputScopes: Scope[]): string {
  try {
    for (const scope of inputScopes || []) {
      if ((scope.methods || []).includes('GET')) {
        return scope.path;
      }
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
    const query = (test.query || '').trim();
    const url = query ? `${urlBase}?${query}` : urlBase;
    const headers: Record<string, string> = {
      Authorization: `Bearer ${test.token}`,
      'X-Requested-With': 'XMLHttpRequest',
    };

    ac = makeAbortController();
    const res = await http.get(url, { headers, validateStatus: () => true, signal: ac.signal });
    const pretty = prettyPrint(res.data);
    testResponse.value = `${res.status} ${res.statusText || ''}\n\n${pretty}`;
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED') {
      testError.value = withDetail(
        t('auth.request_failed'),
        e?.message || t('auth.request_failed'),
      );
    }
  } finally {
    if (ac) {
      releaseAbortController(ac);
    }
    testing.value = false;
  }
}

function prettyPrint(data: any): string {
  try {
    if (typeof data === 'string') {
      try {
        const parsed = JSON.parse(data);
        return JSON.stringify(parsed, null, 2);
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
  const start = () => {
    void loadRouteCatalog();
    void loadTokens();
  };

  if (authStore.isAuthenticated) {
    start();
  } else {
    const off = authStore.onLogin(() => {
      try {
        start();
      } finally {
        off?.();
      }
    });
  }
});

onBeforeUnmount(() => {
  _aborts.forEach((ac) => {
    try {
      ac.abort();
    } catch {
      // noop
    }
  });
  _aborts.clear();
});

watch(
  () => createdToken.value,
  (tokenValue) => {
    if (!tokenValue) {
      return;
    }

    test.token = tokenValue;
    if (!test.path) {
      const first = firstGetScopePath(lastCreatedScopes.value);
      if (first) {
        test.path = first;
      }
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
