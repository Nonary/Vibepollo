<script setup lang="ts">
// Icons are local SVG geometry so the UI has no component-library dependency.
import { computed } from 'vue';
import type { UiIconName } from './types';

type IconDefinition = ReadonlyArray<string>;

const iconPaths: Record<UiIconName, IconDefinition> = {
  activity: ['M3 12h4l2.5-7 5 14L17 12h4'],
  'alert-triangle': [
    'M10.3 3.8 2.6 17.1A2 2 0 0 0 4.3 20h15.4a2 2 0 0 0 1.7-2.9L13.7 3.8a2 2 0 0 0-3.4 0Z',
    'M12 9v4',
    'M12 17h.01',
  ],
  check: ['m5 12 4 4L19 6'],
  'check-circle': ['M22 11.1V12a10 10 0 1 1-5.9-9.1', 'm9 11 3 3L22 4'],
  'chevron-down': ['m6 9 6 6 6-6'],
  'chevron-left': ['m15 18-6-6 6-6'],
  'chevron-right': ['m9 18 6-6-6-6'],
  copy: ['M8 8h11a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H10a2 2 0 0 1-2-2Z', 'M16 8V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h3'],
  devices: ['M4 5h16v11H4z', 'M8 20h8', 'M12 16v4'],
  download: ['M12 3v12', 'm7 10 5 5 5-5', 'M5 21h14'],
  edit: ['M12 20h9', 'M16.5 3.5a2.1 2.1 0 0 1 3 3L8 18l-4 1 1-4Z'],
  'external-link': ['M15 3h6v6', 'm10 14 11-11', 'M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6'],
  gamepad: ['M7 6h10a4 4 0 0 1 3.8 5.3l-1.4 5.2a2 2 0 0 1-3.3 1l-2.3-2h-3.6l-2.3 2a2 2 0 0 1-3.3-1l-1.4-5.2A4 4 0 0 1 7 6Z', 'M8 10v4', 'M6 12h4', 'M16 11h.01', 'M18 13h.01'],
  help: ['M9.1 9a3 3 0 1 1 4.8 2.4c-1.1.8-1.9 1.3-1.9 2.6', 'M12 18h.01', 'M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20Z'],
  info: ['M12 8h.01', 'M11 12h1v4h1', 'M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20Z'],
  integrations: ['M8 3v4', 'M16 3v4', 'M5 7h14v4a7 7 0 0 1-14 0Z', 'M12 18v3'],
  key: ['M15.5 8.5a4.5 4.5 0 1 0-3.8 4.4L15 16h2v2h2v2h2v-4.5l-4.4-4.4a4.5 4.5 0 0 0-1.1-2.6Z', 'M8.5 8.5h.01'],
  library: ['M4 19.5A2.5 2.5 0 0 1 6.5 17H20', 'M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2Z'],
  logs: ['M6 2h9l4 4v16H6z', 'M14 2v5h5', 'M9 13h6', 'M9 17h6'],
  menu: ['M4 7h16', 'M4 12h16', 'M4 17h16'],
  minus: ['M5 12h14'],
  more: ['M5 12h.01', 'M12 12h.01', 'M19 12h.01'],
  overview: ['M3 3h7v7H3z', 'M14 3h7v7h-7z', 'M3 14h7v7H3z', 'M14 14h7v7h-7z'],
  play: ['m8 5 11 7-11 7Z'],
  plus: ['M12 5v14', 'M5 12h14'],
  refresh: ['M20 6v5h-5', 'M4 18v-5h5', 'M18.5 9A7 7 0 0 0 6.2 6.2L4 11', 'M5.5 15A7 7 0 0 0 17.8 17.8L20 13'],
  search: ['m21 21-4.3-4.3', 'M11 19a8 8 0 1 0 0-16 8 8 0 0 0 0 16Z'],
  sessions: ['M4 5h16v12H4z', 'M8 21h8', 'M12 17v4', 'm10 9 5 2.5-5 2.5Z'],
  settings: ['M12 8.5a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Z', 'M12 2v3', 'M12 19v3', 'm4.93 4.93 2.12 2.12', 'm16.95 16.95 2.12 2.12', 'M2 12h3', 'M19 12h3', 'm4.93 19.07 2.12-2.12', 'm16.95 7.05 2.12-2.12'],
  stop: ['M7 7h10v10H7z'],
  trash: ['M4 7h16', 'M9 7V4h6v3', 'M7 7l1 14h8l1-14', 'M10 11v6', 'M14 11v6'],
  upload: ['M12 21V9', 'm7 14 5-5 5 5', 'M5 3h14'],
  user: ['M20 21a8 8 0 0 0-16 0', 'M12 13a5 5 0 1 0 0-10 5 5 0 0 0 0 10Z'],
  warning: ['M12 9v4', 'M12 17h.01', 'M10.3 3.8 2.6 17.1A2 2 0 0 0 4.3 20h15.4a2 2 0 0 0 1.7-2.9L13.7 3.8a2 2 0 0 0-3.4 0Z'],
  x: ['M6 6l12 12', 'M18 6 6 18'],
  'x-circle': ['M15 9 9 15', 'm9 9 6 6', 'M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20Z'],
};

const props = withDefaults(
  defineProps<{
    name: UiIconName;
    size?: number | string;
    label?: string;
    strokeWidth?: number;
  }>(),
  {
    size: 16,
    strokeWidth: 1.8,
  },
);

const paths = computed(() => iconPaths[props.name]);
const labelled = computed(() => Boolean(props.label));
</script>

<template>
  <svg
    class="vs-icon"
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    :stroke-width="strokeWidth"
    stroke-linecap="round"
    stroke-linejoin="round"
    :role="labelled ? 'img' : undefined"
    :aria-label="labelled ? label : undefined"
    :aria-hidden="labelled ? undefined : 'true'"
    focusable="false"
  >
    <path v-for="path in paths" :key="path" :d="path" />
  </svg>
</template>
