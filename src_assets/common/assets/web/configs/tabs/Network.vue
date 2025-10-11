<script setup lang="ts">
import { computed } from 'vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { NSelect, NInput, NInputNumber } from 'naive-ui';

const store = useConfigStore();
const config = store.config;
const defaultMoonlightPort = 47989;
// Ensure a valid numeric base port even if server returns string/undefined
const effectivePort = computed(() => Number(config.port ?? defaultMoonlightPort));

const addressFamilyOptions = [
  { label: 'IPv4', value: 'ipv4' },
  { label: 'Both', value: 'both' },
];
const originUiOptions = [
  { label: 'PC', value: 'pc' },
  { label: 'LAN', value: 'lan' },
  { label: 'WAN', value: 'wan' },
];
const encryptionModeOptionsLan = [
  { label: '_common.disabled_def', value: 0 },
  { label: 'config.lan_encryption_mode_1', value: 1 },
  { label: 'config.lan_encryption_mode_2', value: 2 },
];
const encryptionModeOptionsWan = [
  { label: '_common.disabled', value: 0 },
  { label: 'config.wan_encryption_mode_1', value: 1 },
  { label: 'config.wan_encryption_mode_2', value: 2 },
];
</script>

<template>
  <div id="network" class="config-page">
    <!-- UPnP -->
    <Checkbox id="upnp" v-model="config.upnp" class="mb-3" locale-prefix="config" default="false" />

    <!-- Address family -->
    <div class="mb-6">
      <label for="address_family" class="form-label">{{ $t('config.address_family') }}</label>
      <n-select
        id="address_family"
        v-model:value="config.address_family"
        :options="
          addressFamilyOptions.map((o) => ({
            label: $t('config.address_family_' + o.value),
            value: o.value,
          }))
        "
        :data-search-options="
          addressFamilyOptions
            .map((o) => `${$t('config.address_family_' + o.value)}::${o.value}`)
            .join('|')
        "
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.address_family_desc') }}
      </p>
    </div>

    <!-- Port family -->
    <div class="mb-6">
      <label for="port" class="form-label">{{ $t('config.port') }}</label>
      <n-input-number
        id="port"
        v-model:value="config.port"
        :min="1029"
        :max="65514"
        :placeholder="String(defaultMoonlightPort)"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.port_desc') }}
      </div>
      <!-- Add warning if any port is less than 1024 -->
      <div
        v-if="+effectivePort - 5 < 1024"
        class="mt-2 alert alert-danger p-2 flex items-start gap-2 rounded-md"
      >
        <i class="fa-solid fa-xl fa-triangle-exclamation" />
        <div class="text-sm">
          {{ $t('config.port_alert_1') }}
        </div>
      </div>
      <!-- Add warning if any port is above 65535 -->
      <div
        v-if="+effectivePort + 21 > 65535"
        class="mt-2 alert alert-danger p-2 flex items-start gap-2 rounded-md"
      >
        <i class="fa-solid fa-xl fa-triangle-exclamation" />
        <div class="text-sm">
          {{ $t('config.port_alert_2') }}
        </div>
      </div>
      <!-- Create a port table for the various ports needed by Sunshine -->
      <div class="mt-4 grid grid-cols-12 gap-2 text-sm">
        <div class="col-span-4 font-semibold">
          {{ $t('config.port_protocol') }}
        </div>
        <div class="col-span-4 font-semibold">
          {{ $t('config.port_port') }}
        </div>
        <div class="col-span-4 font-semibold">
          {{ $t('config.port_note') }}
        </div>

        <div class="col-span-4">
          {{ $t('config.port_tcp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort - 5 }}
        </div>
        <div class="col-span-4" />

        <div class="col-span-4">
          {{ $t('config.port_tcp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort }}
        </div>
        <div class="col-span-4">
          <div
            v-if="+effectivePort !== defaultMoonlightPort"
            class="mt-1 alert alert-info p-2 rounded-md"
          >
            <i class="fa-solid fa-xl fa-circle-info" /> {{ $t('config.port_http_port_note') }}
          </div>
        </div>

        <div class="col-span-4">
          {{ $t('config.port_tcp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort + 1 }}
        </div>
        <div class="col-span-4">
          {{ $t('config.port_web_ui') }}
        </div>

        <div class="col-span-4">
          {{ $t('config.port_tcp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort + 21 }}
        </div>
        <div class="col-span-4" />

        <div class="col-span-4">
          {{ $t('config.port_udp') }}
        </div>
        <div class="col-span-4">{{ +effectivePort + 9 }} - {{ +effectivePort + 11 }}</div>
        <div class="col-span-4" />
      </div>
      <!-- add warning about exposing web ui to the internet -->
      <div
        v-if="config.origin_web_ui_allowed === 'wan'"
        class="mt-3 alert alert-warning p-2 flex items-start gap-2 rounded-md"
      >
        <i class="fa-solid fa-xl fa-triangle-exclamation" /> {{ $t('config.port_warning') }}
      </div>
    </div>

    <!-- Origin Web UI Allowed -->
    <div class="mb-6">
      <label for="origin_web_ui_allowed" class="form-label">{{
        $t('config.origin_web_ui_allowed')
      }}</label>
      <n-select
        id="origin_web_ui_allowed"
        v-model:value="config.origin_web_ui_allowed"
        :options="
          originUiOptions.map((o) => ({
            label: $t('config.origin_web_ui_allowed_' + o.value),
            value: o.value,
          }))
        "
        :data-search-options="
          originUiOptions
            .map((o) => `${$t('config.origin_web_ui_allowed_' + o.value)}::${o.value}`)
            .join('|')
        "
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.origin_web_ui_allowed_desc') }}
      </p>
    </div>

    <!-- External IP -->
    <div class="mb-6">
      <label for="external_ip" class="form-label">{{ $t('config.external_ip') }}</label>
      <n-input
        id="external_ip"
        v-model:value="config.external_ip"
        type="text"
        placeholder="123.456.789.12"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.external_ip_desc') }}
      </div>
    </div>

    <!-- LAN Encryption Mode -->
    <div class="mb-6">
      <label for="lan_encryption_mode" class="form-label">{{
        $t('config.lan_encryption_mode')
      }}</label>
      <n-select
        id="lan_encryption_mode"
        v-model:value="config.lan_encryption_mode"
        :options="encryptionModeOptionsLan.map((o) => ({ label: $t(o.label), value: o.value }))"
        :data-search-options="
          encryptionModeOptionsLan.map((o) => `${$t(o.label)}::${o.value}`).join('|')
        "
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.lan_encryption_mode_desc') }}
      </p>
    </div>

    <!-- WAN Encryption Mode -->
    <div class="mb-6">
      <label for="wan_encryption_mode" class="form-label">{{
        $t('config.wan_encryption_mode')
      }}</label>
      <n-select
        id="wan_encryption_mode"
        v-model:value="config.wan_encryption_mode"
        :options="encryptionModeOptionsWan.map((o) => ({ label: $t(o.label), value: o.value }))"
        :data-search-options="
          encryptionModeOptionsWan.map((o) => `${$t(o.label)}::${o.value}`).join('|')
        "
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.wan_encryption_mode_desc') }}
      </p>
    </div>

    <!-- Ping Timeout -->
    <div class="mb-6">
      <label for="ping_timeout" class="form-label">{{ $t('config.ping_timeout') }}</label>
      <n-input-number
        id="ping_timeout"
        v-model:value="config.ping_timeout"
        :min="0"
        :step="100"
        placeholder="10000"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.ping_timeout_desc') }}
      </div>
    </div>
  </div>
</template>

<style scoped></style>
