<template>
  <div class="webrtc-gaming-container">
    <!-- Hero Header -->
    <div class="gaming-header px-6 py-8 mb-6">
      <div class="max-w-7xl mx-auto">
        <div class="flex items-center gap-4 mb-2">
          <div class="w-12 h-12 rounded-xl bg-gradient-to-br from-primary to-secondary flex items-center justify-center shadow-lg shadow-primary/30">
            <i class="fas fa-gamepad text-2xl text-onPrimary"></i>
          </div>
          <div>
            <h1 class="text-3xl font-bold tracking-tight text-onDark">
              {{ $t('webrtc.title') }}
            </h1>
            <p class="text-sm text-onDark/60">{{ $t('webrtc.subtitle') }}</p>
          </div>
        </div>
        <!-- Connection Status Bar -->
        <div class="flex items-center gap-4 mt-4">
          <div class="flex items-center gap-2 px-3 py-1.5 rounded-full text-xs font-medium"
               :class="connectionStatusClass">
            <span class="w-2 h-2 rounded-full animate-pulse" :class="connectionDotClass"></span>
            <span>{{ connectionStatusLabel }}</span>
          </div>
          <div v-if="isConnected" class="flex items-center gap-3 text-xs text-onDark/50">
            <span><i class="fas fa-signal mr-1"></i>{{ formatKbps(stats.videoBitrateKbps) }}</span>
            <span><i class="fas fa-clock mr-1"></i>{{ stats.roundTripTimeMs ? `${stats.roundTripTimeMs.toFixed(0)}ms` : '--' }}</span>
            <span><i class="fas fa-film mr-1"></i>{{ stats.videoFps ? `${stats.videoFps.toFixed(0)} FPS` : '--' }}</span>
          </div>
        </div>
      </div>
    </div>

    <div class="max-w-7xl mx-auto px-6">
      <!-- Main Content Grid -->
      <div class="grid gap-6 lg:grid-cols-[380px_minmax(0,1fr)]">
        
        <!-- Left Sidebar - Settings & Games -->
        <div class="space-y-5">
          <!-- Quick Settings Card -->
          <div class="gaming-card p-5">
            <div class="flex items-center gap-2 mb-4">
              <i class="fas fa-sliders text-primary"></i>
              <h3 class="font-semibold text-onDark">{{ $t('webrtc.session_settings') }}</h3>
            </div>
            
            <div class="space-y-4">
              <!-- Resolution -->
              <div>
                <label class="text-xs text-onDark/50 uppercase tracking-wide mb-1.5 block">{{ $t('webrtc.resolution') }}</label>
                <div class="flex items-center gap-2">
                  <n-input-number v-model:value="config.width" :min="320" :max="7680" size="small" class="flex-1" />
                  <span class="text-onDark/30">×</span>
                  <n-input-number v-model:value="config.height" :min="180" :max="4320" size="small" class="flex-1" />
                </div>
                <div class="flex gap-1.5 mt-2">
                  <button @click="setResolution(1920, 1080)" class="preset-btn" :class="{ active: config.width === 1920 && config.height === 1080 }">1080p</button>
                  <button @click="setResolution(2560, 1440)" class="preset-btn" :class="{ active: config.width === 2560 && config.height === 1440 }">1440p</button>
                  <button @click="setResolution(3840, 2160)" class="preset-btn" :class="{ active: config.width === 3840 && config.height === 2160 }">4K</button>
                </div>
              </div>

              <!-- Framerate -->
              <div>
                <label class="text-xs text-onDark/50 uppercase tracking-wide mb-1.5 block">{{ $t('webrtc.framerate') }}</label>
                <div class="flex gap-1.5">
                  <button @click="config.fps = 30" class="preset-btn flex-1" :class="{ active: config.fps === 30 }">30</button>
                  <button @click="config.fps = 60" class="preset-btn flex-1" :class="{ active: config.fps === 60 }">60</button>
                  <button @click="config.fps = 120" class="preset-btn flex-1" :class="{ active: config.fps === 120 }">120</button>
                  <button @click="config.fps = 144" class="preset-btn flex-1" :class="{ active: config.fps === 144 }">144</button>
                </div>
              </div>

              <!-- Encoding -->
              <div>
                <label class="text-xs text-onDark/50 uppercase tracking-wide mb-1.5 block">{{ $t('webrtc.encoding') }}</label>
                <div class="flex gap-1.5">
                  <button v-for="opt in encodingOptions" :key="opt.value"
                          @click="opt.supported && (config.encoding = opt.value)"
                          class="preset-btn flex-1"
                          :class="{ active: config.encoding === opt.value && opt.supported, disabled: !opt.supported }"
                          :disabled="!opt.supported"
                          :title="opt.supported ? undefined : opt.hint">
                    <span>{{ opt.label }}</span>
                    <span v-if="!opt.supported" class="block text-[9px] leading-tight text-onDark/40">Unsupported</span>
                  </button>
                </div>
              </div>

              <!-- Bitrate -->
              <div>
                <label class="text-xs text-onDark/50 uppercase tracking-wide mb-1.5 block">{{ $t('webrtc.bitrate') }}</label>
                <n-input-number v-model:value="config.bitrateKbps" :min="500" :max="200000" size="small" class="w-full" />
                <div class="flex gap-1.5 mt-2">
                  <button @click="config.bitrateKbps = 10000" class="preset-btn flex-1" :class="{ active: config.bitrateKbps === 10000 }">10 Mbps</button>
                  <button @click="config.bitrateKbps = 30000" class="preset-btn flex-1" :class="{ active: config.bitrateKbps === 30000 }">30 Mbps</button>
                  <button @click="config.bitrateKbps = 60000" class="preset-btn flex-1" :class="{ active: config.bitrateKbps === 60000 }">60 Mbps</button>
                </div>
              </div>

              <!-- Frame Pacing -->
              <div>
                <label class="text-xs text-onDark/50 uppercase tracking-wide mb-1.5 block">{{ $t('webrtc.frame_pacing') }}</label>
                <div class="flex gap-1.5">
                  <button v-for="opt in pacingOptions" :key="opt.value"
                          @click="applyPacingPreset(opt.value)"
                          class="preset-btn flex-1"
                          :class="{ active: config.videoPacingMode === opt.value }">
                    {{ opt.label }}
                  </button>
                </div>
                <div class="text-[11px] text-onDark/40 mt-1">{{ $t('webrtc.frame_pacing_desc') }}</div>
                <div class="grid grid-cols-2 gap-2 mt-2">
                  <div>
                    <label class="text-[10px] text-onDark/50 uppercase tracking-wide mb-1 block">
                      {{ $t('webrtc.frame_pacing_slack') }}
                    </label>
                    <n-input-number v-model:value="config.videoPacingSlackMs" :min="0" :max="10" size="small" />
                  </div>
                  <div>
                    <label class="text-[10px] text-onDark/50 uppercase tracking-wide mb-1 block">
                      {{ $t('webrtc.frame_pacing_max_delay') }}
                    </label>
                    <n-input-number v-model:value="config.videoMaxFrameAgeMs" :min="5" :max="250" size="small" />
                  </div>
                </div>
              </div>

              <!-- Host Audio -->
              <div>
                <div class="flex items-center justify-between">
                  <label class="text-xs text-onDark/50 uppercase tracking-wide">{{ $t('webrtc.mute_host_audio') }}</label>
                  <n-switch v-model:value="config.muteHostAudio" size="small" />
                </div>
                <div class="text-[11px] text-onDark/40 mt-1">{{ $t('webrtc.mute_host_audio_desc') }}</div>
              </div>
            </div>

            <!-- Connect Button -->
            <div class="mt-6 space-y-3">
              <button @click="isConnected ? disconnect() : connect()" 
                      class="w-full py-3 px-4 rounded-xl font-semibold text-sm transition-all duration-200"
                      :class="isConnected 
                        ? 'bg-danger/20 text-danger hover:bg-danger/30 border border-danger/30' 
                        : 'bg-gradient-to-r from-primary to-secondary text-onPrimary hover:shadow-lg hover:shadow-primary/30 hover:-translate-y-0.5'"
                      :disabled="isConnecting">
                <i :class="isConnected ? 'fas fa-stop' : isConnecting ? 'fas fa-spinner fa-spin' : 'fas fa-play'" class="mr-2"></i>
                {{ $t(connectLabelKey) }}
              </button>

              <button v-if="isConnected"
                      @click="terminateSession"
                      class="w-full py-2 px-4 rounded-xl text-xs font-semibold transition-all duration-200 border border-warning/40 text-warning hover:bg-warning/15"
                      :disabled="isConnecting || terminatePending"
                      :title="$t('webrtc.terminate_desc')">
                <i :class="terminatePending ? 'fas fa-spinner fa-spin' : 'fas fa-ban'" class="mr-2"></i>
                {{ $t('webrtc.terminate') }}
              </button>
              
              <div class="flex items-center justify-between text-xs">
                <label class="flex items-center gap-2 cursor-pointer text-onDark/60 hover:text-onDark/80">
                  <n-switch v-model:value="inputEnabled" :disabled="!isConnected" size="small" />
                  <span>Input capture</span>
                </label>
                <label class="flex items-center gap-2 cursor-pointer text-onDark/60 hover:text-onDark/80">
                  <n-switch v-model:value="showOverlay" size="small" />
                  <span>Overlay</span>
                </label>
                <label class="flex items-center gap-2 cursor-pointer text-onDark/60 hover:text-onDark/80">
                  <n-switch v-model:value="autoFullscreen" size="small" />
                  <span>Auto fullscreen</span>
                </label>
              </div>
            </div>
          </div>

          <!-- Game Selection Card -->
          <div class="gaming-card p-5">
            <div class="flex items-center justify-between mb-4">
              <div class="flex items-center gap-2">
                <i class="fas fa-gamepad text-primary"></i>
                <h3 class="font-semibold text-onDark">{{ $t('webrtc.select_game') }}</h3>
              </div>
              <button v-if="selectedAppId" @click="clearSelection" 
                      class="text-xs text-onDark/40 hover:text-onDark/70 transition">
                <i class="fas fa-times mr-1"></i>Clear
              </button>
            </div>

            <div class="text-xs text-onDark/40 mb-3 px-1">
              <i class="fas fa-info-circle mr-1"></i>
              {{ selectedAppId ? selectedAppLabel : $t('webrtc.no_selection') }}
            </div>

            <div v-if="appsList.length" class="grid gap-2 max-h-[320px] overflow-y-auto pr-1 custom-scrollbar">
              <button v-for="app in appsList" :key="appKey(app)"
                      @click="selectApp(app)"
                      class="game-card group"
                      :class="{ 'selected': appNumericId(app) === selectedAppId }">
                <div class="relative w-12 h-16 rounded-lg overflow-hidden flex-shrink-0 bg-surface/30">
                  <img :src="coverUrl(app) || undefined" :alt="app.name || 'Application'"
                       class="w-full h-full object-cover transition-transform group-hover:scale-110" loading="lazy" />
                  <div class="absolute inset-0 bg-gradient-to-t from-dark/60 to-transparent"></div>
                </div>
                <div class="min-w-0 flex-1 text-left">
                  <div class="font-medium text-sm truncate text-onDark">{{ app.name || '(untitled)' }}</div>
                  <div class="text-xs text-onDark/40 truncate">{{ appSubtitle(app) }}</div>
                </div>
                <i v-if="appNumericId(app) === selectedAppId" class="fas fa-check-circle text-primary"></i>
              </button>
            </div>
            <div v-else class="text-sm text-onDark/40 text-center py-8">
              <i class="fas fa-folder-open text-2xl mb-2 block opacity-50"></i>
              No applications configured
            </div>
          </div>
        </div>

        <!-- Right Side - Stream View -->
        <div class="space-y-5">
          <!-- Stream Card -->
          <div class="gaming-card overflow-hidden">
            <div class="flex items-center justify-between p-4 border-b border-surface/30">
              <div class="flex items-center gap-2">
                <i class="fas fa-display text-primary"></i>
                <h3 class="font-semibold text-onDark">Stream</h3>
                <span v-if="isConnected" class="px-2 py-0.5 rounded-full text-[10px] font-medium bg-success/20 text-success uppercase tracking-wide">Live</span>
              </div>
              <button @click="toggleFullscreen" 
                      class="p-2 rounded-lg hover:bg-surface/50 text-onDark/60 hover:text-onDark transition">
                <i :class="isFullscreen ? 'fas fa-compress' : 'fas fa-expand'"></i>
              </button>
            </div>
            
            <div ref="inputTarget"
                 class="relative w-full bg-dark"
                 :class="isFullscreen ? 'h-full webrtc-fullscreen' : 'aspect-video'"
                 tabindex="0"
                 @dblclick="onFullscreenDblClick">
              <video ref="videoEl" class="h-full w-full object-contain" autoplay playsinline></video>
              <audio ref="audioEl" class="hidden" autoplay playsinline></audio>
              
              <!-- Idle State Overlay -->
              <div v-if="!isConnected" class="absolute inset-0 flex flex-col items-center justify-center bg-gradient-to-br from-surface to-dark">
                <div class="relative">
                  <div class="absolute inset-0 animate-ping">
                    <i class="fas fa-satellite-dish text-4xl text-primary/30"></i>
                  </div>
                  <i class="fas fa-satellite-dish text-4xl text-primary/80 relative"></i>
                </div>
                <p class="mt-4 text-onDark/50 text-sm">Ready to stream</p>
                <p class="text-onDark/30 text-xs">Select a game and click Start Streaming</p>
              </div>

              <!-- Live Stats Overlay -->
              <div v-if="showOverlay && isConnected" class="webrtc-overlay">
                <div v-for="(line, idx) in overlayLines" :key="idx">{{ line }}</div>
              </div>
            </div>
          </div>

          <!-- Stats Grid -->
          <div class="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
            <div class="stat-card">
              <div class="stat-icon bg-info/20 text-info">
                <i class="fas fa-video"></i>
              </div>
              <div>
                <div class="stat-label">Video Bitrate</div>
                <div class="stat-value">{{ formatKbps(stats.videoBitrateKbps) }}</div>
              </div>
            </div>
            <div class="stat-card">
              <div class="stat-icon bg-secondary/20 text-secondary">
                <i class="fas fa-film"></i>
              </div>
              <div>
                <div class="stat-label">Frame Rate</div>
                <div class="stat-value">{{ stats.videoFps ? `${stats.videoFps.toFixed(0)} FPS` : '--' }}</div>
              </div>
            </div>
            <div class="stat-card">
              <div class="stat-icon bg-success/20 text-success">
                <i class="fas fa-clock"></i>
              </div>
              <div>
                <div class="stat-label">Latency</div>
                <div class="stat-value">{{ stats.roundTripTimeMs ? `${stats.roundTripTimeMs.toFixed(0)} ms` : '--' }}</div>
              </div>
            </div>
            <div class="stat-card">
              <div class="stat-icon bg-warning/20 text-warning">
                <i class="fas fa-triangle-exclamation"></i>
              </div>
              <div>
                <div class="stat-label">Dropped</div>
                <div class="stat-value">{{ stats.videoFramesDropped ?? '--' }}</div>
              </div>
            </div>
          </div>

          <!-- Debug Panel (Collapsible) -->
          <details class="gaming-card">
            <summary class="p-4 cursor-pointer flex items-center justify-between hover:bg-surface/30 transition">
              <div class="flex items-center gap-2">
                <i class="fas fa-bug text-primary"></i>
                <h3 class="font-semibold text-onDark">{{ $t('webrtc.debug_panel') }}</h3>
              </div>
              <i class="fas fa-chevron-down text-onDark/40 transition-transform"></i>
            </summary>
            <div class="p-4 pt-0 border-t border-surface/30 space-y-2 text-xs">
              <div class="grid gap-2 md:grid-cols-2">
                <div class="debug-row">
                  <span>Connection</span>
                  <n-tag size="tiny" :type="statusTagType(connectionState)">{{ connectionState || 'idle' }}</n-tag>
                </div>
                <div class="debug-row">
                  <span>ICE</span>
                  <n-tag size="tiny" :type="statusTagType(iceState)">{{ iceState || 'idle' }}</n-tag>
                </div>
                <div class="debug-row">
                  <span>Input Channel</span>
                  <n-tag size="tiny" :type="statusTagType(inputChannelState)">{{ inputChannelState || 'closed' }}</n-tag>
                </div>
                <div class="debug-row">
                  <span>Session ID</span>
                  <span class="font-mono text-[10px] truncate max-w-[150px]">{{ displayValue(sessionId) }}</span>
                </div>
              </div>
              <div class="grid gap-2 md:grid-cols-2 mt-3 pt-3 border-t border-surface/30">
                <div class="debug-row">
                  <span>Input move delay</span>
                  <span>{{ formatMs(inputMetrics.lastMoveDelayMs) }}</span>
                </div>
                <div class="debug-row">
                  <span>Video interval</span>
                  <span>{{ formatMs(videoFrameMetrics.lastIntervalMs) }}</span>
                </div>
                <div class="debug-row">
                  <span>Server packets</span>
                  <span>V {{ displayValue(serverSession?.video_packets) }} / A {{ displayValue(serverSession?.audio_packets) }}</span>
                </div>
                <div class="debug-row">
                  <span>Inbound bytes</span>
                  <span>V {{ formatBytes(stats.videoBytesReceived) }} / A {{ formatBytes(stats.audioBytesReceived) }}</span>
                </div>
                <div class="debug-row">
                  <span>Video size</span>
                  <span>{{ videoSizeLabel }}</span>
                </div>
                <div class="debug-row">
                  <span>Codec</span>
                  <span>V {{ displayValue(stats.videoCodec) }} / A {{ displayValue(stats.audioCodec) }}</span>
                </div>
              </div>
              <!-- Video Events -->
              <div class="mt-3 pt-3 border-t border-surface/30">
                <div class="text-onDark/50 mb-2">Video Events</div>
                <div v-if="videoEvents.length" class="space-y-1 font-mono text-[10px]">
                  <div v-for="(event, idx) in videoEvents" :key="idx" class="text-onDark/40">{{ event }}</div>
                </div>
                <div v-else class="text-onDark/30">No events yet</div>
              </div>
            </div>
          </details>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onBeforeUnmount, onMounted, watch, computed } from 'vue';
