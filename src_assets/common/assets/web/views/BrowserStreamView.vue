<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet, apiPost } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
  UiIcon,
  type StatusTone,
} from '@/components/ui';
import { appCoverUrl, appName, fetchApps, type AppRecord } from '@/services/apps';
import {
  BrowserWebRtcSession,
  detectBrowserVideoCapabilities,
  fetchWebRtcHostCapabilities,
  unavailableHostCapabilities,
  WebRtcConnectionCanceledError,
  type BrowserVideoCapabilities,
  type WebRtcHostCapabilities,
} from '@/services/webrtc';
import {
  applyGamepadFeedback,
  attachBrowserGamepadCapture,
  type BrowserGamepadCapture,
} from '@/utils/webrtc/browserGamepad';
import type { SessionStatus } from '@/types/sessions';
import type { EncodingType, StreamConfig, WebRtcStatsSnapshot } from '@/types/webrtc';
import { formatBitrate } from '@/utils/format';

interface LaunchableApp {
  coverUrl: string;
  id: number;
  name: string;
}

interface StreamLaunchForm {
  appId: string;
  bitrateKbps: number;
  encoding: EncodingType;
  fps: number;
  hdr: boolean;
  height: number;
  muteHostAudio: boolean;
  videoMaxFrameAgeFrames: number;
  videoPacingMode: PacingMode;
  videoPacingSlackMs: number;
  width: number;
}

interface PressedMouseButton {
  button: number;
  modifiers: Record<string, boolean>;
  x: number;
  y: number;
}

interface PressedKey {
  code: string;
  key: string;
  modifiers: Record<string, boolean>;
}

interface PointerPosition {
  x: number;
  y: number;
}

interface TouchPointerGesture {
  button: number;
  dragThresholdPx: number;
  dragging: boolean;
  lastPosition: PointerPosition;
  modifiers: Record<string, boolean>;
  startClientX: number;
  startClientY: number;
  startPosition: PointerPosition;
  startedAtMs: number;
}

interface MutationResponse {
  error?: string;
  status?: boolean;
}

interface WebKitFullscreenElement extends HTMLElement {
  webkitRequestFullScreen?: () => Promise<void> | void;
  webkitRequestFullscreen?: () => Promise<void> | void;
}

interface WebKitFullscreenVideoElement extends HTMLVideoElement {
  webkitDisplayingFullscreen?: boolean;
  webkitEnterFullScreen?: () => void;
  webkitEnterFullscreen?: () => void;
  webkitExitFullscreen?: () => void;
}

interface WebKitFullscreenDocument extends Document {
  webkitCancelFullScreen?: () => Promise<void> | void;
  webkitExitFullscreen?: () => Promise<void> | void;
}

interface KeyboardLockNavigator extends Navigator {
  keyboard?: {
    lock?: (keys?: string[]) => Promise<void>;
    unlock?: () => void;
  };
}

interface StandaloneNavigator extends Navigator {
  standalone?: boolean;
}

type PacingMode = 'latency' | 'balanced' | 'smoothness';

const { t } = useI18n();
const codecs: EncodingType[] = ['h264', 'hevc', 'av1'];
const browserSession = new BrowserWebRtcSession();

const pacingPresets: Record<PacingMode, { slackMs: number; maxAgeFrames: number }> = {
  latency: { slackMs: 0, maxAgeFrames: 1 },
  balanced: { slackMs: 2, maxAgeFrames: 1 },
  smoothness: { slackMs: 3, maxAgeFrames: 3 },
};
const minFrameAgeMs = 5;
const maxFrameAgeMs = 100;
const maxFrameAgeFrameCount = 10;

function maxAllowedFramesForFps(fps: number): number {
  const safeFps = Number.isFinite(fps) && fps > 0 ? fps : 60;
  return Math.max(1, Math.min(maxFrameAgeFrameCount, Math.floor((maxFrameAgeMs * safeFps) / 1000)));
}

function clampFrameAgeFrames(
  value: number | null | undefined,
  fps: number,
  mode: PacingMode,
): number {
  const preset = pacingPresets[mode].maxAgeFrames;
  const maxAllowed = maxAllowedFramesForFps(fps);
  if (value === null || value === undefined || !Number.isFinite(value)) {
    return Math.min(preset, maxAllowed);
  }
  return Math.min(maxAllowed, Math.max(1, Math.round(value)));
}

function frameAgeMsFromFrames(fps: number, frames: number): number {
  const safeFps = Number.isFinite(fps) && fps > 0 ? fps : 60;
  return Math.min(maxFrameAgeMs, Math.max(minFrameAgeMs, Math.round((1000 / safeFps) * frames)));
}

const appSearch = ref('');
const apps = ref<AppRecord[]>([]);
const browserCapabilities = ref<BrowserVideoCapabilities>({
  h264: { supported: false, hdr: false },
  hevc: { supported: false, hdr: false },
  av1: { supported: false, hdr: false },
});
const browserStats = ref<WebRtcStatsSnapshot>({});
const connectionState = ref<RTCPeerConnectionState | 'idle'>('idle');
const hostCapabilities = ref<WebRtcHostCapabilities>({ ...unavailableHostCapabilities });
const inputChannelState = ref<RTCDataChannelState>('closed');
const inputForwarding = ref(true);
const windowFocused = ref(true);
const showPerformanceOverlay = ref(false);
const fullscreenExitHoldActive = ref(false);
const installHelpOpen = ref(false);
const isConnecting = ref(false);
const loading = ref(true);
const nativeFullscreen = ref(false);
const nativeVideoFullscreen = ref(false);
const playbackBlocked = ref(false);
const pseudoFullscreen = ref(false);
const autoFullscreen = ref(true);
const streamCollapsed = ref(false);
const refreshError = ref('');
const sessionActionError = ref('');
const sessionActionPending = ref(false);
const sessionStatus = ref<SessionStatus | null>(null);
const startAfterTerminate = ref(false);
const streamError = ref('');
const streamSurface = ref<HTMLElement | null>(null);
const standaloneWebApp = ref(false);
const terminateOpen = ref(false);
const audioEl = ref<HTMLAudioElement | null>(null);
const videoEl = ref<HTMLVideoElement | null>(null);
const failedAppCovers = ref(new Set<number>());
const pressedKeys = new Map<string, PressedKey>();
const pressedMouseButtons = new Map<number, PressedMouseButton>();
const touchPointerGestures = new Map<number, TouchPointerGesture>();
const fullscreenExitHoldMs = 3000;
const fullscreenExitSwipeThresholdPx = 120;
const videoLatencyResetCooldownMs = 4000;
const videoRenderDelayResetThresholdMs = 100;
const videoRenderDelaySustainMs = 900;
let audioPlaybackStream: MediaStream | null = null;
let fullscreenExitEscapePressed = false;
let fullscreenExitHoldTimer: number | undefined;
let fullscreenExitSwipe: { pointerId: number; startX: number; startY: number } | null = null;
let fullscreenKeyboardLockRequest = 0;
let pageOverflowBeforePseudoFullscreen: { body: string; root: string } | null = null;
let sessionStatusTimer: number | undefined;
let videoBufferOverloadedSince: number | null = null;
let videoFrameCallbackHandle: number | undefined;
let videoRenderOverloadedSince: number | null = null;
let videoLatencyResetAt: number | null = null;
let videoPlaybackStream: MediaStream | null = null;
let gamepadCapture: BrowserGamepadCapture | null = null;
let autoFullscreenAttempt = 0;
let preserveAutoFullscreenDuringRestart = false;
let streamSurfaceFocusedBeforeCollapse = false;

const form = reactive<StreamLaunchForm>({
  appId: '',
  bitrateKbps: 20_000,
  encoding: 'h264',
  fps: 60,
  hdr: false,
  height: 1080,
  muteHostAudio: true,
  videoMaxFrameAgeFrames: pacingPresets.balanced.maxAgeFrames,
  videoPacingMode: 'balanced',
  videoPacingSlackMs: pacingPresets.balanced.slackMs,
  width: 1920,
});

const browserStreamSettingsStorageKey = 'sunshine.webrtc.session_config';

function loadSavedForm(): void {
  if (typeof window === 'undefined') return;
  try {
    const raw = window.localStorage.getItem(browserStreamSettingsStorageKey);
    if (!raw) return;
    const saved = JSON.parse(raw) as Record<string, unknown>;
    if (!saved || typeof saved !== 'object') return;
    const mode =
      saved.videoPacingMode === 'latency' ||
      saved.videoPacingMode === 'balanced' ||
      saved.videoPacingMode === 'smoothness'
        ? saved.videoPacingMode
        : form.videoPacingMode;
    const fps = typeof saved.fps === 'number' && Number.isFinite(saved.fps) ? saved.fps : form.fps;
    const frameCount =
      typeof saved.videoMaxFrameAgeFrames === 'number'
        ? saved.videoMaxFrameAgeFrames
        : typeof saved.videoMaxFrameAgeMs === 'number'
          ? Math.round((saved.videoMaxFrameAgeMs / 1000) * fps)
          : form.videoMaxFrameAgeFrames;
    Object.assign(form, {
      appId: typeof saved.appId === 'string' ? saved.appId : form.appId,
      bitrateKbps:
        typeof saved.bitrateKbps === 'number' && Number.isFinite(saved.bitrateKbps)
          ? saved.bitrateKbps
          : form.bitrateKbps,
      encoding:
        saved.encoding === 'h264' || saved.encoding === 'hevc' || saved.encoding === 'av1'
          ? saved.encoding
          : form.encoding,
      fps,
      hdr: typeof saved.hdr === 'boolean' ? saved.hdr : form.hdr,
      height:
        typeof saved.height === 'number' && Number.isFinite(saved.height)
          ? saved.height
          : form.height,
      muteHostAudio:
        typeof saved.muteHostAudio === 'boolean' ? saved.muteHostAudio : form.muteHostAudio,
      videoMaxFrameAgeFrames: clampFrameAgeFrames(frameCount, fps, mode as PacingMode),
      videoPacingMode: mode,
      videoPacingSlackMs:
        typeof saved.videoPacingSlackMs === 'number' && Number.isFinite(saved.videoPacingSlackMs)
          ? Math.min(10, Math.max(0, Math.round(saved.videoPacingSlackMs)))
          : pacingPresets[mode as PacingMode].slackMs,
      width:
        typeof saved.width === 'number' && Number.isFinite(saved.width) ? saved.width : form.width,
    });
    if (typeof saved.autoFullscreen === 'boolean') autoFullscreen.value = saved.autoFullscreen;
  } catch {
    // Settings are a convenience; malformed or unavailable storage uses defaults.
  }
}

function persistForm(): void {
  if (typeof window === 'undefined') return;
  try {
    window.localStorage.setItem(
      browserStreamSettingsStorageKey,
      JSON.stringify({
        ...form,
        autoFullscreen: autoFullscreen.value,
        videoMaxFrameAgeMs: undefined,
      }),
    );
  } catch {
    // Private browsing and storage quotas should not block starting a stream.
  }
}

function unavailableCapabilities(reason: string): WebRtcHostCapabilities {
  return {
    ...unavailableHostCapabilities,
    availability: { state: 'unavailable', reason },
    codecs: {
      h264: { supported: false, hdr: false },
      hevc: { supported: false, hdr: false },
      av1: { supported: false, hdr: false },
    },
  };
}

