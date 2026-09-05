<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { StatusBadge, UiIcon } from '@/components/ui';
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
  <details class="linux-capture" :open="state === 'unavailable' || state === 'unknown'">
    <summary class="linux-capture__heading">
      <UiIcon name="devices" :size="20" />
      <h3 id="linux-capture-title">{{ t('ui.settings.linux.title') }}</h3>
      <StatusBadge
        :label="t(`ui.settings.linux.states.${state}`)"
        :tone="state === 'active' ? 'success' : state === 'unavailable' ? 'warning' : 'neutral'"
      />
      <UiIcon class="linux-capture__chevron" name="chevron-down" />
    </summary>
    <div class="linux-capture__body">
      <p>{{ t('ui.settings.linux.recommendation') }}</p>
      <p class="linux-capture__detail">{{ t('ui.settings.linux.explanation') }}</p>
      <p v-if="state === 'unavailable'">{{ t(reasonKey) }}</p>
      <p v-if="metadata.linux?.session_role === 'greeter'">{{ t('ui.settings.linux.greeter') }}</p>
      <a
        v-if="state === 'unavailable' || state === 'unknown'"
        href="https://github.com/Nonary/Vibepollo/issues"
        target="_blank"
        rel="noopener noreferrer"
        >{{ t('ui.settings.linux.repair') }}</a
      >
    </div>
  </details>
</template>

<style scoped>
.linux-capture {
  margin-bottom: var(--vs-space-24);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}
.linux-capture__heading {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: var(--vs-space-12);
  padding: var(--vs-space-16) var(--vs-space-20);
  list-style: none;
  cursor: pointer;
  color: var(--vs-color-text-muted);
  border-radius: inherit;
}
.linux-capture__heading::-webkit-details-marker {
  display: none;
}
.linux-capture__heading:hover {
  background: var(--vs-color-bg-subtle);
}
.linux-capture__heading h3 {
  flex: 1;
  min-width: 8rem;
}
.linux-capture[open] .linux-capture__chevron {
  transform: rotate(180deg);
}
.linux-capture__body {
  padding: 0 var(--vs-space-20) var(--vs-space-20);
  font-size: var(--vs-type-size-control);
}
h3 {
  margin: 0;
  font-size: var(--vs-type-size-control);
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
