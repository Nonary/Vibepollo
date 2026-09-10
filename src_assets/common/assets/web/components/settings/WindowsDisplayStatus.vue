<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { InlineAlert, LoadingSkeleton, StatusBadge, UiIcon } from '@/components/ui';
import {
  isWindowsHost,
  windowsDisplayHealth,
  type DisplayMetadataWithDriver,
  type WindowsDisplayDriver,
  type WindowsDisplayDriverState,
} from '@/utils/displayHealth';

const props = withDefaults(
  defineProps<{
    metadata: DisplayMetadataWithDriver | null;
    selectedDriver?: WindowsDisplayDriver;
    loading?: boolean;
    refreshing?: boolean;
    error?: string;
  }>(),
  {
    loading: false,
    refreshing: false,
    error: '',
  },
);

const emit = defineEmits<{
  refresh: [];
}>();

const { t } = useI18n();
const health = computed(() => windowsDisplayHealth(props.metadata));
const visible = computed(() => isWindowsHost(props.metadata));
const selectedDriver = computed(() => props.selectedDriver ?? health.value.configuredDriver);

const driverName = (driver?: WindowsDisplayDriver): string => {
  if (!driver) return t('ui.settings.windows_display.drivers.unknown');
  return t(
    `config.virtual_display_driver_${driver === 'vibeshine' ? 'vibeshine' : 'sudovda'}_name`,
  );
};

const stateTone = computed<'success' | 'warning' | 'danger' | 'neutral'>(() => {
  switch (health.value.state) {
    case 'ready':
      return 'success';
    case 'failed':
    case 'watchdog_failed':
      return 'danger';
    case 'uninitialized':
    case 'version_incompatible':
      return 'warning';
    default:
      return 'neutral';
  }
});

const stateLabel = computed(() => {
  if (health.value.state === 'failed') return t('ui.settings.windows_display.states.failed');
  return t(`config.virtual_display_driver_status_${health.value.state}`);
});

const stateTitle = computed(() =>
  t(`ui.settings.windows_display.messages.${health.value.state}.title`),
);

const stateMessage = computed(() =>
  t(`ui.settings.windows_display.messages.${health.value.state}.description`, {
    driver: driverName(health.value.activeDriver ?? selectedDriver.value),
  }),
);

const observedDriverText = computed(() => {
  if (!health.value.activeDriver) return t('ui.settings.windows_display.observed_unknown');
  return t('ui.settings.windows_display.observed', {
    driver: driverName(health.value.activeDriver),
  });
});

const configuredMismatch = computed(
  () =>
    Boolean(health.value.activeDriver && selectedDriver.value) &&
    health.value.activeDriver !== selectedDriver.value,
);

const mismatchText = computed(() =>
  t('ui.settings.windows_display.configured_mismatch', {
    configured: driverName(selectedDriver.value),
    observed: driverName(health.value.activeDriver),
  }),
);

const repairVisible = computed(() =>
  ['failed', 'uninitialized', 'version_incompatible', 'watchdog_failed', 'unknown'].includes(
    health.value.state,
  ),
);

function isState(value: WindowsDisplayDriverState): boolean {
  return health.value.state === value;
}
</script>

<template>
  <section
    v-if="visible"
    id="windows-display-status"
    class="windows-display-status"
    aria-labelledby="windows-display-status-title"
  >
    <div class="windows-display-status__heading">
      <UiIcon name="devices" :size="20" />
      <div>
        <h3 id="windows-display-status-title">{{ t('ui.settings.windows_display.title') }}</h3>
        <p>{{ t('ui.settings.windows_display.description') }}</p>
      </div>
      <StatusBadge
        v-if="!loading && !error"
        :label="stateLabel"
        :tone="stateTone"
        announce="polite"
      />
      <button
        type="button"
        class="button button--secondary windows-display-status__refresh"
        :disabled="loading || refreshing"
        :aria-busy="refreshing"
        @click="emit('refresh')"
      >
        <UiIcon name="refresh" />
        {{
          t(
            refreshing
              ? 'ui.settings.windows_display.refreshing'
              : 'ui.settings.windows_display.refresh',
          )
        }}
      </button>
    </div>

    <div v-if="loading" class="windows-display-status__body" :aria-label="t('ui.settings.loading')">
      <LoadingSkeleton height="52px" />
    </div>
    <InlineAlert
      v-else-if="error"
      class="windows-display-status__alert"
      tone="danger"
      announce="polite"
      :title="t('ui.settings.windows_display.error_title')"
    >
      {{ error }}
    </InlineAlert>
    <div v-else class="windows-display-status__body">
      <p class="windows-display-status__configured">
        {{ t('ui.settings.windows_display.configured', { driver: driverName(selectedDriver) }) }}
      </p>
      <p class="windows-display-status__observed">{{ observedDriverText }}</p>
      <InlineAlert
        class="windows-display-status__alert"
        :tone="stateTone === 'neutral' ? 'info' : stateTone"
        :announce="repairVisible ? 'polite' : 'off'"
        :title="stateTitle"
      >
        {{ stateMessage }}
        <span v-if="health.statusCode !== undefined" class="windows-display-status__code">
          {{ t('ui.settings.windows_display.status_code', { code: health.statusCode }) }}
        </span>
      </InlineAlert>
      <p v-if="configuredMismatch" class="windows-display-status__mismatch">
        {{ mismatchText }}
      </p>
      <p v-if="isState('unknown')" class="windows-display-status__next-step">
        {{ t('ui.settings.windows_display.next_step') }}
      </p>
    </div>
  </section>
</template>

<style scoped>
.windows-display-status {
  margin-bottom: var(--vs-space-24);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.windows-display-status__heading {
  display: flex;
  align-items: flex-start;
  flex-wrap: wrap;
  gap: var(--vs-space-12);
  padding: var(--vs-space-16) var(--vs-space-20);
}

.windows-display-status__heading > div {
  min-width: 12rem;
  flex: 1;
}

.windows-display-status__heading h3 {
  margin: 0;
  font-size: var(--vs-type-size-control);
}

.windows-display-status__heading p,
.windows-display-status__body p {
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-control);
}

.windows-display-status__body {
  padding: 0 var(--vs-space-20) var(--vs-space-20);
}

.windows-display-status__observed {
  margin-bottom: var(--vs-space-12) !important;
  color: var(--vs-color-text-primary) !important;
  font-weight: var(--vs-type-weight-medium);
}

.windows-display-status__alert {
  margin-top: var(--vs-space-12);
}

.windows-display-status__code {
  display: block;
  margin-top: var(--vs-space-8);
  color: var(--vs-color-text-muted);
  font-size: 12px;
}

.windows-display-status__mismatch,
.windows-display-status__next-step {
  margin-top: var(--vs-space-12) !important;
}

@media (max-width: 560px) {
  .windows-display-status__heading {
    padding: var(--vs-space-16);
  }

  .windows-display-status__body {
    padding-inline: var(--vs-space-16);
  }

  .windows-display-status__heading .vs-status-badge {
    flex-basis: 100%;
  }
}
</style>