function appIdFor(app: AppRecord): number | null {
  const id = Number(app.index);
  return Number.isInteger(id) && id > 0 ? id : null;
}

const launchableApps = computed<LaunchableApp[]>(() =>
  apps.value.flatMap((app) => {
    const id = appIdFor(app);
    if (id === null) return [];
    return [
      {
        coverUrl: appCoverUrl(app),
        id,
        name: appName(app) || t('ui.browser_stream.unnamed_application'),
      },
    ];
  }),
);

const filteredLaunchableApps = computed(() => {
  const query = appSearch.value.trim().toLocaleLowerCase();
  if (!query) return launchableApps.value;
  return launchableApps.value.filter((app) => app.name.toLocaleLowerCase().includes(query));
});

const selectedAppId = computed(() => {
  const id = Number(form.appId);
  return Number.isInteger(id) && id > 0 ? id : undefined;
});

const selectedAppName = computed(() => {
  const selected = launchableApps.value.find((app) => app.id === selectedAppId.value);
  if (selected?.name) return selected.name;
  if (sessionStatus.value?.appName && hasRunningSession.value) return sessionStatus.value.appName;
  return t('ui.browser_stream.desktop');
});

const hasRunningSession = computed(
  () =>
    Boolean(sessionStatus.value?.appRunning) ||
    Number(sessionStatus.value?.activeSessions ?? 0) > 0,
);

const resumeAvailable = computed(
  () =>
    selectedAppId.value === undefined &&
    (Number(sessionStatus.value?.activeSessions ?? 0) > 0 || sessionStatus.value?.paused === true),
);

const primaryActionLabel = computed(() =>
  resumeAvailable.value ? t('webrtc.resume') : t('ui.browser_stream.start'),
);

const terminateDescription = computed(() =>
  startAfterTerminate.value
    ? t('webrtc.terminate_confirm_message', {
        app: selectedAppName.value || t('webrtc.terminate_confirm_app_fallback'),
      })
    : t('webrtc.terminate_desc'),
);

const terminateConfirmLabel = computed(() =>
  startAfterTerminate.value ? t('webrtc.terminate_confirm_action') : t('webrtc.terminate'),
);

const fullscreenActive = computed(
  () => nativeFullscreen.value || nativeVideoFullscreen.value || pseudoFullscreen.value,
);

const fullscreenExitControlLabel = computed(() =>
  fullscreenExitHoldActive.value
    ? t('ui.browser_stream.exit_fullscreen_cancel_hint')
    : t('ui.browser_stream.exit_fullscreen'),
);

const showFullscreenSwipeExit = computed(() => fullscreenActive.value && isTouchSafariBrowser());

const showInstallWebAppAction = computed(() => isTouchSafariBrowser() && !standaloneWebApp.value);

function selectApp(appId?: number): void {
  form.appId = appId === undefined ? '' : String(appId);
}

function appSelected(appId?: number): boolean {
  return selectedAppId.value === appId;
}

function appCoverFailed(appId: number): boolean {
  return failedAppCovers.value.has(appId);
}

function markAppCoverFailed(appId: number): void {
  failedAppCovers.value = new Set(failedAppCovers.value).add(appId);
}

const hostReady = computed(
  () => hostCapabilities.value.enabled && hostCapabilities.value.availability.state === 'ready',
);
const hdrForcedOn = computed(() => hostCapabilities.value.hdr_policy === 'force_on');
const hdrForcedOff = computed(() => hostCapabilities.value.hdr_policy === 'force_off');
const effectiveHdr = computed(() =>
  hdrForcedOn.value ? true : hdrForcedOff.value ? false : form.hdr,
);
const isConnected = computed(() => connectionState.value === 'connected');
const connectionPending = computed(
  () =>
    isConnecting.value || connectionState.value === 'new' || connectionState.value === 'connecting',
);
const inputReady = computed(
  () =>
    !streamCollapsed.value &&
    isConnected.value &&
    inputForwarding.value &&
    inputChannelState.value === 'open',
);

const inputStatusLabel = computed(() => {
  if (streamCollapsed.value) return t('ui.browser_stream.input_paused');
  return inputReady.value
    ? t('ui.browser_stream.input_ready')
    : t('ui.browser_stream.input_unavailable');
});

const connectionLabel = computed(() => {
  if (isConnected.value) return t('ui.browser_stream.status.connected');
  if (connectionPending.value) return t('ui.browser_stream.status.connecting');
  if (connectionState.value === 'failed') return t('ui.browser_stream.status.failed');
  if (connectionState.value === 'disconnected' || connectionState.value === 'closed') {
    return t('ui.browser_stream.status.disconnected');
  }
  return t('ui.browser_stream.status.ready');
});

const connectionTone = computed<StatusTone>(() => {
  if (isConnected.value) return 'success';
  if (connectionPending.value) return 'info';
  if (connectionState.value === 'failed') return 'danger';
  if (!hostReady.value) return 'warning';
  return 'neutral';
});

function codecLabel(codec: EncodingType): string {
  return t(`ui.browser_stream.codecs.${codec}`);
}

const pacingOptions = computed(() => [
  { label: t('webrtc.pacing_latency'), value: 'latency' as const },
  { label: t('webrtc.pacing_balanced'), value: 'balanced' as const },
  { label: t('webrtc.pacing_smooth'), value: 'smoothness' as const },
]);

const maxFrameAgeFrames = computed({
  get: () => clampFrameAgeFrames(form.videoMaxFrameAgeFrames, form.fps, form.videoPacingMode),
  set: (value: number) => {
    if (!Number.isFinite(value)) return;
    form.videoMaxFrameAgeFrames = clampFrameAgeFrames(value, form.fps, form.videoPacingMode);
  },
});

const maxFrameAgeMsLabel = computed(() => frameAgeMsFromFrames(form.fps, maxFrameAgeFrames.value));

function applyPacingPreset(mode: PacingMode): void {
  const preset = pacingPresets[mode];
  form.videoPacingMode = mode;
  form.videoPacingSlackMs = preset.slackMs;
  form.videoMaxFrameAgeFrames = clampFrameAgeFrames(preset.maxAgeFrames, form.fps, mode);
}

function baseCodecAvailable(codec: EncodingType): boolean {
  return (
    hostReady.value &&
    hostCapabilities.value.codecs[codec].supported &&
    browserCapabilities.value[codec].supported
  );
}

function hdrAvailable(codec: EncodingType): boolean {
  return (
    baseCodecAvailable(codec) &&
    hostCapabilities.value.hdr_policy_allows &&
    hostCapabilities.value.codecs[codec].hdr &&
    browserCapabilities.value[codec].hdr
  );
}

function codecAvailable(codec: EncodingType): boolean {
  return baseCodecAvailable(codec) && (!hdrForcedOn.value || hdrAvailable(codec));
}

function codecUnavailableReason(codec: EncodingType): string {
  if (!hostCapabilities.value.enabled) {
    return (
      hostCapabilities.value.availability.reason || t('ui.browser_stream.reasons.host_disabled')
    );
  }
  if (!hostReady.value) {
    return (
      hostCapabilities.value.availability.reason || t('ui.browser_stream.reasons.host_unverified')
    );
  }
  if (!hostCapabilities.value.codecs[codec].supported) {
    return t('ui.browser_stream.reasons.host_codec_unavailable', { codec: codecLabel(codec) });
  }
  if (!browserCapabilities.value[codec].supported) {
    return t('ui.browser_stream.reasons.browser_codec_unavailable', { codec: codecLabel(codec) });
  }
  if (hdrForcedOn.value && !hdrAvailable(codec)) {
    return t('ui.browser_stream.reasons.hdr_required');
  }
  return '';
}

const hdrUnavailableReason = computed(() => {
  if (!codecAvailable(form.encoding)) return codecUnavailableReason(form.encoding);
  if (!hostCapabilities.value.hdr_policy_allows)
    return t('ui.browser_stream.reasons.hdr_policy_disabled');
  if (!hostCapabilities.value.codecs[form.encoding].hdr) {
    return t('ui.browser_stream.reasons.host_hdr_unavailable', {
      codec: codecLabel(form.encoding),
    });
  }
  if (!browserCapabilities.value[form.encoding].hdr) {
    return t('ui.browser_stream.reasons.browser_hdr_unavailable', {
      codec: codecLabel(form.encoding),
    });
  }
  return '';
});

const hdrControlDisabled = computed(
  () => hdrForcedOn.value || hdrForcedOff.value || !hdrAvailable(form.encoding),
);

const hdrControlDescription = computed(() => {
  if (hdrForcedOn.value) return t('ui.browser_stream.settings.hdr_forced_on');
  if (hdrForcedOff.value) return t('ui.browser_stream.settings.hdr_forced_off');
  return hdrAvailable(form.encoding)
    ? t('ui.browser_stream.settings.hdr_help')
    : hdrUnavailableReason.value;
});

const validationError = computed(() => {
  if (!hostReady.value) {
    return (
      hostCapabilities.value.availability.reason || t('ui.browser_stream.reasons.host_unverified')
    );
  }
  if (!codecAvailable(form.encoding)) return codecUnavailableReason(form.encoding);
  if (effectiveHdr.value && !hdrAvailable(form.encoding)) return hdrUnavailableReason.value;

  const limits = hostCapabilities.value.limits;
  const dimensions = [form.width, form.height];
  if (
    dimensions.some(
      (value) =>
        !Number.isInteger(value) ||
        value < limits.min_dimension ||
        value > limits.max_dimension ||
        value % 2 !== 0,
    )
  ) {
    return t('ui.browser_stream.reasons.invalid_dimensions', {
      min: limits.min_dimension,
      max: limits.max_dimension,
    });
  }
  if (!Number.isInteger(form.fps) || form.fps < limits.min_fps || form.fps > limits.max_fps) {
    return t('ui.browser_stream.reasons.invalid_fps', {
      min: limits.min_fps,
      max: limits.max_fps,
    });
  }
  if (
    !Number.isInteger(form.bitrateKbps) ||
    form.bitrateKbps < limits.min_bitrate_kbps ||
    form.bitrateKbps > limits.max_bitrate_kbps
  ) {
    return t('ui.browser_stream.reasons.invalid_bitrate', {
      min: limits.min_bitrate_kbps,
      max: limits.max_bitrate_kbps,
    });
  }
  if (!['latency', 'balanced', 'smoothness'].includes(form.videoPacingMode)) {
    return t('ui.browser_stream.reasons.invalid_pacing_mode');
  }
  if (
    !Number.isInteger(form.videoPacingSlackMs) ||
    form.videoPacingSlackMs < 0 ||
    form.videoPacingSlackMs > 10
  ) {
    return t('ui.browser_stream.reasons.invalid_pacing_slack', { min: 0, max: 10 });
  }
  if (
    !Number.isInteger(form.videoMaxFrameAgeFrames) ||
    form.videoMaxFrameAgeFrames < 1 ||
    form.videoMaxFrameAgeFrames > maxAllowedFramesForFps(form.fps)
  ) {
    return t('ui.browser_stream.reasons.invalid_frame_age', {
      min: 1,
      max: maxAllowedFramesForFps(form.fps),
    });
  }
  return '';
});

const startDisabled = computed(
  () =>
    loading.value ||
    connectionPending.value ||
    isConnected.value ||
    sessionActionPending.value ||
    Boolean(validationError.value),
);

function messageFromError(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}

function stopSessionStatusPolling(): void {
  if (sessionStatusTimer === undefined) return;
  window.clearInterval(sessionStatusTimer);
  sessionStatusTimer = undefined;
}

