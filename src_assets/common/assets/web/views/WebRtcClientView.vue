<template>
  <div class="webrtc-app" :class="{ 'settings-open': showSettings }">
    <!-- Main Content Area -->
    <div class="main-content">
      <!-- Compact Header -->
      <header class="app-header">
        <div class="header-left">
          <div class="brand">
            <div class="brand-icon">
              <i class="fas fa-play"></i>
            </div>
            <h1>{{ $t('webrtc.title') }}</h1>
          </div>
        </div>

        <div class="header-center">
          <div class="status-pill" :class="connectionPillClass">
            <span class="status-dot"></span>
            <span>{{ connectionStatusLabel }}</span>
          </div>
        </div>

        <div class="header-right">
          <button class="settings-btn" @click="showSettings = !showSettings" :class="{ active: showSettings }">
            <i class="fas fa-sliders-h"></i>
            <span>Settings</span>
          </button>
        </div>
      </header>

      <!-- Game Library -->
      <section class="library-section">
        <div class="library-header">
          <div class="library-title-row">
            <h2><i class="fas fa-gamepad"></i> {{ $t('webrtc.select_game') }}</h2>
            <span v-if="selectedAppId" class="selection-badge">
              <i class="fas fa-check-circle"></i>
              {{ selectedAppLabel }}
              <button @click="clearSelection" class="clear-btn">
                <i class="fas fa-times"></i>
              </button>
            </span>
          </div>
          <div class="search-box">
            <i class="fas fa-search"></i>
            <input
              v-model="searchQuery"
              type="text"
              :placeholder="$t('webrtc.search_placeholder') || 'Search applications...'"
              class="search-input"
            />
            <button v-if="searchQuery" @click="searchQuery = ''" class="search-clear">
              <i class="fas fa-times"></i>
            </button>
          </div>
        </div>

        <!-- No apps at all -->
        <div v-if="!appsList.length" class="empty-state">
          <i class="fas fa-gamepad"></i>
          <h3>No Applications</h3>
          <p>Add games in the Applications tab to start streaming</p>
        </div>

        <!-- No search results -->
        <div v-else-if="!filteredApps.length" class="empty-state">
          <i class="fas fa-search"></i>
          <h3>No Results</h3>
          <p>No applications match "{{ searchQuery }}"</p>
        </div>

        <template v-else>
          <!-- Games with Box Art -->
          <div v-if="appsWithCovers.length" class="games-grid">
            <button
              v-for="app in appsWithCovers"
              :key="appKey(app)"
              @click="selectApp(app)"
              @dblclick="onAppDoubleClick(app)"
              class="game-card"
              :class="{ selected: appNumericId(app) === selectedAppId }"
            >
              <div class="game-cover">
                <img
                  :src="coverUrl(app) || undefined"
                  :alt="app.name || 'Application'"
                  loading="lazy"
                  @load="onCoverLoad(app)"
                  @error="onCoverError(app)"
                />
                <div class="cover-gradient"></div>
                <div v-if="appNumericId(app) === selectedAppId" class="selected-badge">
                  <i class="fas fa-check"></i>
                </div>
                <div class="play-overlay">
                  <i class="fas fa-play"></i>
                </div>
              </div>
              <div class="game-meta">
                <span class="game-name">{{ app.name || '(untitled)' }}</span>
                <span class="game-source">{{ appSubtitle(app) }}</span>
              </div>
            </button>
          </div>

          <!-- Other Applications (no box art) -->
          <div v-if="appsWithoutCovers.length" class="other-apps-section">
            <h3 class="section-label">
              <i class="fas fa-window-maximize"></i>
              Other Applications
            </h3>
            <div class="apps-list">
              <button
                v-for="app in appsWithoutCovers"
                :key="appKey(app)"
                @click="selectApp(app)"
                @dblclick="onAppDoubleClick(app)"
                class="app-list-item"
                :class="{ selected: appNumericId(app) === selectedAppId }"
              >
                <div class="app-icon">
                  <i class="fas fa-window-maximize"></i>
                </div>
                <div class="app-info">
                  <span class="app-name">{{ app.name || '(untitled)' }}</span>
                  <span class="app-source">{{ appSubtitle(app) }}</span>
                </div>
                <div v-if="appNumericId(app) === selectedAppId" class="app-selected-icon">
                  <i class="fas fa-check"></i>
                </div>
                <div class="app-play-icon">
                  <i class="fas fa-play"></i>
                </div>
              </button>
            </div>
          </div>
        </template>
      </section>

      <!-- Floating Stream Preview -->
      <div class="stream-preview" :class="{ expanded: isFullscreen, minimized: streamMinimized && !isFullscreen }">
        <div class="preview-header" v-if="!isFullscreen">
          <div class="preview-title">
            <i class="fas fa-tv"></i>
            <span>Stream</span>
            <span v-if="isConnected" class="live-indicator">
              <span class="live-dot"></span>
              LIVE
            </span>
          </div>
          <div class="preview-controls">
            <button @click="streamMinimized = !streamMinimized" class="control-btn" v-if="!isFullscreen">
              <i :class="streamMinimized ? 'fas fa-chevron-up' : 'fas fa-chevron-down'"></i>
            </button>
            <button @click="toggleFullscreen" class="control-btn">
              <i :class="isFullscreen ? 'fas fa-compress' : 'fas fa-expand'"></i>
            </button>
          </div>
        </div>

        <div
          ref="inputTarget"
          class="stream-viewport"
          :class="{ 'fullscreen-mode': isFullscreen }"
          tabindex="0"
          @dblclick="onFullscreenDblClick"
        >
          <video
            ref="videoEl"
            class="stream-video"
            :class="{ 'pacer-source-hidden': useFramePacer }"
            autoplay
            playsinline
            :controls="false"
            disablePictureInPicture
          ></video>
          <canvas
            v-if="useFramePacer"
            ref="canvasEl"
            class="stream-video"
          ></canvas>
          <audio ref="audioEl" class="hidden" autoplay playsinline></audio>

          <!-- Idle State -->
          <div v-if="!isConnected && !isConnecting" class="idle-state">
            <div class="idle-content">
              <i :class="selectedAppId ? 'fas fa-play-circle' : 'fas fa-desktop'"></i>
              <p>{{ selectedAppId ? $t('webrtc.idle_game_selected') : $t('webrtc.idle_no_selection') }}</p>
            </div>
          </div>

          <!-- Connecting State -->
          <div v-if="showStartingOverlay" class="connecting-state">
            <div class="spinner"></div>
            <span>Connecting...</span>
          </div>

          <!-- Stats Overlay -->
          <div v-if="showOverlay && isConnected" class="stats-overlay">
            <div v-for="(line, idx) in overlayLines" :key="idx" class="stat-line">{{ line }}</div>
          </div>

          <!-- Notification -->
          <Transition name="notification-fade">
            <div v-if="activeNotification" class="notification-toast" :class="activeNotification.type">
              <i :class="notificationIcon"></i>
              <div class="notification-text">
                <strong>{{ activeNotification.title }}</strong>
                <span v-if="activeNotification.message">{{ activeNotification.message }}</span>
              </div>
              <button @click="dismissNotification"><i class="fas fa-times"></i></button>
            </div>
          </Transition>
        </div>

        <!-- Quick Actions Bar -->
        <div class="quick-actions" v-if="!isFullscreen && !streamMinimized">
          <button
            @click="isConnected ? disconnect() : connect()"
            class="action-btn primary"
            :class="{ connected: isConnected, connecting: isConnecting }"
            :disabled="isConnecting"
          >
            <i :class="isConnected ? 'fas fa-stop' : isConnecting ? 'fas fa-circle-notch fa-spin' : 'fas fa-play'"></i>
            <span>{{ $t(connectLabelKey) }}</span>
          </button>

          <button
            v-if="isConnected"
            @click="terminateSession"
            class="action-btn danger"
            :disabled="terminatePending"
          >
            <i :class="terminatePending ? 'fas fa-circle-notch fa-spin' : 'fas fa-power-off'"></i>
          </button>

          <div class="quick-toggles">
            <label class="toggle" title="Enable input forwarding">
              <n-switch v-model:value="inputEnabled" :disabled="!isConnected" size="small" />
              <span>Input</span>
            </label>
            <label class="toggle" title="Show performance overlay">
              <n-switch v-model:value="showOverlay" size="small" />
              <span>Stats</span>
            </label>
          </div>
        </div>

        <!-- Compact Metrics -->
        <div class="compact-metrics" v-if="isConnected && !isFullscreen && !streamMinimized">
          <div class="metric"><span class="label">Bitrate</span><span class="value">{{ formatKbps(stats.videoBitrateKbps) }}</span></div>
          <div class="metric"><span class="label">Latency</span><span class="value">{{ formatMs(smoothedLatencyMs) }}</span></div>
          <div class="metric"><span class="label">FPS</span><span class="value">{{ displayVideoFps ? displayVideoFps.toFixed(0) : '--' }}</span></div>
          <div class="metric"><span class="label">Dropped</span><span class="value">{{ stats.videoFramesDropped ?? '--' }}</span></div>
        </div>
      </div>
    </div>

    <!-- Settings Slideout -->
    <Transition name="slideout">
      <aside v-if="showSettings" class="settings-drawer">
        <div class="drawer-header">
          <h2><i class="fas fa-sliders-h"></i> {{ $t('webrtc.session_settings') }}</h2>
          <button @click="showSettings = false" class="close-btn">
            <i class="fas fa-times"></i>
          </button>
        </div>

        <div class="drawer-content">
          <!-- Resolution -->
          <div class="setting-group">
            <label class="group-label">{{ $t('webrtc.resolution') }}</label>
            <div class="resolution-inputs">
              <n-input-number v-model:value="config.width" :min="320" :max="7680" size="small" />
              <span class="separator">×</span>
              <n-input-number v-model:value="config.height" :min="180" :max="4320" size="small" />
            </div>
            <div class="preset-chips">
              <button @click="setResolution(1920, 1080)" class="chip" :class="{ active: config.width === 1920 && config.height === 1080 }">1080p</button>
              <button @click="setResolution(2560, 1440)" class="chip" :class="{ active: config.width === 2560 && config.height === 1440 }">1440p</button>
              <button @click="setResolution(3840, 2160)" class="chip" :class="{ active: config.width === 3840 && config.height === 2160 }">4K</button>
            </div>
          </div>

          <!-- Frame Rate -->
          <div class="setting-group">
            <label class="group-label">{{ $t('webrtc.framerate') }}</label>
            <div class="preset-chips">
              <button @click="config.fps = 30" class="chip" :class="{ active: config.fps === 30 }">30</button>
              <button @click="config.fps = 60" class="chip" :class="{ active: config.fps === 60 }">60</button>
              <button @click="config.fps = 120" class="chip" :class="{ active: config.fps === 120 }">120</button>
              <button @click="config.fps = 144" class="chip" :class="{ active: config.fps === 144 }">144</button>
            </div>
          </div>

          <!-- Encoding -->
          <div class="setting-group">
            <label class="group-label">{{ $t('webrtc.encoding') }}</label>
            <div class="preset-chips">
              <button
                v-for="opt in encodingOptions"
                :key="opt.value"
                @click="config.encoding = opt.value"
                class="chip"
                :class="{ active: config.encoding === opt.value, unsupported: !opt.supported }"
                :title="opt.supported ? undefined : opt.hint"
              >
                {{ opt.label }}
              </button>
            </div>
          </div>

          <!-- Bitrate -->
          <div class="setting-group">
            <label class="group-label">{{ $t('webrtc.bitrate') }}</label>
            <n-input-number v-model:value="config.bitrateKbps" :min="500" :max="200000" size="small" class="full-width" />
            <div class="preset-chips">
              <button @click="config.bitrateKbps = 10000" class="chip" :class="{ active: config.bitrateKbps === 10000 }">10 Mbps</button>
              <button @click="config.bitrateKbps = 30000" class="chip" :class="{ active: config.bitrateKbps === 30000 }">30 Mbps</button>
              <button @click="config.bitrateKbps = 60000" class="chip" :class="{ active: config.bitrateKbps === 60000 }">60 Mbps</button>
            </div>
          </div>

          <!-- HDR Toggle -->
          <div class="setting-group toggle-setting">
            <div class="toggle-info">
              <label class="group-label">{{ $t('webrtc.hdr') }}</label>
              <p class="hint">{{ $t('webrtc.hdr_desc') }}</p>
            </div>
            <n-switch v-model:value="config.hdr" />
          </div>
          <n-alert v-if="hdrInlineWarning" type="warning" :show-icon="true" class="setting-alert">
            {{ hdrInlineWarning }}
          </n-alert>

          <!-- Mute Host Audio -->
          <div class="setting-group toggle-setting">
            <div class="toggle-info">
              <label class="group-label">{{ $t('webrtc.mute_host_audio') }}</label>
              <p class="hint">{{ $t('webrtc.mute_host_audio_desc') }}</p>
            </div>
            <n-switch v-model:value="config.muteHostAudio" />
          </div>

          <!-- Frame Pacer -->
          <div class="setting-group toggle-setting">
            <div class="toggle-info">
              <label class="group-label">Frame Pacer</label>
              <p class="hint">Improves performance and lowers latency</p>
            </div>
            <n-switch v-model:value="useFramePacer" :disabled="isConnected" size="small" />
          </div>

          <!-- Advanced Options -->
          <details class="advanced-section">
            <summary><i class="fas fa-cogs"></i> Advanced Options</summary>
            <div class="advanced-content">
              <div class="setting-group toggle-setting">
                <div class="toggle-info">
                  <label class="group-label">Auto Fullscreen</label>
                  <p class="hint">Enter fullscreen when stream starts</p>
                </div>
                <n-switch v-model:value="autoFullscreen" size="small" />
              </div>
              <div class="setting-group toggle-setting">
                <div class="toggle-info">
                  <label class="group-label">Pacer Diagnostics</label>
                  <p class="hint">Enable hitch detection metrics</p>
                </div>
                <n-switch v-model:value="pacerDiagnostics" :disabled="!useFramePacer" size="small" />
              </div>
            </div>
          </details>
        </div>

        <!-- Drawer Footer -->
        <div class="drawer-footer">
          <p class="notice">
            <i class="fas fa-info-circle"></i>
            {{ $t('webrtc.experimental_notice') }}
          </p>
        </div>
      </aside>
    </Transition>

    <!-- Backdrop for settings -->
    <Transition name="fade">
      <div v-if="showSettings" class="drawer-backdrop" @click="showSettings = false"></div>
    </Transition>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onBeforeUnmount, onMounted, watch, computed, nextTick } from 'vue';