import {
  NTag,
  NSwitch,
  NInputNumber,
  useMessage,
} from 'naive-ui';
import { WebRtcHttpApi } from '@/services/webrtcApi';
import { WebRtcClient } from '@/utils/webrtc/client';
import { applyGamepadFeedback, attachInputCapture, type InputCaptureMetrics } from '@/utils/webrtc/input';
import { EncodingType, StreamConfig, WebRtcSessionState, WebRtcStatsSnapshot } from '@/types/webrtc';
import { http } from '@/http';
import { useAppsStore } from '@/stores/apps';
import { storeToRefs } from 'pinia';
import type { App } from '@/stores/apps';

const message = useMessage();

// Helper function for resolution presets
function setResolution(width: number, height: number) {
  config.width = width;
  config.height = height;
}

// Connection status computed properties for the gaming UI
const connectionStatusClass = computed(() => {
  if (isConnected.value) return 'bg-success/20 text-success';
  if (isConnecting.value) return 'bg-warning/20 text-warning';
  return 'bg-surface text-onDark/50';
});

const connectionDotClass = computed(() => {
  if (isConnected.value) return 'bg-success';
  if (isConnecting.value) return 'bg-warning';
  return 'bg-onDark/30';
});

const connectionStatusLabel = computed(() => {
  if (isConnected.value) return 'Connected';
  if (isConnecting.value) return 'Connecting...';
  return 'Ready';
});