async function fetchSessionStatus(): Promise<void> {
  if (connectionPending.value || isConnected.value) return;
  try {
    const status = await apiGet<SessionStatus>('/api/session/status');
    sessionStatus.value = status.status ? status : null;
  } catch {
    sessionStatus.value = null;
  }
}

function startSessionStatusPolling(): void {
  stopSessionStatusPolling();
  if (connectionPending.value || isConnected.value) return;
  void fetchSessionStatus();
  sessionStatusTimer = window.setInterval(fetchSessionStatus, 5000);
}

async function refresh(): Promise<void> {
  if (loading.value && apps.value.length) return;
  loading.value = true;
  refreshError.value = '';

  const [hostResult, browserResult, appResult] = await Promise.allSettled([
    fetchWebRtcHostCapabilities(),
    detectBrowserVideoCapabilities(),
    fetchApps(),
  ]);

  if (hostResult.status === 'fulfilled') {
    hostCapabilities.value = hostResult.value;
  } else {
    hostCapabilities.value = unavailableCapabilities(
      messageFromError(hostResult.reason, t('ui.browser_stream.reasons.host_unavailable')),
    );
  }
  if (browserResult.status === 'fulfilled') {
    browserCapabilities.value = browserResult.value;
  } else {
    browserCapabilities.value = {
      h264: { supported: false, hdr: false },
      hevc: { supported: false, hdr: false },
      av1: { supported: false, hdr: false },
    };
  }
  if (appResult.status === 'fulfilled') {
    apps.value = appResult.value;
  } else {
    refreshError.value = messageFromError(
      appResult.reason,
      t('ui.browser_stream.errors.load_apps'),
    );
  }

  if (browserResult.status === 'rejected' && !refreshError.value) {
    refreshError.value = t('ui.browser_stream.errors.inspect_browser');
  }
  await fetchSessionStatus();
  loading.value = false;
}

function onEncodingChanged(): void {
  if (form.hdr && !hdrAvailable(form.encoding)) form.hdr = false;
}

function setHdr(event: Event): void {
  form.hdr = (event.target as HTMLInputElement).checked;
}

function replaceTracks(
  current: MediaStream | null,
  tracks: MediaStreamTrack[],
): MediaStream | null {
  if (!tracks.length || typeof MediaStream !== 'function') return current;
  const target = current ?? new MediaStream();
  for (const kind of new Set(tracks.map((track) => track.kind))) {
    const incoming = tracks.filter((track) => track.kind === kind);
    const existing = target.getTracks().filter((track) => track.kind === kind);
    if (
      existing.length === incoming.length &&
      incoming.every((track) => existing.some((currentTrack) => currentTrack.id === track.id))
    ) {
      continue;
    }
    for (const track of existing) target.removeTrack(track);
    for (const track of incoming) target.addTrack(track);
  }
  return target;
}

async function playAttachedMedia(): Promise<void> {
  const attempts: Promise<void>[] = [];
  if (videoEl.value?.srcObject) attempts.push(videoEl.value.play());
  if (audioEl.value?.srcObject) attempts.push(audioEl.value.play());
  const results = await Promise.allSettled(attempts);
  playbackBlocked.value = results.some((result) => result.status === 'rejected');
}

function isSafariBrowser(): boolean {
  try {
    const userAgent = navigator.userAgent ?? '';
    const vendor = navigator.vendor ?? '';
    return (
      /\bsafari\//i.test(userAgent) &&
      /apple/i.test(vendor) &&
      !/\b(chrome|chromium|crios|fxios|edgios|edg|opr|opera)\b/i.test(userAgent)
    );
  } catch {
    return false;
  }
}

function isTouchSafariBrowser(): boolean {
  try {
    const userAgent = navigator.userAgent ?? '';
    const platform = navigator.platform ?? '';
    return (
      /apple/i.test(navigator.vendor ?? '') &&
      navigator.maxTouchPoints > 1 &&
      (/\b(iPad|iPhone|iPod)\b/i.test(userAgent) || /MacIntel/i.test(platform))
    );
  } catch {
    return false;
  }
}

function runningAsStandaloneWebApp(): boolean {
  try {
    return (
      (navigator as StandaloneNavigator).standalone === true ||
      window.matchMedia('(display-mode: standalone)').matches ||
      window.matchMedia('(display-mode: fullscreen)').matches
    );
  } catch {
    return false;
  }
}

function resetVideoLatencyFence(): void {
  videoBufferOverloadedSince = null;
  videoRenderOverloadedSince = null;
}

function resetVideoElementForLatency(): void {
  const player = videoEl.value;
  const stream = videoPlaybackStream;
  if (!player || !stream || !stream.getVideoTracks().length) return;

  resetVideoLatencyFence();
  videoLatencyResetAt = performance.now();
  browserSession.requestLatencyResync();
  try {
    player.pause();
    player.srcObject = null;
    player.load();
  } catch {
    // Reattaching on the next frame is still worth attempting.
  }
  window.requestAnimationFrame(() => {
    if (videoEl.value !== player || videoPlaybackStream !== stream || !isConnected.value) return;
    player.muted = true;
    player.playbackRate = 1;
    player.srcObject = stream;
    void player.play().catch(() => {
      playbackBlocked.value = true;
    });
  });
}

function applyVideoLatencyFence(
  delayMs: number | undefined,
  thresholdMs: number,
  sustainMs: number,
  source: 'buffer' | 'render',
): void {
  const overloadedSince =
    source === 'buffer' ? videoBufferOverloadedSince : videoRenderOverloadedSince;
  if (
    !isConnected.value ||
    document.visibilityState !== 'visible' ||
    typeof delayMs !== 'number' ||
    !Number.isFinite(delayMs) ||
    delayMs < thresholdMs
  ) {
    if (source === 'buffer') videoBufferOverloadedSince = null;
    else videoRenderOverloadedSince = null;
    return;
  }

  const now = performance.now();
  const startedAt = overloadedSince ?? now;
  if (source === 'buffer') videoBufferOverloadedSince = startedAt;
  else videoRenderOverloadedSince = startedAt;
  if (now - startedAt < sustainMs) return;
  if (videoLatencyResetAt !== null && now - videoLatencyResetAt < videoLatencyResetCooldownMs)
    return;
  resetVideoElementForLatency();
}

function handleVideoPlayoutDelay(delayMs: number | undefined): void {
  applyVideoLatencyFence(
    delayMs,
    isSafariBrowser() ? 160 : 220,
    isSafariBrowser() ? 1500 : 900,
    'buffer',
  );
}

function stopVideoFrameLatencyMonitoring(): void {
  const player = videoEl.value;
  if (player && videoFrameCallbackHandle !== undefined) {
    player.cancelVideoFrameCallback(videoFrameCallbackHandle);
  }
  videoFrameCallbackHandle = undefined;
}

function startVideoFrameLatencyMonitoring(player: HTMLVideoElement): void {
  stopVideoFrameLatencyMonitoring();
  if (typeof player.requestVideoFrameCallback !== 'function') return;
  const onFrame = (now: number, metadata: VideoFrameCallbackMetadata): void => {
    if (videoEl.value !== player) return;
    const expected = metadata.expectedDisplayTime;
    const delayMs = Number.isFinite(expected) ? Math.max(0, now - expected) : undefined;
    applyVideoLatencyFence(
      delayMs,
      videoRenderDelayResetThresholdMs,
      videoRenderDelaySustainMs,
      'render',
    );
    videoFrameCallbackHandle = player.requestVideoFrameCallback(onFrame);
  };
  videoFrameCallbackHandle = player.requestVideoFrameCallback(onFrame);
}

function attachRemoteStream(stream: MediaStream): void {
  const player = videoEl.value;
  if (!player) return;

  if (typeof MediaStream !== 'function') {
    player.muted = false;
    player.srcObject = stream;
    void playAttachedMedia();
    return;
  }

  const videoTracks = stream.getVideoTracks();
  if (videoTracks.length) {
    videoPlaybackStream = replaceTracks(videoPlaybackStream, videoTracks);
    player.muted = true;
    player.srcObject = videoPlaybackStream;
    startVideoFrameLatencyMonitoring(player);
  }

  const audioTracks = stream.getAudioTracks();
  if (audioTracks.length && audioEl.value) {
    audioPlaybackStream = replaceTracks(audioPlaybackStream, audioTracks);
    audioEl.value.srcObject = audioPlaybackStream;
  }

  void playAttachedMedia();
}

async function connect(resume: boolean): Promise<void> {
  if (startDisabled.value) {
    streamError.value = validationError.value || t('ui.browser_stream.errors.unavailable');
    return;
  }

  stopSessionStatusPolling();
  sessionActionError.value = '';
  streamError.value = '';
  playbackBlocked.value = false;
  isConnecting.value = true;
  connectionState.value = 'connecting';
  const config: StreamConfig = {
    appId: selectedAppId.value,
    audioChannels: 2,
    audioCodec: 'opus',
    bitrateKbps: form.bitrateKbps,
    encoding: form.encoding,
    fps: form.fps,
    hdr: effectiveHdr.value,
    height: form.height,
    muteHostAudio: form.muteHostAudio,
    resume,
    videoMaxFrameAgeFrames: form.videoMaxFrameAgeFrames,
    videoPacingMode: form.videoPacingMode,
    videoPacingSlackMs: form.videoPacingSlackMs,
    width: form.width,
  };

  try {
    browserStats.value = {};
    await browserSession.connect(config, {
      onConnectionState: (state) => {
        connectionState.value = state;
        if (state === 'connected') stopSessionStatusPolling();
        if (state === 'failed') {
          streamError.value = t('ui.browser_stream.errors.connection_failed');
        }
        if (state === 'failed' || state === 'disconnected' || state === 'closed') {
          cancelAutoFullscreen();
          // A peer can disappear without delivering a data-channel close
          // first. Clear browser-side pressed state and controller state on
          // the connection transition as well.
          releaseForwardedInput();
          stopGamepadCapture();
          startSessionStatusPolling();
        }
      },
      onInputState: (state) => {
        if (state !== 'open') releaseForwardedInput();
        inputChannelState.value = state;
        syncGamepadCapture();
      },
      onInputMessage: applyGamepadFeedback,
      onRemoteStream: attachRemoteStream,
      onStats: (stats) => {
        browserStats.value = stats;
      },
      onVideoPlayoutDelay: handleVideoPlayoutDelay,
    });
  } catch (error) {
    if (error instanceof WebRtcConnectionCanceledError) {
      cancelAutoFullscreen();
      connectionState.value = 'idle';
      return;
    }
    cancelAutoFullscreen();
    connectionState.value = 'failed';
    streamError.value = messageFromError(error, t('ui.browser_stream.errors.connect'));
  } finally {
    isConnecting.value = false;
    if (!isConnected.value) startSessionStatusPolling();
  }
}

async function requestPrimaryAction(): Promise<void> {
  if (startDisabled.value) {
    streamError.value = validationError.value || t('ui.browser_stream.errors.unavailable');
    return;
  }
  if (selectedAppId.value !== undefined && hasRunningSession.value) {
    startAfterTerminate.value = true;
    terminateOpen.value = true;
    return;
  }
  requestAutoFullscreen();
  await connect(resumeAvailable.value);
}

function requestTerminate(): void {
  startAfterTerminate.value = false;
  sessionActionError.value = '';
  terminateOpen.value = true;
}