import { useI18n } from 'vue-i18n';
import { NTag, NSwitch, NInputNumber, NAlert, useDialog, useMessage } from 'naive-ui';
import { WebRtcHttpApi } from '@/services/webrtcApi';
import { WebRtcClient } from '@/utils/webrtc/client';
import { FramePacer, type FramePacerMetrics } from '@/utils/webrtc/framePacer';
import {
  applyGamepadFeedback,
  attachInputCapture,
  type InputCaptureMetrics,
  releaseKeyboardLock,
  requestKeyboardLock,
} from '@/utils/webrtc/input';
import {
  EncodingType,
  InputMessage,
  StreamConfig,
  WebRtcSessionState,
  WebRtcStatsSnapshot,
} from '@/types/webrtc';
import { http } from '@/http';
import { useAppsStore } from '@/stores/apps';
import { storeToRefs } from 'pinia';
import type { App } from '@/stores/apps';

const { t } = useI18n();
const dialog = useDialog();
const message = useMessage();

// UI State
const showSettings = ref(false);
const streamMinimized = ref(false);

// ============================================
// NOTIFICATION SYSTEM
// ============================================
type NotificationType = 'error' | 'warning' | 'success' | 'info';

interface Notification {
  id: number;
  type: NotificationType;
  title: string;
  message?: string;
}

const activeNotification = ref<Notification | null>(null);
let notificationId = 0;
let notificationTimeout: number | null = null;

const notificationIcon = computed(() => {
  if (!activeNotification.value) return 'fas fa-info-circle';
  switch (activeNotification.value.type) {
    case 'error': return 'fas fa-circle-exclamation';
    case 'warning': return 'fas fa-triangle-exclamation';
    case 'success': return 'fas fa-circle-check';
    default: return 'fas fa-circle-info';
  }
});

function showNotification(type: NotificationType, title: string, msg?: string, duration = 5000) {
  if (notificationTimeout) {
    clearTimeout(notificationTimeout);
    notificationTimeout = null;
  }
  notificationId++;
  activeNotification.value = { id: notificationId, type, title, message: msg };
  if (duration > 0) {
    notificationTimeout = window.setTimeout(() => dismissNotification(), duration);
  }
}

function dismissNotification() {
  activeNotification.value = null;
  if (notificationTimeout) {
    clearTimeout(notificationTimeout);
    notificationTimeout = null;
  }
}

function notifyError(title: string, msg?: string) { showNotification('error', title, msg, 8000); }
function notifyWarning(title: string, msg?: string) { showNotification('warning', title, msg, 6000); }
function notifySuccess(title: string, msg?: string) { showNotification('success', title, msg, 4000); }
function notifyInfo(title: string, msg?: string) { showNotification('info', title, msg, 5000); }

// Helper function for resolution presets
function setResolution(width: number, height: number) {
  config.width = width;
  config.height = height;
}

// Connection status computed properties
const connectionPillClass = computed(() => {
  if (isConnected.value) return 'connected';
  if (isConnecting.value) return 'connecting';
  return 'idle';
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
  const support: Record<EncodingType, boolean> = { h264: true, hevc: true, av1: true };
  const caps = (typeof RTCRtpReceiver !== 'undefined' ? RTCRtpReceiver.getCapabilities?.('video') : null) ??
    (typeof RTCRtpSender !== 'undefined' ? RTCRtpSender.getCapabilities?.('video') : null);
  if (!caps?.codecs) return support;
  const mimeTypes = caps.codecs.map((codec) => codec.mimeType.toLowerCase());
  (Object.keys(encodingMimes) as EncodingType[]).forEach((encoding) => {
    support[encoding] = encodingMimes[encoding].some((mime) => mimeTypes.includes(mime));
  });
  return support;
}

const encodingSupport = ref<Record<EncodingType, boolean>>(detectEncodingSupport());

const encodingOptions = computed(() =>
  baseEncodingOptions.map((opt) => {
    const supported = opt.value === 'av1' ? encodingSupport.value[opt.value] : true;
    const hint = supported ? '' : `${opt.label} may be unsupported by this browser.`;
    return { ...opt, supported, hint };
  }),
);

const hdrCodecAdvertised = computed(() => {
  if (config.encoding === 'av1') return encodingSupport.value.av1;
  return encodingSupport.value.hevc;
});

const hdrInlineWarning = computed(() => {
  if (!config.hdr) return null;
  if (hdrRuntimeWarning.value) return hdrRuntimeWarning.value;
  if (!hdrCodecAdvertised.value) {
    return `This browser reports no ${config.encoding.toUpperCase()} decode support. If you get a black screen, switch codecs or disable HDR.`;
  }
  return null;
});

function ensureHdrEncoding(): void {
  if (config.encoding === 'h264') config.encoding = 'hevc';
}

const config = reactive<StreamConfig>({
  width: 1920,
  height: 1080,
  fps: 60,
  encoding: 'h264',
  hdr: false,
  bitrateKbps: 20000,
  muteHostAudio: true,
});

const negotiatedEncoding = ref<EncodingType | null>(null);
const hdrRuntimeWarning = ref<string | null>(null);

const CLIENT_CONFIG_STORAGE_KEY = 'sunshine.webrtc.session_config';

function normalizeProfileConfig(profileConfig: StreamConfig): StreamConfig {
  const normalized = { ...profileConfig };
  if (typeof normalized.hdr !== 'boolean') normalized.hdr = false;
  if (normalized.encoding !== 'h264' && normalized.encoding !== 'hevc' && normalized.encoding !== 'av1') {
    normalized.encoding = 'h264';
  }
  if (normalized.hdr && normalized.encoding === 'h264') normalized.encoding = 'hevc';
  return normalized;
}

function loadCachedConfig(): void {
  try {
    const raw = window.localStorage.getItem(CLIENT_CONFIG_STORAGE_KEY);
    if (!raw) return;
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== 'object') return;
    Object.assign(config, normalizeProfileConfig(parsed as StreamConfig));
  } catch { /* ignore */ }
}

function persistCachedConfig(): void {
  try {
    const snapshot = normalizeProfileConfig({ ...config });
    window.localStorage.setItem(CLIENT_CONFIG_STORAGE_KEY, JSON.stringify(snapshot));
  } catch { /* ignore */ }
}

watch(() => config.hdr, (enabled) => { if (enabled) ensureHdrEncoding(); });
watch(() => config.encoding, () => { if (config.hdr) ensureHdrEncoding(); });
watch(() => config.hdr, (enabled) => { if (!enabled) hdrRuntimeWarning.value = null; });
watch(() => config.encoding, () => { hdrRuntimeWarning.value = null; });
watch(() => ({ ...config }), () => { persistCachedConfig(); }, { deep: true });

const appsStore = useAppsStore();
const { apps } = storeToRefs(appsStore);
const appsList = computed(() => (apps.value || []).slice());

// Search and filtering
const searchQuery = ref('');

// Track which apps have valid cover images (loaded successfully)
const appCoverStatus = ref<Map<string, boolean>>(new Map());

function onCoverLoad(app: App) {
  if (app.uuid) appCoverStatus.value.set(app.uuid, true);
}

function onCoverError(app: App) {
  if (app.uuid) appCoverStatus.value.set(app.uuid, false);
}

function appHasCover(app: App): boolean {
  // Check if we've already loaded this cover
  if (app.uuid && appCoverStatus.value.has(app.uuid)) {
    return appCoverStatus.value.get(app.uuid) === true;
  }
  // Assume apps with image-path or playnite-id have covers until proven otherwise
  return !!(app['image-path'] || app['playnite-id']);
}

const filteredApps = computed(() => {
  const query = searchQuery.value.trim().toLowerCase();
  if (!query) return appsList.value;
  return appsList.value.filter(app => {
    const name = (app.name || '').toLowerCase();
    return name.includes(query);
  });
});

const appsWithCovers = computed(() => filteredApps.value.filter(app => appHasCover(app)));
const appsWithoutCovers = computed(() => filteredApps.value.filter(app => !appHasCover(app)));

const selectedAppId = ref<number | null>(null);
const resumeOnConnect = ref(true);
const terminatePending = ref(false);
const sessionStatus = ref<{ activeSessions: number; appRunning: boolean; paused: boolean } | null>(null);
let sessionStatusTimer: number | null = null;

function appKey(app: App): string { return `${app.uuid || ''}-${app.name || 'app'}`; }

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

function selectApp(app: App) {
  const id = appNumericId(app);
  if (id == null) return;
  selectedAppId.value = id;
  resumeOnConnect.value = false;
}

async function onAppDoubleClick(app: App) {
  if (isConnected.value || isConnecting.value) return;
  selectApp(app);
  await connect();
}

function clearSelection() {
  selectedAppId.value = null;
  resumeOnConnect.value = true;
}

const selectedAppLabel = computed(() => {
  if (!selectedAppId.value) return 'No app selected';
  const selected = appsList.value.find((app) => appNumericId(app) === selectedAppId.value);
  return selected?.name ? selected.name : `App ${selectedAppId.value}`;
});

const selectedAppName = computed(() => {
  if (!selectedAppId.value) return null;
  const selected = appsList.value.find((app) => appNumericId(app) === selectedAppId.value);
  return selected?.name ?? null;
});

const hasRunningSession = computed(() => {
  if (!sessionStatus.value) return false;
  return sessionStatus.value.appRunning || sessionStatus.value.activeSessions > 0;
});

