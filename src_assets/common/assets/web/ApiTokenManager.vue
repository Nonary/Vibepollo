<template>
  <n-space vertical size="large" class="w-full">
    <n-page-header :title="t('auth.title')" :subtitle="t('auth.subtitle')">
      <template #extra>
        <n-tag round type="info" size="small" class="inline-flex items-center gap-2">
          <n-icon size="14"><i class="fas fa-shield-halved" /></n-icon>
          {{ t('auth.least_privilege_hint') }}
        </n-tag>
      </template>
    </n-page-header>

    <n-card size="large">
      <template #header>
        <n-space align="center" justify="space-between">
          <n-space align="center" size="small">
            <n-icon size="18"><i class="fas fa-key" /></n-icon>
            <n-text strong>{{ t('auth.generate_new_token') }}</n-text>
          </n-space>
          <n-button
            type="primary"
            strong
            size="small"
            :loading="routeCatalogLoading"
            @click="loadRouteCatalog"
          >
            <n-icon class="mr-1" size="14"><i class="fas fa-rotate" /></n-icon>
            {{ t('auth.refresh_routes') }}
          </n-button>
        </n-space>
      </template>

      <n-space vertical size="large">
        <n-text depth="3">{{ t('auth.generate_token_help') }}</n-text>

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

        <n-form :model="draft" label-placement="top" size="medium">
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
              <n-button type="primary" size="medium" :disabled="!canAddScope" @click="addScope">
                <n-icon class="mr-1" size="16"><i class="fas fa-plus" /></n-icon>
                {{ t('auth.add_scope') }}
              </n-button>
            </n-form-item-gi>
          </n-grid>
        </n-form>

        <n-space v-if="scopes.length" vertical size="small">
          <n-text depth="3" strong>{{ t('auth.selected_scopes') }}</n-text>
          <n-space vertical size="small">
            <n-thing v-for="(scope, idx) in scopes" :key="idx + ':' + scope.path" :title="scope.path">
              <template #header-extra>
                <n-button type="error" strong size="small" text @click="removeScope(idx)">
                  <n-icon size="14"><i class="fas fa-times" /></n-icon>
                  {{ t('auth.remove') }}
                </n-button>
              </template>
              <template #description>
                <n-space wrap size="small">
                  <n-tag v-for="method in scope.methods" :key="method" type="info" size="small" round>
                    {{ method }}
                  </n-tag>
                </n-space>
              </template>
            </n-thing>
          </n-space>
        </n-space>

        <n-space align="center" size="small">
          <n-button
            type="success"
            size="medium"
            :disabled="!canGenerate || creating"
            :loading="creating"
            @click="createToken"
          >
            <n-icon class="mr-1" size="16"><i class="fas fa-key" /></n-icon>
            {{ t('auth.generate_token') }}
          </n-button>
          <n-text v-if="creating" size="small" depth="3">{{ t('auth.creating') }}</n-text>
        </n-space>

        <n-text v-if="!canGenerate" size="small" depth="3">{{ t('auth.generate_disabled_hint') }}</n-text>

        <n-alert v-if="createError" type="error" closable @close="createError = ''">
          {{ createError }}
        </n-alert>
      </n-space>
    </n-card>

    <n-modal :show="showTokenModal" @update:show="(v) => (showTokenModal = v)">
      <n-card :title="t('auth.token_created_title')" :bordered="false" style="max-width: 40rem; width: 100%">
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
                <n-icon class="mr-1" size="14"><i class="fas fa-copy" /></n-icon>
                {{ t('auth.copy_token') }}
              </n-button>
              <n-tag v-if="copied" type="success" size="small" round>{{ t('auth.token_copied') }}</n-tag>
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

    <n-card size="large">
      <template #header>
        <n-space align="center" justify="space-between">
          <n-space align="center" size="small">
            <n-icon size="18"><i class="fas fa-lock" /></n-icon>
            <n-text strong>{{ t('auth.active_tokens') }}</n-text>
          </n-space>
          <n-button
            type="primary"
            strong
            size="small"
            :loading="tokensLoading"
            :aria-label="t('auth.refresh')"
            @click="loadTokens"
          >
            <n-icon class="mr-1" size="14"><i class="fas fa-rotate" /></n-icon>
            {{ t('auth.refresh') }}
          </n-button>
        </n-space>
      </template>

      <n-space vertical size="large">
        <n-form :model="tableControls" inline label-placement="top" class="flex flex-wrap gap-4">
          <n-form-item :label="t('auth.filter')" path="filter">
            <n-input v-model:value="tableControls.filter" :placeholder="t('auth.search_tokens')" />
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

        <div v-else class="overflow-auto">
          <n-table :bordered="false" :single-line="false" class="min-w-[640px]">
            <thead>
              <tr>
                <th class="w-40">{{ t('auth.hash') }}</th>
                <th>{{ t('auth.scopes') }}</th>
                <th class="w-28">{{ t('auth.created') }}</th>
                <th class="w-24"></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="token in filteredTokens" :key="token.hash">
                <td class="align-middle">
                  <n-text
                    class="font-mono text-xs inline-block min-w-[70px] cursor-pointer"
                    @click="copy(token.hash)"
                    @keydown.enter.prevent="copy(token.hash)"
                    tabindex="0"
                  >
                    {{ shortHash(token.hash) }}
                  </n-text>
                </td>
                <td class="align-middle">
                  <n-space wrap size="small">
                    <n-card
                      v-for="(scope, idx) in token.scopes"
                      :key="idx"
                      size="small"
                      bordered
                      class="bg-transparent min-w-[160px]"
                    >
                      <n-space vertical size="small">
                        <n-text strong>{{ scope.path }}</n-text>
                        <n-space wrap size="small">
                          <n-tag v-for="method in scope.methods" :key="method" size="small" type="info" round>
                            {{ method }}
                          </n-tag>
                        </n-space>
                      </n-space>
                    </n-card>
                  </n-space>
                </td>
                <td class="text-xs opacity-70 align-middle">
                  {{ token.createdAt ? formatTime(token.createdAt) : '—' }}
                </td>
                <td class="align-middle">
                  <n-button
                    size="small"
                    type="error"
                    :loading="revoking === token.hash"
                    @click="promptRevoke(token)"
                  >
                    <n-icon class="mr-1" size="14"><i class="fas fa-ban" /></n-icon>
                    {{ t('auth.revoke') }}
                  </n-button>
                </td>
              </tr>
            </tbody>
          </n-table>
        </div>
      </n-space>
    </n-card>

    <n-card size="large">
      <template #header>
        <n-space align="center" size="small">
          <n-icon size="18"><i class="fas fa-vial" /></n-icon>
          <n-text strong>{{ t('auth.test_api_token') }}</n-text>
        </n-space>
      </template>
      <n-space vertical size="large">
        <n-alert type="info" secondary>
          {{ t('auth.testing_help') }}
        </n-alert>

        <n-form :model="test" label-placement="top" size="medium">
          <n-grid cols="24" x-gap="12" y-gap="12" responsive="screen">
            <n-form-item-gi :span="24" :s="12" :label="t('auth.token')" path="token">
              <n-input v-model:value="test.token" :placeholder="t('auth.paste_token_here')" type="password" />
            </n-form-item-gi>
            <n-form-item-gi :span="24" :s="12" :label="t('auth.api_path_get_only')" path="path">
              <n-select
                v-model:value="test.path"
                :options="getRouteOptions"
                :placeholder="t('auth.select_api_path_to_test')"
              />
            </n-form-item-gi>
            <n-form-item-gi :span="24" :s="12" :label="t('auth.test_query')" path="query">
              <n-input v-model:value="test.query" :placeholder="t('auth.test_query_placeholder')" />
            </n-form-item-gi>
          </n-grid>
        </n-form>

        <n-space align="center" size="small">
          <n-button type="primary" :disabled="!canSendTest" :loading="testing" @click="sendTest">
            <n-icon class="mr-1" size="16"><i class="fas fa-paper-plane" /></n-icon>
            {{ t('auth.test_token') }}
          </n-button>
          <n-text v-if="testing" size="small" depth="3">{{ t('auth.sending') }}</n-text>
        </n-space>

        <n-alert v-if="testError" type="error" closable @close="testError = ''">
          {{ testError }}
        </n-alert>

        <n-space v-if="testResponse" vertical size="small">
          <n-text depth="3" strong>{{ t('auth.result') }}</n-text>
          <n-scrollbar style="max-height: 60vh">
            <n-code :code="testResponse" language="json" word-wrap />
          </n-scrollbar>
        </n-space>
      </n-space>
    </n-card>

    <n-modal :show="showRevoke" @update:show="(v) => (showRevoke = v)">
      <n-card
        :title="t('auth.confirm_revoke_title')"
        :bordered="false"
        style="max-width: 32rem; width: 100%"
      >
        <n-space vertical align="center" size="medium">
          <n-text>
            {{
              t('auth.confirm_revoke_message_hash', { hash: shortHash(pendingRevoke?.hash || '') })
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
  NPageHeader,
  NScrollbar,
  NSelect,
  NSpace,
  NTable,
  NTag,
  NText,
  NThing,
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
        .map((method) => String(method || '').trim().toUpperCase())
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
    const res = await http.get('/api/token/routes', { validateStatus: () => true, signal: ac.signal });

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
      const msg = String((res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`);
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
        createError.value = withDetail(t('auth.failed_to_generate_token'), t('auth.request_failed'));
      }
    } else {
      const msg = String((res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`);
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
      tokens.value = (list as any[]).map((entry) => normalizeToken(entry)).filter(Boolean) as TokenRecord[];
    } else {
      const msg = String((res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`);
      tokensError.value = withDetail(t('auth.request_failed'), msg);
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED') {
      tokensError.value = withDetail(t('auth.request_failed'), e?.message || t('auth.request_failed'));
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
      const msg = String((res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`);
      message.error(withDetail(t('auth.failed_to_revoke_token'), msg));
    }
  } catch (e: any) {
    if (e?.code !== 'ERR_CANCELED') {
      message.error(withDetail(t('auth.failed_to_revoke_token'), e?.message || t('auth.request_failed')));
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
    const firstPath = (token: TokenRecord) => token.scopes.map((scope) => scope.path).sort()[0] || '';
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
      testError.value = withDetail(t('auth.request_failed'), e?.message || t('auth.request_failed'));
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
