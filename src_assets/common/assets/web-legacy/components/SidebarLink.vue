<template>
  <RouterLink
    :to="to"
    class="group flex items-center gap-3 rounded-md mx-2 transition"
    :class="[
      collapsed ? 'justify-center px-0 py-2.5' : 'px-4 py-2.5',
      sub ? (collapsed ? 'text-[0]' : 'pl-8 pr-3 py-2 text-[12px]') : 'text-[13px]',
      baseClasses,
      isActive ? activeClasses : hoverClasses,
    ]"
  >
    <i v-if="!sub" :class="['fas text-sm w-4 text-center', 'fa-fw', icon]" />
    <span v-if="!collapsed" class="tracking-wide whitespace-nowrap">
      <slot />
    </span>
  </RouterLink>
</template>
<script setup lang="ts">
import { useRoute } from 'vue-router';
import { computed } from 'vue';
type ToLocation = string | { path: string; query?: Record<string, any> };
const props = withDefaults(
  defineProps<{ to: ToLocation; icon?: string; collapsed?: boolean; sub?: boolean }>(),
  { icon: 'fa-circle', collapsed: false, sub: false },
);
const route = useRoute();
const isActive = computed(() => {
  if (typeof props.to === 'string') return route.path === props.to;
  if (props.to && typeof props.to === 'object') {
    const pathMatch = route.path === (props.to as any).path;
    if (!pathMatch) return false;
    const q = (props.to as any).query;
    if (q && q['sec']) {
      return (route.query as any)['sec'] === q['sec'];
    }
    return pathMatch;
  }
  return false;
});
const baseClasses = 'text-dark/80';
const hoverClasses = 'hover:text-dark hover:bg-primary/10';
const activeClasses = 'bg-primary/15 text-dark font-medium';
</script>
<style scoped></style>