const resumeAvailable = computed(() => {
  if (selectedAppId.value) return false;
  if (!sessionStatus.value) return false;
  return sessionStatus.value.activeSessions > 0 || sessionStatus.value.paused;
});

const api = new WebRtcHttpApi();
const client = new WebRtcClient(api);

const isConnecting = ref(false);
const isConnected = ref(false);

function setWebRtcActive(active: boolean): void {
  try { (window as any).__sunshine_webrtc_active = active; } catch { /* ignore */ }
}

watch(() => [isConnecting.value, isConnected.value] as const, ([connecting, connected]) => {
  setWebRtcActive(connecting || connected);
}, { immediate: true });

const connectLabelKey = computed(() => {
  if (isConnecting.value) return 'webrtc.connecting';
  if (isConnected.value) return 'webrtc.disconnect';
  if (resumeAvailable.value) return 'webrtc.resume';
  if (selectedAppId.value) return 'webrtc.connect';
  return 'webrtc.stream_desktop';
});

const showStartingOverlay = computed(() => {
  if (isConnected.value) return false;
  return isConnecting.value || connectionState.value === 'connecting';
});

const connectionState = ref<RTCPeerConnectionState | null>(null);
const iceState = ref<RTCIceConnectionState | null>(null);
const inputChannelState = ref<RTCDataChannelState | null>(null);
const stats = ref<WebRtcStatsSnapshot>({});
const inputEnabled = ref(true);
const showOverlay = ref(false);
const inputTarget = ref<HTMLElement | null>(null);
const videoEl = ref<HTMLVideoElement | null>(null);
const canvasEl = ref<HTMLCanvasElement | null>(null);
const audioEl = ref<HTMLAudioElement | null>(null);
const isFullscreen = ref(false);
const pseudoFullscreen = ref(false);
const nativeVideoFullscreen = ref(false);
const autoFullscreen = ref(true);
const sessionId = ref<string | null>(null);
const serverSession = ref<WebRtcSessionState | null>(null);
const serverVideoFps = ref<number | undefined>(undefined);

let lastServerSample: { ts: number; videoPackets?: number } | null = null;
const remoteStreamInfo = ref<{ id: string; videoTracks: number; audioTracks: number } | null>(null);
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
  };
});

const videoSizeLabel = computed(() => {
  const width = videoDebug.value?.width ?? 0;
  const height = videoDebug.value?.height ?? 0;
  return width > 0 && height > 0 ? `${width}x${height}` : '--';
});

const inputMetrics = ref<InputCaptureMetrics>({});
const inputBufferedAmount = ref<number | null>(null);
const INPUT_BUFFER_DROP_THRESHOLD_BYTES = 1024;

const shouldDropInput = (payload: InputMessage) => {
  const buffered = client.inputChannelBufferedAmount ?? 0;
  inputBufferedAmount.value = buffered;
  if (buffered <= INPUT_BUFFER_DROP_THRESHOLD_BYTES) return false;
  if (payload.type === 'mouse_move') return true;
  if (payload.type === 'gamepad_state' || payload.type === 'gamepad_motion') return true;
  return false;
};

const videoFrameMetrics = ref<{
  lastIntervalMs?: number;
  avgIntervalMs?: number;
  maxIntervalMs?: number;
  p98IntervalMs?: number;
  avg98IntervalMs?: number;
  p99IntervalMs?: number;
  avg99IntervalMs?: number;
  lastDelayMs?: number;
  avgDelayMs?: number;
  maxDelayMs?: number;
}>({});

const videoPacingMetrics = ref<{
  dtMs?: number | null;
  presentedDelta?: number | null;
  now?: number;
  expectedDisplayTime?: number;
  mediaTime?: number;
  processingDuration?: number;
  receiveTime?: number;
  rtpTimestamp?: number;
}>({});

const inboundVideoStats = ref<{
  fpsReceived?: number;
  fpsDecoded?: number;
  framesDropped?: number;
  avgJitterBufferMs?: number | null;
  avgDecodeMsPerFrame?: number | null;
  packetsLostDelta?: number;
  jitter?: number;
}>({});

type DiagnosticsSample = {
  ts: number;
  pacingDtMs?: number | null;
  presentedDelta?: number | null;
  renderIntervalMs?: number;
  renderDelayMs?: number;
  fpsReceived?: number;
  fpsDecoded?: number;
  framesDropped?: number;
  avgJitterBufferMs?: number | null;
  avgDecodeMsPerFrame?: number | null;
  packetsLostDelta?: number;
  jitter?: number;
  serverQueue?: number;
  serverInflight?: number;
  serverVideoAgeMs?: number;
  serverFps?: number;
};

const DIAGNOSTICS_WINDOW_MS = 30000;
const diagnosticsSamples = ref<DiagnosticsSample[]>([]);
let diagnosticsSampleTimer: number | null = null;

const renderFps = computed(() => {
  const intervalMs = videoFrameMetrics.value.lastIntervalMs ?? videoFrameMetrics.value.avgIntervalMs;
  if (typeof intervalMs !== 'number' || !Number.isFinite(intervalMs) || intervalMs <= 0) return undefined;
  return 1000 / intervalMs;
});

const renderFps98 = computed(() => {
  const intervalMs = videoFrameMetrics.value.avg98IntervalMs ?? videoFrameMetrics.value.p98IntervalMs;
  if (typeof intervalMs !== 'number' || !Number.isFinite(intervalMs) || intervalMs <= 0) return undefined;
  return 1000 / intervalMs;
});

const renderFps99 = computed(() => {
  const intervalMs = videoFrameMetrics.value.avg99IntervalMs ?? videoFrameMetrics.value.p99IntervalMs;
  if (typeof intervalMs !== 'number' || !Number.isFinite(intervalMs) || intervalMs <= 0) return undefined;
  return 1000 / intervalMs;
});

const renderDelayMs = computed(() => videoFrameMetrics.value.lastDelayMs ?? videoFrameMetrics.value.avgDelayMs);
const renderIntervalMs = computed(() => videoFrameMetrics.value.lastIntervalMs ?? videoFrameMetrics.value.avgIntervalMs);

const LATENCY_SAMPLE_WINDOW_MS = 30000;
const LATENCY_SMOOTH_TAU_MS = 2000;
const LATENCY_FAST_TAU_MS = 300;
const LATENCY_FAST_TRIGGER_MS = 12;
const LATENCY_FAST_TRIGGER_RATIO = 1.15;
const VIDEO_FPS_SMOOTH_TAU_MS = 1500;
const latencySamples = ref<{ ts: number; value: number }[]>([]);
const smoothedLatencyMs = ref<number | undefined>(undefined);
let lastLatencySampleAt: number | null = null;
const videoJitterBufferMs = computed(() => stats.value.videoJitterBufferMs);
const oneWayRttMs = computed(() => stats.value.roundTripTimeMs ? stats.value.roundTripTimeMs / 2 : undefined);
const videoPlayoutDelayMs = computed(() => stats.value.videoPlayoutDelayMs ?? stats.value.videoJitterBufferMs);
const smoothedVideoFps = ref<number | undefined>(undefined);
let lastVideoFpsSampleAt: number | null = null;

const displayVideoFps = computed(() =>
  renderFps99.value ?? renderFps98.value ?? renderFps.value ?? smoothedVideoFps.value ?? stats.value.videoFps
);

const estimatedLatencyMs = computed(() => {
  const parts = [oneWayRttMs.value, videoPlayoutDelayMs.value, stats.value.videoDecodeMs].filter(
    (value) => typeof value === 'number',
  ) as number[];
  if (!parts.length) return undefined;
  return parts.reduce((total, value) => total + value, 0);
});

watch(() => estimatedLatencyMs.value, (value) => {
  if (typeof value !== 'number' || Number.isNaN(value)) return;
  const now = Date.now();
  const lastAt = lastLatencySampleAt ?? now;
  const deltaMs = Math.max(0, now - lastAt);
  const current = smoothedLatencyMs.value;
  const jumpMs = typeof current === 'number' && Number.isFinite(current) ? value - current : undefined;
  const jumpRatio = typeof current === 'number' && Number.isFinite(current) && current > 0 ? value / current : undefined;
  const useFastTau = jumpMs != null && (jumpMs >= LATENCY_FAST_TRIGGER_MS || (jumpRatio != null && jumpRatio >= LATENCY_FAST_TRIGGER_RATIO));
  const tauMs = useFastTau ? LATENCY_FAST_TAU_MS : LATENCY_SMOOTH_TAU_MS;
  const alpha = 1 - Math.exp(-deltaMs / tauMs);
  if (smoothedLatencyMs.value == null || !Number.isFinite(smoothedLatencyMs.value)) {
    smoothedLatencyMs.value = value;
  } else {
    smoothedLatencyMs.value = smoothedLatencyMs.value + alpha * (value - smoothedLatencyMs.value);
  }
  lastLatencySampleAt = now;
  latencySamples.value.push({ ts: now, value });
  const cutoff = now - LATENCY_SAMPLE_WINDOW_MS;
  while (latencySamples.value.length && latencySamples.value[0].ts < cutoff) {
    latencySamples.value.shift();
  }
});

watch(() => stats.value.videoFps, (value) => {
  if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0) return;
  const now = Date.now();
  const lastAt = lastVideoFpsSampleAt ?? now;
  const deltaMs = Math.max(0, now - lastAt);
  const alpha = 1 - Math.exp(-deltaMs / VIDEO_FPS_SMOOTH_TAU_MS);
  if (smoothedVideoFps.value == null || !Number.isFinite(smoothedVideoFps.value)) {
    smoothedVideoFps.value = value;
  } else {
    smoothedVideoFps.value = smoothedVideoFps.value + alpha * (value - smoothedVideoFps.value);
  }
  lastVideoFpsSampleAt = now;
});

const overlayLines = computed(() => {
  const fps = displayVideoFps.value ? displayVideoFps.value.toFixed(0) : '--';
  const bitrate = formatKbps(stats.value.videoBitrateKbps);
  const latency = formatMs(smoothedLatencyMs.value);
  const dropped = stats.value.videoFramesDropped ?? '--';
  const codec = stats.value.videoCodec ?? '--';
  return [
    `FPS: ${fps} | Bitrate: ${bitrate}`,
    `Latency: ${latency} | Dropped: ${dropped}`,
    `Codec: ${codec} | Size: ${videoSizeLabel.value}`,
  ];
});

// Frame pacer
const useFramePacer = ref(true);
const pacerDiagnostics = ref(false);
let framePacer: FramePacer | null = null;

function startFramePacer(): void {
  if (!canvasEl.value || !videoEl.value) return;
  destroyFramePacer();
  framePacer = new FramePacer(
    videoEl.value,
    canvasEl.value,
    {
      onMetrics: (metrics: FramePacerMetrics) => {
        videoPacingMetrics.value = {
          dtMs: metrics.lastRafIntervalMs ?? null,
          presentedDelta: null,
        };
      },
      onError: (error: Error) => {
        pushVideoEvent(`pacer-error:${error.message}`);
      },
    },
    { diagnosticsEnabled: pacerDiagnostics.value }
  );
  if (!framePacer.init()) {
    framePacer.destroy();
    framePacer = null;
    return;
  }
  framePacer.start();
}

function destroyFramePacer(): void {
  if (framePacer) {
    framePacer.destroy();
    framePacer = null;
  }
}

watch(pacerDiagnostics, (enabled) => {
  if (framePacer) {
    framePacer.diagnosticsEnabled = enabled;
  }
});

// Video/Audio stream handling
let videoStream: MediaStream | null = null;
let audioStream: MediaStream | null = null;
let audioAutoplayRequested = false;
let audioPlaybackUnlocked = false;
let lastAudioPlayAttemptAtMs = 0;
let lastAudioPlayErrorAtMs = 0;
let audioPlayRetryTimer: number | null = null;
let audioPlayRetryUntilMs: number | null = null;
let detachInput: (() => void) | null = null;
let detachVideoEvents: (() => void) | null = null;
let detachVideoFrames: (() => void) | null = null;
let detachVideoPacing: (() => void) | null = null;
let detachVideoFullscreenEvents: (() => void) | null = null;
let stopInboundVideoStatsTimer: (() => void) | null = null;
let lastTrackSnapshot: { videoReady?: string; audioReady?: string } | null = null;

