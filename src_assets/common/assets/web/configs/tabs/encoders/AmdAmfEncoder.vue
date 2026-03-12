<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="amd-amf-encoder" class="config-page">
    <div class="mb-4 rounded-md overflow-hidden border border-dark/10 dark:border-light/10">
      <div class="bg-surface/40 dark:bg-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">
          AMD AMF Encoder
        </h3>
      </div>
      <div class="p-4">
        <ConfigFieldRenderer setting-key="amd_usage" v-model="config.amd_usage" class="mb-6" />

        <section class="border-t border-dark/10 pt-5 dark:border-light/10">
          <h4 class="group-heading">
            {{ $t('config.amd_rc_group') }}
          </h4>

          <ConfigFieldRenderer setting-key="amd_rc" v-model="config.amd_rc" class="mb-4" />

          <ConfigFieldRenderer
            setting-key="amd_enforce_hrd"
            v-model="config.amd_enforce_hrd"
            class="mb-0"
          />
        </section>

        <section class="border-t border-dark/10 pt-5 dark:border-light/10">
          <h4 class="group-heading">
            {{ $t('config.amd_quality_group') }}
          </h4>

          <ConfigFieldRenderer setting-key="amd_quality" v-model="config.amd_quality" class="mb-6" />

          <ConfigFieldRenderer
            setting-key="amd_preanalysis"
            v-model="config.amd_preanalysis"
            class="mb-3"
          />

          <ConfigFieldRenderer setting-key="amd_vbaq" v-model="config.amd_vbaq" class="mb-3" />

          <ConfigFieldRenderer setting-key="amd_coder" v-model="config.amd_coder" class="mb-0" />
        </section>
      </div>
    </div>
  </div>
</template>

<style scoped>
.group-heading {
  @apply mb-3 text-xs font-semibold uppercase tracking-[0.08em] opacity-70;
}
</style>
