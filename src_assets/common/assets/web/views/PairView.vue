<script setup lang="ts">
import { computed, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiPost } from '@/api/client';
import { AppButton, InlineAlert, PageHeader, StatusBadge, UiIcon } from '@/components/ui';

const { t } = useI18n();

interface PairResponse {
  status?: boolean;
}

const name = ref('');
const pin = ref('');
const submitting = ref(false);
const error = ref('');
const pairedName = ref('');

const normalizedPin = computed(() => pin.value.padStart(4, '0'));
const pinIsValid = computed(() => /^\d{1,4}$/.test(pin.value));

function onPinInput(event: Event): void {
  const input = event.target as HTMLInputElement;
  pin.value = input.value.replace(/\D/g, '').slice(0, 4);
  input.value = pin.value;
}

async function submit(): Promise<void> {
  error.value = '';
  pairedName.value = '';

  const friendlyName = name.value.trim();
  if (!friendlyName) {
    error.value = t('ui.pair.error.name_required');
    return;
  }
  if (!pinIsValid.value) {
    error.value = t('ui.pair.error.pin_invalid');
    return;
  }

  submitting.value = true;
  try {
    const response = await apiPost<PairResponse>('/api/pin', {
      pin: normalizedPin.value,
      name: friendlyName,
    });
    if (response.status !== true) {
      throw new Error(t('ui.pair.error.pin_rejected'));
    }
    pairedName.value = friendlyName;
    pin.value = '';
  } catch (cause) {
    error.value =
      cause instanceof ApiError
        ? t('ui.pair.error.submit')
        : cause instanceof Error
          ? cause.message
          : t('ui.pair.error.submit');
  } finally {
    submitting.value = false;
  }
}

function resetForm(): void {
  name.value = '';
  pin.value = '';
  pairedName.value = '';
  error.value = '';
}
</script>

<template>
  <div class="vs-page vs-page--settings pair-page">
    <PageHeader
      :title="t('ui.pair.page.title')"
      :description="t('ui.pair.page.description')"
    >
      <template #meta>
        <StatusBadge :label="t('ui.pair.page.local_approval')" tone="info" compact />
      </template>
      <template #actions>
        <RouterLink class="vs-button vs-button--secondary" to="/devices">
          <UiIcon name="chevron-left" aria-hidden="true" />
          <span>{{ t('ui.pair.action.back_to_devices') }}</span>
        </RouterLink>
      </template>
    </PageHeader>

    <div class="pair-layout">
      <section class="pair-instructions" aria-labelledby="pair-steps-title">
        <h2 id="pair-steps-title">{{ t('ui.pair.steps.title') }}</h2>
        <ol>
          <li>
            <span>{{ t('ui.pair.steps.number', { number: 1 }) }}</span>
            <div>
              <strong>{{ t('ui.pair.steps.open_moonlight_title') }}</strong>
              <p>{{ t('ui.pair.steps.open_moonlight_description') }}</p>
            </div>
          </li>
          <li>
            <span>{{ t('ui.pair.steps.number', { number: 2 }) }}</span>
            <div>
              <strong>{{ t('ui.pair.steps.start_pairing_title') }}</strong>
              <p>{{ t('ui.pair.steps.start_pairing_description') }}</p>
            </div>
          </li>
          <li>
            <span>{{ t('ui.pair.steps.number', { number: 3 }) }}</span>
            <div>
              <strong>{{ t('ui.pair.steps.approve_title') }}</strong>
              <p>{{ t('ui.pair.steps.approve_description') }}</p>
            </div>
          </li>
        </ol>
      </section>

      <section class="pair-panel vs-surface" aria-labelledby="pair-form-title">
        <div class="pair-panel__heading">
          <div class="pair-panel__icon" aria-hidden="true"><UiIcon name="devices" :size="20" /></div>
          <div>
            <h2 id="pair-form-title">{{ t('ui.pair.form.title') }}</h2>
            <p>{{ t('ui.pair.form.description') }}</p>
          </div>
        </div>

        <InlineAlert
          v-if="error"
          tone="danger"
          :title="t('ui.pair.alert.failed')"
          announce="assertive"
        >
          {{ error }}
        </InlineAlert>

        <InlineAlert
          v-if="pairedName"
          tone="success"
          :title="t('ui.pair.alert.paired')"
          announce="polite"
        >
          {{ t('ui.pair.notice.connected', { name: pairedName }) }}
          <template #actions>
            <RouterLink class="vs-button vs-button--secondary vs-button--compact" to="/devices">
              {{ t('ui.pair.action.view_devices') }}
            </RouterLink>
            <AppButton
              :label="t('ui.pair.action.pair_another')"
              variant="tertiary"
              size="compact"
              @click="resetForm"
            />
          </template>
        </InlineAlert>

        <form v-if="!pairedName" class="pair-form" novalidate @submit.prevent="submit">
          <label class="vs-field" for="pair-name">
            <span class="vs-field__label">{{ t('pin.device_name') }}</span>
            <input
              id="pair-name"
              v-model="name"
              class="vs-input"
              name="device-name"
              autocomplete="off"
              maxlength="96"
              :placeholder="t('ui.pair.form.name_placeholder')"
              :aria-invalid="error && !name.trim() ? 'true' : undefined"
              required
            />
            <span class="vs-field__helper">{{ t('ui.pair.form.name_helper') }}</span>
          </label>

          <label class="vs-field" for="pair-pin">
            <span class="vs-field__label">{{ t('ui.pair.form.pin_label') }}</span>
            <input
              id="pair-pin"
              class="vs-input pair-pin"
              name="pin"
              type="text"
              inputmode="numeric"
              autocomplete="one-time-code"
              pattern="[0-9]{1,4}"
              maxlength="4"
              :placeholder="t('ui.pair.form.pin_placeholder')"
              :value="pin"
              :aria-invalid="error && !pinIsValid ? 'true' : undefined"
              aria-describedby="pair-pin-help"
              required
              @input="onPinInput"
            />
            <span id="pair-pin-help" class="vs-field__helper">
              {{
                pin
                  ? t('ui.pair.form.pin_submit_as', { pin: normalizedPin })
                  : t('ui.pair.form.pin_helper')
              }}
            </span>
          </label>

          <AppButton
            type="submit"
            variant="primary"
            :label="t('ui.pair.action.approve')"
            icon="check"
            :busy="submitting"
            :busy-label="t('ui.pair.action.approving')"
            block
          />
        </form>

        <p class="pair-privacy">
          <UiIcon name="info" :size="16" aria-hidden="true" />
          {{ t('ui.pair.privacy') }}
        </p>
      </section>
    </div>
  </div>
