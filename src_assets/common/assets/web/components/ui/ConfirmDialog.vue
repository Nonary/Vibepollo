<script setup lang="ts">
// Native dialog semantics are augmented with deterministic initial focus and restoration.
import {
  computed,
  nextTick,
  onBeforeUnmount,
  onMounted,
  ref,
  useId,
  useSlots,
  watch,
} from 'vue';
import AppButton from './AppButton.vue';
import UiIcon from './UiIcon.vue';
import { useI18n } from 'vue-i18n';

const { t } = useI18n();

type CancelReason = 'cancel-button' | 'escape' | 'backdrop';

const props = withDefaults(
  defineProps<{
    open: boolean;
    title: string;
    description?: string;
    confirmLabel?: string;
    cancelLabel?: string;
    tone?: 'default' | 'danger';
    busy?: boolean;
    busyLabel?: string;
    closeOnBackdrop?: boolean;
    closeOnConfirm?: boolean;
    initialFocus?: 'cancel' | 'confirm' | 'dialog';
  }>(),
  {
    tone: 'default',
    busy: false,
    closeOnBackdrop: true,
    closeOnConfirm: true,
    initialFocus: 'cancel',
  },
);

const emit = defineEmits<{
  'update:open': [value: boolean];
  confirm: [];
  cancel: [reason: CancelReason];
}>();

const slots = useSlots();
const uid = useId();
const titleId = 'vs-dialog-title-' + uid;
const descriptionId = 'vs-dialog-description-' + uid;
const dialog = ref<HTMLDialogElement | null>(null);
const panel = ref<HTMLElement | null>(null);
const cancelButton = ref<InstanceType<typeof AppButton> | null>(null);
const confirmButton = ref<InstanceType<typeof AppButton> | null>(null);
const hasDescription = computed(() => Boolean(props.description || slots.description));
let restoreFocusTo: HTMLElement | null = null;

function componentElement(component: InstanceType<typeof AppButton> | null): HTMLElement | null {
  const element = component?.$el;
  return element instanceof HTMLElement ? element : null;
}

async function showDialog() {
  const element = dialog.value;
  if (!element) return;

  if (!element.open) {
    restoreFocusTo = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    element.showModal();
  }

  await nextTick();
  const target =
    props.initialFocus === 'confirm'
      ? componentElement(confirmButton.value)
      : props.initialFocus === 'dialog'
        ? panel.value
        : componentElement(cancelButton.value);
  target?.focus();
}

function hideDialog(restoreFocus = true) {
  const element = dialog.value;
  if (element?.open) element.close();
  const focusTarget = restoreFocusTo;
  if (restoreFocus && focusTarget?.isConnected) {
    nextTick(() => focusTarget.focus());
  }
  restoreFocusTo = null;
}

function requestCancel(reason: CancelReason) {
  if (props.busy) return;
  emit('cancel', reason);
  emit('update:open', false);
}

function onNativeCancel(event: Event) {
  event.preventDefault();
  requestCancel('escape');
}

function onDialogClick(event: MouseEvent) {
  if (event.target === dialog.value && props.closeOnBackdrop) requestCancel('backdrop');
}

function onConfirm() {
  if (props.busy) return;
  emit('confirm');
  if (props.closeOnConfirm) emit('update:open', false);
}

function focusableElements(): HTMLElement[] {
  if (!panel.value) return [];
  const selector = [
    'a[href]',
    'button:not([disabled])',
    'input:not([disabled])',
    'select:not([disabled])',
    'textarea:not([disabled])',
    '[tabindex]:not([tabindex="-1"])',
  ].join(',');
  return Array.from(panel.value.querySelectorAll<HTMLElement>(selector)).filter(
    (element) => element.getAttribute('aria-hidden') !== 'true' && !element.hasAttribute('inert'),
  );
}

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape' && props.busy) {
    event.preventDefault();
    event.stopPropagation();
    return;
  }
  if (event.key !== 'Tab') return;

  const focusable = focusableElements();
  if (!focusable.length) {
    event.preventDefault();
    panel.value?.focus();
    return;
  }

  const first = focusable[0];
  const last = focusable[focusable.length - 1];
  if (event.shiftKey && (document.activeElement === first || document.activeElement === panel.value)) {
    event.preventDefault();
    last.focus();
  } else if (!event.shiftKey && document.activeElement === last) {
    event.preventDefault();
    first.focus();
  }
}

watch(
  () => props.open,
  (open) => {
    if (open) showDialog();
    else hideDialog();
  },
  { flush: 'post' },
);

onMounted(() => {
  if (props.open) showDialog();
});

onBeforeUnmount(() => hideDialog(false));
</script>

<template>
  <Teleport to="body">
    <dialog
      ref="dialog"
      class="vs-dialog"
      :aria-labelledby="titleId"
      :aria-describedby="hasDescription ? descriptionId : undefined"
      :aria-busy="busy ? 'true' : undefined"
      @cancel="onNativeCancel"
      @click="onDialogClick"
      @keydown="onKeydown"
    >
      <section ref="panel" class="vs-dialog__panel" tabindex="-1">
        <div class="vs-dialog__heading">
          <div v-if="tone === 'danger'" class="vs-dialog__tone-icon" aria-hidden="true">
            <UiIcon name="alert-triangle" :size="20" />
          </div>
          <div>
            <h2 :id="titleId" class="vs-dialog__title">{{ title }}</h2>
            <div v-if="hasDescription" :id="descriptionId" class="vs-dialog__description">
              <slot name="description">{{ description }}</slot>
            </div>
          </div>
        </div>
        <div v-if="$slots.default" class="vs-dialog__body"><slot /></div>
        <div class="vs-dialog__actions">
          <AppButton
            ref="cancelButton"
            variant="secondary"
            :label="cancelLabel || t('_common.cancel')"
            :disabled="busy"
            @click="requestCancel('cancel-button')"
          />
          <AppButton
            ref="confirmButton"
            :variant="tone === 'danger' ? 'danger' : 'primary'"
            :label="confirmLabel || t('ui.common.confirm')"
            :busy="busy"
            :busy-label="busyLabel || t('ui.common.working')"
            @click="onConfirm"
          />
        </div>
      </section>
    </dialog>
  </Teleport>
</template>
