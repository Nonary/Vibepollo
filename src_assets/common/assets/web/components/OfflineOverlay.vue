<template>
  <transition name="fade-fast">
    <div v-if="visible" class="fixed inset-0 z-[140] flex flex-col">
      <div
        class="absolute inset-0 bg-gradient-to-br from-white/70 via-white/60 to-white/70 dark:from-black/70 dark:via-black/60 dark:to-black/70 backdrop-blur-md"
      ></div>
      <div class="relative flex-1 flex flex-col items-center justify-center p-6 overflow-y-auto">
        <div class="w-full max-w-md mx-auto text-center space-y-6">
          <div>
            <svg
              class="h-24 w-24 opacity-80 mx-auto select-none"
              viewBox="0 0 256 256"
              xmlns="http://www.w3.org/2000/svg"
              aria-hidden="true"
            >
              <path
                fill="#FDD107"
                d="M118.769 20.712s-63.833 26-74.333 83.833 37.167 91.5 86.333 75.333 70.333-51 81.833-87c0 0-9.333 100.5-96.167 115.5s-118.167-50-82.167-119.833C44.269 67.045 80.519 29.629 118.769 20.712z"
              />
              <path
                fill="#F89A1C"
                d="M118.769 20.712s-41.125 3.667-83.25 61.042-28.125 139.125 34.25 149.375 115.875-44.875 133.5-82.375 15.167-61.458 9.75-77.875c0 0 .667 36.417-13.333 59.667s-29.75 46.333-65.083 62.167-74.167 13.75-95.417-19.25-5.917-76.083-.292-85.333S72.394 33.795 118.769 20.712z"
              />
              <path
                fill="#EF3E23"
                d="M73.019 39.629s38.125-28.125 76.875-28.125 63 28.25 68.5 52.25 6 54.125-11.5 87.625-37.375 56-79.125 76.125-84.625 2.75-84.625 2.75 25.977 25.875 71.051 16.5 82.241-40.875 98.408-69.5 28.792-57.375 27.667-92.25-23.75-54.5-31.25-60.25-23.187-17.812-58.187-16.562S86.456 29.816 73.019 39.629z"
              />
              <path
                fill="#F26222"
                d="M73.019 39.629s35-32.813 82.437-32.813 69.188 24.813 78.875 44.688 21.812 70-12.188 123S147.518 242.879 128.518 247.129s-42.431 4.269-59.111-1.23c0 0 35.195 8.397 66.778-7.437s51.667-32.167 74.083-68.834 25.917-72.75 22.167-93.917-12.167-42.417-36.5-56.333-56.729-10.531-74.479-4.531S91.988 26.41 73.019 39.629z"
              />
            </svg>
          </div>
          <div class="space-y-2">
            <h2 class="text-2xl font-semibold tracking-tight">
              {{ $t('offline.title') }}
            </h2>
            <p class="text-sm opacity-80 leading-relaxed">
              {{ $t('offline.description') }}
            </p>
            <p class="text-xs opacity-70 leading-relaxed">
              {{ $t('offline.retrying') }}
            </p>
          </div>
          <p class="mt-4 text-[12px] opacity-75 select-none">
            {{ $t('offline.close_hint') }}
          </p>
        </div>
      </div>
    </div>
  </transition>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useConnectivityStore } from '@/stores/connectivity';

const connectivity = useConnectivityStore();
const visible = computed(() => connectivity.overlayVisible);
</script>