async function confirmTerminate(): Promise<void> {
  if (sessionActionPending.value) return;
  sessionActionPending.value = true;
  sessionActionError.value = '';
  const shouldStart = startAfterTerminate.value;
  if (shouldStart) {
    // The confirmation click is the last trusted activation before the close
    // request. Keep the resulting fullscreen state through the intentional
    // disconnect/reconnect handoff below.
    preserveAutoFullscreenDuringRestart = true;
    requestAutoFullscreen();
  }
  try {
    const response = await apiPost<MutationResponse>('/api/apps/close', {});
    if (response.status !== true) {
      throw new Error(response.error || t('webrtc.termination_failed_desc'));
    }
    terminateOpen.value = false;
    startAfterTerminate.value = false;
    await disconnect();
    await fetchSessionStatus();
    if (shouldStart) {
      sessionActionPending.value = false;
      preserveAutoFullscreenDuringRestart = false;
      await connect(false);
    }
  } catch (error) {
    if (shouldStart) {
      preserveAutoFullscreenDuringRestart = false;
      cancelAutoFullscreen();
    }
    sessionActionError.value = messageFromError(error, t('webrtc.termination_failed_desc'));
  } finally {
    preserveAutoFullscreenDuringRestart = false;
    sessionActionPending.value = false;
  }
}

async function disconnect(restartStatusPolling = true): Promise<void> {
  cancelAutoFullscreen();
  releaseForwardedInput();
  stopGamepadCapture();
  stopVideoFrameLatencyMonitoring();
  resetVideoLatencyFence();
  videoLatencyResetAt = null;
  isConnecting.value = false;
  inputChannelState.value = 'closed';
  await browserSession.disconnect();
  if (videoEl.value) videoEl.value.srcObject = null;
  if (audioEl.value) audioEl.value.srcObject = null;
  videoPlaybackStream = null;
  audioPlaybackStream = null;
  playbackBlocked.value = false;
  browserStats.value = {};
  connectionState.value = 'idle';
  if (restartStatusPolling) startSessionStatusPolling();
}

function resumePlayback(): void {
  void playAttachedMedia();
}

function formatMetric(value: number | undefined, suffix = ''): string {
  return typeof value === 'number' && Number.isFinite(value)
    ? `${Math.round(value * 10) / 10}${suffix}`
    : '--';
}

function formatBitrateMetric(value: number | undefined): string {
  return typeof value === 'number' && Number.isFinite(value) ? formatBitrate(value) : '--';
}

const overlayLines = computed(() => {
  const fps = formatMetric(browserStats.value.videoFps);
  const bitrate = formatBitrateMetric(browserStats.value.videoBitrateKbps);
  const latency = formatMetric(browserStats.value.roundTripTimeMs, ' ms RTT');
  const dropped = formatMetric(browserStats.value.videoFramesDropped);
  const codec = browserStats.value.videoCodec ?? '--';
  const size = `${form.width}×${form.height}`;
  return [
    `${t('sessions.fps')}: ${fps} | ${t('sessions.bitrate')}: ${bitrate}`,
    `${t('webrtc.metric_latency')}: ${latency} | ${t('webrtc.metric_dropped')}: ${dropped}`,
    `${t('webrtc.encoding')}: ${codec} | ${t('webrtc.resolution')}: ${size}`,
  ];
});

function modifiers(event: KeyboardEvent | MouseEvent | WheelEvent): Record<string, boolean> {
  return {
    alt: event.altKey,
    ctrl: event.ctrlKey,
    meta: event.metaKey,
    shift: event.shiftKey,
  };
}

function pointerPosition(
  event: PointerEvent | WheelEvent,
  clampOutside = false,
): PointerPosition | null {
  const video = videoEl.value;
  const surface = streamSurface.value;
  if (!video || !surface) return null;

  const bounds = video.getBoundingClientRect();
  const sourceWidth = video.videoWidth || bounds.width;
  const sourceHeight = video.videoHeight || bounds.height;
  if (bounds.width <= 0 || bounds.height <= 0 || sourceWidth <= 0 || sourceHeight <= 0) {
    return null;
  }
  const scale = Math.min(bounds.width / sourceWidth, bounds.height / sourceHeight);
  const contentWidth = sourceWidth * scale;
  const contentHeight = sourceHeight * scale;
  const left = bounds.left + (bounds.width - contentWidth) / 2;
  const top = bounds.top + (bounds.height - contentHeight) / 2;
  const normalizedX = (event.clientX - left) / contentWidth;
  const normalizedY = (event.clientY - top) / contentHeight;
  if (!clampOutside && (normalizedX < 0 || normalizedX > 1 || normalizedY < 0 || normalizedY > 1)) {
    return null;
  }
  return {
    x: Math.min(1, Math.max(0, normalizedX)),
    y: Math.min(1, Math.max(0, normalizedY)),
  };
}

function sendPointerMove(event: PointerEvent): void {
  if (!inputReady.value) return;
  const touchGesture = touchPointerGestures.get(event.pointerId);
  if (touchGesture) {
    event.preventDefault();
    if (!touchGesture.dragging) {
      const distance = Math.hypot(
        event.clientX - touchGesture.startClientX,
        event.clientY - touchGesture.startClientY,
      );
      const elapsedMs = performance.now() - touchGesture.startedAtMs;
      const immediateDragThresholdPx = Math.max(48, touchGesture.dragThresholdPx * 2.5);
      if (
        distance < touchGesture.dragThresholdPx ||
        (elapsedMs < 140 && distance < immediateDragThresholdPx)
      ) {
        return;
      }
      touchGesture.dragging = true;
      const pressed = {
        button: touchGesture.button,
        modifiers: touchGesture.modifiers,
        ...touchGesture.startPosition,
      };
      pressedMouseButtons.set(touchGesture.button, pressed);
      browserSession.sendInput({ ...pressed, type: 'mouse_down' });
    }
  }

  const position = pointerPosition(event, event.buttons !== 0);
  if (!position) return;
  if (touchGesture) touchGesture.lastPosition = position;
  for (const pressed of pressedMouseButtons.values()) {
    pressed.x = position.x;
    pressed.y = position.y;
  }
  browserSession.sendInput({
    ...position,
    buttons: event.buttons,
    modifiers: modifiers(event),
    type: 'mouse_move',
  });
}

function sendPointerButton(event: PointerEvent, type: 'mouse_down' | 'mouse_up'): void {
  if (!inputReady.value) return;
  const surface = streamSurface.value;
  const touchLike = event.pointerType === 'touch' || event.pointerType === 'pen';
  if (touchLike) event.preventDefault();
  const touchGesture = touchPointerGestures.get(event.pointerId);
  if (type === 'mouse_up' && !pressedMouseButtons.has(event.button) && !touchGesture) {
    // WebKit can emit a late pointerup after pointer capture was already lost.
    // Its stale coordinate must not reposition the host cursor a second time.
    return;
  }
  if (!touchLike && surface && document.activeElement !== surface) {
    try {
      surface.focus({ preventScroll: true });
    } catch {
      surface.focus();
    }
  }

  const position =
    type === 'mouse_up' && touchGesture && !touchGesture.dragging
      ? touchGesture.startPosition
      : (pointerPosition(event, type === 'mouse_up' && pressedMouseButtons.has(event.button)) ??
        touchGesture?.lastPosition);
  if (!position) return;

  if (type === 'mouse_down') {
    if (touchLike) {
      const contactRadius = Math.max(event.width || 0, event.height || 0) / 2;
      const gesture: TouchPointerGesture = {
        button: event.button,
        dragThresholdPx:
          event.pointerType === 'pen' ? 8 : Math.max(18, Math.min(32, contactRadius || 18)),
        dragging: false,
        lastPosition: position,
        modifiers: modifiers(event),
        startClientX: event.clientX,
        startClientY: event.clientY,
        startPosition: position,
        startedAtMs: performance.now(),
      };
      touchPointerGestures.set(event.pointerId, gesture);
      try {
        surface?.setPointerCapture(event.pointerId);
      } catch {
        // Pointer capture is optional; the pending tap remains frozen.
      }
      return;
    }
    pressedMouseButtons.set(event.button, {
      button: event.button,
      modifiers: modifiers(event),
      ...position,
    });
    try {
      surface?.setPointerCapture(event.pointerId);
    } catch {
      // Pointer capture is an optimization; the button event is still valid.
    }
  } else if (touchGesture) {
    const releasePosition = touchGesture.dragging
      ? (pointerPosition(event, true) ?? touchGesture.lastPosition)
      : touchGesture.startPosition;
    const release = {
      button: touchGesture.button,
      modifiers: touchGesture.modifiers,
      ...releasePosition,
    };
    if (!touchGesture.dragging) {
      browserSession.sendInput({ ...release, type: 'mouse_down' });
    }
    pressedMouseButtons.delete(touchGesture.button);
    touchPointerGestures.delete(event.pointerId);
    browserSession.sendInput({ ...release, type: 'mouse_up' });
    return;
  } else {
    pressedMouseButtons.delete(event.button);
  }
  browserSession.sendInput({
    ...position,
    button: event.button,
    modifiers: modifiers(event),
    type,
  });
}

function sendWheel(event: WheelEvent): void {
  if (!inputReady.value) return;
  const position = pointerPosition(event);
  if (!position) return;
  event.preventDefault();
  browserSession.sendInput({
    ...position,
    dx: event.deltaX / 100,
    dy: event.deltaY / 100,
    modifiers: modifiers(event),
    type: 'wheel',
  });
}

function cancelFullscreenExitHold(releaseEscape = true): void {
  if (fullscreenExitHoldTimer !== undefined) {
    window.clearTimeout(fullscreenExitHoldTimer);
    fullscreenExitHoldTimer = undefined;
  }
  fullscreenExitHoldActive.value = false;
  if (releaseEscape) fullscreenExitEscapePressed = false;
}

function handleFullscreenExitHold(event: KeyboardEvent, type: 'key_down' | 'key_up'): boolean {
  if (event.code !== 'Escape' || (!fullscreenActive.value && !fullscreenExitEscapePressed)) {
    return false;
  }

  event.preventDefault();
  event.stopPropagation();
  if (type === 'key_up') {
    cancelFullscreenExitHold();
    return true;
  }
  if (fullscreenExitEscapePressed) return true;

  fullscreenExitEscapePressed = true;
  fullscreenExitHoldActive.value = true;
  fullscreenExitHoldTimer = window.setTimeout(() => {
    fullscreenExitHoldTimer = undefined;
    fullscreenExitHoldActive.value = false;
    if (fullscreenActive.value) void exitFullscreen();
  }, fullscreenExitHoldMs);
  return true;
}

function sendKey(event: KeyboardEvent, type: 'key_down' | 'key_up'): void {
  if (handleFullscreenExitHold(event, type)) return;
  if (!inputReady.value) return;
  event.preventDefault();
  if (type === 'key_down') {
    pressedKeys.set(event.code, { code: event.code, key: event.key, modifiers: modifiers(event) });
  } else {
    pressedKeys.delete(event.code);
  }
  browserSession.sendInput({
    code: event.code,
    key: event.key,
    modifiers: modifiers(event),
    repeat: event.repeat,
    type,
  });
}