type EncodingOption = { label: string; value: EncodingType };

const baseEncodingOptions: EncodingOption[] = [
  { label: 'H.264', value: 'h264' },
  { label: 'HEVC', value: 'hevc' },
  { label: 'AV1', value: 'av1' },
];

const encodingMimes: Record<EncodingType, string[]> = {
  h264: ['video/h264'],
  hevc: ['video/h265', 'video/hevc'],
  av1: ['video/av1'],
};

function detectEncodingSupport(): Record<EncodingType, boolean> {
  const support: Record<EncodingType, boolean> = {
    h264: true,
    hevc: true,
    av1: true,
  };
  if (typeof RTCRtpSender === 'undefined') {
    return support;
  }
  const caps = RTCRtpSender.getCapabilities('video');
  if (!caps?.codecs) {
    return support;
  }
  const mimeTypes = caps.codecs.map((codec) => codec.mimeType.toLowerCase());
  (Object.keys(encodingMimes) as EncodingType[]).forEach((encoding) => {
    support[encoding] = encodingMimes[encoding].some((mime) => mimeTypes.includes(mime));
  });
  return support;
}

const encodingSupport = ref<Record<EncodingType, boolean>>(detectEncodingSupport());

const encodingOptions = computed(() =>
  baseEncodingOptions.map((opt) => {
    const supported = encodingSupport.value[opt.value];
    const hint = supported ? '' : `${opt.label} unsupported by this browser`;
    return { ...opt, supported, hint };
  }),
);

