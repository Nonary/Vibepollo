<script setup lang="ts">
import { onMounted } from 'vue';
import { useI18n } from 'vue-i18n';

import AppShell from '@/components/layout/AppShell.vue';
import AuthView from '@/views/AuthView.vue';
import { useSystemStore } from '@/stores/system';

const system = useSystemStore();
const { t } = useI18n();

onMounted(() => {
  void system.initialize();
});
</script>

<template>
  <div v-if="system.booting" class="boot-screen" role="status" aria-live="polite">
    <img src="/images/logo-apollo-45.png" alt="" width="45" height="45" />
    <div>
      <strong>Vibepollo</strong>
      <span>{{ t('ui.app.connecting') }}</span>
    </div>
  </div>

  <AuthView v-else-if="system.needsSetup || system.needsLogin" />

  <AppShell v-else>
    <RouterView />
  </AppShell>

  <div class="visually-hidden" aria-live="polite" aria-atomic="true">
    {{ system.error ? t(system.error) : '' }}
  </div>
</template>