function startFullscreenExitSwipe(event: PointerEvent): void {
  if (event.pointerType !== 'touch' || !fullscreenActive.value) return;
  event.preventDefault();
  event.stopPropagation();
  fullscreenExitSwipe = {
    pointerId: event.pointerId,
    startX: event.clientX,
    startY: event.clientY,
  };
  try {
    (event.currentTarget as HTMLElement | null)?.setPointerCapture(event.pointerId);
  } catch {
    // The gesture remains usable without pointer capture.
  }
}

function updateFullscreenExitSwipe(event: PointerEvent): void {
  const swipe = fullscreenExitSwipe;
  if (!swipe || swipe.pointerId !== event.pointerId) return;
  event.preventDefault();
  event.stopPropagation();
  const dx = event.clientX - swipe.startX;
  const dy = event.clientY - swipe.startY;
  if (dy < fullscreenExitSwipeThresholdPx || dy < Math.abs(dx) * 1.4) return;
  fullscreenExitSwipe = null;
  void exitFullscreen();
}

function finishFullscreenExitSwipe(event?: PointerEvent): void {
  if (event && fullscreenExitSwipe?.pointerId !== event.pointerId) return;
  event?.preventDefault();
  event?.stopPropagation();
  fullscreenExitSwipe = null;
}

function releaseForwardedInput(): void {
  for (const pressed of pressedMouseButtons.values()) {
    browserSession.sendInput({ ...pressed, type: 'mouse_up' });
  }
  pressedMouseButtons.clear();
  touchPointerGestures.clear();

  for (const pressed of pressedKeys.values()) {
    browserSession.sendInput({ ...pressed, repeat: false, type: 'key_up' });
  }
  pressedKeys.clear();
}

function stopGamepadCapture(): void {
  if (gamepadCapture) {
    // Neutralize pressed controller state before the disconnect message. This
    // covers input being disabled or the data channel closing without a page
    // blur event.
    gamepadCapture.release();
    gamepadCapture.stop();
  }
  gamepadCapture = null;
}

function syncGamepadCapture(): void {
  if (inputReady.value && windowFocused.value) {
    if (!gamepadCapture) {
      gamepadCapture = attachBrowserGamepadCapture((payload) => browserSession.sendInput(payload), {
        // A hidden page must release controller state and wait for a fresh
        // connect/state handshake when it becomes visible again.
        enabled: () => inputReady.value && document.visibilityState === 'visible',
      });
    }
    return;
  }
  stopGamepadCapture();
}

function onWindowBlur(): void {
  windowFocused.value = false;
  cancelFullscreenExitHold();
  finishFullscreenExitSwipe();
  releaseForwardedInput();
  stopGamepadCapture();
}

function onWindowFocus(): void {
  windowFocused.value = true;
  syncGamepadCapture();
}

function onVisibilityChange(): void {
  if (document.visibilityState !== 'visible') {
    windowFocused.value = false;
    cancelFullscreenExitHold();
    finishFullscreenExitSwipe();
    releaseForwardedInput();
    stopGamepadCapture();
  } else {
    windowFocused.value = true;
    syncGamepadCapture();
  }
}

function requestAutoFullscreen(): void {
  if (!autoFullscreen.value || fullscreenActive.value) return;

  // The fullscreen request must start in the click/submit event. Clearing the
  // collapsed presentation synchronously keeps the surface available to the
  // browser before enterFullscreen reaches its first await.
  revealStreamSurfaceForFullscreen();
  const attempt = ++autoFullscreenAttempt;
  void enterFullscreen()
    .then(() => {
      // A failed or cancelled connection must not leave a pseudo/fullscreen
      // surface behind. The token check also handles a rejected native request
      // that resolves after disconnect has already started.
      if (
        !preserveAutoFullscreenDuringRestart &&
        (attempt !== autoFullscreenAttempt ||
          connectionState.value === 'failed' ||
          connectionState.value === 'disconnected' ||
          connectionState.value === 'closed' ||
          connectionState.value === 'idle')
      ) {
        return exitFullscreen();
      }
    })
    .catch(() => {
      // Fullscreen is an enhancement; connection startup still proceeds.
    });
}

function cancelAutoFullscreen(): void {
  if (preserveAutoFullscreenDuringRestart) return;
  autoFullscreenAttempt += 1;
  if (fullscreenActive.value) void exitFullscreen();
}

function toggleStreamCollapsed(): void {
  if (fullscreenActive.value) return;

  const collapsed = !streamCollapsed.value;
  if (collapsed) {
    streamSurfaceFocusedBeforeCollapse = document.activeElement === streamSurface.value;
    // Collapsing a live stream is a presentation change. Release all held
    // input and stop the gamepad poller while the surface is unavailable, but
    // leave the media elements and WebRTC session mounted and running.
    releaseForwardedInput();
    stopGamepadCapture();
    if (document.activeElement === streamSurface.value) streamSurface.value?.blur();
  }
  streamCollapsed.value = collapsed;

  if (!collapsed && streamSurfaceFocusedBeforeCollapse && isConnected.value) {
    window.requestAnimationFrame(() => {
      if (!streamCollapsed.value && inputReady.value) {
        try {
          streamSurface.value?.focus({ preventScroll: true });
        } catch {
          streamSurface.value?.focus();
        }
      }
    });
  }
}

function requestManualFullscreen(): void {
  if (fullscreenActive.value) return;
  // Keep the request in the button's trusted activation. Vue will apply the
  // v-show update after this handler, while the element is still requestable.
  revealStreamSurfaceForFullscreen();
  void enterFullscreen();
}

function revealStreamSurfaceForFullscreen(): void {
  streamCollapsed.value = false;
  // v-show applies its display update after the current event handler. Clear
  // the previous hidden inline style now so a manual fullscreen request made
  // from the collapsed state still sees a rendered element.
  if (streamSurface.value) streamSurface.value.style.display = '';
}

async function enterFullscreen(): Promise<void> {
  const surface = streamSurface.value;
  const video = videoEl.value;
  if (!surface) return;

  if (await requestElementFullscreen(surface)) {
    surface.focus();
    return;
  }
  if (video && (await requestElementFullscreen(video))) return;
  if (video && enterNativeVideoFullscreen(video)) return;

  enterPseudoFullscreen();
  surface.focus();
}

async function requestElementFullscreen(element: HTMLElement): Promise<boolean> {
  try {
    if (typeof element.requestFullscreen === 'function') {
      await element.requestFullscreen({ keyboardLock: 'browser' } as FullscreenOptions);
      void requestFullscreenKeyboardLock();
      return true;
    }
  } catch {
    // Try WebKit's prefixed API before falling back to video fullscreen.
  }

  const webkitElement = element as WebKitFullscreenElement;
  const request = webkitElement.webkitRequestFullscreen ?? webkitElement.webkitRequestFullScreen;
  if (typeof request !== 'function') return false;
  try {
    await request.call(webkitElement);
    return true;
  } catch {
    return false;
  }
}

function currentFullscreenElement(): Element | null {
  const webkitDocument = document as Document & { webkitFullscreenElement?: Element | null };
  return document.fullscreenElement ?? webkitDocument.webkitFullscreenElement ?? null;
}

async function requestFullscreenKeyboardLock(): Promise<void> {
  const keyboard = (navigator as KeyboardLockNavigator).keyboard;
  if (typeof keyboard?.lock !== 'function' || !window.isSecureContext) return;
  const request = ++fullscreenKeyboardLockRequest;
  try {
    await keyboard.lock();
    if (request !== fullscreenKeyboardLockRequest || !currentFullscreenElement()) {
      keyboard.unlock?.();
    }
  } catch {
    // Safari's fullscreen keyboardLock option remains the primary lock path.
  }
}

function releaseFullscreenKeyboardLock(): void {
  fullscreenKeyboardLockRequest += 1;
  try {
    (navigator as KeyboardLockNavigator).keyboard?.unlock?.();
  } catch {
    // Browsers also release keyboard lock automatically when fullscreen ends.
  }
}

function onFullscreenChange(): void {
  nativeFullscreen.value = Boolean(currentFullscreenElement());
  if (
    nativeFullscreen.value &&
    ['idle', 'failed', 'disconnected', 'closed'].includes(String(connectionState.value))
  ) {
    void exitFullscreen();
    return;
  }
  if (nativeFullscreen.value) {
    void requestFullscreenKeyboardLock();
  } else {
    cancelFullscreenExitHold(false);
    finishFullscreenExitSwipe();
    releaseFullscreenKeyboardLock();
  }
}

function onNativeVideoFullscreenBegin(): void {
  nativeVideoFullscreen.value = true;
  if (['idle', 'failed', 'disconnected', 'closed'].includes(String(connectionState.value))) {
    void exitFullscreen();
  }
}

function onNativeVideoFullscreenEnd(): void {
  nativeVideoFullscreen.value = false;
  cancelFullscreenExitHold(false);
  finishFullscreenExitSwipe();
}

function enterNativeVideoFullscreen(video: HTMLVideoElement): boolean {
  const webkitVideo = video as WebKitFullscreenVideoElement;
  const enter = webkitVideo.webkitEnterFullscreen ?? webkitVideo.webkitEnterFullScreen;
  if (typeof enter !== 'function') return false;
  try {
    enter.call(webkitVideo);
    return true;
  } catch {
    return false;
  }
}

function enterPseudoFullscreen(): void {
  if (pseudoFullscreen.value) return;
  pageOverflowBeforePseudoFullscreen = {
    body: document.body.style.overflow,
    root: document.documentElement.style.overflow,
  };
  document.body.style.overflow = 'hidden';
  document.documentElement.style.overflow = 'hidden';
  pseudoFullscreen.value = true;
}

function exitPseudoFullscreen(): void {
  if (!pseudoFullscreen.value) return;
  pseudoFullscreen.value = false;
  document.body.style.overflow = pageOverflowBeforePseudoFullscreen?.body ?? '';
  document.documentElement.style.overflow = pageOverflowBeforePseudoFullscreen?.root ?? '';
  pageOverflowBeforePseudoFullscreen = null;
}

async function exitFullscreen(): Promise<void> {
  releaseFullscreenKeyboardLock();

  if (currentFullscreenElement()) {
    const webkitDocument = document as WebKitFullscreenDocument;
    const exits = [
      document.exitFullscreen,
      webkitDocument.webkitExitFullscreen,
      webkitDocument.webkitCancelFullScreen,
    ];
    for (const exit of exits) {
      if (typeof exit !== 'function') continue;
      try {
        await exit.call(document);
        return;
      } catch {
        // Try the next browser-specific exit API.
      }
    }
  }

  const webkitVideo = videoEl.value as WebKitFullscreenVideoElement | null;
  if (webkitVideo?.webkitDisplayingFullscreen && webkitVideo.webkitExitFullscreen) {
    try {
      webkitVideo.webkitExitFullscreen();
      return;
    } catch {
      // The compatibility fallback may still be active.
    }
  }

  exitPseudoFullscreen();
}

watch(inputForwarding, (enabled, wasEnabled) => {
  if (!enabled && wasEnabled) releaseForwardedInput();
  syncGamepadCapture();
});

watch(inputReady, syncGamepadCapture);
watch(
  () => form.fps,
  () => {
    form.videoMaxFrameAgeFrames = clampFrameAgeFrames(
      form.videoMaxFrameAgeFrames,
      form.fps,
      form.videoPacingMode,
    );
  },
);
watch([form, autoFullscreen], persistForm, { deep: true });