// Video drain/pacing state
const VIDEO_MODE_SWITCH_DRAIN_MS = 800;
const VIDEO_STARTUP_DRAIN_MS = 2500;
let modeSwitchDrainUntil: number | null = null;
let videoStartupDrainUntil: number | null = null;
let videoStartupDrainReleaseSince: number | null = null;
let lastVideoTargetMs: number | undefined = undefined;
let lastPlaybackRateUpdateAt: number | null = null;

const AUDIO_TARGET_BUFFER_MS = 60;
const AUDIO_TARGET_PLAYOUT_MS = 40;

function resolveVideoBaseTargetMs(): number { return (1000 / config.fps) * 2.5; }
function resolveVideoStartupTargetMs(): number { return resolveVideoBaseTargetMs() * 3; }

function setVideoDrainMode(_mode: string, _baseTarget: number, _startupTarget: number): void { /* simplified */ }
function applyVideoTargetMs(_targetMs: number): void { /* simplified */ }
function triggerVideoDrainWindow(_durationMs: number, _reason: string): void { /* simplified */ }
function resetVideoDrainState(): void { /* simplified */ }
function resetAudioDrainState(): void { /* simplified */ }
function resetServerRates(): void { lastServerSample = null; serverVideoFps.value = undefined; }

let serverSessionTimer: number | null = null;

function stopServerSessionPolling(): void {
  if (serverSessionTimer) { window.clearInterval(serverSessionTimer); serverSessionTimer = null; }
}

function startServerSessionPolling(): void {
  stopServerSessionPolling();
  if (!sessionId.value) return;
  const poll = async () => {
    if (!sessionId.value) return;
    try {
      const result = await api.getSessionState(sessionId.value);
      if (result.state) {
        serverSession.value = result.state;
        const now = Date.now();
        if (lastServerSample && typeof result.state.video_packets === 'number') {
          const dt = (now - lastServerSample.ts) / 1000;
          const packets = result.state.video_packets - (lastServerSample.videoPackets ?? 0);
          if (dt > 0) serverVideoFps.value = packets / dt;
        }
        lastServerSample = { ts: now, videoPackets: result.state.video_packets };
      }
    } catch { /* ignore */ }
  };
  void poll();
  serverSessionTimer = window.setInterval(poll, 1000);
}

let webrtcDiagTimer: number | null = null;
const WEBRTC_DIAG_LOG_INTERVAL_MS = 5000;

function startWebrtcDiagnostics(): void {
  stopWebrtcDiagnostics();
  webrtcDiagTimer = window.setInterval(() => {
    if (!isConnected.value) return;
    // Diagnostics logging (simplified)
  }, WEBRTC_DIAG_LOG_INTERVAL_MS);
}

function stopWebrtcDiagnostics(): void {
  if (webrtcDiagTimer != null) { window.clearInterval(webrtcDiagTimer); webrtcDiagTimer = null; }
}

function stopDiagnosticsSampling(): void {
  if (diagnosticsSampleTimer != null) { window.clearInterval(diagnosticsSampleTimer); diagnosticsSampleTimer = null; }
}

function startDiagnosticsSampling(): void {
  stopDiagnosticsSampling();
  diagnosticsSampleTimer = window.setInterval(() => {
    if (!isConnected.value) return;
    const now = Date.now();
    const sample: DiagnosticsSample = {
      ts: now,
      pacingDtMs: videoPacingMetrics.value.dtMs ?? null,
      presentedDelta: videoPacingMetrics.value.presentedDelta ?? null,
      renderIntervalMs: renderIntervalMs.value,
      renderDelayMs: renderDelayMs.value,
      fpsReceived: inboundVideoStats.value.fpsReceived,
      fpsDecoded: inboundVideoStats.value.fpsDecoded,
      framesDropped: inboundVideoStats.value.framesDropped,
      avgJitterBufferMs: inboundVideoStats.value.avgJitterBufferMs ?? null,
      avgDecodeMsPerFrame: inboundVideoStats.value.avgDecodeMsPerFrame ?? null,
      packetsLostDelta: inboundVideoStats.value.packetsLostDelta,
      jitter: inboundVideoStats.value.jitter,
      serverQueue: serverSession.value?.video_queue_frames,
      serverInflight: serverSession.value?.video_inflight_frames,
      serverVideoAgeMs: serverSession.value?.last_video_age_ms,
      serverFps: serverVideoFps.value,
    };
    diagnosticsSamples.value.push(sample);
    const cutoff = now - DIAGNOSTICS_WINDOW_MS;
    while (diagnosticsSamples.value.length && diagnosticsSamples.value[0].ts < cutoff) {
      diagnosticsSamples.value.shift();
    }
  }, 1000);
}

function stopAudioPlayRetry(): void {
  if (audioPlayRetryTimer != null) { window.clearInterval(audioPlayRetryTimer); audioPlayRetryTimer = null; }
  audioPlayRetryUntilMs = null;
}

function ensureAudioPlayback(reason: string): void {
  if (!audioAutoplayRequested) return;
  if (!audioEl.value) return;
  if (!audioStream) audioStream = new MediaStream();
  if (audioEl.value.srcObject !== audioStream) audioEl.value.srcObject = audioStream;
  audioEl.value.volume = 1;
  const hasTrack = audioStream.getAudioTracks().length > 0;
  if (!hasTrack) audioEl.value.muted = true;
  const now = Date.now();
  if (now - lastAudioPlayAttemptAtMs < 250) return;
  lastAudioPlayAttemptAtMs = now;
  const playPromise = (() => {
    try { return audioEl.value.play(); }
    catch (error) {
      const name = error && typeof error === 'object' ? (error as any).name : '';
      if (now - lastAudioPlayErrorAtMs > 1500) {
        lastAudioPlayErrorAtMs = now;
        pushVideoEvent(`audio-play-throw${name ? `:${name}` : ''}:${reason}`);
      }
      return null;
    }
  })();
  if (!playPromise || typeof (playPromise as any).then !== 'function') return;
  playPromise.then(() => {
    if (!audioEl.value) return;
    if (!audioEl.value.paused) {
      audioPlaybackUnlocked = true;
      if (hasTrack) stopAudioPlayRetry();
    }
  }).catch((error) => {
    const name = error && typeof error === 'object' ? (error as any).name : '';
    if (now - lastAudioPlayErrorAtMs > 1500) {
      lastAudioPlayErrorAtMs = now;
      pushVideoEvent(`audio-play-error${name ? `:${name}` : ''}:${reason}`);
    }
  });
}

function primeAudioAutoplay(): void {
  if (!audioEl.value) return;
  if (!audioStream) audioStream = new MediaStream();
  audioPlaybackUnlocked = false;
  audioEl.value.srcObject = audioStream;
  audioEl.value.volume = 1;
  audioEl.value.muted = true;
  stopAudioPlayRetry();
  audioPlayRetryUntilMs = Date.now() + 8000;
  audioPlayRetryTimer = window.setInterval(() => {
    if (!audioAutoplayRequested) { stopAudioPlayRetry(); return; }
    if (audioPlayRetryUntilMs != null && Date.now() > audioPlayRetryUntilMs) { stopAudioPlayRetry(); return; }
    ensureAudioPlayback('retry');
  }, 500);
  ensureAudioPlayback('prime');
}

function stopSessionStatusPolling(): void {
  if (sessionStatusTimer) { window.clearInterval(sessionStatusTimer); sessionStatusTimer = null; }
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
  } catch { /* ignore */ }
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
let fullscreenKeyboardLockRequested = false;

function getFullscreenElement(): Element | null {
  return document.fullscreenElement ?? (document as any).webkitFullscreenElement ?? null;
}

function isIosPhone(): boolean {
  try {
    const ua = navigator.userAgent ?? '';
    return /\b(iPhone|iPod)\b/i.test(ua);
  } catch {
    return false;
  }
}

function isNativeVideoFullscreenActive(): boolean {
  if (nativeVideoFullscreen.value) return true;
  try {
    const anyVideo = videoEl.value as any;
    return Boolean(anyVideo?.webkitDisplayingFullscreen);
  } catch {
    return false;
  }
}

async function requestFullscreen(target: HTMLElement): Promise<boolean> {
  const anyTarget = target as any;
  if (typeof target.requestFullscreen === 'function') {
    try {
      await target.requestFullscreen();
      return true;
    } catch { /* try fallback */ }
  }
  if (typeof anyTarget.webkitRequestFullscreen === 'function') {
    try {
      const result = anyTarget.webkitRequestFullscreen();
      if (result && typeof result.then === 'function') await result;
      return true;
    } catch { /* try fallback */ }
  }
  return false;
}

function tryEnterNativeVideoFullscreen(): boolean {
  const video = videoEl.value;
  if (!video) return false;
  const anyVideo = video as any;
  const enter = anyVideo?.webkitEnterFullscreen ?? anyVideo?.webkitEnterFullScreen;
  if (typeof enter !== 'function') return false;
  try {
    enter.call(video);
    return true;
  } catch {
    return false;
  }
}

async function tryEnterFullscreen(target: HTMLElement): Promise<boolean> {
  const video = videoEl.value;
  // iOS phones are the most restrictive; prefer native video fullscreen there.
  if (isIosPhone() && video) {
    if (await requestFullscreen(video)) return true;
    if (tryEnterNativeVideoFullscreen()) return true;
    if (await requestFullscreen(target)) return true;
    return false;
  }

  if (await requestFullscreen(target)) return true;
  if (video) {
    if (await requestFullscreen(video)) return true;
    if (tryEnterNativeVideoFullscreen()) return true;
  }
  return false;
}

async function exitFullscreen(): Promise<void> {
  const anyDoc = document as any;
  if (typeof document.exitFullscreen === 'function') { await document.exitFullscreen(); return; }
  if (typeof anyDoc.webkitExitFullscreen === 'function') {
    const result = anyDoc.webkitExitFullscreen();
    if (result && typeof result.then === 'function') await result;
  }
}

function isFullscreenActive(): boolean {
  if (isNativeVideoFullscreenActive()) return true;
  const fullscreenEl = getFullscreenElement();
  return fullscreenEl === inputTarget.value || fullscreenEl === videoEl.value;
}

function isTabActive(): boolean {
  try {
    const visible = typeof document !== 'undefined' ? document.visibilityState === 'visible' : true;
    const focus = typeof document !== 'undefined' && document.hasFocus ? document.hasFocus() : true;
    return visible && focus;
  } catch { return true; }
}

const onFullscreenChange = () => {
  const active = isFullscreenActive();
  if (active) pseudoFullscreen.value = false;
  isFullscreen.value = active || pseudoFullscreen.value;
  if (!isFullscreen.value) { cancelEscHold(); releaseFullscreenKeyboardLock(); }
  modeSwitchDrainUntil = Date.now() + VIDEO_MODE_SWITCH_DRAIN_MS;
  triggerVideoDrainWindow(VIDEO_MODE_SWITCH_DRAIN_MS, 'fullscreen');
  ensureAudioPlayback('fullscreen');
};

const onOverlayHotkey = (event: KeyboardEvent) => {
  if (!event.ctrlKey || !event.altKey || !event.shiftKey) return;
  if (event.code !== 'KeyS') return;
  event.preventDefault();
  event.stopPropagation();
  showOverlay.value = !showOverlay.value;
};

const onPageHide = () => { void client.disconnect({ keepalive: true }); };

const onVisibilityChange = () => {
  if (document.visibilityState === 'visible') {
    modeSwitchDrainUntil = Date.now() + VIDEO_MODE_SWITCH_DRAIN_MS;
    triggerVideoDrainWindow(VIDEO_MODE_SWITCH_DRAIN_MS, 'resume');
  }
  ensureAudioPlayback('visibility');
};

