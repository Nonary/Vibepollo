<template>
  <div class="px-4">
    <h1 class="text-2xl font-semibold my-6 flex items-center gap-3 text-brand">
      <i class="fas fa-users-cog" /> {{ $t('clients.title') }}
    </h1>

    <!-- Pair New Client -->
    <n-card class="mb-8" :segmented="{ content: true, footer: true }">
      <template #header>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-link" /> {{ $t('clients.pair_title') }}
        </h2>
      </template>
      <div class="space-y-4">
        <p class="text-sm opacity-75">{{ $t('clients.pair_desc') }}</p>
        <n-form
          class="grid grid-cols-1 md:grid-cols-3 gap-4 items-end"
          @submit.prevent="registerDevice"
        >
          <n-form-item class="flex flex-col" :label="$t('navbar.pin')" label-placement="top">
            <n-input
              id="pin-input"
              v-model:value="pin"
              :placeholder="$t('navbar.pin')"
              :input-props="{
                inputmode: 'numeric',
                pattern: '^[0-9]{4}$',
                title: 'Enter 4 digits',
                maxlength: 4,
                required: true,
              }"
            />
          </n-form-item>
          <n-form-item class="flex flex-col" :label="$t('pin.device_name')" label-placement="top">
            <n-input
              id="name-input"
              v-model:value="deviceName"
              :placeholder="$t('pin.device_name')"
              :input-props="{ required: true }"
            />
          </n-form-item>
          <n-form-item class="flex flex-col md:items-end">
            <n-button
              :disabled="pairing"
              class="w-full md:w-auto"
              type="primary"
              html-type="submit"
            >
              <span v-if="!pairing">{{ $t('pin.send') }}</span>
              <span v-else>{{ $t('clients.pairing') }}</span>
            </n-button>
          </n-form-item>
        </n-form>
        <div class="space-y-2">
          <n-alert v-if="pairStatus === true" type="success">{{ $t('pin.pair_success') }}</n-alert>
          <n-alert v-if="pairStatus === false" type="error">{{ $t('pin.pair_failure') }}</n-alert>
        </div>
        <n-alert type="warning" :title="$t('_common.warning')" class="text-sm">
          {{ $t('pin.warning_msg') }}
        </n-alert>
      </div>
    </n-card>

    <!-- Existing Clients -->
    <n-card class="mb-8" :segmented="{ content: true, footer: true }">
      <template #header>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-users" /> {{ $t('clients.existing_title') }}
        </h2>
      </template>
      <div class="flex items-center">
        <n-button
          class="ml-auto"
          secondary
          :disabled="unpairAllPressed || clients.length === 0"
          @click="askConfirmUnpairAll"
        >
          <i class="fas fa-user-slash" />
          {{ $t('troubleshooting.unpair_all') }}
        </n-button>
      </div>
      <p class="text-sm opacity-75 mb-3">{{ $t('troubleshooting.unpair_desc') }}</p>
      <n-alert v-if="unpairAllStatus === true" type="success" class="mb-3">{{
        $t('troubleshooting.unpair_all_success')
      }}</n-alert>
      <n-alert v-if="unpairAllStatus === false" type="error" class="mb-3">{{
        $t('troubleshooting.unpair_all_error')
      }}</n-alert>
      <ul v-if="clients && clients.length > 0" class="divide-y divide-dark/10 dark:divide-light/10">
        <li
          v-for="client in clients"
          :key="client.uuid"
          class="flex items-center py-2 px-2 rounded hover:bg-primary/5 transition"
        >
          <div class="flex-1 truncate">
            {{ client.name !== '' ? client.name : $t('troubleshooting.unpair_single_unknown') }}
          </div>
          <n-button
            secondary
            size="small"
            :disabled="removing[client.uuid] === true"
            aria-label="Remove"
            @click="askConfirmUnpair(client.uuid)"
          >
            <i class="fas fa-trash" />
          </n-button>
        </li>
      </ul>
      <div v-else class="p-4 text-center italic opacity-75">
        {{ $t('troubleshooting.unpair_single_no_devices') }}
      </div>
    </n-card>

    <ApiTokenManager></ApiTokenManager>

    <!-- Confirm remove single client -->
    <n-modal :show="showConfirmRemove" @update:show="(v) => (showConfirmRemove = v)">
      <n-card
        :title="
          $t('clients.confirm_remove_title_named', {
            name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
          })
        "
        style="max-width: 32rem; width: 100%"
        :bordered="false"
      >
        <div class="text-sm text-center">
          {{
            $t('clients.confirm_remove_message_named', {
              name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
            })
          }}
        </div>
        <template #footer>
          <div class="w-full flex items-center justify-center gap-3">
            <n-button tertiary @click="showConfirmRemove = false">{{ $t('cancel') }}</n-button>
            <n-button secondary @click="confirmRemove">{{ $t('clients.remove') }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>

    <!-- Confirm unpair all -->
    <n-modal :show="showConfirmUnpairAll" @update:show="(v) => (showConfirmUnpairAll = v)">
      <n-card
        :title="$t('clients.confirm_unpair_all_title')"
        style="max-width: 32rem; width: 100%"
        :bordered="false"
      >
        <div class="text-sm text-center">
          {{ $t('clients.confirm_unpair_all_message_count', { count: clients.length }) }}
        </div>
        <template #footer>
          <div class="w-full flex items-center justify-center gap-3">
            <n-button tertiary @click="showConfirmUnpairAll = false">{{ $t('cancel') }}</n-button>
            <n-button secondary @click="confirmUnpairAll">{{
              $t('troubleshooting.unpair_all')
            }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue';
import { http } from '@/http';
import { NCard, NButton, NAlert, NModal, NInput, NForm, NFormItem } from 'naive-ui';
import ApiTokenManager from '@/ApiTokenManager.vue';
import { useAuthStore } from '@/stores/auth';

// ----- Types -----
interface ClientInfo {
  uuid: string;
  name: string;
}
interface ClientsListResponse {
  status: boolean;
  named_certs: ClientInfo[];
}

// ----- Client pairing & management state -----
const clients = ref<ClientInfo[]>([]);
const pin = ref<string>('');
const deviceName = ref<string>('');
const pairing = ref<boolean>(false);
const pairStatus = ref<boolean | null>(null); // true/false/null
const unpairAllPressed = ref<boolean>(false);
const unpairAllStatus = ref<boolean | null>(null);
const removing = ref<Record<string, boolean>>({});
const showConfirmRemove = ref<boolean>(false);
const pendingRemoveUuid = ref<string>('');
const pendingRemoveName = ref<string>('');
const showConfirmUnpairAll = ref<boolean>(false);

async function refreshClients(): Promise<void> {
  const auth = useAuthStore();
  if (!auth.isAuthenticated) return;
  try {
    const r = await http.get<ClientsListResponse>('./api/clients/list', {
      validateStatus: () => true,
    });
    const response = r.data || ({} as ClientsListResponse);
    if (
      response.status === true &&
      Array.isArray(response.named_certs) &&
      response.named_certs.length
    ) {
      clients.value = response.named_certs.sort((a, b) =>
        a.name.toLowerCase() > b.name.toLowerCase() || a.name === '' ? 1 : -1,
      );
    } else {
      clients.value = [];
    }
  } catch {
    clients.value = [];
  }
}

async function registerDevice(): Promise<void> {
  if (pairing.value) return;
  pairStatus.value = null;
  pairing.value = true;
  try {
    const trimmedName = deviceName.value.trim();
    const body = { pin: pin.value.trim(), name: trimmedName };
    const r = await http.post('./api/pin', body, { validateStatus: () => true });
    const ok =
      r &&
      r.status >= 200 &&
      r.status < 300 &&
      (r.data?.status === true || r.data?.status === 'true' || r.data?.status === 1);
    pairStatus.value = !!ok;
    if (ok) {
      const prevCount = clients.value?.length || 0;
      // Kick one immediate refresh
      await refreshClients();
      // Poll briefly to catch eventual consistency from backend
      const deadline = Date.now() + 5000; // up to 5s
      const target = trimmedName.toLowerCase();
      while (Date.now() < deadline) {
        const found = clients.value?.some((c) => (c.name || '').toLowerCase() === target);
        if (found || (clients.value?.length || 0) > prevCount) break;
        await new Promise((res) => setTimeout(res, 400));
        await refreshClients();
      }
      // Clear inputs after we tried to load the updated list
      pin.value = '';
      deviceName.value = '';
    }
  } catch {
    pairStatus.value = false;
  } finally {
    pairing.value = false;
    setTimeout(() => {
      pairStatus.value = null;
    }, 5000);
  }
}

function askConfirmUnpair(uuid: string): void {
  pendingRemoveUuid.value = uuid;
  const c = clients.value.find((x) => x.uuid === uuid);
  pendingRemoveName.value = c && c.name ? c.name : '';
  showConfirmRemove.value = true;
}

async function confirmRemove(): Promise<void> {
  const uuid = pendingRemoveUuid.value;
  showConfirmRemove.value = false;
  pendingRemoveUuid.value = '';
  pendingRemoveName.value = '';
  if (!uuid) return;
  await unpairSingle(uuid);
}

async function unpairSingle(uuid: string): Promise<void> {
  if (removing.value[uuid]) return;
  removing.value = { ...removing.value, [uuid]: true };
  try {
    await http.post('./api/clients/unpair', { uuid }, { validateStatus: () => true });
  } catch {
  } finally {
    delete removing.value[uuid];
    removing.value = { ...removing.value };
    refreshClients();
  }
}

function askConfirmUnpairAll(): void {
  showConfirmUnpairAll.value = true;
}

async function confirmUnpairAll(): Promise<void> {
  showConfirmUnpairAll.value = false;
  await unpairAll();
}

async function unpairAll(): Promise<void> {
  unpairAllPressed.value = true;
  try {
    const r = await http.post('./api/clients/unpair-all', {}, { validateStatus: () => true });
    unpairAllStatus.value = r.data?.status === true;
  } catch {
    unpairAllStatus.value = false;
  } finally {
    unpairAllPressed.value = false;
    setTimeout(() => {
      unpairAllStatus.value = null;
    }, 5000);
    refreshClients();
  }
}

onMounted(async () => {
  const auth = useAuthStore();
  await auth.waitForAuthentication();
  await refreshClients();
});
</script>

<style scoped></style>
