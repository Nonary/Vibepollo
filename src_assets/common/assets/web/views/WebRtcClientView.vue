<template>
  <div class="space-y-4">
    <div class="flex flex-col gap-2">
      <h2 class="text-2xl font-semibold tracking-tight">WebRTC Client</h2>
      <p class="text-sm opacity-80">
        Prototype web client using a placeholder Web API and mock server-side WebRTC stream.
      </p>
    </div>

    <div class="grid gap-4 lg:grid-cols-[320px_minmax(0,1fr)]">
      <n-card title="Session Settings" size="small">
        <n-form :label-width="90" size="small">
          <n-form-item label="Resolution">
            <div class="flex items-center gap-2">
              <n-input-number v-model:value="config.width" :min="320" :max="7680" />
              <span class="text-xs opacity-70">x</span>
              <n-input-number v-model:value="config.height" :min="180" :max="4320" />
            </div>
          </n-form-item>
          <n-form-item label="Framerate">
            <n-input-number v-model:value="config.fps" :min="1" :max="240" />
          </n-form-item>
          <n-form-item label="Encoding">
            <n-select v-model:value="config.encoding" :options="encodingOptions" />
          </n-form-item>
          <n-form-item label="Bitrate">
            <n-input-number
              v-model:value="config.bitrateKbps"
              :min="500"
              :max="200000"
              placeholder="kbps"
            />
          </n-form-item>
        </n-form>
        <div class="flex flex-col gap-2 mt-3">
          <div class="flex gap-2">
            <n-button type="primary" :loading="isConnecting" @click="connect">
              {{ isConnected ? 'Reconnect' : 'Connect' }}
            </n-button>
            <n-button :disabled="!isConnected" @click="disconnect">Disconnect</n-button>
          </div>
          <n-switch v-model:value="inputEnabled" :disabled="!isConnected">
            <template #checked>Input capture on</template>
            <template #unchecked>Input capture off</template>
          </n-switch>
        </div>
        <div class="mt-4 space-y-2 text-xs">
          <div class="flex items-center gap-2">
            <span class="opacity-70">Connection</span>
            <n-tag size="small" :type="statusTagType(connectionState)">
              {{ connectionState || 'idle' }}
            </n-tag>
          </div>
          <div class="flex items-center gap-2">
            <span class="opacity-70">ICE</span>
            <n-tag size="small" :type="statusTagType(iceState)">
              {{ iceState || 'idle' }}
            </n-tag>
          </div>
          <div class="flex items-center gap-2">
            <span class="opacity-70">Input channel</span>
            <n-tag size="small" :type="statusTagType(inputChannelState)">
              {{ inputChannelState || 'closed' }}
            </n-tag>
          </div>
        </div>
      </n-card>

      <div class="space-y-4">
        <n-card title="Applications" size="small">
          <div v-if="appsList.length" class="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
            <button
              v-for="app in appsList"
              :key="appKey(app)"
              class="group text-left rounded-lg border border-white/10 bg-white/60 dark:bg-white/5 px-3 py-2 hover:border-primary/60 hover:bg-white/80 dark:hover:bg-white/10 transition"
            >
              <div class="flex items-center gap-3">
                <img
                  :src="coverUrl(app) || undefined"
                  :alt="app.name || 'Application'"
                  class="h-14 w-10 rounded-md object-cover shadow-sm"
                  loading="lazy"
                />
                <div class="min-w-0">
                  <div class="text-sm font-semibold truncate">
                    {{ app.name || '(untitled)' }}
                  </div>
                  <div class="text-xs opacity-70">{{ appSubtitle(app) }}</div>
                </div>
              </div>
            </button>
          </div>
          <div v-else class="text-sm opacity-70">No applications configured.</div>
        </n-card>

        <n-card title="Stream" size="small">
          <div
            ref="inputTarget"
            class="relative aspect-video w-full overflow-hidden rounded-xl bg-slate-950"
            tabindex="0"
          >
            <video
              ref="videoEl"
              class="h-full w-full object-contain"
              autoplay
              playsinline
              muted
            ></video>
            <div
              v-if="!isConnected"
              class="absolute inset-0 flex flex-col items-center justify-center gap-2 text-sm text-white/80"
            >
              <i class="fas fa-satellite-dish text-xl"></i>
              <span>Connect to start the mock WebRTC stream.</span>
            </div>
          </div>
        </n-card>

        <n-card title="Live Stats" size="small">
          <div class="grid gap-3 text-sm md:grid-cols-2">
            <div class="flex items-center justify-between">
              <span class="opacity-70">Video bitrate</span>
              <span>{{ formatKbps(stats.videoBitrateKbps) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Audio bitrate</span>
              <span>{{ formatKbps(stats.audioBitrateKbps) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Video FPS</span>
              <span>{{ stats.videoFps ? stats.videoFps.toFixed(0) : '--' }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Packets lost</span>
              <span>{{ stats.packetsLost ?? '--' }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Round-trip time</span>
              <span>{{ stats.roundTripTimeMs ? `${stats.roundTripTimeMs.toFixed(0)} ms` : '--' }}</span>
            </div>
          </div>
        </n-card>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onBeforeUnmount, onMounted, watch, computed } from 'vue';
import {
  NCard,
  NButton,
  NSelect,
  NInputNumber,
  NTag,
  NSwitch,
  NForm,
  NFormItem,
  useMessage,
} from 'naive-ui';
import { MockWebRtcApi } from '@/services/webrtcApi';
import { WebRtcClient } from '@/utils/webrtc/client';
import { attachInputCapture } from '@/utils/webrtc/input';
import { StreamConfig, WebRtcStatsSnapshot } from '@/types/webrtc';
import { useAppsStore } from '@/stores/apps';
import { storeToRefs } from 'pinia';
import type { App } from '@/stores/apps';

const message = useMessage();

const encodingOptions = [
  { label: 'H.264', value: 'h264' },
  { label: 'HEVC', value: 'hevc' },
  { label: 'AV1', value: 'av1' },
];

const config = reactive<StreamConfig>({
  width: 1920,
  height: 1080,
  fps: 60,
  encoding: 'h264',
  bitrateKbps: 20000,
});

const appsStore = useAppsStore();
const { apps } = storeToRefs(appsStore);
const appsList = computed(() => (apps.value || []).slice());

function appKey(app: App): string {
  return `${app.uuid || ''}-${app.name || 'app'}`;
}

function coverUrl(app: App): string {
  if (!app.uuid) return '';
  return `/api/apps/${encodeURIComponent(app.uuid)}/cover`;
}

function appSubtitle(app: App): string {
  if (app['playnite-id']) return 'Playnite';
  if (app['working-dir']) return String(app['working-dir']);
  return 'Custom';
}

const api = new MockWebRtcApi();
const client = new WebRtcClient(api);

const isConnecting = ref(false);
const isConnected = ref(false);
const connectionState = ref<RTCPeerConnectionState | null>(null);
const iceState = ref<RTCIceConnectionState | null>(null);
const inputChannelState = ref<RTCDataChannelState | null>(null);
const stats = ref<WebRtcStatsSnapshot>({});
const inputEnabled = ref(false);
const inputTarget = ref<HTMLElement | null>(null);
const videoEl = ref<HTMLVideoElement | null>(null);
let detachInput: (() => void) | null = null;

function formatKbps(value?: number): string {
  return value ? `${value.toFixed(0)} kbps` : '--';
}

function statusTagType(state?: string | null) {
  if (!state) return 'default';
  if (state === 'connected' || state === 'completed' || state === 'open') return 'success';
  if (state === 'connecting' || state === 'checking') return 'warning';
  if (state === 'failed' || state === 'disconnected' || state === 'closed') return 'error';
  return 'default';
}

async function connect() {
  isConnecting.value = true;
  try {
    await client.connect({ ...config }, {
      onRemoteStream: (stream) => {
        if (videoEl.value) {
          videoEl.value.srcObject = stream;
          void videoEl.value.play();
        }
      },
      onConnectionState: (state) => {
        connectionState.value = state;
        isConnected.value = state === 'connected';
      },
      onIceState: (state) => {
        iceState.value = state;
      },
      onInputChannelState: (state) => {
        inputChannelState.value = state;
      },
      onStats: (snapshot) => {
        stats.value = snapshot;
      },
    });
  } catch (error) {
    message.error('Failed to establish WebRTC session.');
    console.error(error);
  } finally {
    isConnecting.value = false;
  }
}

async function disconnect() {
  await client.disconnect();
  isConnected.value = false;
  connectionState.value = null;
  iceState.value = null;
  inputChannelState.value = null;
  stats.value = {};
  detachInputCapture();
  if (videoEl.value) videoEl.value.srcObject = null;
}

function detachInputCapture() {
  if (detachInput) {
    detachInput();
    detachInput = null;
  }
}

watch(
  () => [inputEnabled.value, isConnected.value],
  ([enabled, connected]) => {
    detachInputCapture();
    if (!enabled || !connected || !inputTarget.value) return;
    detachInput = attachInputCapture(inputTarget.value, (payload) => {
      client.sendInput(payload);
    });
  },
);

onBeforeUnmount(() => {
  void disconnect();
});

onMounted(async () => {
  try {
    await appsStore.loadApps(true);
  } catch {
    /* ignore */
  }
});
</script>