const onAudioUserGesture = () => {
  if (!audioAutoplayRequested) return;
  if (audioPlayRetryUntilMs != null && Date.now() <= audioPlayRetryUntilMs) { ensureAudioPlayback('gesture'); return; }
  if (!audioPlaybackUnlocked && isConnected.value) ensureAudioPlayback('gesture');
};

const onFullscreenEscapeDown = (event: KeyboardEvent) => {
  if (event.code !== 'Escape') return;
  if (!isFullscreen.value) return;
  if (escHoldTimer) { event.preventDefault(); event.stopPropagation(); return; }
  event.preventDefault();
  event.stopPropagation();
  escHoldTimer = window.setTimeout(async () => {
    escHoldTimer = null;
    if (getFullscreenElement()) { try { await exitFullscreen(); } catch { /* ignore */ } }
  }, ESC_HOLD_MS);
};

const onFullscreenEscapeUp = (event: KeyboardEvent) => {
  if (event.code !== 'Escape') return;
  if (!isFullscreen.value) return;
  event.preventDefault();
  event.stopPropagation();
  cancelEscHold();
};

function cancelEscHold() { if (escHoldTimer) { window.clearTimeout(escHoldTimer); escHoldTimer = null; } }

function requestFullscreenKeyboardLock(): void {
  if (fullscreenKeyboardLockRequested) return;
  fullscreenKeyboardLockRequested = true;
  void requestKeyboardLock().then((locked) => { if (!locked) fullscreenKeyboardLockRequested = false; });
}

function releaseFullscreenKeyboardLock(): void {
  if (!fullscreenKeyboardLockRequested) return;
  fullscreenKeyboardLockRequested = false;
  releaseKeyboardLock();
}

function formatKbps(value?: number): string { return value ? `${value.toFixed(0)} kbps` : '--'; }
function formatMs(value?: number): string { return value != null ? `${value.toFixed(1)} ms` : '--'; }
function displayValue(value: unknown): string { return value === null || value === undefined || value === '' ? '--' : String(value); }

function pushVideoEvent(label: string): void {
  const stamp = new Date().toLocaleTimeString();
  videoEvents.value = [`${stamp} ${label}`, ...videoEvents.value].slice(0, 8);
  videoStateTick.value += 1;
}

function updateRemoteStreamInfo(stream: MediaStream): void {
  remoteStreamInfo.value = {
    id: stream.id,
    videoTracks: stream.getVideoTracks().length,
    audioTracks: stream.getAudioTracks().length,
  };
}

function updateVideoElement(stream: MediaStream): boolean {
  if (!videoEl.value) return false;
  const videoTracks = stream.getVideoTracks();
  if (!videoTracks.length) return false;
  if (!videoStream) videoStream = new MediaStream();
  videoStream.getVideoTracks().forEach((t) => videoStream!.removeTrack(t));
  videoTracks.forEach((t) => videoStream!.addTrack(t));
  videoEl.value.srcObject = videoStream;
  return true;
}

function updateAudioElement(stream: MediaStream): void {
  if (!audioEl.value) return;
  const audioTracks = stream.getAudioTracks();
  if (!audioTracks.length) return;
  if (!audioStream) audioStream = new MediaStream();
  audioStream.getAudioTracks().forEach((t) => audioStream!.removeTrack(t));
  audioTracks.forEach((t) => audioStream!.addTrack(t));
  audioEl.value.srcObject = audioStream;
  audioEl.value.muted = false;
}

function attachVideoDebug(el: HTMLVideoElement): () => void {
  const events = ['loadedmetadata', 'canplay', 'playing', 'waiting', 'stalled', 'suspend', 'error', 'ended'];
  const handlers = events.map((event) => {
    const handler = () => { pushVideoEvent(event); videoStateTick.value++; };
    el.addEventListener(event, handler);
    return { event, handler };
  });
  return () => { handlers.forEach(({ event, handler }) => el.removeEventListener(event, handler)); };
}

function attachVideoFrameMetrics(el: HTMLVideoElement): () => void {
  const intervalSamples: number[] = [];
  const delaySamples: number[] = [];
  const maxSamples = 120;
  let lastTs: number | null = null;

  if ('requestVideoFrameCallback' in el) {
    let handle = 0;
    const cb = (now: number, meta: VideoFrameCallbackMetadata) => {
      const interval = lastTs != null ? now - lastTs : null;
      lastTs = now;
      if (interval != null) {
        intervalSamples.push(interval);
        if (intervalSamples.length > maxSamples) intervalSamples.shift();
        const sorted = [...intervalSamples].sort((a, b) => a - b);
        const p98Idx = Math.floor(sorted.length * 0.98);
        const p99Idx = Math.floor(sorted.length * 0.99);
        videoFrameMetrics.value = {
          lastIntervalMs: interval,
          avgIntervalMs: sorted.reduce((a, b) => a + b, 0) / sorted.length,
          maxIntervalMs: sorted[sorted.length - 1],
          p98IntervalMs: sorted[p98Idx],
          avg98IntervalMs: sorted.slice(0, p98Idx + 1).reduce((a, b) => a + b, 0) / (p98Idx + 1),
          p99IntervalMs: sorted[p99Idx],
          avg99IntervalMs: sorted.slice(0, p99Idx + 1).reduce((a, b) => a + b, 0) / (p99Idx + 1),
        };
      }
      handle = el.requestVideoFrameCallback(cb);
    };
    handle = el.requestVideoFrameCallback(cb);
    return () => { if (handle) el.cancelVideoFrameCallback(handle); };
  }

  let rafId = 0;
  let lastT = el.currentTime;
  const raf = (now: number) => {
    if (el.currentTime !== lastT) {
      const interval = lastTs != null ? now - lastTs : null;
      lastTs = now;
      lastT = el.currentTime;
      if (interval != null) {
        intervalSamples.push(interval);
        if (intervalSamples.length > maxSamples) intervalSamples.shift();
        videoFrameMetrics.value = {
          lastIntervalMs: interval,
          avgIntervalMs: intervalSamples.reduce((a, b) => a + b, 0) / intervalSamples.length,
        };
      }
    }
    rafId = requestAnimationFrame(raf);
  };
  rafId = requestAnimationFrame(raf);
  return () => cancelAnimationFrame(rafId);
}

function attachVideoPacingProbe(
  el: HTMLVideoElement,
  onSample: (sample: { dtMs?: number | null; presentedDelta?: number | null; now?: number; mediaTime?: number }) => void
): () => void {
  if ('requestVideoFrameCallback' in el) {
    let handle = 0;
    let lastNow: number | null = null;
    let lastPresented: number | null = null;
    const cb = (now: number, meta: VideoFrameCallbackMetadata) => {
      const dtMs = lastNow != null ? now - lastNow : null;
      const presentedFrames = (meta as any).presentedFrames;
      const presentedDelta = lastPresented != null && typeof presentedFrames === 'number' ? presentedFrames - lastPresented : null;
      onSample({ dtMs, presentedDelta, now, mediaTime: meta.mediaTime });
      lastPresented = presentedFrames ?? lastPresented;
      lastNow = now;
      handle = el.requestVideoFrameCallback(cb);
    };
    handle = el.requestVideoFrameCallback(cb);
    return () => { if (handle) el.cancelVideoFrameCallback(handle); };
  }

  let rafId = 0;
  let lastT = el.currentTime;
  const raf = (now: number) => {
    if (el.currentTime !== lastT) {
      onSample({ dtMs: null, presentedDelta: null, now, mediaTime: el.currentTime });
      lastT = el.currentTime;
    }
    rafId = requestAnimationFrame(raf);
  };
  rafId = requestAnimationFrame(raf);
  return () => cancelAnimationFrame(rafId);
}

function startInboundVideoStats(
  pc: RTCPeerConnection,
  onStats: (stats: { fpsReceived?: number; fpsDecoded?: number; framesDropped?: number; avgJitterBufferMs?: number | null; avgDecodeMsPerFrame?: number | null; packetsLostDelta?: number; jitter?: number }) => void,
  intervalMs = 1000
): () => void {
  let prev: { now: number; framesReceived?: number; framesDecoded?: number; framesDropped?: number; packetsLost?: number; jitter?: number; jitterBufferDelay?: number; jitterBufferEmittedCount?: number; totalDecodeTime?: number } | null = null;
  const id = window.setInterval(async () => {
    try {
      const report = await pc.getStats();
      let best: any = null;
      report.forEach((s) => {
        if (s.type !== 'inbound-rtp') return;
        if (s.kind !== 'video' && s.mediaType !== 'video') return;
        const frames = typeof s.framesReceived === 'number' ? s.framesReceived : 0;
        if (!best || frames > (best.framesReceived ?? 0)) best = s;
      });
      if (!best) return;
      const now = performance.now();
      const cur = {
        now,
        framesReceived: best.framesReceived,
        framesDecoded: best.framesDecoded,
        framesDropped: best.framesDropped,
        packetsLost: best.packetsLost,
        jitter: best.jitter,
        jitterBufferDelay: best.jitterBufferDelay,
        jitterBufferEmittedCount: best.jitterBufferEmittedCount,
        totalDecodeTime: best.totalDecodeTime,
      };
      if (prev) {
        const dt = (cur.now - prev.now) / 1000;
        const dRecv = typeof cur.framesReceived === 'number' && typeof prev.framesReceived === 'number' ? cur.framesReceived - prev.framesReceived : undefined;
        const dDec = typeof cur.framesDecoded === 'number' && typeof prev.framesDecoded === 'number' ? cur.framesDecoded - prev.framesDecoded : undefined;
        const dDrop = typeof cur.framesDropped === 'number' && typeof prev.framesDropped === 'number' ? cur.framesDropped - prev.framesDropped : undefined;
        const avgJbMs = typeof cur.jitterBufferDelay === 'number' && typeof cur.jitterBufferEmittedCount === 'number' && cur.jitterBufferEmittedCount > 0 ? (cur.jitterBufferDelay / cur.jitterBufferEmittedCount) * 1000 : null;
        const avgDecodeMs = typeof cur.totalDecodeTime === 'number' && typeof cur.framesDecoded === 'number' && cur.framesDecoded > 0 ? (cur.totalDecodeTime / cur.framesDecoded) * 1000 : null;
        onStats({
          fpsReceived: typeof dRecv === 'number' ? dRecv / dt : undefined,
          fpsDecoded: typeof dDec === 'number' ? dDec / dt : undefined,
          framesDropped: typeof dDrop === 'number' ? dDrop : undefined,
          avgJitterBufferMs: avgJbMs,
          avgDecodeMsPerFrame: avgDecodeMs,
          packetsLostDelta: typeof cur.packetsLost === 'number' && typeof prev.packetsLost === 'number' ? cur.packetsLost - prev.packetsLost : undefined,
          jitter: cur.jitter,
        });
      }
      prev = cur;
    } catch { /* ignore */ }
  }, intervalMs);
  return () => { window.clearInterval(id); };
}

async function confirmTerminateAndConnect(): Promise<void> {
  dialog.warning({
    title: t('webrtc.terminate_confirm_title'),
    content: t('webrtc.terminate_confirm_message', { app: selectedAppName.value ?? t('webrtc.terminate_confirm_app_fallback') }),
    positiveText: t('webrtc.terminate_confirm_action'),
    negativeText: t('_common.cancel'),
    onPositiveClick: async () => {
      await terminateSession();
      await startConnect();
    },
  });
}

async function waitForSpinnerFrame(): Promise<void> {
  await nextTick();
  await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
  await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
}