onMounted(() => {
  loadSavedForm();
  standaloneWebApp.value = runningAsStandaloneWebApp();
  void refresh();
  startSessionStatusPolling();
  window.addEventListener('blur', onWindowBlur);
  window.addEventListener('focus', onWindowFocus);
  document.addEventListener('fullscreenchange', onFullscreenChange);
  document.addEventListener('webkitfullscreenchange', onFullscreenChange as EventListener);
  document.addEventListener('visibilitychange', onVisibilityChange);
  videoEl.value?.addEventListener('webkitbeginfullscreen', onNativeVideoFullscreenBegin);
  videoEl.value?.addEventListener('webkitendfullscreen', onNativeVideoFullscreenEnd);
});
onBeforeUnmount(() => {
  stopSessionStatusPolling();
  cancelFullscreenExitHold();
  finishFullscreenExitSwipe();
  exitPseudoFullscreen();
  releaseFullscreenKeyboardLock();
  window.removeEventListener('blur', onWindowBlur);
  window.removeEventListener('focus', onWindowFocus);
  document.removeEventListener('fullscreenchange', onFullscreenChange);
  document.removeEventListener('webkitfullscreenchange', onFullscreenChange as EventListener);
  document.removeEventListener('visibilitychange', onVisibilityChange);
  videoEl.value?.removeEventListener('webkitbeginfullscreen', onNativeVideoFullscreenBegin);
  videoEl.value?.removeEventListener('webkitendfullscreen', onNativeVideoFullscreenEnd);
  releaseForwardedInput();
  stopVideoFrameLatencyMonitoring();
  void disconnect(false);
});
</script>

