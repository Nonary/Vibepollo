<template>
  <div class="streaming-app">
    <!-- Cinematic Header -->
    <header class="streaming-header">
      <div class="header-content">
        <div class="brand-section">
          <div class="brand-icon">
            <i class="fas fa-play"></i>
          </div>
          <div class="brand-text">
            <div class="brand-title-row">
              <h1 class="brand-title">{{ $t('webrtc.title') }}</h1>
              <span class="alpha-badge">
                <i class="fas fa-flask"></i>
                ALPHA
              </span>
            </div>
            <p class="brand-subtitle">{{ $t('webrtc.subtitle') }}</p>
          </div>
        </div>

        <!-- Live Status Indicator -->
        <div class="status-bar">
          <div class="connection-pill" :class="connectionPillClass">
            <span class="status-dot"></span>
            <span class="status-text">{{ connectionStatusLabel }}</span>
          </div>

          <div v-if="isConnected" class="live-metrics">
            <div class="metric">
              <i class="fas fa-gauge-high"></i>
              <span>{{ formatKbps(stats.videoBitrateKbps) }}</span>
            </div>
            <div class="metric">
              <i class="fas fa-bolt"></i>
              <span>{{ formatMs(smoothedLatencyMs) }}</span>
            </div>
            <div class="metric">
              <i class="fas fa-film"></i>
              <span>{{ stats.videoFps ? `${stats.videoFps.toFixed(0)} FPS` : '--' }}</span>
            </div>
          </div>

          <n-popover trigger="click" placement="bottom-end" :show-arrow="false">
            <template #trigger>
              <button class="settings-trigger">
                <i class="fas fa-cog"></i>
                <span>{{ $t('webrtc.session_settings') }}</span>
                <i class="fas fa-chevron-down chevron"></i>
              </button>
            </template>
            <div class="settings-panel">
              <div class="settings-header">
                <i class="fas fa-sliders-h"></i>
                <span>{{ $t('webrtc.session_settings') }}</span>
              </div>
              <div class="settings-body">
                <div class="setting-group">
                  <label class="setting-label">{{ $t('webrtc.resolution') }}</label>
                  <div class="resolution-inputs">
                    <n-input-number
                      v-model:value="config.width"
                      :min="320"
                      :max="7680"
                      size="small"
                    />
                    <span class="resolution-x">×</span>
                    <n-input-number
                      v-model:value="config.height"
                      :min="180"
                      :max="4320"
                      size="small"
                    />
                  </div>
                  <div class="preset-row">
                    <button
                      @click="setResolution(1920, 1080)"
                      class="preset-chip"
                      :class="{ active: config.width === 1920 && config.height === 1080 }"
                    >
                      1080p
                    </button>
                    <button
                      @click="setResolution(2560, 1440)"
                      class="preset-chip"
                      :class="{ active: config.width === 2560 && config.height === 1440 }"
                    >
                      1440p
                    </button>
                    <button
                      @click="setResolution(3840, 2160)"
                      class="preset-chip"
                      :class="{ active: config.width === 3840 && config.height === 2160 }"
                    >
                      4K
                    </button>
                  </div>
                </div>

                <div class="setting-group">
                  <label class="setting-label">{{ $t('webrtc.framerate') }}</label>
                  <div class="preset-row">
                    <button
                      @click="config.fps = 30"
                      class="preset-chip"
                      :class="{ active: config.fps === 30 }"
                    >
                      30
                    </button>
                    <button
                      @click="config.fps = 60"
                      class="preset-chip"
                      :class="{ active: config.fps === 60 }"
                    >
                      60
                    </button>
                    <button
                      @click="config.fps = 120"
                      class="preset-chip"
                      :class="{ active: config.fps === 120 }"
                    >
                      120
                    </button>
                    <button
                      @click="config.fps = 144"
                      class="preset-chip"
                      :class="{ active: config.fps === 144 }"
                    >
                      144
                    </button>
                  </div>
                </div>

                <div class="setting-group">
                  <label class="setting-label">{{ $t('webrtc.encoding') }}</label>
                  <div class="preset-row">
                    <button
                      v-for="opt in encodingOptions"
                      :key="opt.value"
                      @click="opt.supported && (config.encoding = opt.value)"
                      class="preset-chip"
                      :class="{
                        active: config.encoding === opt.value && opt.supported,
                        disabled: !opt.supported,
                      }"
                      :disabled="!opt.supported"
                      :title="opt.supported ? undefined : opt.hint"
                    >
                      {{ opt.label }}
                      <span v-if="!opt.supported" class="unsupported-tag">N/A</span>
                    </button>
                  </div>
                </div>

                <div class="setting-group">
                  <label class="setting-label">{{ $t('webrtc.bitrate') }}</label>
                  <n-input-number
                    v-model:value="config.bitrateKbps"
                    :min="500"
                    :max="200000"
                    size="small"
                    class="w-full"
                  />
                  <div class="preset-row">
                    <button
                      @click="config.bitrateKbps = 10000"
                      class="preset-chip"
                      :class="{ active: config.bitrateKbps === 10000 }"
                    >
                      10 Mbps
                    </button>
                    <button
                      @click="config.bitrateKbps = 30000"
                      class="preset-chip"
                      :class="{ active: config.bitrateKbps === 30000 }"
                    >
                      30 Mbps
                    </button>
                    <button
                      @click="config.bitrateKbps = 60000"
                      class="preset-chip"
                      :class="{ active: config.bitrateKbps === 60000 }"
                    >
                      60 Mbps
                    </button>
                  </div>
                </div>

                <div class="setting-group">
                  <label class="setting-label">{{ $t('webrtc.frame_pacing') }}</label>
                  <div class="preset-row">
                    <button
                      v-for="opt in pacingOptions"
                      :key="opt.value"
                      @click="applyPacingPreset(opt.value)"
                      class="preset-chip"
                      :class="{ active: config.videoPacingMode === opt.value }"
                    >
                      {{ opt.label }}
                    </button>
                  </div>
                  <p class="setting-hint">{{ $t('webrtc.frame_pacing_desc') }}</p>
                  <div class="sub-settings">
                    <div class="sub-setting">
                      <label>{{ $t('webrtc.frame_pacing_slack') }}</label>
                      <n-input-number
                        v-model:value="config.videoPacingSlackMs"
                        :min="0"
                        :max="10"
                        size="small"
                      />
                    </div>
                    <div class="sub-setting">
                      <label>{{ $t('webrtc.frame_pacing_max_delay') }}</label>
                      <n-input-number
                        v-model:value="config.videoMaxFrameAgeMs"
                        :min="5"
                        :max="250"
                        size="small"
                      />
                    </div>
                  </div>
                </div>

                <div class="setting-group toggle-group">
                  <div class="toggle-row">
                    <div class="toggle-info">
                      <label class="setting-label">{{ $t('webrtc.mute_host_audio') }}</label>
                      <p class="setting-hint">{{ $t('webrtc.mute_host_audio_desc') }}</p>
                    </div>
                    <n-switch v-model:value="config.muteHostAudio" />
                  </div>
                </div>
              </div>
            </div>
          </n-popover>
        </div>
      </div>

      <!-- Alpha Notice Banner -->
      <div class="alpha-banner">
        <div class="alpha-banner-content">
          <i class="fas fa-info-circle"></i>
          <span>{{ $t('webrtc.experimental_notice') }}</span>
        </div>
      </div>
    </header>

    <main class="streaming-main">
      <div class="main-grid">
        <!-- Control Panel -->
        <aside class="control-panel">
          <!-- Primary Actions -->
          <div class="panel-card action-card">
            <button
              @click="isConnected ? disconnect() : connect()"
              class="primary-action"
              :class="{ connected: isConnected, connecting: isConnecting }"
              :disabled="isConnecting"
            >
              <div class="action-icon">
                <i
                  :class="
                    isConnected
                      ? 'fas fa-stop'
                      : isConnecting
                        ? 'fas fa-circle-notch fa-spin'
                        : 'fas fa-play'
                  "
                ></i>
              </div>
              <span class="action-text">{{ $t(connectLabelKey) }}</span>
            </button>

            <button
              v-if="isConnected"
              @click="terminateSession"
              class="secondary-action danger"
              :disabled="isConnecting || terminatePending"
              :title="$t('webrtc.terminate_desc')"
            >
              <i :class="terminatePending ? 'fas fa-circle-notch fa-spin' : 'fas fa-power-off'"></i>
              <span>{{ $t('webrtc.terminate') }}</span>
            </button>

            <div class="quick-toggles">
              <label class="toggle-item">
                <n-switch v-model:value="inputEnabled" :disabled="!isConnected" size="small" />
                <span>Input</span>
              </label>
              <label class="toggle-item">
                <n-switch v-model:value="showOverlay" size="small" />
                <span>Stats</span>
              </label>
              <label class="toggle-item">
                <n-switch v-model:value="autoFullscreen" size="small" />
                <span>Fullscreen</span>
              </label>
            </div>
          </div>

          <!-- Game Library -->
          <div class="panel-card library-card">
            <div class="card-header">
              <div class="header-title">
                <i class="fas fa-gamepad"></i>
                <span>{{ $t('webrtc.select_game') }}</span>
              </div>
              <button v-if="selectedAppId" @click="clearSelection" class="clear-btn">
                <i class="fas fa-times"></i>
              </button>
            </div>

            <div class="selection-info">
              <i class="fas fa-circle-info"></i>
              <span>{{ selectedAppId ? selectedAppLabel : $t('webrtc.no_selection') }}</span>
            </div>

            <div v-if="appsList.length" class="game-grid">
              <button
                v-for="app in appsList"
                :key="appKey(app)"
                @click="selectApp(app)"
                @dblclick="onAppDoubleClick(app)"
                class="game-tile"
                :class="{ selected: appNumericId(app) === selectedAppId }"
              >
                <div class="game-cover">
                  <img
                    :src="coverUrl(app) || undefined"
                    :alt="app.name || 'Application'"
                    loading="lazy"
                  />
                  <div class="cover-overlay"></div>
                  <div v-if="appNumericId(app) === selectedAppId" class="selected-indicator">
                    <i class="fas fa-check"></i>
                  </div>
                </div>
                <div class="game-info">
                  <span class="game-title">{{ app.name || '(untitled)' }}</span>
                  <span class="game-source">{{ appSubtitle(app) }}</span>
                </div>
              </button>
            </div>
            <div v-else class="empty-library">
              <i class="fas fa-gamepad"></i>
              <p>No applications configured</p>
              <span>Add games in the Applications tab</span>
            </div>
          </div>
        </aside>

        <!-- Stream Viewport -->
        <section class="stream-section">
          <div class="stream-container">
            <div class="stream-header">
              <div class="stream-title">
                <i class="fas fa-tv"></i>
                <span>Stream</span>
                <span v-if="isConnected" class="live-badge">
                  <span class="live-dot"></span>
                  LIVE
                </span>
              </div>
              <button
                @click="toggleFullscreen"
                class="fullscreen-btn"
                :title="isFullscreen ? 'Exit Fullscreen' : 'Enter Fullscreen'"
              >
                <i :class="isFullscreen ? 'fas fa-compress' : 'fas fa-expand'"></i>
              </button>
            </div>

            <div
              ref="inputTarget"
              class="stream-viewport"
              :class="{ 'fullscreen-mode': isFullscreen }"
              tabindex="0"
              @dblclick="onFullscreenDblClick"
            >
              <video ref="videoEl" class="stream-video" autoplay playsinline></video>
              <audio ref="audioEl" class="hidden" autoplay playsinline></audio>

              <!-- Idle State -->
              <div v-if="!isConnected && !isConnecting" class="idle-overlay">
                <div class="idle-content">
                  <div class="idle-icon">
                    <i class="fas fa-play-circle"></i>
                  </div>
                  <h3>Ready to Stream</h3>
                  <p>Select a game and click Start Streaming, or double-click a game to begin</p>
                </div>
              </div>

              <!-- Connecting State -->
              <div v-if="showStartingOverlay" class="connecting-overlay">
                <div class="connecting-content">
                  <div class="spinner"></div>
                  <span>Starting session...</span>
                </div>
              </div>

              <!-- Stats Overlay -->
              <div v-if="showOverlay && isConnected" class="stats-overlay">
                <div v-for="(line, idx) in overlayLines" :key="idx" class="stat-line">
                  {{ line }}
                </div>
              </div>

              <!-- Notification Overlay (for fullscreen) -->
              <Transition name="notification-slide">
                <div v-if="activeNotification" class="notification-overlay">
                  <div class="notification-toast" :class="activeNotification.type">
                    <div class="notification-icon">
                      <i :class="notificationIcon"></i>
                    </div>
                    <div class="notification-content">
                      <span class="notification-title">{{ activeNotification.title }}</span>
                      <span v-if="activeNotification.message" class="notification-message">{{
                        activeNotification.message
                      }}</span>
                    </div>
                    <button class="notification-close" @click="dismissNotification">
                      <i class="fas fa-times"></i>
                    </button>
                  </div>
                </div>
              </Transition>
            </div>
          </div>

          <!-- Performance Metrics -->
          <div class="metrics-grid">
            <div class="metric-card">
              <div class="metric-icon blue">
                <i class="fas fa-video"></i>
              </div>
              <div class="metric-data">
                <span class="metric-label">Bitrate</span>
                <span class="metric-value">{{ formatKbps(stats.videoBitrateKbps) }}</span>
              </div>
            </div>
            <div class="metric-card">
              <div class="metric-icon purple">
                <i class="fas fa-film"></i>
              </div>
              <div class="metric-data">
                <span class="metric-label">Frame Rate</span>
                <span class="metric-value">{{
                  stats.videoFps ? `${stats.videoFps.toFixed(0)} FPS` : '--'
                }}</span>
              </div>
            </div>
            <div class="metric-card">
              <div class="metric-icon green">
                <i class="fas fa-bolt"></i>
              </div>
              <div class="metric-data">
                <span class="metric-label">Latency</span>
                <span class="metric-value">{{ formatMs(smoothedLatencyMs) }}</span>
              </div>
            </div>
            <div class="metric-card">
              <div class="metric-icon amber">
                <i class="fas fa-chart-line"></i>
              </div>
              <div class="metric-data">
                <span class="metric-label">Dropped</span>
                <span class="metric-value">{{ stats.videoFramesDropped ?? '--' }}</span>
              </div>
            </div>
          </div>

          <!-- Debug Panel -->
          <details class="debug-panel">
            <summary class="debug-summary">
              <div class="debug-title">
                <i class="fas fa-terminal"></i>
                <span>{{ $t('webrtc.debug_panel') }}</span>
              </div>
              <i class="fas fa-chevron-down chevron"></i>
            </summary>
            <div class="debug-content">
              <div class="debug-grid">
                <div class="debug-item">
                  <span class="debug-label">Connection</span>
                  <n-tag size="tiny" :type="statusTagType(connectionState)">{{
                    connectionState || 'idle'
                  }}</n-tag>
                </div>
                <div class="debug-item">
                  <span class="debug-label">ICE</span>
                  <n-tag size="tiny" :type="statusTagType(iceState)">{{
                    iceState || 'idle'
                  }}</n-tag>
                </div>
                <div class="debug-item">
                  <span class="debug-label">Input Channel</span>
                  <n-tag size="tiny" :type="statusTagType(inputChannelState)">{{
                    inputChannelState || 'closed'
                  }}</n-tag>
                </div>
                <div class="debug-item">
                  <span class="debug-label">Session ID</span>
                  <span class="debug-value mono">{{ displayValue(sessionId) }}</span>
                </div>
              </div>

              <div class="debug-section">
                <div class="debug-grid">
                  <div class="debug-item">
                    <span class="debug-label">Input move delay</span>
                    <span class="debug-value">{{ formatMs(inputMetrics.lastMoveDelayMs) }}</span>
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Video interval</span>
                    <span class="debug-value">{{
                      formatMs(videoFrameMetrics.lastIntervalMs)
                    }}</span>
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Server packets</span>
                    <span class="debug-value"
                      >V {{ displayValue(serverSession?.video_packets) }} / A
                      {{ displayValue(serverSession?.audio_packets) }}</span
                    >
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Inbound bytes</span>
                    <span class="debug-value"
                      >V {{ formatBytes(stats.videoBytesReceived) }} / A
                      {{ formatBytes(stats.audioBytesReceived) }}</span
                    >
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Video size</span>
                    <span class="debug-value">{{ videoSizeLabel }}</span>
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Codec</span>
                    <span class="debug-value"
                      >V {{ displayValue(stats.videoCodec) }} / A
                      {{ displayValue(stats.audioCodec) }}</span
                    >
                  </div>
                </div>
              </div>

              <div class="debug-section">
                <div class="debug-grid">
                  <div class="debug-item">
                    <span class="debug-label">Latency est</span>
                    <span class="debug-value">{{ formatMs(smoothedLatencyMs) }}</span>
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Latency avg 30s</span>
                    <span class="debug-value">{{ formatMs(averageLatency30sMs) }}</span>
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">RTT / one-way</span>
                    <span class="debug-value"
                      >{{ formatMs(stats.roundTripTimeMs) }} / {{ formatMs(oneWayRttMs) }}</span
                    >
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Decoder</span>
                    <span class="debug-value">{{ formatMs(stats.videoDecodeMs) }}</span>
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Jitter / buffer</span>
                    <span class="debug-value"
                      >{{ formatMs(stats.videoJitterMs) }} /
                      {{ formatMs(stats.videoPlayoutDelayMs ?? videoJitterBufferMs) }}</span
                    >
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Audio latency</span>
                    <span class="debug-value">{{ formatMs(audioLatencyMs) }}</span>
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Audio jitter / buffer</span>
                    <span class="debug-value"
                      >{{ formatMs(stats.audioJitterMs) }} /
                      {{ formatMs(stats.audioPlayoutDelayMs ?? stats.audioJitterBufferMs) }}</span
                    >
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Render delay</span>
                    <span class="debug-value">{{ formatMs(videoFrameMetrics.lastDelayMs) }}</span>
                  </div>
                  <div class="debug-item">
                    <span class="debug-label">Frames dropped</span>
                    <span class="debug-value"
                      >{{ displayValue(stats.videoFramesDropped) }} /
                      {{ displayValue(stats.videoFramesReceived) }}</span
                    >
                  </div>
                </div>
              </div>

              <div class="debug-section">
                <div class="debug-section-title">Video Events</div>
                <div v-if="videoEvents.length" class="video-events">
                  <div v-for="(event, idx) in videoEvents" :key="idx" class="event-line">
                    {{ event }}
                  </div>
                </div>
                <div v-else class="no-events">No events yet</div>
              </div>
            </div>
          </details>
        </section>
      </div>
    </main>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive, onBeforeUnmount, onMounted, watch, computed } from 'vue';