</template>

<style scoped>
.pair-page {
  display: grid;
  gap: var(--vs-space-24);
}

.pair-layout {
  display: grid;
  grid-template-columns: minmax(16rem, 0.8fr) minmax(20rem, 1.2fr);
  gap: var(--vs-space-24);
  align-items: start;
}

.pair-instructions,
.pair-panel {
  padding: var(--vs-space-24);
}

.pair-instructions h2,
.pair-panel h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.pair-instructions ol {
  display: grid;
  gap: var(--vs-space-20);
  padding: 0;
  margin-top: var(--vs-space-20);
  list-style: none;
}

.pair-instructions li {
  display: grid;
  grid-template-columns: 32px minmax(0, 1fr);
  gap: var(--vs-space-12);
}

.pair-instructions li > span {
  display: grid;
  width: 32px;
  height: 32px;
  place-items: center;
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  color: var(--vs-color-accent-default);
  font-weight: var(--vs-type-weight-semibold);
  font-variant-numeric: tabular-nums;
}

.pair-instructions strong {
  display: block;
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
}

.pair-instructions p,
.pair-panel__heading p,
.pair-privacy {
  margin-top: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.pair-panel,
.pair-form {
  display: grid;
  gap: var(--vs-space-20);
}

.pair-panel__heading {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-12);
}

.pair-panel__icon {
  display: grid;
  width: 40px;
  height: 40px;
  flex: none;
  place-items: center;
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-accent-default);
}

.pair-pin {
  max-width: 13rem;
  font-family: var(--vs-type-family-mono);
  font-size: 1.25rem;
  font-variant-numeric: tabular-nums;
  letter-spacing: 0.28em;
}

.pair-privacy {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-8);
  padding-top: var(--vs-space-16);
  border-top: 1px solid var(--vs-color-border-subtle);
}

.pair-privacy > svg {
  flex: none;
  margin-top: 1px;
}

@media (max-width: 767px) {
  .pair-layout {
    grid-template-columns: minmax(0, 1fr);
  }

  .pair-instructions,
  .pair-panel {
    padding: var(--vs-space-16);
  }

  .pair-pin {
    max-width: none;
  }
}

@media (forced-colors: active) {
  .pair-instructions li > span,
  .pair-panel__icon {
    border: 1px solid CanvasText;
  }
}
</style>
