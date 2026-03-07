<script setup lang="ts">
import { computed } from 'vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
const defaultMoonlightPort = 47989;
const effectivePort = computed(() => Number(config.port ?? defaultMoonlightPort));
</script>

<template>
  <div id="network" class="config-page">
    <ConfigFieldRenderer setting-key="upnp" v-model="config.upnp" class="mb-3" />

    <ConfigFieldRenderer
      setting-key="address_family"
      v-model="config.address_family"
      class="mb-6"
    />

    <ConfigFieldRenderer setting-key="bind_address" v-model="config['bind_address']" class="mb-6" />

    <div class="mb-6">
      <ConfigFieldRenderer setting-key="port" v-model="config.port" />

      <div
        v-if="+effectivePort - 5 < 1024"
        class="mt-2 alert alert-danger p-2 flex items-start gap-2 rounded-md"
      >
        <i class="fa-solid fa-xl fa-triangle-exclamation" />
        <div class="text-sm">
          {{ $t('config.port_alert_1') }}
        </div>
      </div>

      <div
        v-if="+effectivePort + 21 > 65535"
        class="mt-2 alert alert-danger p-2 flex items-start gap-2 rounded-md"
      >
        <i class="fa-solid fa-xl fa-triangle-exclamation" />
        <div class="text-sm">
          {{ $t('config.port_alert_2') }}
        </div>
      </div>

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

      <div
        v-if="config.origin_web_ui_allowed === 'wan'"
        class="mt-3 alert alert-warning p-2 flex items-start gap-2 rounded-md"
      >
        <i class="fa-solid fa-xl fa-triangle-exclamation" /> {{ $t('config.port_warning') }}
      </div>
    </div>

    <ConfigFieldRenderer
      setting-key="origin_web_ui_allowed"
      v-model="config.origin_web_ui_allowed"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="external_ip"
      v-model="config.external_ip"
      class="mb-6"
      placeholder="123.456.789.12"
    />

    <ConfigFieldRenderer
      setting-key="lan_encryption_mode"
      v-model="config.lan_encryption_mode"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="wan_encryption_mode"
      v-model="config.wan_encryption_mode"
      class="mb-6"
    />

    <ConfigFieldRenderer setting-key="ping_timeout" v-model="config.ping_timeout" class="mb-6" />
  </div>
</template>

<style scoped></style>