const pacingOptions = [
  { label: 'Latency', value: 'latency' },
  { label: 'Balanced', value: 'balanced' },
  { label: 'Smooth', value: 'smoothness' },
];

type PacingMode = (typeof pacingOptions)[number]['value'];

const pacingPresets: Record<PacingMode, { slackMs: number; maxFrameAgeMs: number }> = {
  latency: { slackMs: 0, maxFrameAgeMs: 33 },
  balanced: { slackMs: 2, maxFrameAgeMs: 50 },
  smoothness: { slackMs: 3, maxFrameAgeMs: 100 },
};

function applyPacingPreset(mode: PacingMode) {
  const preset = pacingPresets[mode];
  config.videoPacingMode = mode;
  config.videoPacingSlackMs = preset.slackMs;
  config.videoMaxFrameAgeMs = preset.maxFrameAgeMs;
}

const config = reactive<StreamConfig>({
  width: 1920,
  height: 1080,
  fps: 60,
  encoding: 'h264',
  bitrateKbps: 20000,
  muteHostAudio: true,
  videoPacingMode: 'balanced',
  videoPacingSlackMs: pacingPresets.balanced.slackMs,
  videoMaxFrameAgeMs: pacingPresets.balanced.maxFrameAgeMs,
});

function resolveFallbackEncoding(): EncodingType {
  const support = encodingSupport.value;
  const fallback = (Object.keys(support) as EncodingType[]).find((key) => support[key]);
  return fallback ?? 'h264';
}