import { NTag, NSwitch, NInputNumber, NPopover, useMessage } from 'naive-ui';
import { WebRtcHttpApi } from '@/services/webrtcApi';
import { WebRtcClient } from '@/utils/webrtc/client';
import {
  applyGamepadFeedback,
  attachInputCapture,
  type InputCaptureMetrics,
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

const message = useMessage();

// ============================================
// NOTIFICATION SYSTEM (for fullscreen support)
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
    case 'error':
      return 'fas fa-circle-exclamation';
    case 'warning':
      return 'fas fa-triangle-exclamation';
    case 'success':
      return 'fas fa-circle-check';
    default:
      return 'fas fa-circle-info';
  }
});

function showNotification(type: NotificationType, title: string, msg?: string, duration = 5000) {
  if (notificationTimeout) {
    clearTimeout(notificationTimeout);
    notificationTimeout = null;
  }
  notificationId++;
  activeNotification.value = {
    id: notificationId,
    type,
    title,
    message: msg,
  };
  if (duration > 0) {
    notificationTimeout = window.setTimeout(() => {
      dismissNotification();
    }, duration);
  }
}

function dismissNotification() {
  activeNotification.value = null;
  if (notificationTimeout) {
    clearTimeout(notificationTimeout);
    notificationTimeout = null;
  }
}