<template>
  <div class="page page--wide browser-stream-page">
    <PageHeader
      :title="t('ui.browser_stream.title')"
      :description="t('ui.browser_stream.description')"
    >
      <template #actions>
        <AppButton
          icon="refresh"
          :label="t('_common.refresh')"
          variant="secondary"
          :busy="loading"
          :busy-label="t('ui.browser_stream.refreshing')"
          @click="refresh"
        />
      </template>
    </PageHeader>

    <InlineAlert
      v-if="refreshError"
      tone="warning"
      :title="t('ui.browser_stream.errors.refresh')"
      announce="polite"
    >
      {{ refreshError }}
    </InlineAlert>

    <InlineAlert
      v-if="!loading && !hostReady"
      tone="warning"
      :title="t('ui.browser_stream.host_not_ready')"
    >
      {{ hostCapabilities.availability.reason || t('ui.browser_stream.reasons.host_unverified') }}
    </InlineAlert>

    <InlineAlert
      v-if="streamError"
      tone="danger"
      :title="t('ui.browser_stream.errors.connect')"
      announce="assertive"
    >
      {{ streamError }}
    </InlineAlert>

    <InlineAlert
      v-if="sessionActionError"
      tone="danger"
      :title="t('webrtc.termination_failed')"
      announce="assertive"
    >
      {{ sessionActionError }}
    </InlineAlert>

    <InlineAlert
      v-if="playbackBlocked"
      tone="warning"
      :title="t('ui.browser_stream.errors.playback')"
      announce="polite"
    >
      {{ t('ui.browser_stream.errors.playback_detail') }}
      <template #actions>
        <AppButton
          icon="play"
          :label="t('ui.browser_stream.resume_playback')"
          variant="secondary"
          @click="resumePlayback"
        />
      </template>
    </InlineAlert>

    <section class="app-picker panel" aria-labelledby="browser-stream-app-picker-title">
      <div class="panel__heading app-picker__heading">
        <div>
          <h2 id="browser-stream-app-picker-title">
            {{ t('ui.browser_stream.settings.application') }}
          </h2>
          <p>{{ t('ui.browser_stream.settings.application_help') }}</p>
        </div>
        <label v-if="launchableApps.length" class="app-picker__search">
          <span class="vs-sr-only">{{ t('webrtc.search_placeholder') }}</span>
          <UiIcon name="search" :size="17" />
          <input
            v-model="appSearch"
            class="vs-input"
            type="search"
            :placeholder="t('webrtc.search_placeholder')"
            :disabled="connectionPending || isConnected"
          />
        </label>
      </div>

      <LoadingSkeleton v-if="loading" variant="block" height="250px" aria-hidden="true" />
      <template v-else>
        <div
          class="app-picker__grid"
          role="listbox"
          :aria-label="t('ui.browser_stream.settings.application')"
        >
          <button
            class="app-picker__card app-picker__card--desktop"
            :class="{ 'app-picker__card--selected': appSelected() }"
            type="button"
            role="option"
            :aria-selected="appSelected()"
            :disabled="connectionPending || isConnected"
            @click="selectApp()"
          >
            <span class="app-picker__artwork app-picker__artwork--desktop">
              <UiIcon name="devices" :size="42" />
              <span v-if="appSelected()" class="app-picker__selected-mark">
                <UiIcon name="check" :size="16" />
              </span>
            </span>
            <span class="app-picker__copy">
              <strong>{{ t('ui.browser_stream.desktop') }}</strong>
              <small>
                {{
                  resumeAvailable
                    ? t('webrtc.no_selection')
                    : t('ui.browser_stream.picker.desktop_detail')
                }}
              </small>
            </span>
          </button>

          <button
            v-for="app in filteredLaunchableApps"
            :key="app.id"
            class="app-picker__card"
            :class="{ 'app-picker__card--selected': appSelected(app.id) }"
            type="button"
            role="option"
            :aria-selected="appSelected(app.id)"
            :aria-label="app.name"
            :disabled="connectionPending || isConnected"
            @click="selectApp(app.id)"
          >
            <span class="app-picker__artwork">
              <img
                v-if="app.coverUrl && !appCoverFailed(app.id)"
                :src="app.coverUrl"
                :alt="t('ui.browser_stream.picker.cover_alt', { name: app.name })"
                loading="lazy"
                @error="markAppCoverFailed(app.id)"
              />
              <span v-else class="app-picker__artwork-fallback" aria-hidden="true">
                <UiIcon name="gamepad" :size="34" />
              </span>
              <span v-if="appSelected(app.id)" class="app-picker__selected-mark">
                <UiIcon name="check" :size="16" />
              </span>
            </span>
            <span class="app-picker__copy">
              <strong>{{ app.name }}</strong>
            </span>
          </button>
        </div>

        <p v-if="!launchableApps.length" class="app-picker__empty">
          {{ t('webrtc.no_applications_hint') }}
        </p>
        <p v-else-if="!filteredLaunchableApps.length" class="app-picker__empty">
          {{ t('webrtc.no_applications_match', { query: appSearch.trim() }) }}
        </p>
      </template>
    </section>

    <section class="stream-stage panel" aria-labelledby="browser-stream-stage-title">
      <div class="panel__heading stream-stage__heading">
        <div>
          <h2 id="browser-stream-stage-title">{{ selectedAppName }}</h2>
          <p>{{ t('ui.browser_stream.stage_description') }}</p>
        </div>
        <div class="stream-stage__heading-actions">
          <StatusBadge :label="connectionLabel" :tone="connectionTone" announce="polite" />
          <AppButton
            v-if="!fullscreenActive"
            icon="chevron-down"
            :label="
              streamCollapsed ? t('ui.browser_stream.expand') : t('ui.browser_stream.collapse')
            "
            variant="tertiary"
            :aria-expanded="!streamCollapsed"
            aria-controls="browser-stream-surface"
            @click="toggleStreamCollapsed"
          />
        </div>
      </div>

      <div
        ref="streamSurface"
        id="browser-stream-surface"
        class="stream-surface"
        v-show="!streamCollapsed"
        :class="{
          'stream-surface--interactive': inputReady,
          'stream-surface--pseudo-fullscreen': pseudoFullscreen,
          'stream-surface--touch-exit': showFullscreenSwipeExit,
        }"
        :tabindex="streamCollapsed ? -1 : 0"
        :aria-hidden="streamCollapsed ? 'true' : undefined"
        :aria-label="t('ui.browser_stream.stream_surface')"
        @keydown="sendKey($event, 'key_down')"
        @keyup="sendKey($event, 'key_up')"
        @pointerdown="sendPointerButton($event, 'mouse_down')"
        @pointermove="sendPointerMove"
        @pointerup="sendPointerButton($event, 'mouse_up')"
        @pointercancel="releaseForwardedInput"
        @lostpointercapture="releaseForwardedInput"
        @blur="releaseForwardedInput"
        @wheel="sendWheel"
      >
        <video ref="videoEl" autoplay muted playsinline disablepictureinpicture />
        <audio ref="audioEl" autoplay hidden />
        <div
          v-if="showPerformanceOverlay && isConnected"
          class="stream-performance-overlay"
          :aria-label="t('webrtc.show_performance_overlay')"
          aria-live="off"
        >
          <div v-for="line in overlayLines" :key="line" class="stream-performance-overlay__line">
            {{ line }}
          </div>
        </div>
        <div
          v-if="showFullscreenSwipeExit"
          class="stream-surface__exit-swipe"
          aria-hidden="true"
          @click.stop
          @keydown.stop
          @keyup.stop
          @lostpointercapture="finishFullscreenExitSwipe"
          @pointercancel="finishFullscreenExitSwipe"
          @pointerdown="startFullscreenExitSwipe"
          @pointermove="updateFullscreenExitSwipe"
          @pointerup="finishFullscreenExitSwipe"
        >
          <span aria-hidden="true" />
          {{ t('ui.browser_stream.exit_fullscreen_swipe_hint') }}
        </div>
        <div
          v-if="fullscreenActive"
          class="stream-surface__exit-fullscreen"
          @click.stop
          @keydown.stop
          @keyup.stop
          @pointercancel.stop
          @pointerdown.stop
          @pointermove.stop
          @pointerup.stop
        >
          <label class="stream-surface__overlay-toggle">
            <input v-model="showPerformanceOverlay" type="checkbox" />
            <span>{{ t('webrtc.show_performance_overlay') }}</span>
          </label>
          <AppButton
            icon="x"
            :label="fullscreenExitControlLabel"
            variant="secondary"
            @click="exitFullscreen"
          />
        </div>
        <div v-if="!isConnected && !connectionPending" class="stream-surface__empty">
          <span class="stream-surface__empty-icon" aria-hidden="true"
            ><UiIcon name="play" :size="28"
          /></span>
          <strong>{{ t('ui.browser_stream.ready_to_start') }}</strong>
          <span>{{ t('ui.browser_stream.ready_to_start_detail') }}</span>
        </div>
        <div v-else-if="connectionPending" class="stream-surface__empty">
          <span class="stream-surface__spinner" aria-hidden="true" />
          <strong>{{ t('ui.browser_stream.status.connecting') }}</strong>
          <span>{{ t('ui.browser_stream.connecting_detail') }}</span>
        </div>
      </div>

      <div class="stream-stage__actions">
        <AppButton
          v-if="connectionPending"
          icon="stop"
          :label="t('ui.browser_stream.cancel')"
          variant="secondary"
          @click="disconnect()"
        />
        <AppButton
          v-else-if="!isConnected"
          icon="play"
          :label="primaryActionLabel"
          variant="primary"
          :disabled="startDisabled"
          @click="requestPrimaryAction"
        />
        <AppButton
          v-else
          icon="stop"
          :label="t('ui.browser_stream.disconnect')"
          variant="danger"
          @click="disconnect()"
        />
        <AppButton
          v-if="!isConnected && !connectionPending && hasRunningSession"
          icon="stop"
          :label="t('webrtc.terminate')"
          variant="danger"
          :disabled="sessionActionPending"
          @click="requestTerminate"
        />
        <AppButton
          icon="external-link"
          :label="t('ui.browser_stream.fullscreen')"
          variant="secondary"
          :disabled="!isConnected"
          @click="requestManualFullscreen"
        />
        <AppButton
          v-if="showInstallWebAppAction"
          icon="help"
          :label="t('ui.browser_stream.install.action')"
          variant="secondary"
          @click="installHelpOpen = true"
        />
        <span class="stream-stage__input-status" :data-ready="inputReady">
          <UiIcon :name="inputReady ? 'check-circle' : 'info'" :size="16" />
          {{ inputStatusLabel }}
        </span>
      </div>
    </section>

    <div v-if="loading" class="browser-stream-loading" aria-hidden="true">
      <LoadingSkeleton variant="block" height="310px" />
      <LoadingSkeleton variant="block" height="310px" />
    </div>

    <div v-else class="browser-stream-grid">
      <section class="panel" aria-labelledby="browser-stream-settings-title">
        <div class="panel__heading">
          <div>
            <h2 id="browser-stream-settings-title">{{ t('ui.browser_stream.settings.title') }}</h2>
            <p>{{ t('ui.browser_stream.settings.description') }}</p>
          </div>
        </div>

        <form class="stream-form" @submit.prevent="requestPrimaryAction">
          <fieldset class="stream-form__group" :disabled="connectionPending || isConnected">
            <legend>{{ t('ui.browser_stream.settings.video') }}</legend>
            <label class="vs-field" for="browser-stream-codec">
              <span class="vs-field__label">{{ t('ui.browser_stream.settings.codec') }}</span>
              <select
                id="browser-stream-codec"
                v-model="form.encoding"
                class="vs-select"
                @change="onEncodingChanged"
              >
                <option
                  v-for="codec in codecs"
                  :key="codec"
                  :value="codec"
                  :disabled="!codecAvailable(codec)"
                >
                  {{ codecLabel(codec)
                  }}{{ codecAvailable(codec) ? '' : ` - ${codecUnavailableReason(codec)}` }}
                </option>
              </select>
            </label>

            <label
              class="stream-form__check"
              :class="{ 'stream-form__check--disabled': hdrControlDisabled }"
            >
              <input
                :checked="effectiveHdr"
                type="checkbox"
                :disabled="hdrControlDisabled"
                @change="setHdr"
              />
              <span>
                <strong>{{ t('ui.browser_stream.settings.hdr') }}</strong>
                <small>{{ hdrControlDescription }}</small>
              </span>
            </label>

            <div class="stream-form__numeric-grid">
              <label class="vs-field" for="browser-stream-width">
                <span class="vs-field__label">{{ t('ui.browser_stream.settings.width') }}</span>
                <input
                  id="browser-stream-width"
                  v-model.number="form.width"
                  class="vs-input"
                  type="number"
                  :min="hostCapabilities.limits.min_dimension"
                  :max="hostCapabilities.limits.max_dimension"
                  step="2"
                />
              </label>
              <label class="vs-field" for="browser-stream-height">
                <span class="vs-field__label">{{ t('ui.browser_stream.settings.height') }}</span>
                <input
                  id="browser-stream-height"
                  v-model.number="form.height"
                  class="vs-input"
                  type="number"
                  :min="hostCapabilities.limits.min_dimension"
                  :max="hostCapabilities.limits.max_dimension"
                  step="2"
                />
              </label>
              <label class="vs-field" for="browser-stream-fps">
                <span class="vs-field__label">{{ t('ui.browser_stream.settings.fps') }}</span>
                <input
                  id="browser-stream-fps"
                  v-model.number="form.fps"
                  class="vs-input"
                  type="number"
                  :min="hostCapabilities.limits.min_fps"
                  :max="hostCapabilities.limits.max_fps"
                  step="1"
                />
              </label>
              <label class="vs-field" for="browser-stream-bitrate">
                <span class="vs-field__label">{{ t('ui.browser_stream.settings.bitrate') }}</span>
                <input
                  id="browser-stream-bitrate"
                  v-model.number="form.bitrateKbps"
                  class="vs-input"
                  type="number"
                  :min="hostCapabilities.limits.min_bitrate_kbps"
                  :max="hostCapabilities.limits.max_bitrate_kbps"
                  step="1000"
                />
              </label>
            </div>

            <div class="stream-pacing">
              <div class="stream-pacing__heading">
                <span class="vs-field__label">{{ t('webrtc.frame_pacing') }}</span>
                <small>{{ t('webrtc.frame_pacing_desc') }}</small>
              </div>
              <div
                class="stream-pacing__presets"
                role="group"
                :aria-label="t('webrtc.frame_pacing')"
              >
                <button
                  v-for="option in pacingOptions"
                  :key="option.value"
                  class="stream-pacing__preset"
                  :class="{
                    'stream-pacing__preset--selected': form.videoPacingMode === option.value,
                  }"
                  type="button"
                  :aria-pressed="form.videoPacingMode === option.value"
                  @click="applyPacingPreset(option.value)"
                >
                  {{ option.label }}
                </button>
              </div>
              <div class="stream-form__numeric-grid stream-pacing__values">
                <label class="vs-field" for="browser-stream-pacing-slack">
                  <span class="vs-field__label">{{ t('webrtc.frame_pacing_slack') }}</span>
                  <input
                    id="browser-stream-pacing-slack"
                    v-model.number="form.videoPacingSlackMs"
                    class="vs-input"
                    type="number"
                    min="0"
                    max="10"
                    step="1"
                  />
                  <span class="vs-field__help">0–10 ms</span>
                </label>
                <label class="vs-field" for="browser-stream-max-frame-age">
                  <span class="vs-field__label">{{ t('webrtc.frame_pacing_max_delay') }}</span>
                  <input
                    id="browser-stream-max-frame-age"
                    v-model.number="maxFrameAgeFrames"
                    class="vs-input"
                    type="number"
                    min="1"
                    :max="maxAllowedFramesForFps(form.fps)"
                    step="1"
                  />
                  <span class="vs-field__help">
                    {{
                      t('ui.browser_stream.settings.frame_age_help', {
                        milliseconds: maxFrameAgeMsLabel,
                        fps: form.fps,
                      })
                    }}
                  </span>
                </label>
              </div>
            </div>
          </fieldset>

          <label
            class="stream-form__check"
            :class="{ 'stream-form__check--disabled': connectionPending || isConnected }"
          >
            <input
              v-model="form.muteHostAudio"
              type="checkbox"
              :disabled="connectionPending || isConnected"
            />
            <span>
              <strong>{{ t('ui.browser_stream.settings.mute_host_audio') }}</strong>
              <small>{{ t('ui.browser_stream.settings.mute_host_audio_help') }}</small>
            </span>
          </label>

          <p v-if="validationError" class="stream-form__validation" role="status">
            <UiIcon name="alert-triangle" :size="16" />
            {{ validationError }}
          </p>
        </form>
      </section>

      <section class="panel" aria-labelledby="browser-stream-controls-title">
        <div class="panel__heading">
          <div>
            <h2 id="browser-stream-controls-title">{{ t('ui.browser_stream.controls.title') }}</h2>
            <p>{{ t('ui.browser_stream.controls.description') }}</p>
          </div>
        </div>

        <label class="stream-form__check" :class="{ 'stream-form__check--disabled': !isConnected }">
          <input v-model="inputForwarding" type="checkbox" :disabled="!isConnected" />
          <span>
            <strong>{{ t('ui.browser_stream.controls.input') }}</strong>
            <small>{{ t('ui.browser_stream.controls.input_help') }}</small>
          </span>
        </label>

        <label class="stream-form__check" :class="{ 'stream-form__check--disabled': !isConnected }">
          <input v-model="showPerformanceOverlay" type="checkbox" :disabled="!isConnected" />
          <span>
            <strong>{{ t('webrtc.show_performance_overlay') }}</strong>
            <small>{{ t('ui.browser_stream.controls.performance_overlay_help') }}</small>
          </span>
        </label>

        <label class="stream-form__check">
          <input v-model="autoFullscreen" type="checkbox" />
          <span>
            <strong>{{ t('webrtc.auto_fullscreen') }}</strong>
            <small>{{ t('webrtc.auto_fullscreen_desc') }}</small>
          </span>
        </label>

        <div class="browser-capabilities" aria-labelledby="browser-stream-capabilities-title">
          <h3 id="browser-stream-capabilities-title">
            {{ t('ui.browser_stream.capabilities.title') }}
          </h3>
          <ul>
            <li v-for="codec in codecs" :key="codec">
              <span>{{ codecLabel(codec) }}</span>
              <StatusBadge
                :label="
                  codecAvailable(codec)
                    ? t('ui.browser_stream.capabilities.available')
                    : t('ui.browser_stream.capabilities.unavailable')
                "
                :tone="codecAvailable(codec) ? 'success' : 'neutral'"
                compact
              />
              <small
                v-if="
                  codecAvailable(codec) &&
                  hostCapabilities.codecs[codec].hdr &&
                  browserCapabilities[codec].hdr
                "
              >
                {{ t('ui.browser_stream.capabilities.hdr_ready') }}
              </small>
              <small v-else>
                {{
                  codecAvailable(codec)
                    ? t('ui.browser_stream.capabilities.sdr_only')
                    : codecUnavailableReason(codec)
                }}
              </small>
            </li>
          </ul>
        </div>
      </section>
    </div>

    <ConfirmDialog
      v-model:open="installHelpOpen"
      :title="t('ui.browser_stream.install.title')"
      :description="t('ui.browser_stream.install.description')"
      :confirm-label="t('ui.browser_stream.install.done')"
      :cancel-label="t('_common.cancel')"
      initial-focus="confirm"
    />

    <ConfirmDialog
      v-model:open="terminateOpen"
      :title="t('webrtc.terminate_confirm_title')"
      :description="terminateDescription"
      :confirm-label="terminateConfirmLabel"
      :cancel-label="t('_common.cancel')"
      tone="danger"
      :busy="sessionActionPending"
      :busy-label="terminateConfirmLabel"
      :close-on-confirm="false"
      @confirm="confirmTerminate"
    />
  </div>
</template>

<style scoped>
.browser-stream-page {
  display: grid;
  gap: var(--vs-space-24);
}

.app-picker {
  display: grid;
  gap: var(--vs-space-16);
}

.app-picker__heading {
  align-items: end;
  margin-bottom: 0;
}

.app-picker__search {
  position: relative;
  display: flex;
  min-width: min(22rem, 100%);
  align-items: center;
}

.app-picker__search > .vs-icon {
  position: absolute;
  left: var(--vs-space-12);
  z-index: 1;
  color: var(--vs-color-text-muted);
  pointer-events: none;
}

.app-picker__search .vs-input {
  width: 100%;
  padding-left: 2.35rem;
}

.app-picker__grid {
  display: grid;
  max-height: 34rem;
  grid-template-columns: repeat(auto-fill, minmax(8.5rem, 1fr));
  gap: var(--vs-space-12);
  padding: var(--vs-space-2);
  overflow-y: auto;
}

.app-picker__card {
  display: grid;
  min-width: 0;
  align-content: start;
  padding: 0;
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-primary);
  text-align: left;
  cursor: pointer;
  transition:
    border-color var(--vs-motion-duration-control) var(--vs-motion-easing-standard),
    transform var(--vs-motion-duration-control) var(--vs-motion-easing-standard);
}