async function startConnect() {
  isConnecting.value = true;
  // Yield to allow the connecting spinner to render and start animating before heavy work
  await waitForSpinnerFrame();
  negotiatedEncoding.value = null;
  hdrRuntimeWarning.value = null;
  audioAutoplayRequested = true;
  primeAudioAutoplay();
  resetAudioDrainState();
  client.setAudioLatencyTargets(AUDIO_TARGET_BUFFER_MS, AUDIO_TARGET_PLAYOUT_MS);
  if (autoFullscreen.value && inputTarget.value && !isFullscreen.value) {
    try {
      const target = inputTarget.value;
      const entered = await tryEnterFullscreen(target);
      if (!entered) pseudoFullscreen.value = true;
      onFullscreenChange();
      try { target.focus(); } catch { /* ignore */ }
      requestFullscreenKeyboardLock();
    } catch { /* ignore */ }
  }
  ensureAudioPlayback('connect');
  stopServerSessionPolling();
  sessionId.value = null;
  serverSession.value = null;
  resetServerRates();
  try {
    // Determine app launch mode:
    // - If an app is selected, launch that app (appId = selectedAppId, resume = false)
    // - If no app selected but session can be resumed, resume it (appId = undefined, resume = true)
    // - If no app selected and nothing to resume, start desktop (appId = undefined, resume = false)
    const shouldResume = !selectedAppId.value && resumeOnConnect.value && resumeAvailable.value;
    const effectiveAppId = selectedAppId.value ?? undefined;
    const id = await client.connect(
      { ...config, appId: effectiveAppId, resume: shouldResume },
      {
        onRemoteStream: (stream) => {
          if (videoEl.value) {
            const hasVideo = updateVideoElement(stream);
            videoEl.value.muted = false;
            videoEl.value.volume = 1;
            updateRemoteStreamInfo(stream);
            updateAudioElement(stream);
            ensureAudioPlayback('remote-stream');
            if (hasVideo) {
              videoStartupDrainUntil = Date.now() + VIDEO_STARTUP_DRAIN_MS;
              videoStartupDrainReleaseSince = null;
              const baseTargetMs = resolveVideoBaseTargetMs();
              setVideoDrainMode('startup', baseTargetMs, resolveVideoStartupTargetMs());
              const playPromise = videoEl.value.play();
              if (playPromise && typeof playPromise.catch === 'function') {
                playPromise.catch((error) => {
                  const name = error && typeof error === 'object' ? (error as any).name : '';
                  pushVideoEvent(`play-error${name ? `:${name}` : ''}`);
                });
              }
              if (useFramePacer.value) setTimeout(() => startFramePacer(), 0);
            }
          }
        },
        onConnectionState: (state) => {
          connectionState.value = state;
          isConnected.value = state === 'connected';
          if (state === 'connected') {
            applyVideoTargetMs(resolveVideoBaseTargetMs());
            if (!stopInboundVideoStatsTimer) {
              const pc = client.peerConnection;
              if (pc) stopInboundVideoStatsTimer = startInboundVideoStats(pc, (sample) => { inboundVideoStats.value = sample; });
            }
            if (!diagnosticsSampleTimer) startDiagnosticsSampling();
          } else if (state === 'failed' || state === 'disconnected' || state === 'closed') {
            if (stopInboundVideoStatsTimer) { stopInboundVideoStatsTimer(); stopInboundVideoStatsTimer = null; }
            inboundVideoStats.value = {};
            stopDiagnosticsSampling();
            diagnosticsSamples.value = [];
          }
        },
        onIceState: (state) => { iceState.value = state; },
        onInputChannelState: (state) => { inputChannelState.value = state; },
        onInputMessage: (message) => { applyGamepadFeedback(message); },
        onStats: (snapshot) => { stats.value = snapshot; },
        onNegotiatedEncoding: (encoding) => {
          if (encoding === 'h264' || encoding === 'hevc' || encoding === 'av1') negotiatedEncoding.value = encoding;
        },
        onWarning: (warning) => {
          notifyWarning('Configuration Warning', warning);
          if (config.hdr && /^hdr\b/i.test(warning)) hdrRuntimeWarning.value = warning;
        },
      },
      { inputPriority: isFullscreenActive() || isTabActive() ? 'high' : 'low' }
    );
    sessionId.value = id;
    startServerSessionPolling();
  } catch (error) {
    const msg = error instanceof Error ? error.message : 'Failed to establish WebRTC session.';
    notifyError('Connection Failed', msg);
    console.error(error);
    audioAutoplayRequested = false;
    stopAudioPlayRetry();
  } finally {
    isConnecting.value = false;
    if (!isConnected.value) startSessionStatusPolling();
  }
}

async function connect() {
  if (isConnecting.value) return;
  // Always fetch session status to know if we can resume
  if (!sessionStatus.value) await fetchSessionStatus();
  if (selectedAppId.value && hasRunningSession.value) { await confirmTerminateAndConnect(); return; }
  await startConnect();
}

async function disconnect() {
  await client.disconnect();
  stopServerSessionPolling();
  isConnected.value = false;
  connectionState.value = null;
  iceState.value = null;
  inputChannelState.value = null;
  stats.value = {};
  inputMetrics.value = {};
  inputBufferedAmount.value = null;
  videoFrameMetrics.value = {};
  videoPacingMetrics.value = {};
  inboundVideoStats.value = {};
  diagnosticsSamples.value = [];
  stopDiagnosticsSampling();
  if (stopInboundVideoStatsTimer) { stopInboundVideoStatsTimer(); stopInboundVideoStatsTimer = null; }
  smoothedVideoFps.value = undefined;
  lastVideoFpsSampleAt = null;
  lastPlaybackRateUpdateAt = null;
  modeSwitchDrainUntil = null;
  detachInputCapture();
  destroyFramePacer();
  if (videoEl.value) {
    try { videoEl.value.playbackRate = 1; } catch { /* ignore */ }
    videoEl.value.srcObject = null;
  }
  if (audioEl.value) audioEl.value.srcObject = null;
  videoStream = null;
  audioStream = null;
  audioAutoplayRequested = false;
  stopAudioPlayRetry();
  resetAudioDrainState();
  resetVideoDrainState();
  lastVideoTargetMs = undefined;
  videoStartupDrainUntil = null;
  videoStartupDrainReleaseSince = null;
  sessionId.value = null;
  serverSession.value = null;
  resetServerRates();
  remoteStreamInfo.value = null;
  lastTrackSnapshot = null;
  videoEvents.value = [];
  videoStateTick.value += 1;
  startSessionStatusPolling();
}

async function terminateSession() {
  if (terminatePending.value) return;
  terminatePending.value = true;
  try {
    await http.post('/api/apps/close', {}, { validateStatus: () => true });
  } catch (error) {
    const msg = error instanceof Error ? error.message : 'Failed to terminate session.';
    notifyError('Termination Failed', msg);
  } finally {
    await disconnect();
    terminatePending.value = false;
  }
}

async function toggleFullscreen() {
  try {
    if (pseudoFullscreen.value && !isFullscreenActive()) {
      pseudoFullscreen.value = false;
      onFullscreenChange();
      releaseFullscreenKeyboardLock();
      return;
    }
    if (isFullscreenActive()) {
      await exitFullscreen();
      releaseFullscreenKeyboardLock();
      return;
    }
    if (!inputTarget.value) return;
    const target = inputTarget.value;
    const entered = await tryEnterFullscreen(target);
    if (!entered) pseudoFullscreen.value = true;
    onFullscreenChange();
    requestFullscreenKeyboardLock();
    try { target.focus(); } catch { /* ignore */ }
    requestFullscreenKeyboardLock();
  } catch { /* ignore */ }
}

async function onFullscreenDblClick() {
  if (isFullscreenActive()) return;
  await toggleFullscreen();
}

function detachInputCapture() {
  if (detachInput) { detachInput(); detachInput = null; }
}

watch(() => [inputEnabled.value, isConnected.value], ([enabled, connected]) => {
  detachInputCapture();
  if (!enabled || !connected || !inputTarget.value) { releaseFullscreenKeyboardLock(); return; }
  detachInput = attachInputCapture(
    inputTarget.value,
    (payload) => {
      client.sendInput(payload);
      inputBufferedAmount.value = client.inputChannelBufferedAmount ?? null;
    },
    {
      video: videoEl.value,
      onMetrics: (metrics) => { inputMetrics.value = metrics; },
      shouldDrop: shouldDropInput,
    }
  );
  if (isFullscreenActive()) requestFullscreenKeyboardLock();
});

function attachVideoFullscreenEvents(el: HTMLVideoElement): () => void {
  const onBegin = () => { nativeVideoFullscreen.value = true; onFullscreenChange(); };
  const onEnd = () => { nativeVideoFullscreen.value = false; onFullscreenChange(); };
  el.addEventListener('webkitbeginfullscreen', onBegin as EventListener);
  el.addEventListener('webkitendfullscreen', onEnd as EventListener);
  return () => {
    el.removeEventListener('webkitbeginfullscreen', onBegin as EventListener);
    el.removeEventListener('webkitendfullscreen', onEnd as EventListener);
  };
}

watch(videoEl, (el) => {
  if (detachVideoEvents) { detachVideoEvents(); detachVideoEvents = null; }
  if (detachVideoFrames) { detachVideoFrames(); detachVideoFrames = null; }
  if (detachVideoPacing) { detachVideoPacing(); detachVideoPacing = null; }
  if (detachVideoFullscreenEvents) { detachVideoFullscreenEvents(); detachVideoFullscreenEvents = null; }
  if (!el) return;
  detachVideoEvents = attachVideoDebug(el);
  detachVideoFrames = attachVideoFrameMetrics(el);
  detachVideoPacing = attachVideoPacingProbe(el, (sample) => { videoPacingMetrics.value = sample; });
  detachVideoFullscreenEvents = attachVideoFullscreenEvents(el);
});

onBeforeUnmount(() => {
  setWebRtcActive(false);
  document.removeEventListener('fullscreenchange', onFullscreenChange);
  document.removeEventListener('webkitfullscreenchange', onFullscreenChange as EventListener);
  document.removeEventListener('visibilitychange', onVisibilityChange);
  window.removeEventListener('pointerdown', onAudioUserGesture as EventListener, true);
  window.removeEventListener('keydown', onAudioUserGesture as EventListener, true);
  window.removeEventListener('keydown', onOverlayHotkey, true);
  window.removeEventListener('keydown', onFullscreenEscapeDown, true);
  window.removeEventListener('keyup', onFullscreenEscapeUp, true);
  window.removeEventListener('pagehide', onPageHide);
  cancelEscHold();
  if (detachVideoEvents) { detachVideoEvents(); detachVideoEvents = null; }
  if (detachVideoFrames) { detachVideoFrames(); detachVideoFrames = null; }
  if (detachVideoPacing) { detachVideoPacing(); detachVideoPacing = null; }
  if (detachVideoFullscreenEvents) { detachVideoFullscreenEvents(); detachVideoFullscreenEvents = null; }
  if (stopInboundVideoStatsTimer) { stopInboundVideoStatsTimer(); stopInboundVideoStatsTimer = null; }
  stopDiagnosticsSampling();
  stopWebrtcDiagnostics();
  stopSessionStatusPolling();
  releaseFullscreenKeyboardLock();
  stopServerSessionPolling();
  void disconnect();
});

onMounted(async () => {
  loadCachedConfig();
  document.addEventListener('fullscreenchange', onFullscreenChange);
  document.addEventListener('webkitfullscreenchange', onFullscreenChange as EventListener);
  document.addEventListener('visibilitychange', onVisibilityChange);
  window.addEventListener('pointerdown', onAudioUserGesture as EventListener, true);
  window.addEventListener('keydown', onAudioUserGesture as EventListener, true);
  window.addEventListener('keydown', onOverlayHotkey, true);
  window.addEventListener('keydown', onFullscreenEscapeDown, true);
  window.addEventListener('keyup', onFullscreenEscapeUp, true);
  window.addEventListener('pagehide', onPageHide);
  try { await appsStore.loadApps(true); } catch { /* ignore */ }
  encodingSupport.value = detectEncodingSupport();
  if (config.hdr) ensureHdrEncoding();
  startSessionStatusPolling();
});

watch(() => isConnected.value, (connected) => {
  if (connected) { stopSessionStatusPolling(); startWebrtcDiagnostics(); return; }
  stopWebrtcDiagnostics();
  startSessionStatusPolling();
});
</script>

