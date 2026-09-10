<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import {
  NETWORK_MOONLIGHT_PORT,
  NETWORK_PORT_MAX,
  NETWORK_PORT_MIN,
  networkPortError,
  networkPortErrorKey,
  networkPortRows,
  parseNetworkPort,
  type NetworkPortRow,
} from '@/utils/network';

const props = withDefaults(
  defineProps<{
    modelValue?: unknown;
    inputId: string;
    labelId: string;
    describedBy?: string;
    originWebUiAllowed?: unknown;
    disabled?: boolean;
  }>(),
  {
    modelValue: () => '',
    describedBy: undefined,
    originWebUiAllowed: undefined,
    disabled: false,
  },
);

const emit = defineEmits<{
  'update:modelValue': [value: number | string];
}>();

const { t } = useI18n();

const portError = computed(() => networkPortError(props.modelValue));
const portErrorKey = computed(() => networkPortErrorKey(portError.value));
const portRows = computed<NetworkPortRow[]>(() => networkPortRows(props.modelValue));
const parsedPort = computed(() => parseNetworkPort(props.modelValue));
const validPort = computed(() => portRows.value.length > 0);
const customMoonlightPort = computed(
  () => validPort.value && parsedPort.value !== NETWORK_MOONLIGHT_PORT,
);
const wanWarningVisible = computed(() => props.originWebUiAllowed === 'wan');
const inputValue = computed(() => {
  if (props.modelValue == null) return '';
  return String(props.modelValue);
});

const errorId = computed(() => `${props.inputId}-error`);
const wanWarningId = computed(() => `${props.inputId}-wan-warning`);
const inputDescribedBy = computed(
  () =>
    [
      props.describedBy,
      portErrorKey.value ? errorId.value : '',
      wanWarningVisible.value ? wanWarningId.value : '',
    ]
      .filter(Boolean)
      .join(' ') || undefined,
);

function rowNote(row: NetworkPortRow): string {
  if (row.note === 'moonlight') {
    return customMoonlightPort.value ? t('config.port_http_port_note') : '';
  }
  if (row.note === 'web-ui') return t('config.port_web_ui');
  return '';
}

function onInput(event: Event): void {
  const raw = (event.target as HTMLInputElement).value;
  emit('update:modelValue', raw === '' ? '' : Number(raw));
}
</script>

<template>
  <div class="network-port-details">
    <input
      :id="inputId"
      class="vs-input"
      type="number"
      inputmode="numeric"
      :min="NETWORK_PORT_MIN"
      :max="NETWORK_PORT_MAX"
      step="1"
      required
      :value="inputValue"
      :disabled="disabled"
      :aria-labelledby="labelId"
      :aria-describedby="inputDescribedBy"
      :aria-invalid="portError ? 'true' : undefined"
      @input="onInput"
    />

    <p class="network-port-details__hint">
      {{ t('ui.settings.network.listener_hint') }}
    </p>

    <p v-if="portErrorKey" :id="errorId" class="network-port-details__error" role="alert">
      {{
        t(portErrorKey, {
          minimum: NETWORK_PORT_MIN,
          maximum: NETWORK_PORT_MAX,
        })
      }}
    </p>

    <table v-if="validPort" class="network-port-details__table">
      <caption class="visually-hidden">
        {{
          t('ui.settings.network.table_caption')
        }}
      </caption>
      <thead>
        <tr>
          <th scope="col">{{ t('config.port_protocol') }}</th>
          <th scope="col">{{ t('config.port_port') }}</th>
          <th scope="col">{{ t('config.port_note') }}</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="row in portRows" :key="`${row.protocol}-${row.ports}`">
          <th scope="row" :data-label="t('config.port_protocol')">
            {{ t(`config.port_${row.protocol}`) }}
          </th>
          <td :data-label="t('config.port_port')" class="monospace">{{ row.ports }}</td>
          <td
            :class="{ 'network-port-details__note--empty': !rowNote(row) }"
            :data-label="t('config.port_note')"
            :aria-hidden="rowNote(row) ? undefined : 'true'"
          >
            <span v-if="rowNote(row)">{{ rowNote(row) }}</span>
          </td>
        </tr>
      </tbody>
    </table>

    <p
      v-if="wanWarningVisible"
      :id="wanWarningId"
      class="network-port-details__warning"
      role="alert"
    >
      {{ t('config.port_warning') }}
    </p>
  </div>
</template>

<style scoped>
.network-port-details {
  display: grid;
  min-width: 0;
  width: 100%;
  gap: var(--vs-space-8);
}

.network-port-details > input {
  width: 100%;
  min-width: 0;
}

.network-port-details__hint,
.network-port-details__error,
.network-port-details__warning {
  margin: 0;
  font-size: 13px;
  line-height: 18px;
}

.network-port-details__hint {
  color: var(--vs-color-text-secondary);
}

.network-port-details__error {
  color: var(--vs-color-status-danger);
}

.network-port-details__warning {
  padding: var(--vs-space-8) var(--vs-space-12);
  border: 1px solid var(--vs-color-status-warning);
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-status-warning);
}

.network-port-details__table {
  width: 100%;
  border-collapse: collapse;
  font-size: var(--vs-type-size-metadata);
}

.network-port-details__table th,
.network-port-details__table td {
  padding: var(--vs-space-8) var(--vs-space-12);
  border-top: 1px solid var(--vs-color-border-subtle);
  text-align: left;
  vertical-align: top;
}

.network-port-details__table th {
  color: var(--vs-color-text-secondary);
  font-weight: var(--vs-type-weight-medium);
}

.network-port-details__table td {
  color: var(--vs-color-text-primary);
  overflow-wrap: anywhere;
}

@media (max-width: 600px) {
  .network-port-details__table,
  .network-port-details__table tbody,
  .network-port-details__table tr,
  .network-port-details__table th,
  .network-port-details__table td {
    display: block;
  }

  .network-port-details__table thead {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0 0 0 0);
    clip-path: inset(50%);
    white-space: nowrap;
  }

  .network-port-details__table tr {
    padding: var(--vs-space-8) 0;
    border-top: 1px solid var(--vs-color-border-subtle);
  }

  .network-port-details__table th,
  .network-port-details__table td {
    padding: var(--vs-space-4) 0;
    border-top: 0;
  }

  .network-port-details__table td.network-port-details__note--empty {
    display: none;
  }

  .network-port-details__table th::before,
  .network-port-details__table td::before {
    display: inline-block;
    width: 5.5rem;
    margin-right: var(--vs-space-8);
    color: var(--vs-color-text-secondary);
    content: attr(data-label);
    font-size: 11px;
    font-weight: var(--vs-type-weight-medium);
    letter-spacing: 0.04em;
    text-transform: uppercase;
    vertical-align: top;
  }
}
</style>