.app-picker__card:hover:not(:disabled),
.app-picker__card:focus-visible,
.app-picker__card--selected {
  border-color: var(--vs-color-accent-default);
}

.app-picker__card:hover:not(:disabled) {
  transform: translateY(-2px);
}

.app-picker__card--selected {
  box-shadow: inset 0 0 0 var(--vs-border-width) var(--vs-color-accent-default);
}

.app-picker__card:disabled {
  cursor: not-allowed;
  opacity: 0.66;
}

.app-picker__artwork {
  position: relative;
  display: grid;
  aspect-ratio: 2 / 3;
  overflow: hidden;
  place-items: stretch;
  background: var(--vs-color-bg-subtle);
}

.app-picker__artwork img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}

.app-picker__artwork--desktop,
.app-picker__artwork-fallback {
  place-items: center;
  color: var(--vs-color-text-muted);
}

.app-picker__artwork--desktop {
  background:
    radial-gradient(
      circle at 50% 30%,
      color-mix(in srgb, var(--vs-color-accent-default) 24%, transparent),
      transparent 55%
    ),
    var(--vs-color-bg-subtle);
}

.app-picker__selected-mark {
  position: absolute;
  top: var(--vs-space-8);
  right: var(--vs-space-8);
  display: grid;
  width: 1.8rem;
  height: 1.8rem;
  place-items: center;
  border-radius: var(--vs-radius-pill);
  background: var(--vs-color-accent-default);
  color: var(--vs-color-text-on-accent);
  box-shadow: 0 0 0 2px var(--vs-color-bg-surface);
}

.app-picker__copy {
  display: grid;
  gap: var(--vs-space-2);
  padding: var(--vs-space-12);
}

.app-picker__copy strong {
  overflow: hidden;
  font-size: var(--vs-type-size-control);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.app-picker__copy small,
.app-picker__empty {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.app-picker__empty {
  margin: 0;
}

.stream-stage {
  display: grid;
  gap: var(--vs-space-16);
}

.stream-stage__heading {
  margin-bottom: 0;
}

.stream-stage__heading-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

.stream-stage__heading-actions [aria-controls='browser-stream-surface'] .vs-button__icon {
  transition: transform 160ms ease;
}

.stream-stage__heading-actions
  [aria-controls='browser-stream-surface'][aria-expanded='true']
  .vs-button__icon {
  transform: rotate(180deg);
}

.stream-surface {
  position: relative;
  display: grid;
  min-height: min(60vw, 42rem);
  overflow: hidden;
  place-items: center;
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-card);
  outline: none;
  background: #090b10;
}

.stream-surface:focus-visible {
  box-shadow: 0 0 0 3px color-mix(in srgb, var(--vs-color-accent-default) 44%, transparent);
}

.stream-surface--interactive {
  cursor: none;
  overscroll-behavior: none;
  touch-action: none;
  -webkit-touch-callout: none;
  user-select: none;
}

.stream-surface video {
  display: block;
  width: 100%;
  height: 100%;
  max-height: 42rem;
  object-fit: contain;
}

.stream-performance-overlay {
  position: absolute;
  z-index: 1;
  top: var(--vs-space-12);
  left: var(--vs-space-12);
  max-width: calc(100% - var(--vs-space-24));
  padding: var(--vs-space-8) var(--vs-space-12);
  border: 1px solid rgb(255 255 255 / 0.16);
  border-radius: var(--vs-radius-control);
  background: rgb(0 0 0 / 0.72);
  box-shadow: 0 0.25rem 1rem rgb(0 0 0 / 0.22);
  color: rgb(255 255 255 / 0.9);
  pointer-events: none;
  backdrop-filter: blur(8px);
}

/* Keep Safari's top-edge fullscreen exit affordance visible on touch devices. */
.stream-surface--touch-exit .stream-performance-overlay {
  top: max(4.75rem, calc(env(safe-area-inset-top) + 3.25rem));
}

.stream-performance-overlay__line {
  overflow-wrap: anywhere;
  font-family: ui-monospace, SFMono-Regular, Consolas, monospace;
  font-size: 0.72rem;
  line-height: 1.55;
}

.stream-surface:fullscreen,
.stream-surface:-webkit-full-screen,
.stream-surface--pseudo-fullscreen {
  width: 100vw;
  height: 100vh;
  height: 100dvh;
  min-height: 0;
  border: 0;
  border-radius: 0;
  background: #000;
}

.stream-surface--pseudo-fullscreen {
  position: fixed;
  z-index: 10000;
  width: 100lvw;
  height: 100lvh;
  inset: 0;
}

.stream-surface:fullscreen video,
.stream-surface:-webkit-full-screen video,
.stream-surface--pseudo-fullscreen video,
.stream-surface video:fullscreen,
.stream-surface video:-webkit-full-screen {
  width: 100%;
  height: 100%;
  max-height: none;
  object-fit: contain;
}

.stream-surface__empty {
  position: absolute;
  inset: 0;
  display: grid;
  align-content: center;
  justify-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-24);
  color: rgb(255 255 255 / 0.82);
  text-align: center;
}

.stream-surface__exit-fullscreen {
  position: absolute;
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
  z-index: 2;
  right: max(var(--vs-space-16), env(safe-area-inset-right));
  bottom: max(var(--vs-space-16), env(safe-area-inset-bottom));
  cursor: default;
}

.stream-surface__overlay-toggle {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  min-height: 2.25rem;
  padding: 0 var(--vs-space-8);
  border: 1px solid rgb(255 255 255 / 0.18);
  border-radius: var(--vs-radius-control);
  background: rgb(10 12 18 / 0.68);
  color: rgb(255 255 255 / 0.88);
  cursor: pointer;
  font-size: var(--vs-type-size-helper);
  touch-action: manipulation;
  backdrop-filter: blur(10px);
}

.stream-surface__overlay-toggle input {
  width: 1rem;
  height: 1rem;
  margin: 0;
  accent-color: var(--vs-color-accent-default);
}

.stream-surface__exit-swipe {
  position: absolute;
  z-index: 2;
  top: max(var(--vs-space-12), env(safe-area-inset-top));
  left: 50%;
  display: grid;
  width: min(13rem, 50vw);
  min-height: 2.75rem;
  padding: var(--vs-space-8) var(--vs-space-12);
  transform: translateX(-50%);
  place-items: center;
  gap: var(--vs-space-4);
  border: 1px solid rgb(255 255 255 / 0.18);
  border-radius: 999px;
  background: rgb(10 12 18 / 0.68);
  color: rgb(255 255 255 / 0.82);
  font-size: var(--vs-type-size-helper);
  cursor: default;
  touch-action: none;
  backdrop-filter: blur(10px);
}

.stream-surface__exit-swipe span {
  width: 2.25rem;
  height: 0.2rem;
  border-radius: 999px;
  background: rgb(255 255 255 / 0.72);
}

.stream-surface__empty span:not(.stream-surface__empty-icon):not(.stream-surface__spinner) {
  max-width: 28rem;
  color: rgb(255 255 255 / 0.66);
}

.stream-surface__empty-icon {
  display: grid;
  width: 3.25rem;
  height: 3.25rem;
  place-items: center;
  border: 1px solid rgb(255 255 255 / 0.26);
  border-radius: 50%;
  background: rgb(255 255 255 / 0.1);
}

.stream-surface__spinner {
  width: 2rem;
  height: 2rem;
  border: 2px solid rgb(255 255 255 / 0.28);
  border-right-color: rgb(255 255 255 / 0.9);
  border-radius: 50%;
  animation: stream-spin 0.8s linear infinite;
}

.stream-stage__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.stream-stage__input-status {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  margin-inline-start: auto;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.stream-stage__input-status[data-ready='true'] {
  color: var(--vs-color-status-success);
}

.browser-stream-loading,
.browser-stream-grid {
  display: grid;
  grid-template-columns: minmax(0, 1.25fr) minmax(18rem, 0.75fr);
  gap: var(--vs-space-24);
}

.stream-form,
.stream-form__group {
  display: grid;
  gap: var(--vs-space-16);
}

.stream-form__group {
  padding: var(--vs-space-16);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
}

.stream-form__group legend {
  padding-inline: var(--vs-space-4);
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.stream-form__numeric-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.stream-form__check {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-12);
  padding: var(--vs-space-12);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  cursor: pointer;
}

.stream-form__check input {
  width: 1rem;
  height: 1rem;
  margin-top: 0.15rem;
  accent-color: var(--vs-color-accent-default);
}

.stream-form__check span {
  display: grid;
  gap: var(--vs-space-2);
}

.stream-form__check strong {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
}

.stream-form__check small,
.vs-field__help,
.browser-capabilities small {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: 1.4;
}

.stream-form__check--disabled {
  cursor: not-allowed;
  opacity: 0.68;
}

.stream-form__validation {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-8);
  margin: 0;
  color: var(--vs-color-status-warning);
  font-size: var(--vs-type-size-helper);
  line-height: 1.4;
}

.stream-pacing {
  display: grid;
  gap: var(--vs-space-12);
  padding-top: var(--vs-space-4);
  border-top: 1px solid var(--vs-color-border-subtle);
}

.stream-pacing__heading {
  display: grid;
  gap: var(--vs-space-4);
}

.stream-pacing__heading small {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: 1.4;
}

.stream-pacing__presets {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.stream-pacing__preset {
  min-height: 2.25rem;
  padding: 0 var(--vs-space-12);
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-secondary);
  cursor: pointer;
  font: inherit;
}

.stream-pacing__preset:hover,
.stream-pacing__preset:focus-visible,
.stream-pacing__preset--selected {
  border-color: var(--vs-color-accent-default);
  color: var(--vs-color-text-primary);
}

.stream-pacing__preset--selected {
  background: color-mix(in srgb, var(--vs-color-accent-default) 14%, transparent);
}

.stream-pacing__values {
  gap: var(--vs-space-12);
}

.browser-capabilities {
  margin-top: var(--vs-space-24);
}

.browser-capabilities h3 {
  margin: 0 0 var(--vs-space-12);
  font-size: var(--vs-type-size-control);
}

.browser-capabilities ul {
  display: grid;
  gap: var(--vs-space-8);
  padding: 0;
  margin: 0;
  list-style: none;
}

.browser-capabilities li {
  display: grid;
  grid-template-columns: minmax(4.5rem, auto) auto minmax(0, 1fr);
  align-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) 0;
  border-top: 1px solid var(--vs-color-border-subtle);
}

@keyframes stream-spin {
  to {
    transform: rotate(1turn);
  }
}

@media (max-width: 1023px) {
  .browser-stream-loading,
  .browser-stream-grid {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 767px) {
  .app-picker__heading {
    align-items: stretch;
  }

  .app-picker__search {
    min-width: 0;
  }

  .app-picker__grid {
    grid-template-columns: repeat(auto-fill, minmax(7.5rem, 1fr));
  }

  .stream-surface {
    min-height: 15rem;
  }

  .stream-stage__input-status {
    width: 100%;
    margin-inline-start: 0;
  }

  .stream-form__numeric-grid {
    grid-template-columns: 1fr;
  }

  .browser-capabilities li {
    grid-template-columns: minmax(0, 1fr) auto;
  }

  .browser-capabilities small {
    grid-column: 1 / -1;
  }
}

@media (prefers-reduced-motion: reduce) {
  .stream-surface__spinner {
    animation: none;
  }
}
</style>
