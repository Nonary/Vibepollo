<script setup lang="ts">
import { EmptyState } from '@/components/ui';
import { useI18n } from 'vue-i18n';

const { t } = useI18n();
</script>

<template>
  <div class="page page--narrow">
    <EmptyState
      icon="help"
      :title="t('ui.not_found.title')"
      :description="t('ui.not_found.description')"
    >
      <RouterLink class="button button--primary" to="/">{{ t('ui.not_found.action') }}</RouterLink>
    </EmptyState>
  </div>
</template>