function ensureEncodingSupported(): void {
  const current = config.encoding as EncodingType;
  if (!encodingSupport.value[current]) {
    config.encoding = resolveFallbackEncoding();
  }
}

watch(
  () => config.encoding,
  () => {
    ensureEncodingSupported();
  },
  { immediate: true },
);

const appsStore = useAppsStore();
const { apps } = storeToRefs(appsStore);
const appsList = computed(() => (apps.value || []).slice());
const selectedAppId = ref<number | null>(null);
const resumeOnConnect = ref(true);
const canResumeSelection = computed(() => !selectedAppId.value);
const terminatePending = ref(false);
const sessionStatus = ref<{ activeSessions: number; appRunning: boolean; paused: boolean } | null>(null);
let sessionStatusTimer: number | null = null;

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

const resumeAvailable = computed(() => {
  if (selectedAppId.value) return false;
  return sessionStatus.value?.paused ?? false;
});

const api = new WebRtcHttpApi();
const client = new WebRtcClient(api);

const isConnecting = ref(false);
const isConnected = ref(false);
const connectLabelKey = computed(() => {
  if (isConnecting.value) return 'webrtc.connecting';
  if (isConnected.value) return 'webrtc.disconnect';
  if (resumeAvailable.value) return 'webrtc.resume';
  return 'webrtc.connect';
});
const connectionState = ref<RTCPeerConnectionState | null>(null);
const iceState = ref<RTCIceConnectionState | null>(null);
const inputChannelState = ref<RTCDataChannelState | null>(null);
const stats = ref<WebRtcStatsSnapshot>({});
const inputEnabled = ref(true);
const showOverlay = ref(false);
const inputTarget = ref<HTMLElement | null>(null);
const videoEl = ref<HTMLVideoElement | null>(null);
const audioEl = ref<HTMLAudioElement | null>(null);
const isFullscreen = ref(false);
const autoFullscreen = ref(true);
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
const inputMetrics = ref<InputCaptureMetrics>({});
const inputBufferedAmount = ref<number | null>(null);
const videoFrameMetrics = ref<{
  lastIntervalMs?: number;
  avgIntervalMs?: number;
  maxIntervalMs?: number;
  lastDelayMs?: number;
  avgDelayMs?: number;
  maxDelayMs?: number;
}>({});
const overlayLines = computed(() => {
  const fps = stats.value.videoFps ? stats.value.videoFps.toFixed(0) : '--';
  const dropped = stats.value.videoFramesDropped ?? '--';
  return [
    `Conn ${connectionState.value ?? 'idle'} | ICE ${iceState.value ?? 'idle'} | Input ${inputChannelState.value ?? 'closed'}`,
    `RTT ${formatMs(stats.value.roundTripTimeMs)} | FPS ${fps} | Drop ${dropped}`,
    `Bitrate V ${formatKbps(stats.value.videoBitrateKbps)} / A ${formatKbps(stats.value.audioBitrateKbps)}`,
    `Input send ${formatRate(inputMetrics.value.moveSendRateHz)} | cap ${formatRate(inputMetrics.value.moveRateHz)} | coalesce ${formatPercent(inputMetrics.value.moveCoalesceRatio)}`,
    `Input lag ${formatMs(inputMetrics.value.lastMoveEventLagMs)} ev / ${formatMs(inputMetrics.value.lastMoveDelayMs)} send | buf ${formatBytes(inputBufferedAmount.value ?? undefined)}`,
    `Video interval ${formatMs(videoFrameMetrics.value.lastIntervalMs)} | delay ${formatMs(videoFrameMetrics.value.lastDelayMs)} | size ${videoSizeLabel.value}`,
  ];
});
let detachInput: (() => void) | null = null;
let detachVideoEvents: (() => void) | null = null;
let detachVideoFrames: (() => void) | null = null;
let lastTrackSnapshot: { video: number; audio: number } | null = null;
let serverSessionTimer: number | null = null;
let audioStream: MediaStream | null = null;
let audioAutoplayRequested = false;
function stopSessionStatusPolling(): void {
  if (sessionStatusTimer) {
    window.clearInterval(sessionStatusTimer);
    sessionStatusTimer = null;
  }
}

