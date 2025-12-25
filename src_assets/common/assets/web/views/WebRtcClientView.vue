<template>
  <div class="space-y-4">
    <div class="flex flex-col gap-2">
      <h2 class="text-2xl font-semibold tracking-tight">WebRTC Client</h2>
      <p class="text-sm opacity-80">
        WebRTC client using Sunshine's WebRTC session API.
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
          <n-switch v-model:value="resumeOnConnect" :disabled="!canResumeSelection">
            <template #checked>Resume running app</template>
            <template #unchecked>Resume running app</template>
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
              class="group text-left rounded-lg border bg-white/60 dark:bg-white/5 px-3 py-2 transition"
              :class="appCardClass(app)"
              @click="selectApp(app)"
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
          <div class="mt-3 flex items-center justify-between text-xs opacity-70">
            <span>{{ selectedAppLabel }}</span>
            <n-button size="tiny" tertiary @click="clearSelection" :disabled="!selectedAppId">
              Clear selection
            </n-button>
          </div>
        </n-card>

        <n-card title="Stream" size="small">
          <template #header-extra>
            <n-button size="small" tertiary @click="toggleFullscreen">
              {{ isFullscreen ? 'Exit fullscreen' : 'Enter fullscreen' }}
            </n-button>
          </template>
          <div
            ref="inputTarget"
            class="relative w-full overflow-hidden rounded-xl bg-slate-950"
            :class="isFullscreen ? 'h-full webrtc-fullscreen' : 'aspect-video'"
            tabindex="0"
            @dblclick="toggleFullscreen"
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

        <n-card title="Debug" size="small">
          <div class="space-y-2 text-xs">
            <div class="flex items-center justify-between">
              <span class="opacity-70">Session id</span>
              <span class="font-mono text-[11px] break-all">{{ displayValue(sessionId) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Server status</span>
              <span>{{ displayValue(serverSessionStatus) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Server updated</span>
              <span>{{ displayValue(serverSessionUpdatedAt) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Server packets</span>
              <span>
                V {{ displayValue(serverSession?.video_packets) }}
                / A {{ displayValue(serverSession?.audio_packets) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Server age ms</span>
              <span>
                V {{ displayValue(serverSession?.last_video_age_ms) }}
                / A {{ displayValue(serverSession?.last_audio_age_ms) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Server offer/answer</span>
              <span>
                R {{ displayValue(serverSession?.has_remote_offer) }}
                / L {{ displayValue(serverSession?.has_local_answer) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Server ICE</span>
              <span>{{ displayValue(serverSession?.ice_candidates) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Server video cfg</span>
              <span>{{ serverVideoConfigLabel }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Server audio cfg</span>
              <span>{{ serverAudioConfigLabel }}</span>
            </div>
            <div class="flex items-center justify-between" v-if="serverSessionError">
              <span class="opacity-70">Server error</span>
              <span>{{ serverSessionError }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Inbound bytes</span>
              <span>
                V {{ formatBytes(stats.videoBytesReceived) }}
                / A {{ formatBytes(stats.audioBytesReceived) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Inbound packets</span>
              <span>
                V {{ displayValue(stats.videoPacketsReceived) }}
                / A {{ displayValue(stats.audioPacketsReceived) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Video frames</span>
              <span>
                decoded {{ displayValue(stats.videoFramesDecoded) }}
                / dropped {{ displayValue(stats.videoFramesDropped) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Codec</span>
              <span>
                V {{ displayValue(stats.videoCodec) }}
                / A {{ displayValue(stats.audioCodec) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Candidate pair</span>
              <span class="text-[11px] break-all">{{ candidatePairLabel }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Remote stream</span>
              <span>{{ displayValue(remoteStreamInfo?.id) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Tracks</span>
              <span>
                V {{ displayValue(remoteStreamInfo?.videoTracks) }}
                / A {{ displayValue(remoteStreamInfo?.audioTracks) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Track state</span>
              <span>
                V {{ displayValue(trackDebug?.video?.readyState) }}/{{ displayValue(trackDebug?.video?.muted) }}
                / A {{ displayValue(trackDebug?.audio?.readyState) }}/{{ displayValue(trackDebug?.audio?.muted) }}
              </span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Last track</span>
              <span>{{ displayValue(lastTrackKind) }} @ {{ displayValue(lastTrackAt) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Video readyState</span>
              <span>{{ displayValue(videoDebug?.readyState) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Video size</span>
              <span>{{ videoSizeLabel }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Current time</span>
              <span>{{ formatSeconds(videoDebug?.currentTime) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Paused</span>
              <span>{{ displayValue(videoDebug?.paused) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Ended</span>
              <span>{{ displayValue(videoDebug?.ended) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Muted</span>
              <span>{{ displayValue(videoDebug?.muted) }}</span>
            </div>
            <div class="flex items-center justify-between">
              <span class="opacity-70">Volume</span>
              <span>{{ displayValue(videoDebug?.volume) }}</span>
            </div>
          </div>
          <div class="mt-3 space-y-1 text-xs">
            <div class="opacity-70">Video events</div>
            <div v-if="videoEvents.length" class="space-y-1">
              <div v-for="(event, idx) in videoEvents" :key="idx">
                {{ event }}
              </div>
            </div>
            <div v-else class="opacity-60">No events yet.</div>
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
import { WebRtcHttpApi } from '@/services/webrtcApi';
import { WebRtcClient } from '@/utils/webrtc/client';
import { attachInputCapture } from '@/utils/webrtc/input';
import { StreamConfig, WebRtcSessionState, WebRtcStatsSnapshot } from '@/types/webrtc';
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
const selectedAppId = ref<number | null>(null);
const resumeOnConnect = ref(true);
const canResumeSelection = computed(() => !selectedAppId.value);

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

function appNumericId(app: App): number | null {
  const raw = (app as any).id ?? (app as any).index;
  const parsed = Number(raw);
  return Number.isFinite(parsed) ? parsed : null;
}

function appCardClass(app: App): string {
  const id = appNumericId(app);
  const isSelected = id != null && id === selectedAppId.value;
  return [
    'border-white/10',
    'hover:border-primary/60',
    'hover:bg-white/80',
    'dark:hover:bg-white/10',
    isSelected ? 'border-primary/70 bg-white/90 dark:bg-white/10' : '',
  ]
    .filter(Boolean)
    .join(' ');
}

function selectApp(app: App) {
  const id = appNumericId(app);
  if (id == null) return;
  selectedAppId.value = id;
  resumeOnConnect.value = false;
}

function clearSelection() {
  selectedAppId.value = null;
  resumeOnConnect.value = true;
}

const selectedAppLabel = computed(() => {
  if (!selectedAppId.value) return 'No app selected (resume running app).';
  const selected = appsList.value.find((app) => appNumericId(app) === selectedAppId.value);
  return selected?.name ? `Selected: ${selected.name}` : `Selected app id: ${selectedAppId.value}`;
});

const api = new WebRtcHttpApi();
const client = new WebRtcClient(api);

const isConnecting = ref(false);
const isConnected = ref(false);
const connectionState = ref<RTCPeerConnectionState | null>(null);
const iceState = ref<RTCIceConnectionState | null>(null);
const inputChannelState = ref<RTCDataChannelState | null>(null);
const stats = ref<WebRtcStatsSnapshot>({});
const inputEnabled = ref(true);
const inputTarget = ref<HTMLElement | null>(null);
const videoEl = ref<HTMLVideoElement | null>(null);
const isFullscreen = ref(false);
const sessionId = ref<string | null>(null);
const serverSession = ref<WebRtcSessionState | null>(null);
const serverSessionUpdatedAt = ref<string | null>(null);
const serverSessionError = ref<string | null>(null);
const serverSessionStatus = ref<number | null>(null);
const remoteStreamInfo = ref<{ id: string; videoTracks: number; audioTracks: number } | null>(null);
const lastTrackAt = ref<string | null>(null);
const lastTrackKind = ref<string | null>(null);
const videoEvents = ref<string[]>([]);
const videoStateTick = ref(0);
const videoDebug = computed(() => {
  void videoStateTick.value;
  const el = videoEl.value;
  if (!el) return null;
  return {
    readyState: el.readyState,
    width: el.videoWidth,
    height: el.videoHeight,
    currentTime: el.currentTime,
    paused: el.paused,
    ended: el.ended,
    muted: el.muted,
    volume: el.volume,
  };
});
const videoSizeLabel = computed(() => {
  const width = videoDebug.value?.width ?? 0;
  const height = videoDebug.value?.height ?? 0;
  return width > 0 && height > 0 ? `${width}x${height}` : '--';
});
const serverVideoConfigLabel = computed(() => {
  if (!serverSession.value) return '--';
  const width = serverSession.value.width ?? 0;
  const height = serverSession.value.height ?? 0;
  const fps = serverSession.value.fps ?? null;
  const codec = serverSession.value.codec ?? null;
  const base = width > 0 && height > 0 ? `${width}x${height}` : '--';
  const rate = fps ? `${fps}fps` : '--';
  const name = codec ? codec.toUpperCase() : '--';
  return `${base} ${rate} ${name}`;
});
const serverAudioConfigLabel = computed(() => {
  if (!serverSession.value) return '--';
  const codec = serverSession.value.audio_codec ?? null;
  const channels = serverSession.value.audio_channels ?? null;
  const codecLabel = codec ? codec.toUpperCase() : '--';
  const channelLabel = channels ? `${channels}ch` : '--';
  return `${codecLabel} ${channelLabel}`;
});
const trackDebug = computed(() => {
  const stream = videoEl.value?.srcObject;
  if (!stream || !(stream instanceof MediaStream)) return null;
  const videoTrack = stream.getVideoTracks()[0];
  const audioTrack = stream.getAudioTracks()[0];
  return {
    video: videoTrack
      ? { readyState: videoTrack.readyState, muted: videoTrack.muted, enabled: videoTrack.enabled }
      : null,
    audio: audioTrack
      ? { readyState: audioTrack.readyState, muted: audioTrack.muted, enabled: audioTrack.enabled }
      : null,
  };
});
const candidatePairLabel = computed(() => {
  const pair = stats.value.candidatePair;
  if (!pair) return '--';
  const local = pair.localAddress ? `${pair.localAddress}:${pair.localPort ?? ''}` : '--';
  const remote = pair.remoteAddress ? `${pair.remoteAddress}:${pair.remotePort ?? ''}` : '--';
  const proto = pair.protocol ? pair.protocol.toUpperCase() : '--';
  const types = pair.localType && pair.remoteType ? `${pair.localType}/${pair.remoteType}` : '--';
  const state = pair.state ?? '--';
  return `${local} -> ${remote} (${proto}, ${types}, ${state})`;
});
let detachInput: (() => void) | null = null;
let detachVideoEvents: (() => void) | null = null;
let lastTrackSnapshot: { video: number; audio: number } | null = null;
let serverSessionTimer: number | null = null;
const onFullscreenChange = () => {
  isFullscreen.value = document.fullscreenElement === inputTarget.value;
};

function formatKbps(value?: number): string {
  return value ? `${value.toFixed(0)} kbps` : '--';
}

function formatBytes(value?: number): string {
  if (value == null) return '--';
  if (value < 1024) return `${value} B`;
  const kb = value / 1024;
  if (kb < 1024) return `${kb.toFixed(1)} KB`;
  const mb = kb / 1024;
  return `${mb.toFixed(2)} MB`;
}

function formatSeconds(value?: number): string {
  if (value == null) return '--';
  return value.toFixed(2);
}

function displayValue(value: unknown): string {
  if (value === null || value === undefined || value === '') return '--';
  return String(value);
}

function pushVideoEvent(label: string): void {
  const stamp = new Date().toLocaleTimeString();
  videoEvents.value = [`${stamp} ${label}`, ...videoEvents.value].slice(0, 8);
  videoStateTick.value += 1;
}

function updateRemoteStreamInfo(stream: MediaStream): void {
  const info = {
    id: stream.id || '',
    videoTracks: stream.getVideoTracks().length,
    audioTracks: stream.getAudioTracks().length,
  };
  if (lastTrackSnapshot) {
    if (info.videoTracks > lastTrackSnapshot.video) {
      lastTrackKind.value = 'video';
    } else if (info.audioTracks > lastTrackSnapshot.audio) {
      lastTrackKind.value = 'audio';
    } else {
      lastTrackKind.value = 'unknown';
    }
  } else {
    lastTrackKind.value = info.videoTracks ? 'video' : info.audioTracks ? 'audio' : 'unknown';
  }
  lastTrackAt.value = new Date().toLocaleTimeString();
  lastTrackSnapshot = { video: info.videoTracks, audio: info.audioTracks };
  remoteStreamInfo.value = info;
}

function stopServerSessionPolling(): void {
  if (serverSessionTimer) {
    window.clearInterval(serverSessionTimer);
    serverSessionTimer = null;
  }
}

async function fetchServerSession(): Promise<void> {
  if (!sessionId.value) return;
  try {
    const result = await api.getSessionState(sessionId.value);
    serverSessionStatus.value = result.status;
    if (result.session) {
      serverSession.value = result.session;
      serverSessionUpdatedAt.value = new Date().toLocaleTimeString();
      serverSessionError.value = null;
    } else {
      serverSessionError.value = result.error ?? `HTTP ${result.status}`;
    }
  } catch {
    serverSessionStatus.value = null;
    serverSessionError.value = 'fetch failed';
  }
}

function startServerSessionPolling(): void {
  stopServerSessionPolling();
  if (!sessionId.value) return;
  void fetchServerSession();
  serverSessionTimer = window.setInterval(fetchServerSession, 1000);
}

function attachVideoDebug(el: HTMLVideoElement): () => void {
  const events = [
    'loadedmetadata',
    'loadeddata',
    'canplay',
    'canplaythrough',
    'playing',
    'pause',
    'waiting',
    'stalled',
    'emptied',
    'ended',
    'error',
  ];
  const listeners = new Map<string, () => void>();
  events.forEach((eventName) => {
    const handler = () => pushVideoEvent(eventName);
    el.addEventListener(eventName, handler);
    listeners.set(eventName, handler);
  });
  return () => {
    listeners.forEach((handler, eventName) => {
      el.removeEventListener(eventName, handler);
    });
  };
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
  stopServerSessionPolling();
  sessionId.value = null;
  serverSession.value = null;
  serverSessionUpdatedAt.value = null;
  serverSessionError.value = null;
  serverSessionStatus.value = null;
  try {
    const id = await client.connect({
      ...config,
      appId: selectedAppId.value ?? undefined,
      resume: selectedAppId.value ? false : resumeOnConnect.value,
    }, {
      onRemoteStream: (stream) => {
        if (videoEl.value) {
          videoEl.value.srcObject = stream;
          updateRemoteStreamInfo(stream);
          const playPromise = videoEl.value.play();
          if (playPromise && typeof playPromise.catch === 'function') {
            playPromise.catch((error) => {
              const name = error && typeof error === 'object' ? (error as any).name : '';
              pushVideoEvent(`play-error${name ? `:${name}` : ''}`);
            });
          }
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
    sessionId.value = id;
    startServerSessionPolling();
  } catch (error) {
    const msg = error instanceof Error ? error.message : 'Failed to establish WebRTC session.';
    message.error(msg);
    console.error(error);
  } finally {
    isConnecting.value = false;
  }
}

async function disconnect() {
  await client.disconnect();
  stopServerSessionPolling();
  isConnected.value = false;
  connectionState.value = null;
  iceState.value = null;
  inputChannelState.value = null;
  stats.value = {};
  detachInputCapture();
  if (videoEl.value) videoEl.value.srcObject = null;
  sessionId.value = null;
  serverSession.value = null;
  serverSessionUpdatedAt.value = null;
  serverSessionError.value = null;
  serverSessionStatus.value = null;
  remoteStreamInfo.value = null;
  lastTrackAt.value = null;
  lastTrackKind.value = null;
  lastTrackSnapshot = null;
  videoEvents.value = [];
  videoStateTick.value += 1;
}

async function toggleFullscreen() {
  try {
    if (document.fullscreenElement) {
      await document.exitFullscreen();
      return;
    }
    if (!inputTarget.value) return;
    await inputTarget.value.requestFullscreen();
  } catch {
    /* ignore */
  }
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

watch(videoEl, (el) => {
  if (detachVideoEvents) {
    detachVideoEvents();
    detachVideoEvents = null;
  }
  if (!el) return;
  detachVideoEvents = attachVideoDebug(el);
});

onBeforeUnmount(() => {
  document.removeEventListener('fullscreenchange', onFullscreenChange);
  if (detachVideoEvents) {
    detachVideoEvents();
    detachVideoEvents = null;
  }
  stopServerSessionPolling();
  void disconnect();
});

onMounted(async () => {
  document.addEventListener('fullscreenchange', onFullscreenChange);
  try {
    await appsStore.loadApps(true);
  } catch {
    /* ignore */
  }
});
</script>

<style scoped>
.webrtc-fullscreen {
  cursor: none;
}
</style>
