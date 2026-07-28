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
        <h3 class="text-sm font-medium">{{ $t('config.amd_section_title') }}</h3>
      </div>
      <div class="p-4">
        <ConfigFieldRenderer v-model="config.amd_usage" setting-key="amd_usage" class="mb-6" />

        <section class="border-t border-dark/10 pt-5 dark:border-light/10">
          <h4 class="group-heading">
            {{ $t('config.amd_rc_group') }}
          </h4>

          <ConfigFieldRenderer v-model="config.amd_rc" setting-key="amd_rc" class="mb-4" />

          <ConfigFieldRenderer
            v-model="config.amd_qvbr_quality_level"
            setting-key="amd_qvbr_quality_level"
            class="mb-4"
          />

          <ConfigFieldRenderer
            v-model="config.amd_enforce_hrd"
            setting-key="amd_enforce_hrd"
            class="mb-0"
          />
        </section>

        <section class="border-t border-dark/10 pt-5 dark:border-light/10">
          <h4 class="group-heading">
            {{ $t('config.amd_quality_group') }}
          </h4>

          <ConfigFieldRenderer
            v-model="config.amd_quality"
            setting-key="amd_quality"
            class="mb-6"
          />

          <ConfigFieldRenderer
            v-model="config.amd_preanalysis"
            setting-key="amd_preanalysis"
            class="mb-3"
          />

          <ConfigFieldRenderer v-model="config.amd_vbaq" setting-key="amd_vbaq" class="mb-3" />

          <ConfigFieldRenderer v-model="config.amd_coder" setting-key="amd_coder" class="mb-0" />
        </section>

        <section class="border-t border-dark/10 pt-5 dark:border-light/10">
          <h4 class="group-heading">
            {{ $t('config.amd_native_group') }}
          </h4>

          <ConfigFieldRenderer
            v-model="config.amd_av1_screen_content"
            setting-key="amd_av1_screen_content"
            class="mb-4"
          />

          <ConfigFieldRenderer
            v-model="config.amd_av1_latency_mode"
            setting-key="amd_av1_latency_mode"
            class="mb-0"
          />
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