async function fetchSessionStatus(): Promise<void> {
  if (isConnected.value) return;
  try {
    const result = await http.get('/api/session/status', { validateStatus: () => true });
    if (result.status === 200 && result.data?.status) {
      sessionStatus.value = {
        activeSessions: Number(result.data.activeSessions ?? 0),
        appRunning: Boolean(result.data.appRunning),
        paused: Boolean(result.data.paused),
      };
      return;
    }
  } catch {
    /* ignore */
  }
  sessionStatus.value = null;
}

function startSessionStatusPolling(): void {
  stopSessionStatusPolling();
  if (isConnected.value) return;
  void fetchSessionStatus();
  sessionStatusTimer = window.setInterval(fetchSessionStatus, 5000);
}
const ESC_HOLD_MS = 2000;
let escHoldTimer: number | null = null;
function getFullscreenElement(): Element | null {
  return document.fullscreenElement ?? (document as any).webkitFullscreenElement ?? null;
}

async function requestFullscreen(target: HTMLElement): Promise<void> {
  const anyTarget = target as any;
  if (typeof target.requestFullscreen === 'function') {
    await target.requestFullscreen();
    return;
  }
  if (typeof anyTarget.webkitRequestFullscreen === 'function') {
    const result = anyTarget.webkitRequestFullscreen();
    if (result && typeof result.then === 'function') {
      await result;
    }
    return;
  }
  if (videoEl.value && typeof (videoEl.value as any).webkitEnterFullscreen === 'function') {
    (videoEl.value as any).webkitEnterFullscreen();
  }
}

async function exitFullscreen(): Promise<void> {
  const anyDoc = document as any;
  if (typeof document.exitFullscreen === 'function') {
    await document.exitFullscreen();
    return;
  }
  if (typeof anyDoc.webkitExitFullscreen === 'function') {
    const result = anyDoc.webkitExitFullscreen();
    if (result && typeof result.then === 'function') {
      await result;
    }
  }
}

function isFullscreenActive(): boolean {
  const fullscreenEl = getFullscreenElement();
  return fullscreenEl === inputTarget.value || fullscreenEl === videoEl.value;
}

const onFullscreenChange = () => {
  isFullscreen.value = isFullscreenActive();
  if (!isFullscreen.value) {
    cancelEscHold();
  }
};
const onOverlayHotkey = (event: KeyboardEvent) => {
  if (!event.ctrlKey || !event.altKey || !event.shiftKey) return;
  if (event.code !== 'KeyS') return;
  event.preventDefault();
  event.stopPropagation();
  showOverlay.value = !showOverlay.value;
};

const onPageHide = () => {
  void client.disconnect({ keepalive: true });
};

const onFullscreenEscapeDown = (event: KeyboardEvent) => {
  if (event.code !== 'Escape') return;
  if (!isFullscreen.value) return;
  if (escHoldTimer) {
    event.preventDefault();
    event.stopPropagation();
    return;
  }
  event.preventDefault();
  event.stopPropagation();
  escHoldTimer = window.setTimeout(async () => {
    escHoldTimer = null;
    if (getFullscreenElement()) {
      try {
        await exitFullscreen();
      } catch {
        /* ignore */
      }
    }
  }, ESC_HOLD_MS);
};

const onFullscreenEscapeUp = (event: KeyboardEvent) => {
  if (event.code !== 'Escape') return;
  if (!isFullscreen.value) return;
  event.preventDefault();
  event.stopPropagation();
  cancelEscHold();
};

function cancelEscHold() {
  if (!escHoldTimer) return;
  window.clearTimeout(escHoldTimer);
  escHoldTimer = null;
}

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

function formatMs(value?: number): string {
  if (value == null) return '--';
  return `${value.toFixed(1)} ms`;
}

function formatRate(value?: number): string {
  if (value == null) return '--';
  return `${value.toFixed(0)} / s`;
}