<style scoped>
/* ============================================
   MODERN WEBRTC UI
   ============================================ */

.webrtc-app {
  --accent: rgb(var(--color-primary));
  --surface: rgb(var(--color-surface));
  --border: rgb(var(--color-dark) / 0.1);
  --text-1: rgb(var(--color-on-light));
  --text-2: rgb(var(--color-on-light) / 0.7);
  --text-3: rgb(var(--color-on-light) / 0.5);

  display: flex;
  min-height: 100vh;
  background: linear-gradient(135deg, rgb(var(--color-light)) 0%, rgb(var(--color-surface)) 100%);
  transition: padding-right 0.3s ease;
}

.dark .webrtc-app {
  --border: rgb(255 255 255 / 0.08);
  --text-1: rgb(var(--color-on-dark));
  --text-2: rgb(var(--color-on-dark) / 0.7);
  --text-3: rgb(var(--color-on-dark) / 0.5);
  background: linear-gradient(135deg, rgb(var(--color-dark)) 0%, rgb(10 12 20) 100%);
}

.webrtc-app.settings-open {
  padding-right: 380px;
}

@media (max-width: 768px) {
  .webrtc-app.settings-open {
    padding-right: 0;
  }
}

.main-content {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-width: 0;
}

/* Header */
.app-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 1rem 1.5rem;
  border-bottom: 1px solid var(--border);
  background: rgb(var(--color-surface) / 0.5);
  backdrop-filter: blur(10px);
  gap: 1rem;
  flex-wrap: wrap;
}

.header-left, .header-right {
  display: flex;
  align-items: center;
  gap: 0.75rem;
}

.header-center {
  display: flex;
  align-items: center;
  gap: 1rem;
  flex-wrap: wrap;
}

.brand {
  display: flex;
  align-items: center;
  gap: 0.625rem;
}

.brand-icon {
  width: 2rem;
  height: 2rem;
  background: linear-gradient(135deg, rgb(var(--color-primary)), rgb(var(--color-secondary)));
  border-radius: 0.5rem;
  display: flex;
  align-items: center;
  justify-content: center;
  color: white;
  font-size: 0.875rem;
  box-shadow: 0 2px 8px rgb(var(--color-primary) / 0.25);
}

.brand h1 {
  font-size: 1rem;
  font-weight: 600;
  margin: 0;
  color: var(--text-1);
}

.status-pill {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.375rem 0.875rem;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 2rem;
  font-size: 0.75rem;
  font-weight: 600;
  color: var(--text-2);
  transition: all 0.2s;
}

.status-pill.connected {
  background: rgb(var(--color-success) / 0.1);
  border-color: rgb(var(--color-success) / 0.3);
  color: rgb(var(--color-success));
}

.status-pill.connecting {
  background: rgb(var(--color-warning) / 0.1);
  border-color: rgb(var(--color-warning) / 0.3);
  color: rgb(var(--color-warning));
}

.status-dot {
  width: 0.5rem;
  height: 0.5rem;
  background: currentColor;
  border-radius: 50%;
  animation: pulse 2s ease-in-out infinite;
}

@keyframes pulse {
  0%, 100% { opacity: 0.5; }
  50% { opacity: 1; }
}

.settings-btn {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.5rem 0.875rem;
  background: rgb(var(--color-primary) / 0.1);
  border: 1px solid rgb(var(--color-primary) / 0.2);
  border-radius: 0.5rem;
  color: rgb(var(--color-primary));
  font-size: 0.8125rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.2s;
}

.settings-btn i {
  font-size: 0.875rem;
}

.settings-btn:hover {
  background: rgb(var(--color-primary) / 0.15);
  border-color: rgb(var(--color-primary) / 0.35);
}

.settings-btn.active {
  background: rgb(var(--color-primary));
  border-color: rgb(var(--color-primary));
  color: white;
}

/* Library Section */
.library-section {
  flex: 1;
  padding: 1.5rem;
  overflow-y: auto;
}

.library-header {
  display: flex;
  flex-direction: column;
  gap: 1rem;
  margin-bottom: 1.25rem;
}

.library-title-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 0.75rem;
}

.library-header h2 {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 1.125rem;
  font-weight: 600;
  margin: 0;
  color: var(--text-1);
}

.library-header h2 i {
  color: rgb(var(--color-primary));
}

/* Search Box */
.search-box {
  display: flex;
  align-items: center;
  gap: 0.625rem;
  padding: 0.625rem 0.875rem;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 0.5rem;
  transition: all 0.2s;
}

.search-box:focus-within {
  border-color: rgb(var(--color-primary) / 0.5);
  box-shadow: 0 0 0 3px rgb(var(--color-primary) / 0.1);
}

.search-box i {
  color: var(--text-3);
  font-size: 0.875rem;
}

.search-input {
  flex: 1;
  background: none;
  border: none;
  outline: none;
  font-size: 0.875rem;
  color: var(--text-1);
  min-width: 0;
}

.search-input::placeholder {
  color: var(--text-3);
}

.search-clear {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 1.25rem;
  height: 1.25rem;
  background: var(--text-3);
  border: none;
  border-radius: 50%;
  color: var(--surface);
  font-size: 0.625rem;
  cursor: pointer;
  opacity: 0.6;
  transition: opacity 0.2s;
}

.search-clear:hover {
  opacity: 1;
}

.selection-badge {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.375rem 0.75rem;
  background: rgb(var(--color-primary) / 0.1);
  border: 1px solid rgb(var(--color-primary) / 0.3);
  border-radius: 0.5rem;
  font-size: 0.75rem;
  font-weight: 500;
  color: rgb(var(--color-primary));
}

.selection-badge i {
  font-size: 0.75rem;
}

.clear-btn {
  background: none;
  border: none;
  padding: 0.25rem;
  cursor: pointer;
  color: inherit;
  opacity: 0.7;
  transition: opacity 0.2s;
}

.clear-btn:hover {
  opacity: 1;
}

/* Games Grid */
.games-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
  gap: 1rem;
}

@media (min-width: 1200px) {
  .games-grid {
    grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  }
}

.game-card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 0.75rem;
  overflow: hidden;
  cursor: pointer;
  transition: all 0.2s;
  text-align: left;
}

.game-card:hover {
  transform: translateY(-4px);
  box-shadow: 0 8px 24px rgb(0 0 0 / 0.15);
  border-color: rgb(var(--color-primary) / 0.3);
}

.game-card.selected {
  border-color: rgb(var(--color-primary));
  box-shadow: 0 0 0 2px rgb(var(--color-primary) / 0.2), 0 8px 24px rgb(0 0 0 / 0.15);
}

.game-cover {
  position: relative;
  aspect-ratio: 3 / 4;
  background: rgb(var(--color-dark) / 0.1);
  overflow: hidden;
}

.game-cover img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  transition: transform 0.3s;
}

.game-card:hover .game-cover img {
  transform: scale(1.05);
}

.cover-gradient {
  position: absolute;
  inset: 0;
  background: linear-gradient(180deg, transparent 50%, rgb(0 0 0 / 0.7) 100%);
}

.selected-badge {
  position: absolute;
  top: 0.5rem;
  right: 0.5rem;
  width: 1.5rem;
  height: 1.5rem;
  background: rgb(var(--color-primary));
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  color: white;
  font-size: 0.625rem;
}

.play-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgb(var(--color-primary) / 0.9);
  color: white;
  font-size: 2rem;
  opacity: 0;
  transition: opacity 0.2s;
}

.game-card:hover .play-overlay {
  opacity: 1;
}

.game-meta {
  padding: 0.75rem;
}

.game-name {
  display: block;
  font-size: 0.8125rem;
  font-weight: 600;
  color: var(--text-1);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.game-source {
  display: block;
  font-size: 0.6875rem;
  color: var(--text-3);
  margin-top: 0.125rem;
}

/* Other Applications Section (list view) */
.other-apps-section {
  margin-top: 1.5rem;
  padding-top: 1.5rem;
  border-top: 1px solid var(--border);
}

.section-label {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 0.875rem;
  font-weight: 600;
  color: var(--text-2);
  margin: 0 0 0.875rem;
}

.section-label i {
  font-size: 0.75rem;
  opacity: 0.7;
}

.apps-list {
  display: flex;
  flex-direction: column;
  gap: 0.375rem;
}

.app-list-item {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  padding: 0.625rem 0.875rem;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 0.5rem;
  cursor: pointer;
  transition: all 0.15s;
  text-align: left;
}

.app-list-item:hover {
  background: rgb(var(--color-primary) / 0.05);
  border-color: rgb(var(--color-primary) / 0.2);
}

.app-list-item.selected {
  background: rgb(var(--color-primary) / 0.1);
  border-color: rgb(var(--color-primary) / 0.4);
}

.app-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 2rem;
  height: 2rem;
  background: rgb(var(--color-primary) / 0.1);
  border-radius: 0.375rem;
  color: rgb(var(--color-primary));
  font-size: 0.875rem;
  flex-shrink: 0;
}

.app-info {
  flex: 1;
  min-width: 0;
}

.app-info .app-name {
  display: block;
  font-size: 0.8125rem;
  font-weight: 500;
  color: var(--text-1);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.app-info .app-source {
  display: block;
  font-size: 0.6875rem;
  color: var(--text-3);
  margin-top: 0.125rem;
}

.app-selected-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 1.5rem;
  height: 1.5rem;
  background: rgb(var(--color-primary));
  border-radius: 50%;
  color: white;
  font-size: 0.625rem;
  flex-shrink: 0;
}

.app-play-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 1.5rem;
  height: 1.5rem;
  color: var(--text-3);
  font-size: 0.75rem;
  flex-shrink: 0;
  opacity: 0;
  transition: opacity 0.15s;
}

.app-list-item:hover .app-play-icon {
  opacity: 1;
  color: rgb(var(--color-primary));
}

.app-list-item.selected .app-play-icon {
  display: none;
}

.empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 4rem 2rem;
  text-align: center;
  color: var(--text-3);
}

.empty-state i {
  font-size: 3rem;
  opacity: 0.3;
  margin-bottom: 1rem;
}

.empty-state h3 {
  font-size: 1.125rem;
  font-weight: 600;
  color: var(--text-2);
  margin: 0 0 0.5rem;
}

.empty-state p {
  margin: 0;
  font-size: 0.875rem;
}

/* Stream Preview */
.stream-preview {
  position: fixed;
  bottom: 1.5rem;
  right: 1.5rem;
  width: 400px;
  background: rgb(var(--color-surface));
  border: 1px solid var(--border);
  border-radius: 1rem;
  overflow: hidden;
  box-shadow: 0 8px 32px rgb(0 0 0 / 0.2);
  z-index: 100;
  transition: all 0.3s ease;
}

.webrtc-app.settings-open .stream-preview {
  right: calc(380px + 1.5rem);
}

@media (max-width: 768px) {
  .stream-preview {
    width: calc(100% - 2rem);
    left: 1rem;
    right: 1rem;
  }

  .webrtc-app.settings-open .stream-preview {
    right: 1rem;
  }
}

.stream-preview.minimized {
  width: 280px;
}

.stream-preview.minimized .stream-viewport,
.stream-preview.minimized .quick-actions,
.stream-preview.minimized .compact-metrics {
  display: none;
}

.stream-preview.expanded {
  position: fixed;
  inset: 0;
  width: 100%;
  border-radius: 0;
  z-index: 9999;
}

.preview-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.75rem 1rem;
  background: rgb(var(--color-dark) / 0.03);
  border-bottom: 1px solid var(--border);
}

.dark .preview-header {
  background: rgb(0 0 0 / 0.2);
}

.preview-title {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 0.8125rem;
  font-weight: 600;
  color: var(--text-1);
}

.preview-title i {
  color: rgb(var(--color-primary));
}

.live-indicator {
  display: inline-flex;
  align-items: center;
  gap: 0.25rem;
  padding: 0.125rem 0.5rem;
  background: rgb(var(--color-danger));
  border-radius: 0.25rem;
  font-size: 0.5625rem;
  font-weight: 700;
  color: white;
}