function notifyError(title: string, msg?: string) {
  showNotification('error', title, msg, 8000);
}

function notifyWarning(title: string, msg?: string) {
  showNotification('warning', title, msg, 6000);
}

function notifySuccess(title: string, msg?: string) {
  showNotification('success', title, msg, 4000);
}

function notifyInfo(title: string, msg?: string) {
  showNotification('info', title, msg, 5000);
}

// Helper function for resolution presets
function setResolution(width: number, height: number) {
  config.width = width;
  config.height = height;
}

// Connection status computed properties for the gaming UI
const connectionPillClass = computed(() => {
  if (isConnected.value) return 'connected';
  if (isConnecting.value) return 'connecting';
  return 'idle';
});

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
  const caps =
    (typeof RTCRtpReceiver !== 'undefined' ? RTCRtpReceiver.getCapabilities?.('video') : null) ??
    (typeof RTCRtpSender !== 'undefined' ? RTCRtpSender.getCapabilities?.('video') : null);
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
const sessionStatus = ref<{ activeSessions: number; appRunning: boolean; paused: boolean } | null>(
  null,
);
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
function setWebRtcActive(active: boolean): void {
  try {
    (window as any).__sunshine_webrtc_active = active;
  } catch {
    /* ignore */
  }
}
watch(
  () => [isConnecting.value, isConnected.value] as const,
  ([connecting, connected]) => {
    setWebRtcActive(connecting || connected);
  },
  { immediate: true },
);
const connectLabelKey = computed(() => {
  if (isConnecting.value) return 'webrtc.connecting';
  if (isConnected.value) return 'webrtc.disconnect';
  if (resumeAvailable.value) return 'webrtc.resume';
  return 'webrtc.connect';
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
  lastDelayMs?: number;
  avgDelayMs?: number;
  maxDelayMs?: number;
}>({});
const LATENCY_SAMPLE_WINDOW_MS = 30000;
const LATENCY_SMOOTH_TAU_MS = 2000;
const latencySamples = ref<{ ts: number; value: number }[]>([]);
const smoothedLatencyMs = ref<number | undefined>(undefined);
let lastLatencySampleAt: number | null = null;
const videoJitterBufferMs = computed(() => stats.value.videoJitterBufferMs);
const oneWayRttMs = computed(() =>
  stats.value.roundTripTimeMs ? stats.value.roundTripTimeMs / 2 : undefined,
);
const videoPlayoutDelayMs = computed(
  () => stats.value.videoPlayoutDelayMs ?? stats.value.videoJitterBufferMs,
);
const estimatedLatencyMs = computed(() => {
  const parts = [oneWayRttMs.value, videoPlayoutDelayMs.value, stats.value.videoDecodeMs].filter(
    (value) => typeof value === 'number',
  ) as number[];
  if (!parts.length) return undefined;
  return parts.reduce((total, value) => total + value, 0);
});
const averageLatency30sMs = computed(() => {
  const samples = latencySamples.value;
  if (!samples.length) return undefined;
  const total = samples.reduce((sum, sample) => sum + sample.value, 0);
  return total / samples.length;
});
const audioLatencyMs = computed(() => {
  const parts = [
    oneWayRttMs.value,
    stats.value.audioPlayoutDelayMs ?? stats.value.audioJitterBufferMs,
  ].filter((value) => typeof value === 'number') as number[];
  if (!parts.length) return undefined;
  return parts.reduce((total, value) => total + value, 0);
});
watch(
  () => estimatedLatencyMs.value,
  (value) => {
    if (typeof value !== 'number' || Number.isNaN(value)) return;
    const now = Date.now();
    const lastAt = lastLatencySampleAt ?? now;
    const deltaMs = Math.max(0, now - lastAt);
    const alpha = 1 - Math.exp(-deltaMs / LATENCY_SMOOTH_TAU_MS);
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
  },
);
const overlayLines = computed(() => {
  const fps = stats.value.videoFps ? stats.value.videoFps.toFixed(0) : '--';
  const dropped = stats.value.videoFramesDropped ?? '--';
  const rttHalf = formatMs(oneWayRttMs.value);
  const jbuf = formatMs(videoPlayoutDelayMs.value);
  const dec = formatMs(stats.value.videoDecodeMs);
  return [
    `Conn ${connectionState.value ?? 'idle'} | ICE ${iceState.value ?? 'idle'} | Input ${inputChannelState.value ?? 'closed'}`,
    `Lat ${formatMs(smoothedLatencyMs.value)} (net ${rttHalf} + buf ${jbuf} + dec ${dec}) | Avg30 ${formatMs(averageLatency30sMs.value)}`,
    `Decode ${dec} | FPS ${fps} | Drop ${dropped} | RTT ${formatMs(stats.value.roundTripTimeMs)}`,
    `Bitrate V ${formatKbps(stats.value.videoBitrateKbps)} / A ${formatKbps(stats.value.audioBitrateKbps)}`,
    `Audio lat ${formatMs(audioLatencyMs.value)} | jitter ${formatMs(stats.value.audioJitterMs)} | playout ${formatMs(stats.value.audioPlayoutDelayMs ?? stats.value.audioJitterBufferMs)}`,
    `Input send ${formatRate(inputMetrics.value.moveSendRateHz)} | cap ${formatRate(inputMetrics.value.moveRateHz)} | coalesce ${formatPercent(inputMetrics.value.moveCoalesceRatio)}`,
    `Input lag ${formatMs(inputMetrics.value.lastMoveEventLagMs)} ev / ${formatMs(inputMetrics.value.lastMoveDelayMs)} send | buf ${formatBytes(inputBufferedAmount.value ?? undefined)}`,
    `Render ${formatMs(videoFrameMetrics.value.lastDelayMs)} | frame ${formatMs(videoFrameMetrics.value.lastIntervalMs)} | size ${videoSizeLabel.value}`,
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

function isTabActive(): boolean {
  try {
    const visible = typeof document !== 'undefined' ? document.visibilityState === 'visible' : true;
    const focus = typeof document !== 'undefined' && document.hasFocus ? document.hasFocus() : true;
    return visible && focus;
  } catch {
    return true;
  }
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
      videoFrameMetrics.value.maxIntervalMs = Math.max(
        videoFrameMetrics.value.maxIntervalMs ?? 0,
        interval,
      );
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
    const id = await client.connect(
      {
        ...config,
        appId: selectedAppId.value ?? undefined,
        resume: selectedAppId.value ? false : resumeOnConnect.value,
      },
      {
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
        onNegotiatedEncoding: (encoding) => {
          if (encoding === 'h264' || encoding === 'hevc' || encoding === 'av1') {
            config.encoding = encoding;
          }
        },
      },
      {
        inputPriority: isFullscreenActive() || isTabActive() ? 'high' : 'low',
      },
    );
    sessionId.value = id;
    startServerSessionPolling();
  } catch (error) {
    const msg = error instanceof Error ? error.message : 'Failed to establish WebRTC session.';
    notifyError('Connection Failed', msg);
    console.error(error);
  } finally {
    isConnecting.value = false;
    if (!isConnected.value) {
      startSessionStatusPolling();
    }
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
        shouldDrop: shouldDropInput,
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
  setWebRtcActive(false);
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
/* ============================================
   STREAMING APP - Professional UI Design
   ============================================ */

/* Base Container - Light Mode (default) */
.streaming-app {
  --accent: rgb(var(--color-primary));
  --accent-glow: rgb(var(--color-primary) / 0.3);
  --surface-elevated: rgb(var(--color-surface));
  --border-subtle: rgb(var(--color-dark) / 0.1);
  --border-hover: rgb(var(--color-dark) / 0.2);
  --text-primary: rgb(var(--color-on-light));
  --text-secondary: rgb(var(--color-on-light) / 0.7);
  --text-muted: rgb(var(--color-on-light) / 0.5);

  background: linear-gradient(180deg, rgb(var(--color-light)) 0%, rgb(var(--color-surface)) 100%);
  min-height: 100vh;
  color: var(--text-primary);
}

/* Base Container - Dark Mode */
.dark .streaming-app {
  --surface-elevated: rgb(var(--color-surface) / 0.7);
  --border-subtle: rgb(255 255 255 / 0.06);
  --border-hover: rgb(255 255 255 / 0.12);
  --text-primary: rgb(var(--color-on-dark));
  --text-secondary: rgb(var(--color-on-dark) / 0.6);
  --text-muted: rgb(var(--color-on-dark) / 0.4);

  background: linear-gradient(180deg, rgb(var(--color-dark)) 0%, rgb(18 18 24) 100%);
}

/* ============================================
   HEADER
   ============================================ */
.streaming-header {
  background: linear-gradient(180deg, rgb(var(--color-surface) / 0.6) 0%, transparent 100%);
  border-bottom: 1px solid var(--border-subtle);
  padding: 1.5rem 2rem 0;
}

.dark .streaming-header {
  background: linear-gradient(180deg, rgb(0 0 0 / 0.4) 0%, transparent 100%);
}

.header-content {
  max-width: 1600px;
  margin: 0 auto;
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 1rem;
}

.brand-section {
  display: flex;
  align-items: center;
  gap: 1rem;
}

.brand-icon {
  width: 3rem;
  height: 3rem;
  background: rgb(var(--color-primary));
  border-radius: 0.75rem;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 1.25rem;
  color: rgb(var(--color-on-primary));
  box-shadow: 0 4px 20px rgb(var(--color-primary) / 0.3);
}

.brand-text {
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
}

.brand-title-row {
  display: flex;
  align-items: center;
  gap: 0.75rem;
}

.brand-title {
  font-size: 1.5rem;
  font-weight: 700;
  letter-spacing: -0.02em;
  color: var(--text-primary);
  margin: 0;
}

.alpha-badge {
  display: inline-flex;
  align-items: center;
  gap: 0.35rem;
  padding: 0.25rem 0.6rem;
  background: linear-gradient(
    135deg,
    rgb(var(--color-warning) / 0.15) 0%,
    rgb(var(--color-warning) / 0.08) 100%
  );
  border: 1px solid rgb(var(--color-warning) / 0.3);
  border-radius: 0.375rem;
  font-size: 0.625rem;
  font-weight: 700;
  letter-spacing: 0.08em;
  color: rgb(var(--color-warning));
  text-transform: uppercase;
}

.brand-subtitle {
  font-size: 0.8125rem;
  color: var(--text-secondary);
  margin: 0;
  max-width: 400px;
}

/* Status Bar */
.status-bar {
  display: flex;
  align-items: center;
  gap: 1.25rem;
  flex-wrap: wrap;
}

.connection-pill {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.5rem 1rem;
  background: var(--surface-elevated);
  border: 1px solid var(--border-subtle);
  border-radius: 2rem;
  font-size: 0.75rem;
  font-weight: 600;
  transition: all 0.3s ease;
}

.connection-pill.connected {
  background: rgb(var(--color-success) / 0.12);
  border-color: rgb(var(--color-success) / 0.3);
  color: rgb(var(--color-success));
}

.connection-pill.connecting {
  background: rgb(var(--color-warning) / 0.12);
  border-color: rgb(var(--color-warning) / 0.3);
  color: rgb(var(--color-warning));
}

.connection-pill.idle {
  color: var(--text-secondary);
}

.status-dot {
  width: 0.5rem;
  height: 0.5rem;
  border-radius: 50%;
  background: currentColor;
  animation: pulse 2s ease-in-out infinite;
}

.connection-pill.connected .status-dot {
  animation: pulse-success 1.5s ease-in-out infinite;
}

@keyframes pulse {
  0%,
  100% {
    opacity: 0.5;
  }
  50% {
    opacity: 1;
  }
}

@keyframes pulse-success {
  0%,
  100% {
    opacity: 1;
    box-shadow: 0 0 0 0 currentColor;
  }
  50% {
    box-shadow: 0 0 8px 2px currentColor;
  }
}

.live-metrics {
  display: flex;
  align-items: center;
  gap: 1rem;
}

.metric {
  display: flex;
  align-items: center;
  gap: 0.375rem;
  font-size: 0.75rem;
  color: var(--text-secondary);
}

.metric i {
  font-size: 0.625rem;
  opacity: 0.7;
}

/* Settings Trigger */
.settings-trigger {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.5rem 1rem;
  background: rgb(var(--color-surface) / 0.7);
  border: 1px solid rgb(var(--color-primary) / 0.2);
  border-radius: 0.5rem;
  font-size: 0.75rem;
  font-weight: 500;
  color: rgb(var(--color-on-dark) / 0.7);
  cursor: pointer;
  transition: all 0.2s ease;
}

.settings-trigger:hover {
  background: rgb(var(--color-primary) / 0.15);
  border-color: rgb(var(--color-primary) / 0.4);
  color: rgb(var(--color-on-dark));
}

.settings-trigger .chevron {
  font-size: 0.625rem;
  opacity: 0.5;
  transition: transform 0.2s ease;
}

/* Settings Panel */
.settings-panel {
  width: min(440px, 90vw);
  max-height: 75vh;
  overflow-y: auto;
  background: rgb(var(--color-surface));
  border: 1px solid rgb(var(--color-primary) / 0.15);
  border-radius: 1rem;
  box-shadow: 0 20px 60px rgb(0 0 0 / 0.5);
}

.settings-header {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  padding: 1rem 1.25rem;
  border-bottom: 1px solid rgb(var(--color-primary) / 0.1);
  font-weight: 600;
  color: rgb(var(--color-on-dark));
}

.settings-header i {
  color: rgb(var(--color-primary));
}

.settings-body {
  padding: 1.25rem;
  display: flex;
  flex-direction: column;
  gap: 1.25rem;
}

.setting-group {
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
}

.setting-label {
  font-size: 0.6875rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  color: var(--text-muted);
}

.setting-hint {
  font-size: 0.6875rem;
  color: var(--text-muted);
  margin: 0;
  line-height: 1.4;
}

.resolution-inputs {
  display: flex;
  align-items: center;
  gap: 0.5rem;
}

.resolution-x {
  color: var(--text-muted);
  font-size: 0.875rem;
}

.preset-row {
  display: flex;
  gap: 0.375rem;
  flex-wrap: wrap;
}

.preset-chip {
  flex: 1;
  min-width: 60px;
  padding: 0.5rem 0.75rem;
  background: var(--surface-elevated);
  border: 1px solid var(--border-subtle);
  border-radius: 0.5rem;
  font-size: 0.75rem;
  font-weight: 500;
  color: var(--text-secondary);
  cursor: pointer;
  transition: all 0.2s ease;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 0.125rem;
}

.preset-chip:hover:not(:disabled) {
  background: rgb(var(--color-primary) / 0.1);
  border-color: rgb(var(--color-primary) / 0.3);
  color: var(--text-primary);
}

.preset-chip.active {
  background: rgb(var(--color-primary));
  border-color: rgb(var(--color-primary));
  color: rgb(var(--color-on-primary));
  font-weight: 600;
  box-shadow: 0 4px 16px rgb(var(--color-primary) / 0.35);
}

.preset-chip.disabled,
.preset-chip:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.unsupported-tag {
  font-size: 0.5625rem;
  opacity: 0.6;
  text-transform: uppercase;
}

.sub-settings {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 0.75rem;
  margin-top: 0.5rem;
}

.sub-setting {
  display: flex;
  flex-direction: column;
  gap: 0.375rem;
}

.sub-setting label {
  font-size: 0.625rem;
  text-transform: uppercase;
  letter-spacing: 0.03em;
  color: var(--text-muted);
}

.toggle-group {
  padding-top: 0.75rem;
  border-top: 1px solid var(--border-subtle);
}

.toggle-row {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
}

.toggle-info {
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
}

/* Alpha Banner */
.alpha-banner {
  margin-top: 1rem;
  padding: 0.625rem 0;
  background: linear-gradient(
    90deg,
    rgb(var(--color-warning) / 0.08) 0%,
    transparent 50%,
    rgb(var(--color-warning) / 0.08) 100%
  );
  border-top: 1px solid rgb(var(--color-warning) / 0.15);
}

.alpha-banner-content {
  max-width: 1600px;
  margin: 0 auto;
  padding: 0 2rem;
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 0.75rem;
  color: rgb(var(--color-warning) / 0.9);
}

/* ============================================
   MAIN CONTENT
   ============================================ */
.streaming-main {
  max-width: 1600px;
  margin: 0 auto;
  padding: 1.5rem 2rem 3rem;
}

.main-grid {
  display: grid;
  grid-template-columns: 380px minmax(0, 1fr);
  gap: 1.5rem;
}

@media (max-width: 1024px) {
  .main-grid {
    grid-template-columns: 1fr;
  }
}

/* ============================================
   CONTROL PANEL
   ============================================ */
.control-panel {
  display: flex;
  flex-direction: column;
  gap: 1rem;
}

.panel-card {
  background: var(--surface-elevated);
  border: 1px solid var(--border-subtle);
  border-radius: 1rem;
  overflow: hidden;
}

/* Action Card */
.action-card {
  padding: 1.25rem;
  display: flex;
  flex-direction: column;
  gap: 0.75rem;
}

.primary-action {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 0.75rem;
  width: 100%;
  padding: 1rem;
  background: rgb(var(--color-primary));
  border: none;
  border-radius: 0.75rem;
  font-size: 0.9375rem;
  font-weight: 600;
  color: rgb(var(--color-on-primary));
  cursor: pointer;
  transition: all 0.3s ease;
}

.primary-action:hover:not(:disabled) {
  transform: translateY(-2px);
  box-shadow: 0 8px 30px rgb(var(--color-primary) / 0.35);
  filter: brightness(1.1);
}

.primary-action:active:not(:disabled) {
  transform: translateY(0);
}

.primary-action.connected {
  background: rgb(var(--color-danger) / 0.15);
  border: 1px solid rgb(var(--color-danger) / 0.4);
  color: rgb(var(--color-danger));
}

.primary-action.connected:hover:not(:disabled) {
  background: rgb(var(--color-danger) / 0.25);
  box-shadow: 0 4px 20px rgb(var(--color-danger) / 0.25);
}

.primary-action.connecting {
  opacity: 0.7;
  cursor: wait;
}

.primary-action:disabled {
  opacity: 0.5;
  cursor: not-allowed;
  transform: none;
}

.action-icon {
  width: 2rem;
  height: 2rem;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgb(var(--color-on-primary) / 0.2);
  border-radius: 50%;
  font-size: 0.875rem;
}

.primary-action.connected .action-icon {
  background: rgb(var(--color-danger) / 0.25);
}

.secondary-action {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 0.5rem;
  width: 100%;
  padding: 0.625rem;
  background: transparent;
  border: 1px solid rgb(var(--color-warning) / 0.4);
  border-radius: 0.5rem;
  font-size: 0.75rem;
  font-weight: 600;
  color: rgb(var(--color-warning));
  cursor: pointer;
  transition: all 0.2s ease;
}

.secondary-action:hover:not(:disabled) {
  background: rgb(var(--color-warning) / 0.1);
  border-color: rgb(var(--color-warning) / 0.5);
}

.secondary-action:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.quick-toggles {
  display: flex;
  justify-content: space-between;
  gap: 0.5rem;
  padding-top: 0.5rem;
  border-top: 1px solid var(--border-subtle);
}

.toggle-item {
  display: flex;
  align-items: center;
  gap: 0.375rem;
  font-size: 0.6875rem;
  color: rgb(var(--color-on-dark) / 0.6);
  cursor: pointer;
  transition: color 0.2s ease;
}

.toggle-item:hover {
  color: rgb(var(--color-on-dark));
}

/* Library Card */
.library-card {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}

.card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 1rem 1.25rem;
  border-bottom: 1px solid rgb(var(--color-primary) / 0.1);
}

.header-title {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-weight: 600;
  color: rgb(var(--color-on-dark));
}

.header-title i {
  color: rgb(var(--color-primary));
}

.clear-btn {
  padding: 0.375rem;
  background: transparent;
  border: none;
  border-radius: 0.375rem;
  color: rgb(var(--color-on-dark) / 0.4);
  cursor: pointer;
  transition: all 0.2s ease;
}

.clear-btn:hover {
  background: rgb(var(--color-primary) / 0.1);
  color: rgb(var(--color-on-dark));
}

.selection-info {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.75rem 1.25rem;
  background: rgb(var(--color-info) / 0.08);
  font-size: 0.75rem;
  color: rgb(var(--color-on-dark) / 0.6);
}

.selection-info i {
  color: rgb(var(--color-info) / 0.8);
}

.game-grid {
  flex: 1;
  overflow-y: auto;
  padding: 1rem;
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
  max-height: 400px;
}

.game-tile {
  display: flex;
  align-items: center;
  gap: 0.875rem;
  padding: 0.5rem;
  background: transparent;
  border: 1px solid transparent;
  border-radius: 0.75rem;
  cursor: pointer;
  transition: all 0.2s ease;
  text-align: left;
}

.game-tile:hover {
  background: rgb(var(--color-primary) / 0.08);
  border-color: rgb(var(--color-primary) / 0.2);
}

.game-tile.selected {
  background: linear-gradient(
    135deg,
    rgb(var(--color-primary) / 0.18) 0%,
    rgb(var(--color-secondary) / 0.12) 100%
  );
  border-color: rgb(var(--color-primary));
  box-shadow: 0 0 20px rgb(var(--color-primary) / 0.15);
}

.game-cover {
  position: relative;
  width: 3.5rem;
  height: 4.5rem;
  flex-shrink: 0;
  border-radius: 0.5rem;
  overflow: hidden;
  background: rgb(var(--color-surface));
}

.game-cover img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  transition: transform 0.3s ease;
}

.game-tile:hover .game-cover img {
  transform: scale(1.1);
}

.cover-overlay {
  position: absolute;
  inset: 0;
  background: linear-gradient(180deg, transparent 50%, rgb(0 0 0 / 0.6) 100%);
}

.selected-indicator {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgb(var(--color-primary) / 0.9);
  color: rgb(var(--color-on-primary));
  font-size: 1rem;
}

.game-info {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 0.125rem;
}

.game-title {
  font-size: 0.875rem;
  font-weight: 600;
  color: var(--text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.game-source {
  font-size: 0.6875rem;
  color: var(--text-muted);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.empty-library {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 2rem;
  text-align: center;
  color: var(--text-muted);
}

.empty-library i {
  font-size: 2.5rem;
  opacity: 0.3;
  margin-bottom: 1rem;
}

.empty-library p {
  font-weight: 500;
  color: var(--text-secondary);
  margin: 0 0 0.25rem;
}

.empty-library span {
  font-size: 0.75rem;
}

/* ============================================
   STREAM SECTION
   ============================================ */
.stream-section {
  display: flex;
  flex-direction: column;
  gap: 1rem;
}

.stream-container {
  background: var(--surface-elevated);
  border: 1px solid var(--border-subtle);
  border-radius: 1rem;
  overflow: hidden;
}

.stream-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.875rem 1.25rem;
  border-bottom: 1px solid var(--border-subtle);
}

.stream-title {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-weight: 600;
  color: var(--text-primary);
}

.stream-title i {
  color: rgb(var(--color-primary));
}

.live-badge {
  display: inline-flex;
  align-items: center;
  gap: 0.375rem;
  padding: 0.25rem 0.625rem;
  background: rgb(var(--color-danger));
  border-radius: 0.25rem;
  font-size: 0.625rem;
  font-weight: 700;
  letter-spacing: 0.05em;
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
  0%,
  100% {
    opacity: 1;
  }
  50% {
    opacity: 0.3;
  }
}

.fullscreen-btn {
  padding: 0.5rem;
  background: transparent;
  border: none;
  border-radius: 0.5rem;
  color: var(--text-secondary);
  cursor: pointer;
  transition: all 0.2s ease;
}

.fullscreen-btn:hover {
  background: var(--border-hover);
  color: var(--text-primary);
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

.stream-viewport.fullscreen-mode * {
  cursor: none;
}

.stream-video {
  width: 100%;
  height: 100%;
  object-fit: contain;
}

/* Idle Overlay */
.idle-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(135deg, rgb(var(--color-surface)) 0%, rgb(var(--color-dark)) 100%);
}

.idle-content {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  padding: 2rem;
}

.idle-icon {
  font-size: 4rem;
  color: rgb(var(--color-primary));
  opacity: 0.6;
  margin-bottom: 1.5rem;
  animation: float 3s ease-in-out infinite;
}

@keyframes float {
  0%,
  100% {
    transform: translateY(0);
  }
  50% {
    transform: translateY(-10px);
  }
}

.idle-content h3 {
  font-size: 1.25rem;
  font-weight: 600;
  color: var(--text-primary);
  margin: 0 0 0.5rem;
}

.idle-content p {
  font-size: 0.875rem;
  color: var(--text-muted);
  margin: 0;
  max-width: 280px;
}

/* Connecting Overlay */
.connecting-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgb(0 0 0 / 0.85);
  backdrop-filter: blur(8px);
}

.connecting-content {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 1.25rem;
}

.spinner {
  width: 3.5rem;
  height: 3.5rem;
  border: 3px solid var(--border-subtle);
  border-top-color: rgb(var(--color-primary));
  border-radius: 50%;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}

.connecting-content span {
  font-size: 0.875rem;
  color: var(--text-secondary);
}

/* Stats Overlay */
.stats-overlay {
  position: absolute;
  top: 0.75rem;
  left: 0.75rem;
  z-index: 10;
  max-width: min(500px, calc(100% - 1.5rem));
  padding: 0.75rem 1rem;
  background: rgb(0 0 0 / 0.8);
  border: 1px solid var(--border-subtle);
  border-radius: 0.5rem;
  backdrop-filter: blur(8px);
  pointer-events: none;
}

.stat-line {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  font-size: 0.6875rem;
  line-height: 1.5;
  color: rgb(255 255 255 / 0.85);
  white-space: pre-wrap;
}

/* ============================================
   NOTIFICATION OVERLAY
   ============================================ */
.notification-overlay {
  position: absolute;
  top: 1rem;
  right: 1rem;
  z-index: 100;
  max-width: min(400px, calc(100% - 2rem));
}

.notification-toast {
  display: flex;
  align-items: flex-start;
  gap: 0.875rem;
  padding: 1rem 1.25rem;
  background: rgb(30 30 35 / 0.95);
  border: 1px solid var(--border-subtle);
  border-radius: 0.75rem;
  backdrop-filter: blur(12px);
  box-shadow: 0 8px 32px rgb(0 0 0 / 0.4);
}

.notification-toast.error {
  background: linear-gradient(
    135deg,
    rgb(var(--color-danger) / 0.15) 0%,
    rgb(30 30 35 / 0.95) 100%
  );
  border-color: rgb(var(--color-danger) / 0.4);
}

.notification-toast.warning {
  background: linear-gradient(
    135deg,
    rgb(var(--color-warning) / 0.15) 0%,
    rgb(30 30 35 / 0.95) 100%
  );
  border-color: rgb(var(--color-warning) / 0.4);
}

.notification-toast.success {
  background: linear-gradient(
    135deg,
    rgb(var(--color-success) / 0.15) 0%,
    rgb(30 30 35 / 0.95) 100%
  );
  border-color: rgb(var(--color-success) / 0.4);
}

.notification-toast.info {
  background: linear-gradient(135deg, rgb(var(--color-info) / 0.15) 0%, rgb(30 30 35 / 0.95) 100%);
  border-color: rgb(var(--color-info) / 0.4);
}

.notification-icon {
  flex-shrink: 0;
  width: 2rem;
  height: 2rem;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 50%;
  font-size: 1rem;
}

.notification-toast.error .notification-icon {
  background: rgb(var(--color-danger) / 0.2);
  color: rgb(var(--color-danger));
}

.notification-toast.warning .notification-icon {
  background: rgb(var(--color-warning) / 0.2);
  color: rgb(var(--color-warning));
}

.notification-toast.success .notification-icon {
  background: rgb(var(--color-success) / 0.2);
  color: rgb(var(--color-success));
}

.notification-toast.info .notification-icon {
  background: rgb(var(--color-info) / 0.2);
  color: rgb(var(--color-info));
}

.notification-content {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
}

.notification-title {
  font-size: 0.875rem;
  font-weight: 600;
  color: white;
}

.notification-message {
  font-size: 0.8125rem;
  color: rgb(255 255 255 / 0.7);
  line-height: 1.4;
}

.notification-close {
  flex-shrink: 0;
  padding: 0.375rem;
  background: transparent;
  border: none;
  border-radius: 0.375rem;
  color: rgb(255 255 255 / 0.4);
  cursor: pointer;
  transition: all 0.2s ease;
}

.notification-close:hover {
  background: rgb(255 255 255 / 0.1);
  color: rgb(255 255 255 / 0.8);
}

/* Notification Animation */
.notification-slide-enter-active,
.notification-slide-leave-active {
  transition: all 0.3s ease;
}

.notification-slide-enter-from {
  opacity: 0;
  transform: translateX(20px);
}

.notification-slide-leave-to {
  opacity: 0;
  transform: translateY(-10px);
}

/* ============================================
   METRICS GRID
   ============================================ */
.metrics-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 0.75rem;
}

@media (max-width: 768px) {
  .metrics-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}

.metric-card {
  display: flex;
  align-items: center;
  gap: 0.875rem;
  padding: 1rem;
  background: var(--surface-elevated);
  border: 1px solid var(--border-subtle);
  border-radius: 0.75rem;
}

.metric-icon {
  width: 2.5rem;
  height: 2.5rem;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 0.625rem;
  font-size: 1rem;
}

.metric-icon.blue {
  background: rgb(var(--color-info) / 0.15);
  color: rgb(var(--color-info));
}

.metric-icon.purple {
  background: rgb(var(--color-secondary) / 0.15);
  color: rgb(var(--color-secondary));
}

.metric-icon.green {
  background: rgb(var(--color-success) / 0.15);
  color: rgb(var(--color-success));
}

.metric-icon.amber {
  background: rgb(var(--color-warning) / 0.15);
  color: rgb(var(--color-warning));
}

.metric-data {
  display: flex;
  flex-direction: column;
  gap: 0.125rem;
}

.metric-label {
  font-size: 0.6875rem;
  text-transform: uppercase;
  letter-spacing: 0.03em;
  color: var(--text-muted);
}

.metric-value {
  font-size: 1.125rem;
  font-weight: 600;
  color: var(--text-primary);
}

/* ============================================
   DEBUG PANEL
   ============================================ */
.debug-panel {
  background: var(--surface-elevated);
  border: 1px solid var(--border-subtle);
  border-radius: 0.75rem;
  overflow: hidden;
}

.debug-summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.875rem 1.25rem;
  cursor: pointer;
  transition: background 0.2s ease;
  list-style: none;
}

.debug-summary::-webkit-details-marker {
  display: none;
}

.debug-summary:hover {
  background: var(--border-subtle);
}

.debug-title {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-weight: 600;
  font-size: 0.875rem;
  color: var(--text-primary);
}

.debug-title i {
  color: rgb(var(--color-primary));
}

.debug-summary .chevron {
  font-size: 0.75rem;
  color: var(--text-muted);
  transition: transform 0.2s ease;
}

.debug-panel[open] .debug-summary .chevron {
  transform: rotate(180deg);
}

.debug-content {
  padding: 0 1.25rem 1.25rem;
  border-top: 1px solid var(--border-subtle);
}

.debug-grid {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 0.5rem 1rem;
  padding-top: 1rem;
}

.debug-section {
  padding-top: 1rem;
  margin-top: 1rem;
  border-top: 1px solid var(--border-subtle);
}

.debug-section-title {
  font-size: 0.6875rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.03em;
  color: var(--text-muted);
  margin-bottom: 0.75rem;
}

.debug-item {
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 0.75rem;
}

.debug-label {
  color: var(--text-muted);
}

.debug-value {
  color: var(--text-secondary);
}

.debug-value.mono {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  font-size: 0.625rem;
  max-width: 120px;
  overflow: hidden;
  text-overflow: ellipsis;
}

.video-events {
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
}

.event-line {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  font-size: 0.625rem;
  color: var(--text-muted);
}

.no-events {
  font-size: 0.75rem;
  color: var(--text-muted);
  font-style: italic;
}

/* ============================================
   NAIVE UI OVERRIDES
   ============================================ */
:deep(.n-input-number) {
  --n-color: rgb(var(--color-surface)) !important;
  --n-border: 1px solid var(--border-subtle) !important;
  --n-text-color: var(--text-primary) !important;
  --n-color-focus: rgb(var(--color-surface)) !important;
  --n-border-focus: 1px solid rgb(var(--color-primary)) !important;
}

:deep(.n-switch) {
  --n-rail-color: var(--border-subtle) !important;
  --n-rail-color-active: rgb(var(--color-primary)) !important;
}

/* ============================================
   SCROLLBAR
   ============================================ */
.game-grid::-webkit-scrollbar {
  width: 6px;
}

.game-grid::-webkit-scrollbar-track {
  background: transparent;
}

.game-grid::-webkit-scrollbar-thumb {
  background: rgb(255 255 255 / 0.1);
  border-radius: 3px;
}

.game-grid::-webkit-scrollbar-thumb:hover {
  background: rgb(255 255 255 / 0.2);
}

.settings-panel::-webkit-scrollbar {
  width: 6px;
}

.settings-panel::-webkit-scrollbar-track {
  background: transparent;
}

.settings-panel::-webkit-scrollbar-thumb {
  background: rgb(255 255 255 / 0.1);
  border-radius: 3px;
}
</style>