function formatPercent(value?: number): string {
  if (value == null) return '--';
  return `${(value * 100).toFixed(0)}%`;
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

function updateAudioElement(stream: MediaStream): void {
  const audioTrack = stream.getAudioTracks()[0];
  if (!audioEl.value || !audioTrack) return;
  if (!audioStream) {
    audioStream = new MediaStream();
  }
  audioStream.getAudioTracks().forEach((track) => audioStream?.removeTrack(track));
  audioStream.addTrack(audioTrack);
  audioEl.value.srcObject = audioStream;
  audioEl.value.muted = false;
  if (audioAutoplayRequested) {
    const playPromise = audioEl.value.play();
    if (playPromise && typeof playPromise.catch === 'function') {
      playPromise.catch(() => {
        /* ignore */
      });
    }
  }
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

function attachVideoFrameMetrics(el: HTMLVideoElement): () => void {
  if (typeof el.requestVideoFrameCallback !== 'function') {
    return () => {};
  }
  let handle = 0;
  let lastFrameAt: number | null = null;
  let intervalSum = 0;
  let intervalSamples = 0;
  let delaySum = 0;
  let delaySamples = 0;
  const onFrame = (now: DOMHighResTimeStamp, metadata: VideoFrameCallbackMetadata) => {
    if (lastFrameAt != null) {
      const interval = Math.max(0, now - lastFrameAt);
      intervalSum += interval;
      intervalSamples += 1;
      videoFrameMetrics.value.lastIntervalMs = interval;
      videoFrameMetrics.value.avgIntervalMs = intervalSum / intervalSamples;
      videoFrameMetrics.value.maxIntervalMs = Math.max(videoFrameMetrics.value.maxIntervalMs ?? 0, interval);
    }
    lastFrameAt = now;
    if (typeof metadata.expectedDisplayTime === 'number') {
      const delay = Math.max(0, now - metadata.expectedDisplayTime);
      delaySum += delay;
      delaySamples += 1;
      videoFrameMetrics.value.lastDelayMs = delay;
      videoFrameMetrics.value.avgDelayMs = delaySum / delaySamples;
      videoFrameMetrics.value.maxDelayMs = Math.max(videoFrameMetrics.value.maxDelayMs ?? 0, delay);
    }
    handle = el.requestVideoFrameCallback(onFrame);
  };
  handle = el.requestVideoFrameCallback(onFrame);
  return () => {
    if (handle) {
      el.cancelVideoFrameCallback(handle);
    }
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
  audioAutoplayRequested = true;
  if (autoFullscreen.value && inputTarget.value && !isFullscreenActive()) {
    try {
      await requestFullscreen(inputTarget.value);
    } catch {
      /* ignore */
    }
  }
  if (audioEl.value) {
    audioEl.value.muted = false;
    audioEl.value.volume = 1;
  }
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
          videoEl.value.muted = false;
          videoEl.value.volume = 1;
          updateRemoteStreamInfo(stream);
          updateAudioElement(stream);
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
      onInputMessage: (message) => {
        applyGamepadFeedback(message);
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
  startSessionStatusPolling();
  isConnected.value = false;
  connectionState.value = null;
  iceState.value = null;
  inputChannelState.value = null;
  stats.value = {};
  inputMetrics.value = {};
  inputBufferedAmount.value = null;
  videoFrameMetrics.value = {};
  detachInputCapture();
  if (videoEl.value) videoEl.value.srcObject = null;
  if (audioEl.value) audioEl.value.srcObject = null;
  audioStream = null;
  audioAutoplayRequested = false;
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

async function terminateSession() {
  if (terminatePending.value) return;
  terminatePending.value = true;
  try {
    await http.post('/api/apps/close', {}, { validateStatus: () => true });
  } catch (error) {
    const msg = error instanceof Error ? error.message : 'Failed to terminate session.';
    message.error(msg);
  } finally {
    await disconnect();
    terminatePending.value = false;
  }
}

async function toggleFullscreen() {
  try {
    if (isFullscreenActive()) {
      await exitFullscreen();
      return;
    }
    if (!inputTarget.value) return;
    await requestFullscreen(inputTarget.value);
  } catch {
    /* ignore */
  }
}

async function onFullscreenDblClick() {
  if (isFullscreenActive()) return;
  await toggleFullscreen();
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
    detachInput = attachInputCapture(
      inputTarget.value,
      (payload) => {
        client.sendInput(payload);
        inputBufferedAmount.value = client.inputChannelBufferedAmount ?? null;
      },
      {
        video: videoEl.value,
        onMetrics: (metrics) => {
          inputMetrics.value = metrics;
        },
      },
    );
  },
);

watch(videoEl, (el) => {
  if (detachVideoEvents) {
    detachVideoEvents();
    detachVideoEvents = null;
  }
  if (detachVideoFrames) {
    detachVideoFrames();
    detachVideoFrames = null;
  }
  if (!el) return;
  detachVideoEvents = attachVideoDebug(el);
  detachVideoFrames = attachVideoFrameMetrics(el);
});

onBeforeUnmount(() => {
  document.removeEventListener('fullscreenchange', onFullscreenChange);
  document.removeEventListener('webkitfullscreenchange', onFullscreenChange as EventListener);
  window.removeEventListener('keydown', onOverlayHotkey, true);
  window.removeEventListener('keydown', onFullscreenEscapeDown, true);
  window.removeEventListener('keyup', onFullscreenEscapeUp, true);
  window.removeEventListener('pagehide', onPageHide);
  cancelEscHold();
  if (detachVideoEvents) {
    detachVideoEvents();
    detachVideoEvents = null;
  }
  stopSessionStatusPolling();
  stopServerSessionPolling();
  void disconnect();
});

onMounted(async () => {
  document.addEventListener('fullscreenchange', onFullscreenChange);
  document.addEventListener('webkitfullscreenchange', onFullscreenChange as EventListener);
  window.addEventListener('keydown', onOverlayHotkey, true);
  window.addEventListener('keydown', onFullscreenEscapeDown, true);
  window.addEventListener('keyup', onFullscreenEscapeUp, true);
  window.addEventListener('pagehide', onPageHide);
  try {
    await appsStore.loadApps(true);
  } catch {
    /* ignore */
  }
  encodingSupport.value = detectEncodingSupport();
  ensureEncodingSupported();
  startSessionStatusPolling();
});

watch(
  () => isConnected.value,
  (connected) => {
    if (connected) {
      stopSessionStatusPolling();
      return;
    }
    startSessionStatusPolling();
  },
);
</script>

<style scoped>
/* Gaming Theme Container - matches Sunshine palette */
.webrtc-gaming-container {
  background: rgb(var(--color-dark));
  color: rgb(var(--color-on-dark));
  min-height: calc(100vh - 4rem);
  overscroll-behavior: none;
}

@supports (height: 100svh) {
  .webrtc-gaming-container {
    min-height: calc(100svh - 4rem);
  }
}

.gaming-header {
  background: linear-gradient(180deg, rgba(0, 0, 0, 0.3) 0%, transparent 100%);
  border-bottom: 1px solid rgb(var(--color-surface) / 0.3);
}

/* Gaming Card Style */
.gaming-card {
  background: rgb(var(--color-surface) / 0.6);
  border: 1px solid rgb(var(--color-primary) / 0.1);
  border-radius: 1rem;
  backdrop-filter: blur(10px);
}

/* Preset Buttons */
.preset-btn {
  @apply px-3 py-1.5 text-xs font-medium rounded-lg transition-all duration-200;
  background: rgb(var(--color-surface));
  border: 1px solid rgb(var(--color-primary) / 0.15);
  color: rgb(var(--color-on-dark) / 0.6);
}

.preset-btn:hover {
  background: rgb(var(--color-primary) / 0.15);
  border-color: rgb(var(--color-primary) / 0.4);
  color: rgb(var(--color-on-dark) / 0.9);
}

.preset-btn.active {
  background: linear-gradient(135deg, rgb(var(--color-primary)) 0%, rgb(var(--color-secondary)) 100%);
  border-color: rgb(var(--color-primary));
  color: rgb(var(--color-on-primary));
  box-shadow: 0 4px 15px rgb(var(--color-primary) / 0.3);
}

.preset-btn.disabled,
.preset-btn:disabled {
  background: rgb(var(--color-surface) / 0.6);
  border-color: rgb(var(--color-primary) / 0.08);
  color: rgb(var(--color-on-dark) / 0.35);
  cursor: not-allowed;
  box-shadow: none;
}

.preset-btn.disabled:hover,
.preset-btn:disabled:hover {
  background: rgb(var(--color-surface) / 0.6);
  border-color: rgb(var(--color-primary) / 0.08);
  color: rgb(var(--color-on-dark) / 0.35);
}

/* Game Card */
.game-card {
  @apply flex items-center gap-3 p-2 rounded-xl transition-all duration-200;
  background: rgb(var(--color-surface) / 0.4);
  border: 1px solid rgb(var(--color-primary) / 0.08);
}

.game-card:hover {
  background: rgb(var(--color-primary) / 0.1);
  border-color: rgb(var(--color-primary) / 0.3);
  transform: translateX(4px);
}

.game-card.selected {
  background: linear-gradient(135deg, rgb(var(--color-primary) / 0.2) 0%, rgb(var(--color-secondary) / 0.15) 100%);
  border-color: rgb(var(--color-primary));
}

/* Stats Cards */
.stat-card {
  @apply flex items-center gap-3 p-4 rounded-xl;
  background: rgb(var(--color-surface) / 0.6);
  border: 1px solid rgb(var(--color-primary) / 0.1);
}

.stat-icon {
  @apply w-10 h-10 rounded-lg flex items-center justify-center text-lg;
}

.stat-label {
  @apply text-xs uppercase tracking-wide;
  color: rgb(var(--color-on-dark) / 0.4);
}

.stat-value {
  @apply text-lg font-semibold;
  color: rgb(var(--color-on-dark));
}

/* Debug Row */
.debug-row {
  @apply flex items-center justify-between py-1;
  color: rgb(var(--color-on-dark) / 0.5);
}

.debug-row span:last-child {
  color: rgb(var(--color-on-dark) / 0.8);
}

/* Custom Scrollbar */
.custom-scrollbar::-webkit-scrollbar {
  width: 6px;
}

.custom-scrollbar::-webkit-scrollbar-track {
  background: rgb(var(--color-surface) / 0.3);
  border-radius: 3px;
}

.custom-scrollbar::-webkit-scrollbar-thumb {
  background: rgb(var(--color-primary) / 0.3);
  border-radius: 3px;
}

.custom-scrollbar::-webkit-scrollbar-thumb:hover {
  background: rgb(var(--color-primary) / 0.5);
}

/* Details/Summary Arrow */
details summary::-webkit-details-marker {
  display: none;
}

details[open] summary .fa-chevron-down {
  transform: rotate(180deg);
}

/* Fullscreen Mode */
.webrtc-fullscreen {
  cursor: none;
}

.webrtc-fullscreen * {
  cursor: none;
}

/* Overlay */
.webrtc-overlay {
  position: absolute;
  top: 0.75rem;
  left: 0.75rem;
  z-index: 10;
  pointer-events: none;
  background: rgb(var(--color-dark) / 0.85);
  color: rgb(var(--color-on-dark));
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, 'Liberation Mono',
    'Courier New', monospace;
  font-size: 11px;
  line-height: 1.4;
  padding: 0.5rem 0.75rem;
  border-radius: 0.5rem;
  border: 1px solid rgb(var(--color-primary) / 0.2);
  max-width: min(92vw, 520px);
  white-space: pre-wrap;
  backdrop-filter: blur(8px);
}

/* Override Naive UI inputs for dark theme */
:deep(.n-input-number) {
  --n-color: rgb(var(--color-surface)) !important;
  --n-border: 1px solid rgb(var(--color-primary) / 0.2) !important;
  --n-text-color: rgb(var(--color-on-dark)) !important;
}

:deep(.n-switch) {
  --n-rail-color: rgb(var(--color-surface)) !important;
}
</style>