.live-dot {
  width: 0.375rem;
  height: 0.375rem;
  background: white;
  border-radius: 50%;
  animation: blink 1s ease-in-out infinite;
}

@keyframes blink {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.3; }
}

.preview-controls {
  display: flex;
  gap: 0.25rem;
}

.control-btn {
  width: 1.75rem;
  height: 1.75rem;
  display: flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: none;
  border-radius: 0.375rem;
  color: var(--text-2);
  cursor: pointer;
  transition: all 0.2s;
}

.control-btn:hover {
  background: rgb(var(--color-primary) / 0.1);
  color: rgb(var(--color-primary));
}

/* Stream Viewport */
.stream-viewport {
  position: relative;
  aspect-ratio: 16 / 9;
  background: rgb(0 0 0);
  outline: none;
}

.stream-viewport.fullscreen-mode {
  position: fixed;
  inset: 0;
  aspect-ratio: unset;
  z-index: 9999;
  cursor: none;
}

.stream-video {
  width: 100%;
  height: 100%;
  object-fit: contain;
}

.pacer-source-hidden {
  position: absolute;
  top: 0;
  left: 0;
  width: 1px !important;
  height: 1px !important;
  opacity: 0 !important;
  pointer-events: none !important;
}

.hidden {
  display: none !important;
}

.idle-state {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(135deg, rgb(var(--color-surface)) 0%, rgb(var(--color-dark) / 0.8) 100%);
}

.idle-content {
  text-align: center;
  color: var(--text-3);
}

.idle-content i {
  font-size: 2.5rem;
  opacity: 0.4;
  margin-bottom: 0.75rem;
  display: block;
}

.idle-content p {
  margin: 0;
  font-size: 0.8125rem;
}

.connecting-state {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 1rem;
  background: rgb(0 0 0 / 0.85);
  backdrop-filter: blur(8px);
}

.spinner {
  width: 2.5rem;
  height: 2.5rem;
  border: 3px solid var(--border);
  border-top-color: rgb(var(--color-primary));
  border-radius: 50%;
  animation: spin 1s linear infinite;
  /* Force GPU layer for smooth animation during heavy JS work */
  will-change: transform;
  transform: translateZ(0);
  backface-visibility: hidden;
}

@keyframes spin {
  to { transform: translateZ(0) rotate(360deg); }
}

.connecting-state span {
  font-size: 0.875rem;
  color: var(--text-2);
}

.stats-overlay {
  position: absolute;
  top: 0.5rem;
  left: 0.5rem;
  padding: 0.5rem 0.75rem;
  background: rgb(0 0 0 / 0.75);
  border-radius: 0.375rem;
  backdrop-filter: blur(8px);
}

.stat-line {
  font-family: ui-monospace, monospace;
  font-size: 0.625rem;
  line-height: 1.5;
  color: rgb(255 255 255 / 0.85);
}

/* Notification */
.notification-toast {
  position: absolute;
  top: 0.75rem;
  right: 0.75rem;
  display: flex;
  align-items: flex-start;
  gap: 0.75rem;
  padding: 0.75rem 1rem;
  background: rgb(30 30 35 / 0.95);
  border: 1px solid var(--border);
  border-radius: 0.5rem;
  backdrop-filter: blur(12px);
  max-width: 300px;
}

.notification-toast.error {
  border-color: rgb(var(--color-danger) / 0.4);
}

.notification-toast.warning {
  border-color: rgb(var(--color-warning) / 0.4);
}

.notification-toast.success {
  border-color: rgb(var(--color-success) / 0.4);
}

.notification-toast i {
  font-size: 1rem;
  margin-top: 0.125rem;
}

.notification-toast.error i { color: rgb(var(--color-danger)); }
.notification-toast.warning i { color: rgb(var(--color-warning)); }
.notification-toast.success i { color: rgb(var(--color-success)); }
.notification-toast.info i { color: rgb(var(--color-info)); }

.notification-text {
  flex: 1;
  min-width: 0;
}

.notification-text strong {
  display: block;
  font-size: 0.8125rem;
  color: white;
}

.notification-text span {
  display: block;
  font-size: 0.75rem;
  color: rgb(255 255 255 / 0.7);
  margin-top: 0.125rem;
}

.notification-toast button {
  background: none;
  border: none;
  padding: 0.25rem;
  color: rgb(255 255 255 / 0.5);
  cursor: pointer;
}

.notification-toast button:hover {
  color: white;
}

.notification-fade-enter-active,
.notification-fade-leave-active {
  transition: all 0.3s ease;
}

.notification-fade-enter-from,
.notification-fade-leave-to {
  opacity: 0;
  transform: translateY(-10px);
}

/* Quick Actions */
.quick-actions {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.75rem 1rem;
  border-top: 1px solid var(--border);
}

.action-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 0.5rem;
  padding: 0.625rem 1.25rem;
  border: none;
  border-radius: 0.5rem;
  font-size: 0.8125rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s;
}

.action-btn.primary {
  flex: 1;
  background: rgb(var(--color-primary));
  color: rgb(var(--color-on-primary));
}

.action-btn.primary:hover:not(:disabled) {
  filter: brightness(1.1);
  box-shadow: 0 4px 16px rgb(var(--color-primary) / 0.35);
}

.action-btn.primary.connected {
  background: rgb(var(--color-danger) / 0.15);
  border: 1px solid rgb(var(--color-danger) / 0.4);
  color: rgb(var(--color-danger));
}

.action-btn.primary.connecting {
  opacity: 0.7;
  cursor: wait;
}

.action-btn.danger {
  padding: 0.625rem;
  background: transparent;
  border: 1px solid rgb(var(--color-warning) / 0.4);
  color: rgb(var(--color-warning));
}

.action-btn.danger:hover:not(:disabled) {
  background: rgb(var(--color-warning) / 0.1);
}

.action-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.quick-toggles {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  margin-left: auto;
}

.toggle {
  display: flex;
  align-items: center;
  gap: 0.375rem;
  font-size: 0.6875rem;
  color: var(--text-2);
  cursor: pointer;
}

/* Compact Metrics */
.compact-metrics {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 0.5rem;
  padding: 0.75rem 1rem;
  border-top: 1px solid var(--border);
  background: rgb(var(--color-dark) / 0.02);
}

.dark .compact-metrics {
  background: rgb(0 0 0 / 0.15);
}

.metric {
  text-align: center;
}

.metric .label {
  display: block;
  font-size: 0.5625rem;
  text-transform: uppercase;
  letter-spacing: 0.03em;
  color: var(--text-3);
}

.metric .value {
  display: block;
  font-size: 0.8125rem;
  font-weight: 600;
  color: var(--text-1);
}

/* Settings Drawer */
.settings-drawer {
  position: fixed;
  top: 0;
  right: 0;
  bottom: 0;
  width: 380px;
  background: rgb(var(--color-surface));
  border-left: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  z-index: 200;
  box-shadow: -8px 0 32px rgb(0 0 0 / 0.15);
}

@media (max-width: 768px) {
  .settings-drawer {
    width: 100%;
  }
}

.drawer-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 1.25rem 1.5rem;
  border-bottom: 1px solid var(--border);
}

.drawer-header h2 {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 1rem;
  font-weight: 600;
  margin: 0;
  color: var(--text-1);
}

.drawer-header h2 i {
  color: rgb(var(--color-primary));
}

.close-btn {
  width: 2rem;
  height: 2rem;
  display: flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: none;
  border-radius: 0.375rem;
  color: var(--text-2);
  cursor: pointer;
  transition: all 0.2s;
}

.close-btn:hover {
  background: rgb(var(--color-danger) / 0.1);
  color: rgb(var(--color-danger));
}

.drawer-content {
  flex: 1;
  overflow-y: auto;
  padding: 1.5rem;
  display: flex;
  flex-direction: column;
  gap: 1.5rem;
}

.setting-group {
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
}

.group-label {
  font-size: 0.6875rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  color: var(--text-3);
}

.resolution-inputs {
  display: flex;
  align-items: center;
  gap: 0.5rem;
}

.separator {
  color: var(--text-3);
}

.preset-chips {
  display: flex;
  gap: 0.375rem;
  flex-wrap: wrap;
}

.chip {
  flex: 1;
  min-width: 60px;
  padding: 0.5rem 0.75rem;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 0.5rem;
  font-size: 0.75rem;
  font-weight: 500;
  color: var(--text-2);
  cursor: pointer;
  transition: all 0.2s;
}

.chip:hover {
  background: rgb(var(--color-primary) / 0.1);
  border-color: rgb(var(--color-primary) / 0.3);
  color: var(--text-1);
}

.chip.active {
  background: rgb(var(--color-primary));
  border-color: rgb(var(--color-primary));
  color: rgb(var(--color-on-primary));
  font-weight: 600;
}

.chip.unsupported {
  border-style: dashed;
  opacity: 0.6;
}

.toggle-setting {
  flex-direction: row;
  align-items: flex-start;
  justify-content: space-between;
  padding-top: 1rem;
  border-top: 1px solid var(--border);
}

.toggle-info {
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
}

.hint {
  font-size: 0.6875rem;
  color: var(--text-3);
  margin: 0;
  line-height: 1.4;
}

.setting-alert {
  margin-top: 0.5rem;
}

.full-width {
  width: 100%;
}

/* Advanced Section */
.advanced-section {
  margin-top: 0.5rem;
  border: 1px solid var(--border);
  border-radius: 0.5rem;
  overflow: hidden;
}

.advanced-section summary {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.75rem 1rem;
  background: rgb(var(--color-dark) / 0.03);
  font-size: 0.8125rem;
  font-weight: 500;
  color: var(--text-2);
  cursor: pointer;
  list-style: none;
}

.dark .advanced-section summary {
  background: rgb(0 0 0 / 0.15);
}

.advanced-section summary::-webkit-details-marker {
  display: none;
}

.advanced-section summary i {
  color: rgb(var(--color-primary));
}

.advanced-content {
  padding: 1rem;
  display: flex;
  flex-direction: column;
  gap: 1rem;
  border-top: 1px solid var(--border);
}

.drawer-footer {
  padding: 1rem 1.5rem;
  border-top: 1px solid var(--border);
}

.notice {
  display: flex;
  align-items: flex-start;
  gap: 0.5rem;
  font-size: 0.75rem;
  color: rgb(var(--color-warning));
  margin: 0;
}

.notice i {
  margin-top: 0.125rem;
}

/* Backdrop */
.drawer-backdrop {
  position: fixed;
  inset: 0;
  background: rgb(0 0 0 / 0.3);
  z-index: 150;
}

@media (min-width: 769px) {
  .drawer-backdrop {
    display: none;
  }
}

/* Transitions */
.slideout-enter-active,
.slideout-leave-active {
  transition: transform 0.3s ease;
}

.slideout-enter-from,
.slideout-leave-to {
  transform: translateX(100%);
}

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.3s ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

/* Naive UI Overrides */
:deep(.n-input-number) {
  --n-color: var(--surface) !important;
  --n-border: 1px solid var(--border) !important;
  --n-text-color: var(--text-1) !important;
  --n-color-focus: var(--surface) !important;
  --n-border-focus: 1px solid rgb(var(--color-primary)) !important;
}

:deep(.n-switch) {
  --n-rail-color: var(--border) !important;
  --n-rail-color-active: rgb(var(--color-primary)) !important;
}

/* Scrollbar */
.drawer-content::-webkit-scrollbar,
.library-section::-webkit-scrollbar {
  width: 6px;
}

.drawer-content::-webkit-scrollbar-track,
.library-section::-webkit-scrollbar-track {
  background: transparent;
}

.drawer-content::-webkit-scrollbar-thumb,
.library-section::-webkit-scrollbar-thumb {
  background: rgb(var(--color-dark) / 0.1);
  border-radius: 3px;
}

.dark .drawer-content::-webkit-scrollbar-thumb,
.dark .library-section::-webkit-scrollbar-thumb {
  background: rgb(255 255 255 / 0.1);
}
</style>
