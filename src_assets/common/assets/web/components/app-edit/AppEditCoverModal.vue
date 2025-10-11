<template>
  <n-modal
    :show="visible"
    :z-index="3300"
    :mask-style="{ backgroundColor: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }"
    @update:show="(v) => emit('update:visible', v)"
  >
    <n-card :bordered="false" style="max-width: 48rem; width: 100%">
      <template #header>
        <div class="flex items-center justify-between w-full">
          <span class="font-semibold">Covers Found</span>
          <n-button type="default" strong size="small" @click="emit('update:visible', false)">
            Close
          </n-button>
        </div>
      </template>
      <div class="min-h-[160px]">
        <div v-if="coverSearching" class="flex items-center justify-center py-10">
          <n-spin size="large">Loading…</n-spin>
        </div>
        <div v-else>
          <div
            class="grid grid-cols-2 sm:grid-cols-3 md:grid-cols-4 gap-3 max-h-[420px] overflow-auto pr-1"
          >
            <div
              v-for="(cover, i) in coverCandidates"
              :key="i"
              class="cursor-pointer group"
              @click="emit('pick', cover)"
            >
              <div
                class="relative rounded overflow-hidden aspect-[3/4] bg-black/5 dark:bg-white/5"
              >
                <img :src="cover.url" class="absolute inset-0 w-full h-full object-cover" />
                <div
                  v-if="coverBusy"
                  class="absolute inset-0 bg-black/20 dark:bg-white/10 flex items-center justify-center"
                >
                  <n-spin size="small" />
                </div>
              </div>
              <div class="mt-1 text-xs text-center truncate" :title="cover.name">
                {{ cover.name }}
              </div>
            </div>
            <div v-if="!coverCandidates.length" class="col-span-full text-center opacity-70 py-8">
              No results. Try adjusting the app name.
            </div>
          </div>
        </div>
      </div>
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { toRefs } from 'vue';
import { NModal, NCard, NButton, NSpin } from 'naive-ui';

export interface CoverCandidate {
  name: string;
  key: string;
  url: string;
  saveUrl: string;
}

const rawProps = defineProps<{
  visible: boolean;
  coverSearching: boolean;
  coverBusy: boolean;
  coverCandidates: CoverCandidate[];
}>();

const emit = defineEmits<{
  (e: 'update:visible', value: boolean): void;
  (e: 'pick', cover: CoverCandidate): void;
}>();

const { visible, coverSearching, coverBusy, coverCandidates } = toRefs(rawProps);
</script>
