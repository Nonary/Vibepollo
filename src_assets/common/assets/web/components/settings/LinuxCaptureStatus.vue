<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { StatusBadge } from '@/components/ui';
import { linuxCaptureState, type ReadinessMetadata } from '@/utils/hostReadiness';
const props = defineProps<{ metadata: ReadinessMetadata; virtualMode?: string }>();
const { t, te } = useI18n();
const reasonKey = computed(() => {
  const key = `ui.settings.linux.reasons.${props.metadata.virtual_display?.reason || 'unknown'}`;
  return te(key) || te(key, 'en') ? key : 'ui.settings.linux.reasons.unknown';
});
const state = computed(() => linuxCaptureState(props.metadata, props.virtualMode));
</script>

<template>
  <section class="linux-capture" aria-labelledby="linux-capture-title">
    <div class="linux-capture__heading">
      <h3 id="linux-capture-title">{{ t('ui.settings.linux.title') }}</h3>
      <StatusBadge
        :label="t(`ui.settings.linux.states.${state}`)"
        :tone="state === 'active' ? 'success' : state === 'unavailable' ? 'warning' : 'neutral'"
      />
    </div>
    <p>{{ t('ui.settings.linux.recommendation') }}</p>
    <p class="linux-capture__detail">{{ t('ui.settings.linux.explanation') }}</p>
    <p v-if="state === 'unavailable'">{{ t(reasonKey) }}</p>
    <p v-if="metadata.linux?.session_role === 'greeter'">{{ t('ui.settings.linux.greeter') }}</p>
    <a
      v-if="state === 'unavailable' || state === 'unknown'"
      href="https://github.com/Nonary/Vibepollo/blob/vibe/docs/linux/install.md"
      target="_blank"
      rel="noopener noreferrer"
      >{{ t('ui.settings.linux.repair') }}</a
    >
  </section>
</template>

<style scoped>
.linux-capture {
  margin-bottom: var(--vs-space-24);
  padding: var(--vs-space-20);
  border: 1px solid var(--vs-color-border-strong);
  border-inline-start: 3px solid var(--vs-color-accent-default);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}
.linux-capture__heading {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: var(--vs-space-12);
}
h3 {
  margin: 0;
  font-size: 16px;
}
p {
  margin: var(--vs-space-12) 0 0;
}
.linux-capture__detail {
  color: var(--vs-color-text-secondary);
}
a {
  display: inline-block;
  margin-top: var(--vs-space-12);
}
</style>
