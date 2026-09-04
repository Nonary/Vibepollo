import { onBeforeUnmount, onMounted, type ComputedRef } from 'vue';
import { onBeforeRouteLeave } from 'vue-router';
import { useI18n } from 'vue-i18n';

export function useUnsavedChanges(dirty: ComputedRef<boolean>): void {
  const { t } = useI18n();
  function beforeUnload(event: BeforeUnloadEvent) {
    if (dirty.value) {
      event.preventDefault();
      event.returnValue = '';
    }
  }
  onBeforeRouteLeave(() => !dirty.value || window.confirm(t('ui.settings.leave_warning')));
  onMounted(() => window.addEventListener('beforeunload', beforeUnload));
  onBeforeUnmount(() => window.removeEventListener('beforeunload', beforeUnload));
}
