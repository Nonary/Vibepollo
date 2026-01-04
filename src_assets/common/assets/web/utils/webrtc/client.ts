import { WebRtcApi } from '@/services/webrtcApi';
import { GamepadFeedbackMessage, StreamConfig, WebRtcStatsSnapshot } from '@/types/webrtc';

export interface WebRtcClientCallbacks {
  onRemoteStream?: (stream: MediaStream) => void;
  onConnectionState?: (state: RTCPeerConnectionState) => void;
  onIceState?: (state: RTCIceConnectionState) => void;
  onInputChannelState?: (state: RTCDataChannelState) => void;
  onStats?: (stats: WebRtcStatsSnapshot) => void;
  onInputMessage?: (message: GamepadFeedbackMessage) => void;
  onNegotiatedEncoding?: (encoding: string) => void;
  onError?: (error: Error) => void;
}

export interface WebRtcClientConnectOptions {
  inputPriority?: RTCPriorityType;
}

export interface WebRtcDisconnectOptions {
  keepalive?: boolean;
}

interface StatsState {
  lastVideoInboundId?: string;
  lastAudioInboundId?: string;
  lastVideoBytes?: number;
  lastAudioBytes?: number;
  lastTimestampMs?: number;
  lastVideoJitterBufferDelay?: number;
  lastVideoJitterBufferEmittedCount?: number;
  lastAudioJitterBufferDelay?: number;
  lastAudioJitterBufferEmittedCount?: number;
  lastVideoTotalDecodeTime?: number;
  lastVideoFramesDecoded?: number;
  lastVideoFramesReceived?: number;
}

const ENCODING_MIME: Record<string, string[]> = {
  h264: ['video/h264'],
  hevc: ['video/h265', 'video/hevc'],
  av1: ['video/av1'],
};
const DEFAULT_AUDIO_JITTER_TARGET_MS = 20;
const DEFAULT_AUDIO_PLAYOUT_DELAY_MS = 20;
const RECEIVER_HINT_REFRESH_MS = 250;
const STATS_POLL_FAST_MS = 250;
const STATS_POLL_SLOW_MS = 1000;
const STATS_POLL_FAST_BOOT_MS = 10000;
const STATS_POLL_FAST_HOLD_MS = 2500;
const STATS_POLL_FAST_JITTER_THRESHOLD_MS = 60;
const ICE_CANDIDATE_BATCH_WINDOW_MS = 75;
const ICE_CANDIDATE_BATCH_LIMIT = 256;

function getVideoCodecCapabilities(): RTCRtpCapabilities | null {
  try {
    const receiverCaps =
      typeof RTCRtpReceiver !== 'undefined' ? RTCRtpReceiver.getCapabilities?.('video') : null;
    if (receiverCaps?.codecs?.length) return receiverCaps;
  } catch {
    /* ignore */
  }
  try {
    const senderCaps =
      typeof RTCRtpSender !== 'undefined' ? RTCRtpSender.getCapabilities?.('video') : null;
    if (senderCaps?.codecs?.length) return senderCaps;
  } catch {
    /* ignore */
  }
  return null;
}

function resolveEncodingPreference(encoding: string): string {
  const mimes = ENCODING_MIME[encoding.toLowerCase()];
  if (!mimes) return encoding;
  const caps = getVideoCodecCapabilities();
  if (!caps?.codecs) return encoding;
  const supported = caps.codecs.some((codec) => mimes.includes(codec.mimeType.toLowerCase()));
  return supported ? encoding : 'h264';
}

function getOfferedVideoCodecNames(sdp: string): Set<string> {
  const codecs = new Set<string>();
  if (!sdp) return codecs;
  const lines = sdp.split(/\r\n/);
  let inVideo = false;

  for (const line of lines) {
    if (line.startsWith('m=')) {
      inVideo = line.startsWith('m=video');
      continue;
    }
    if (!inVideo || !line.startsWith('a=rtpmap:')) continue;
    const rest = line.slice('a=rtpmap:'.length);
    const space = rest.indexOf(' ');
    if (space < 0) continue;
    const codecPart = rest.slice(space + 1).trim();
    if (!codecPart) continue;
    const slash = codecPart.indexOf('/');
    const codecName = (slash >= 0 ? codecPart.slice(0, slash) : codecPart).trim();
    if (codecName) codecs.add(codecName.toLowerCase());
  }

  return codecs;
}

function offerSupportsEncoding(sdp: string, encoding: string): boolean {
  const offered = getOfferedVideoCodecNames(sdp);
  if (!offered.size) return false;
  const normalized = encoding.toLowerCase();
  if (normalized === 'hevc') return offered.has('h265') || offered.has('hevc');
  if (normalized === 'av1') return offered.has('av1') || offered.has('av1x');
  if (normalized === 'h264') return offered.has('h264');
  return true;
}

function applyCodecPreferences(transceiver: RTCRtpTransceiver | null, encoding: string): void {
  if (!transceiver) return;
  const caps = getVideoCodecCapabilities();
  if (!caps?.codecs) return;
  const mimes = ENCODING_MIME[encoding.toLowerCase()];
  if (!mimes) return;
  const preferred = caps.codecs.filter((codec) => mimes.includes(codec.mimeType.toLowerCase()));
  if (!preferred.length) return;
  let filteredPreferred = preferred;
  if (mimes.includes('video/h264')) {
    const packetizationMode1 = preferred.filter((codec) =>
      /(?:^|;)\s*packetization-mode=1(?:;|$)/i.test(codec.sdpFmtpLine ?? ''),
    );
    if (packetizationMode1.length) {
      // Prefer H.264 packetization-mode=1 to avoid receiver assembly mismatches.
      filteredPreferred = packetizationMode1;
    }
  }
  const rest = caps.codecs.filter((codec) => !mimes.includes(codec.mimeType.toLowerCase()));
  try {
    transceiver.setCodecPreferences([...filteredPreferred, ...rest]);
  } catch {
    /* ignore */
  }
}

function applyInitialBitrateHints(sdp: string, bitrateKbps?: number): string {
  if (!sdp || !bitrateKbps || bitrateKbps <= 0) return sdp;
  const normalizedBitrateKbps = Math.max(1, Math.round(bitrateKbps));
  const bitrateBps = normalizedBitrateKbps * 1000;
  const lines = sdp.split(/\r\n/);
  const output: string[] = [];
  let inVideo = false;
  let pendingBandwidth = false;

  const pushBandwidth = () => {
    output.push(`b=AS:${normalizedBitrateKbps}`);
    output.push(`b=TIAS:${bitrateBps}`);
  };

  for (const line of lines) {
    if (line.startsWith('m=')) {
      if (inVideo && pendingBandwidth) {
        pushBandwidth();
      }
      inVideo = line.startsWith('m=video');
      pendingBandwidth = inVideo;
      output.push(line);
      continue;
    }

    if (inVideo) {
      if (line.startsWith('c=') && pendingBandwidth) {
        output.push(line);
        pushBandwidth();
        pendingBandwidth = false;
        continue;
      }
      if (line.startsWith('b=AS:') || line.startsWith('b=TIAS:')) {
        continue;
      }
      if (line.startsWith('a=fmtp:')) {
        const match = line.match(/^a=fmtp:(\d+)\s*(.*)$/);
        if (!match) {
          output.push(line);
          continue;
        }
        const payloadType = match[1];
        const params = match[2] ?? '';
        if (/(?:^|;)\s*apt=\d+/i.test(params)) {
          output.push(line);
          continue;
        }
        const trimmed = params.trim();
        let updatedParams = trimmed;
        if (!trimmed) {
          updatedParams = `x-google-start-bitrate=${normalizedBitrateKbps}`;
        } else if (/x-google-start-bitrate=\d+/i.test(trimmed)) {
          updatedParams = trimmed.replace(
            /x-google-start-bitrate=\d+/i,
            `x-google-start-bitrate=${normalizedBitrateKbps}`,
          );
        } else {
          updatedParams = `${trimmed};x-google-start-bitrate=${normalizedBitrateKbps}`;
        }
        output.push(`a=fmtp:${payloadType} ${updatedParams}`);
        continue;
      }
    }

    output.push(line);
  }

  if (inVideo && pendingBandwidth) {
    pushBandwidth();
  }

  const joined = output.join('\r\n');
  return sdp.endsWith('\n') && !joined.endsWith('\r\n') ? `${joined}\r\n` : joined;
}

function applyAudioReceiverHints(
  receiver?: RTCRtpReceiver,
  targetMs?: number,
  playoutDelayHintMs?: number,
): void {
  if (!receiver) return;
  const receiverAny = receiver as any;
  const target = resolveJitterTargetMs(targetMs);
  const delayHintMs =
    typeof playoutDelayHintMs === 'number' && Number.isFinite(playoutDelayHintMs)
      ? Math.max(0, playoutDelayHintMs)
      : undefined;
  try {
    if (delayHintMs != null && 'playoutDelayHint' in receiverAny) {
      receiverAny.playoutDelayHint = delayHintMs / 1000;
    }
  } catch {
    /* ignore */
  }
  try {
    if (target != null && typeof receiverAny.jitterBufferTarget === 'number') {
      receiverAny.jitterBufferTarget = target;
    }
  } catch {
    /* ignore */
  }
  if (target == null) return;
  try {
    if (
      typeof receiverAny.getParameters === 'function' &&
      typeof receiverAny.setParameters === 'function'
    ) {
      const parameters = receiverAny.getParameters();
      if (parameters && typeof parameters === 'object' && 'jitterBufferTarget' in parameters) {
        parameters.jitterBufferTarget = target;
        receiverAny.setParameters(parameters);
      }
    }
  } catch {
    /* ignore */
  }
}

function resolveJitterTargetMs(value?: number): number | undefined {
  if (typeof value !== 'number' || !Number.isFinite(value)) return undefined;
  return Math.max(0, value);
}

const VIDEO_MAX_FRAME_AGE_MIN_MS = 5;
const VIDEO_MAX_FRAME_AGE_MAX_MS = 100;

function resolveVideoJitterTargetMs(config: StreamConfig): number | undefined {
  const fps =
    typeof config.fps === 'number' && Number.isFinite(config.fps) && config.fps > 0 ? config.fps : 60;
  const minMs = VIDEO_MAX_FRAME_AGE_MIN_MS;
  const maxMs = VIDEO_MAX_FRAME_AGE_MAX_MS;
  if (
    typeof config.videoMaxFrameAgeFrames === 'number' &&
    Number.isFinite(config.videoMaxFrameAgeFrames) &&
    config.videoMaxFrameAgeFrames > 0
  ) {
    const frames = Math.round(config.videoMaxFrameAgeFrames);
    const computed = Math.round((1000 / fps) * frames);
    if (Number.isFinite(computed)) {
      return Math.min(maxMs, Math.max(minMs, computed));
    }
  }
  const targetMs = resolveJitterTargetMs(config.videoMaxFrameAgeMs);
  if (targetMs != null) return Math.min(maxMs, Math.max(minMs, targetMs));
  return undefined;
}

function applyVideoReceiverHints(receiver?: RTCRtpReceiver, targetMs?: number): void {
  if (!receiver) return;
  const target = resolveJitterTargetMs(targetMs);
  if (target == null) return;
  const receiverAny = receiver as any;
  try {
    if ('playoutDelayHint' in receiverAny) {
      receiverAny.playoutDelayHint = target / 1000;
    }
  } catch {
    /* ignore */
  }
  try {
    if (typeof receiverAny.jitterBufferTarget === 'number') {
      receiverAny.jitterBufferTarget = target;
    }
  } catch {
    /* ignore */
  }
  try {
    if (
      typeof receiverAny.getParameters === 'function' &&
      typeof receiverAny.setParameters === 'function'
    ) {
      const parameters = receiverAny.getParameters();
      if (parameters && typeof parameters === 'object' && 'jitterBufferTarget' in parameters) {
        parameters.jitterBufferTarget = target;
        receiverAny.setParameters(parameters);
      }
    }
  } catch {
    /* ignore */
  }
}

export class WebRtcClient {
  private api: WebRtcApi;
  private pc?: RTCPeerConnection;
  private sessionId?: string;
  private remoteStream = new MediaStream();
  private inputChannel?: RTCDataChannel;
  private unsubscribeCandidates?: () => void;
  private statsTimer?: number;
  private statsFastUntilMs?: number;
  private statsConnectedAtMs?: number;
  private statsState: StatsState = {};
  private pendingRemoteCandidates: RTCIceCandidateInit[] = [];
  private pendingLocalCandidates: RTCIceCandidateInit[] = [];
  private pendingLocalCandidatesTimer?: number;
  private autoDisconnectTimer?: number;
  private disconnecting = false;
  private pendingInput: (string | ArrayBuffer)[] = [];
  private maxPendingInput = 256;
  private receiverHintTimer?: number;
  private videoJitterTargetMs?: number;
  private audioJitterTargetMs = DEFAULT_AUDIO_JITTER_TARGET_MS;
  private audioPlayoutDelayHintMs = DEFAULT_AUDIO_PLAYOUT_DELAY_MS;

  constructor(api: WebRtcApi) {
    this.api = api;
  }

  get connectionState(): RTCPeerConnectionState | undefined {
    return this.pc?.connectionState;
  }

  get inputChannelState(): RTCDataChannelState | undefined {
    return this.inputChannel?.readyState;
  }

  get inputChannelBufferedAmount(): number | undefined {
    return this.inputChannel?.bufferedAmount;
  }

  async connect(
    config: StreamConfig,
    callbacks: WebRtcClientCallbacks = {},
    options: WebRtcClientConnectOptions = {},
  ): Promise<string> {
    const resolvedEncoding = resolveEncodingPreference(config.encoding);
    const primaryConfig =
      resolvedEncoding === config.encoding ? config : { ...config, encoding: resolvedEncoding };

    if (primaryConfig.encoding !== config.encoding) {
      const id = await this.connectAttempt(primaryConfig, callbacks, options);
      callbacks.onNegotiatedEncoding?.(primaryConfig.encoding);
      return id;
    }

    try {
      return await this.connectAttempt(primaryConfig, callbacks, options);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      const requested = primaryConfig.encoding.toLowerCase();
      const shouldFallback =
        requested !== 'h264' && message.startsWith('Browser did not offer requested video codec');
      if (!shouldFallback) {
        const finalError =
          error instanceof Error ? error : new Error('Failed to establish WebRTC session.');
        callbacks.onError?.(finalError);
        throw finalError;
      }
    }

    const id = await this.connectAttempt(
      { ...primaryConfig, encoding: 'h264' },
      callbacks,
      options,
    );
    callbacks.onNegotiatedEncoding?.('h264');
    return id;
  }

  private async connectAttempt(
    config: StreamConfig,
    callbacks: WebRtcClientCallbacks = {},
    options: WebRtcClientConnectOptions = {},
  ): Promise<string> {
    await this.disconnect();
    this.clearAutoDisconnectTimer();
    this.disconnecting = false;
    const sessionConfig = config;
    const session = await this.api.createSession(sessionConfig);
    this.sessionId = session.sessionId;
    this.pendingRemoteCandidates = [];
    this.videoJitterTargetMs = resolveVideoJitterTargetMs(sessionConfig);
    this.audioJitterTargetMs = DEFAULT_AUDIO_JITTER_TARGET_MS;
    this.audioPlayoutDelayHintMs = DEFAULT_AUDIO_PLAYOUT_DELAY_MS;
    this.statsFastUntilMs = undefined;
    this.statsConnectedAtMs = undefined;
    const requestedEncoding = sessionConfig.encoding.toLowerCase();
    const bundlePolicy: RTCBundlePolicy = requestedEncoding === 'hevc' ? 'balanced' : 'max-bundle';
    const rtcpMuxPolicy: RTCRtcpMuxPolicy = requestedEncoding === 'hevc' ? 'negotiate' : 'require';
    this.pc = new RTCPeerConnection({
      iceServers: session.iceServers,
      bundlePolicy,
      rtcpMuxPolicy,
    });

    const videoTransceiver = this.pc.addTransceiver('video', { direction: 'recvonly' });
    this.pc.addTransceiver('audio', { direction: 'recvonly' });
    applyCodecPreferences(videoTransceiver, sessionConfig.encoding);

    const inputPriority = options.inputPriority ?? 'high';
    this.inputChannel = this.pc.createDataChannel('input', {
      ordered: false,
      maxRetransmits: 0,
      priority: inputPriority,
    });
    this.inputChannel.onopen = () => {
      callbacks.onInputChannelState?.('open');
      this.flushPendingInput();
    };
    this.inputChannel.onclose = () => callbacks.onInputChannelState?.('closed');
    this.inputChannel.onerror = () => callbacks.onInputChannelState?.('closing');
    this.inputChannel.onmessage = (event) => {
      if (!callbacks.onInputMessage) return;
      if (typeof event.data !== 'string') return;
      try {
        const message = JSON.parse(event.data) as GamepadFeedbackMessage;
        if (message?.type !== 'gamepad_feedback') return;
        callbacks.onInputMessage(message);
      } catch {
        /* ignore */
      }
    };

    this.pc.ontrack = (event) => {
      this.remoteStream.addTrack(event.track);
      if (event.track.kind === 'audio') {
        applyAudioReceiverHints(
          event.receiver,
          this.audioJitterTargetMs,
          this.audioPlayoutDelayHintMs,
        );
      } else if (event.track.kind === 'video') {
        event.track.contentHint = 'motion';
        applyVideoReceiverHints(event.receiver, this.videoJitterTargetMs);
      }
      callbacks.onRemoteStream?.(this.remoteStream);
    };

    this.pc.onconnectionstatechange = () => {
      if (!this.pc) return;
      const state = this.pc.connectionState;
      callbacks.onConnectionState?.(state);
      if (state === 'connected') {
        const now = Date.now();
        this.statsConnectedAtMs = now;
        this.statsFastUntilMs = now + STATS_POLL_FAST_BOOT_MS;
        this.clearAutoDisconnectTimer();
        this.startReceiverHintRefresh();
      } else if (state === 'failed' || state === 'closed') {
        this.stopReceiverHintRefresh();
        this.scheduleAutoDisconnect(0);
      } else if (state === 'disconnected') {
        this.stopReceiverHintRefresh();
        this.scheduleAutoDisconnect(5000);
      }
    };

    this.pc.oniceconnectionstatechange = () => {
      if (!this.pc) return;
      callbacks.onIceState?.(this.pc.iceConnectionState);
    };

    this.pc.onicecandidate = (event) => {
      if (!event.candidate || !this.sessionId) return;
      this.queueLocalCandidate(event.candidate.toJSON());
    };

    this.unsubscribeCandidates = this.api.subscribeRemoteCandidates(
      session.sessionId,
      (candidate) => {
        if (!this.pc || !candidate) return;
        if (this.pc.remoteDescription) {
          void this.pc.addIceCandidate(candidate).catch(() => {});
          return;
        }
        this.pendingRemoteCandidates.push(candidate);
      },
    );

    try {
      const offer = await this.pc.createOffer({
        offerToReceiveAudio: true,
        offerToReceiveVideo: true,
      });
      const mungedOffer: RTCSessionDescriptionInit = {
        type: offer.type,
        sdp: applyInitialBitrateHints(offer.sdp ?? '', sessionConfig.bitrateKbps),
      };
      if (!offerSupportsEncoding(mungedOffer.sdp ?? '', sessionConfig.encoding)) {
        const offered =
          Array.from(getOfferedVideoCodecNames(mungedOffer.sdp ?? '')).join(', ') || 'none';
        throw new Error(
          `Browser did not offer requested video codec '${sessionConfig.encoding}' (offered: ${offered})`,
        );
      }
      await this.pc.setLocalDescription(mungedOffer);
      const answer = await this.api.sendOffer(session.sessionId, {
        type: mungedOffer.type,
        sdp: mungedOffer.sdp ?? '',
      });
      if (!answer?.sdp) {
        throw new Error('WebRTC answer not received');
      }
      try {
        await this.pc.setRemoteDescription(answer);
      } catch (error) {
        const offered =
          Array.from(getOfferedVideoCodecNames(mungedOffer.sdp ?? '')).join(', ') || 'none';
        console.error('Failed to apply WebRTC answer SDP', {
          encoding: sessionConfig.encoding,
          offered,
          offerSdp: mungedOffer.sdp,
          answerSdp: answer.sdp,
          error,
        });
        const message = error instanceof Error ? error.message : String(error);
        throw new Error(
          `Failed to apply WebRTC answer SDP (${sessionConfig.encoding}; offered: ${offered}): ${message}`,
        );
      }
      await this.flushPendingCandidates();
    } catch (error) {
      await this.disconnect();
      throw error;
    }

    this.startStatsPolling(callbacks);
    return session.sessionId;
  }

  private queueLocalCandidate(candidate: RTCIceCandidateInit): void {
    if (!candidate?.candidate || !this.sessionId) return;
    this.pendingLocalCandidates.push(candidate);
    if (this.pendingLocalCandidates.length >= ICE_CANDIDATE_BATCH_LIMIT) {
      this.flushLocalCandidates();
      return;
    }
    if (this.pendingLocalCandidatesTimer) return;
    this.pendingLocalCandidatesTimer = window.setTimeout(() => {
      this.pendingLocalCandidatesTimer = undefined;
      this.flushLocalCandidates();
    }, ICE_CANDIDATE_BATCH_WINDOW_MS);
  }

  private flushLocalCandidates(): void {
    if (!this.sessionId || !this.pendingLocalCandidates.length) return;
    const candidates = this.pendingLocalCandidates;
    this.pendingLocalCandidates = [];
    void this.api.sendIceCandidates(this.sessionId, candidates).catch(() => {});
  }

  async disconnect(options: WebRtcDisconnectOptions = {}): Promise<void> {
    if (this.disconnecting) return;
    this.disconnecting = true;
    this.clearAutoDisconnectTimer();
    this.stopReceiverHintRefresh();
    if (this.statsTimer) {
      window.clearTimeout(this.statsTimer);
      this.statsTimer = undefined;
    }
    this.statsFastUntilMs = undefined;
    this.statsConnectedAtMs = undefined;
    this.unsubscribeCandidates?.();
    this.unsubscribeCandidates = undefined;
    if (this.inputChannel) {
      try {
        this.inputChannel.close();
      } catch {
        /* ignore */
      }
    }
    if (this.pc) {
      try {
        this.pc.close();
      } catch {
        /* ignore */
      }
    }
    if (this.sessionId) {
      try {
        await this.api.endSession(this.sessionId, { keepalive: options.keepalive });
      } catch {
        /* ignore */
      }
    }
    if (this.pendingLocalCandidatesTimer) {
      window.clearTimeout(this.pendingLocalCandidatesTimer);
      this.pendingLocalCandidatesTimer = undefined;
    }
    this.remoteStream = new MediaStream();
    this.pendingRemoteCandidates = [];
    this.pendingLocalCandidates = [];
    this.pc = undefined;
    this.sessionId = undefined;
    this.inputChannel = undefined;
    this.pendingInput = [];
    this.statsState = {};
    this.videoJitterTargetMs = undefined;
    this.statsFastUntilMs = undefined;
    this.statsConnectedAtMs = undefined;
    this.disconnecting = false;
  }

  private startReceiverHintRefresh(): void {
    if (this.receiverHintTimer) return;
    this.receiverHintTimer = window.setInterval(() => {
      if (!this.pc) return;
      for (const receiver of this.pc.getReceivers()) {
        if (receiver.track?.kind === 'audio') {
          applyAudioReceiverHints(receiver, this.audioJitterTargetMs, this.audioPlayoutDelayHintMs);
        } else if (receiver.track?.kind === 'video') {
          applyVideoReceiverHints(receiver, this.videoJitterTargetMs);
        }
      }
    }, RECEIVER_HINT_REFRESH_MS);
  }

  private stopReceiverHintRefresh(): void {
    if (!this.receiverHintTimer) return;
    window.clearInterval(this.receiverHintTimer);
    this.receiverHintTimer = undefined;
  }

  setAudioLatencyTargets(targetMs: number, playoutDelayHintMs?: number): void {
    const resolvedTarget = resolveJitterTargetMs(targetMs) ?? DEFAULT_AUDIO_JITTER_TARGET_MS;
    const resolvedHint =
      typeof playoutDelayHintMs === 'number' && Number.isFinite(playoutDelayHintMs)
        ? Math.max(0, playoutDelayHintMs)
        : resolvedTarget;
    this.audioJitterTargetMs = resolvedTarget;
    this.audioPlayoutDelayHintMs = resolvedHint;
    if (!this.pc) return;
    for (const receiver of this.pc.getReceivers()) {
      if (receiver.track?.kind === 'audio') {
        applyAudioReceiverHints(receiver, this.audioJitterTargetMs, this.audioPlayoutDelayHintMs);
      }
    }
  }

  setVideoLatencyTarget(targetMs?: number): void {
    this.videoJitterTargetMs = resolveJitterTargetMs(targetMs);
    if (!this.pc) return;
    for (const receiver of this.pc.getReceivers()) {
      if (receiver.track?.kind === 'video') {
        applyVideoReceiverHints(receiver, this.videoJitterTargetMs);
      }
    }
  }

  sendInput(payload: string | ArrayBuffer): boolean {
    if (!this.inputChannel || this.inputChannel.readyState !== 'open') {
      this.queueInput(payload);
      return false;
    }
    try {
      this.inputChannel.send(payload);
      return true;
    } catch {
      this.queueInput(payload);
      return false;
    }
  }

  private queueInput(payload: string | ArrayBuffer): void {
    if (this.pendingInput.length >= this.maxPendingInput) {
      this.pendingInput.shift();
    }
    this.pendingInput.push(payload);
  }

  private flushPendingInput(): void {
    if (!this.inputChannel || this.inputChannel.readyState !== 'open') return;
    if (!this.pendingInput.length) return;
    const pending = this.pendingInput;
    this.pendingInput = [];
    for (const payload of pending) {
      try {
        this.inputChannel.send(payload);
      } catch {
        this.queueInput(payload);
        break;
      }
    }
  }

  private startStatsPolling(callbacks: WebRtcClientCallbacks): void {
    if (!this.pc) return;
    if (this.statsTimer) return;
    const poll = async () => {
      if (!this.pc) return;
      let snapshot: WebRtcStatsSnapshot | null = null;
      try {
        const stats = await this.pc.getStats();
        snapshot = this.extractStats(stats);
        callbacks.onStats?.(snapshot);
      } catch {
        /* ignore */
      }

      if (!this.pc) return;
      const now = Date.now();
      const jitter = snapshot?.videoPlayoutDelayMs ?? snapshot?.videoJitterBufferMs;
      if (typeof jitter === 'number' && Number.isFinite(jitter) && jitter >= STATS_POLL_FAST_JITTER_THRESHOLD_MS) {
        this.statsFastUntilMs = Math.max(this.statsFastUntilMs ?? 0, now + STATS_POLL_FAST_HOLD_MS);
      }
      const shouldFast = (this.statsFastUntilMs != null && now <= this.statsFastUntilMs) ||
        (this.statsConnectedAtMs != null && now - this.statsConnectedAtMs <= STATS_POLL_FAST_BOOT_MS);
      const delay = shouldFast ? STATS_POLL_FAST_MS : STATS_POLL_SLOW_MS;
      this.statsTimer = window.setTimeout(() => {
        this.statsTimer = undefined;
        void poll();
      }, delay);
    };
    void poll();
  }

  private async flushPendingCandidates(): Promise<void> {
    if (!this.pc || !this.pc.remoteDescription || !this.pendingRemoteCandidates.length) return;
    const pc = this.pc;
    const pending = this.pendingRemoteCandidates;
    this.pendingRemoteCandidates = [];
    for (const candidate of pending) {
      try {
        await pc.addIceCandidate(candidate);
      } catch {
        /* ignore */
      }
    }
  }

  private clearAutoDisconnectTimer(): void {
    if (this.autoDisconnectTimer) {
      window.clearTimeout(this.autoDisconnectTimer);
      this.autoDisconnectTimer = undefined;
    }
  }

  private scheduleAutoDisconnect(delayMs: number): void {
    if (this.disconnecting || !this.sessionId) return;
    this.clearAutoDisconnectTimer();
    if (delayMs <= 0) {
      void this.disconnect();
      return;
    }
    this.autoDisconnectTimer = window.setTimeout(() => {
      this.autoDisconnectTimer = undefined;
      void this.disconnect();
    }, delayMs);
  }

  private extractStats(report: RTCStatsReport): WebRtcStatsSnapshot {
    const inboundVideo: any[] = [];
    const inboundAudio: any[] = [];
    let rttMs: number | undefined;
    let selectedPair: any | undefined;
    const candidates = new Map<string, any>();

    report.forEach((item) => {
      if (item.type === 'inbound-rtp' && item.kind === 'video') {
        inboundVideo.push(item as any);
      }
      if (item.type === 'inbound-rtp' && item.kind === 'audio') {
        inboundAudio.push(item as any);
      }
      if (item.type === 'candidate-pair' && (item as any).state === 'succeeded') {
        rttMs = (item as any).currentRoundTripTime
          ? (item as any).currentRoundTripTime * 1000
          : rttMs;
        if ((item as any).selected || (item as any).nominated || !selectedPair) {
          selectedPair = item;
        }
      }
      if (item.type === 'local-candidate' || item.type === 'remote-candidate') {
        candidates.set(item.id, item);
      }
    });

    const pickInbound = (items: any[]): any | undefined => {
      if (!items.length) return undefined;
      const asNumber = (value: unknown): number => (typeof value === 'number' ? value : 0);
      const sorted = [...items].sort((left, right) => {
        const leftFramesDecoded = asNumber(left.framesDecoded);
        const rightFramesDecoded = asNumber(right.framesDecoded);
        const leftFramesReceived = asNumber(left.framesReceived);
        const rightFramesReceived = asNumber(right.framesReceived);
        const leftHasFrames = leftFramesDecoded > 0 || leftFramesReceived > 0;
        const rightHasFrames = rightFramesDecoded > 0 || rightFramesReceived > 0;
        if (leftHasFrames !== rightHasFrames) {
          return leftHasFrames ? -1 : 1;
        }
        if (leftFramesDecoded !== rightFramesDecoded) {
          return rightFramesDecoded - leftFramesDecoded;
        }
        if (leftFramesReceived !== rightFramesReceived) {
          return rightFramesReceived - leftFramesReceived;
        }
        const leftBytes = asNumber(left.bytesReceived);
        const rightBytes = asNumber(right.bytesReceived);
        if (leftBytes !== rightBytes) {
          return rightBytes - leftBytes;
        }
        const leftPackets = asNumber(left.packetsReceived);
        const rightPackets = asNumber(right.packetsReceived);
        return rightPackets - leftPackets;
      });
      return sorted[0];
    };

    const videoInbound = pickInbound(inboundVideo);
    const audioInbound = pickInbound(inboundAudio);

    const videoInboundId: string | undefined = videoInbound?.id;
    const audioInboundId: string | undefined = audioInbound?.id;

    const videoBytes: number | undefined = videoInbound?.bytesReceived;
    const audioBytes: number | undefined = audioInbound?.bytesReceived;
    const inboundVideoFps: number | undefined = videoInbound?.framesPerSecond;
    const packetsLost: number | undefined =
      (typeof videoInbound?.packetsLost === 'number' ? videoInbound.packetsLost : undefined) ??
      (typeof audioInbound?.packetsLost === 'number' ? audioInbound.packetsLost : undefined);
    const videoPackets: number | undefined = videoInbound?.packetsReceived;
    const audioPackets: number | undefined = audioInbound?.packetsReceived;
    const videoFramesReceived: number | undefined = videoInbound?.framesReceived;
    const videoFramesDecoded: number | undefined = videoInbound?.framesDecoded;
    const videoFramesDropped: number | undefined = videoInbound?.framesDropped;
    const videoTotalDecodeTime: number | undefined = videoInbound?.totalDecodeTime;
    const videoJitterMs: number | undefined =
      typeof videoInbound?.jitter === 'number' ? videoInbound.jitter * 1000 : undefined;
    const audioJitterMs: number | undefined =
      typeof audioInbound?.jitter === 'number' ? audioInbound.jitter * 1000 : undefined;
    const videoJitterBufferDelay: number | undefined = videoInbound?.jitterBufferDelay;
    const videoJitterBufferEmittedCount: number | undefined =
      videoInbound?.jitterBufferEmittedCount;
    const audioJitterBufferDelay: number | undefined = audioInbound?.jitterBufferDelay;
    const audioJitterBufferEmittedCount: number | undefined =
      audioInbound?.jitterBufferEmittedCount;
    const videoCodecId: string | undefined = videoInbound?.codecId;
    const audioCodecId: string | undefined = audioInbound?.codecId;

    let videoCodec: string | undefined;
    let audioCodec: string | undefined;
    if (videoCodecId) {
      const codec = report.get(videoCodecId) as any;
      if (codec?.mimeType) {
        videoCodec = codec.mimeType;
      }
    }
    if (audioCodecId) {
      const codec = report.get(audioCodecId) as any;
      if (codec?.mimeType) {
        audioCodec = codec.mimeType;
      }
    }

    let candidatePair: WebRtcStatsSnapshot['candidatePair'];
    if (selectedPair) {
      const local = candidates.get((selectedPair as any).localCandidateId);
      const remote = candidates.get((selectedPair as any).remoteCandidateId);
      candidatePair = {
        state: (selectedPair as any).state,
        protocol: (selectedPair as any).protocol,
        localAddress: local?.address,
        localPort: local?.port,
        localType: local?.candidateType,
        remoteAddress: remote?.address,
        remotePort: remote?.port,
        remoteType: remote?.candidateType,
      };
    }

    const now = Date.now();
    const last = this.statsState;
    const deltaMs = last.lastTimestampMs ? Math.max(1, now - last.lastTimestampMs) : 0;
    const sameVideoInbound = videoInboundId && last.lastVideoInboundId === videoInboundId;
    const sameAudioInbound = audioInboundId && last.lastAudioInboundId === audioInboundId;
    const calcRate = (bytes?: number, lastBytes?: number) => {
      if (bytes == null || lastBytes == null || !deltaMs) return undefined;
      return Math.round(((bytes - lastBytes) * 8) / deltaMs);
    };
    const calcFps = (frames?: number, lastFrames?: number) => {
      if (frames == null || lastFrames == null || !deltaMs) return undefined;
      const deltaFrames = frames - lastFrames;
      if (deltaFrames <= 0) return undefined;
      return (deltaFrames * 1000) / deltaMs;
    };
    const videoBitrate = calcRate(videoBytes, sameVideoInbound ? last.lastVideoBytes : undefined);
    const audioBitrate = calcRate(audioBytes, sameAudioInbound ? last.lastAudioBytes : undefined);
    const calcJitterBufferMs = (
      delay?: number,
      emitted?: number,
      lastDelay?: number,
      lastEmitted?: number,
    ) => {
      if (delay == null || emitted == null || emitted <= 0) return undefined;
      if (lastDelay == null || lastEmitted == null) return undefined;
      const deltaDelay = delay - lastDelay;
      const deltaEmitted = emitted - lastEmitted;
      if (deltaEmitted <= 0 || deltaDelay < 0) return undefined;
      return (deltaDelay / deltaEmitted) * 1000;
    };
    // Note: calcPlayoutDelayMs attempts to compute delay from estimatedPlayoutTimestamp
    // but this is unreliable due to timestamp format ambiguity. We prefer jitterBufferMs
    // which is well-defined. Keeping this function for potential future use if browsers
    // standardize the format.
    const calcPlayoutDelayMs = (inbound?: any): number | undefined => {
      // estimatedPlayoutTimestamp represents WHEN content will play (wall-clock time),
      // not the delay. The computation is complex and browser-dependent.
      // Return undefined to fall back to delta-based jitterBufferMs.
      return undefined;
    };
    const calcDecodeMs = (
      totalDecodeTime?: number,
      framesDecoded?: number,
      lastTotalDecodeTime?: number,
      lastFramesDecoded?: number,
    ) => {
      if (totalDecodeTime == null || framesDecoded == null || framesDecoded <= 0) return undefined;
      // Use delta-based calculation if we have previous values
      if (lastTotalDecodeTime != null && lastFramesDecoded != null) {
        const deltaTime = totalDecodeTime - lastTotalDecodeTime;
        const deltaFrames = framesDecoded - lastFramesDecoded;
        if (deltaFrames > 0 && deltaTime >= 0) {
          return (deltaTime / deltaFrames) * 1000;
        }
      }
      // Fall back to lifetime average for first sample
      return (totalDecodeTime / framesDecoded) * 1000;
    };
    const videoJitterBufferMs = calcJitterBufferMs(
      videoJitterBufferDelay,
      videoJitterBufferEmittedCount,
      sameVideoInbound ? last.lastVideoJitterBufferDelay : undefined,
      sameVideoInbound ? last.lastVideoJitterBufferEmittedCount : undefined,
    );
    const audioJitterBufferMs = calcJitterBufferMs(
      audioJitterBufferDelay,
      audioJitterBufferEmittedCount,
      sameAudioInbound ? last.lastAudioJitterBufferDelay : undefined,
      sameAudioInbound ? last.lastAudioJitterBufferEmittedCount : undefined,
    );
    const videoDecodeMs = calcDecodeMs(
      videoTotalDecodeTime,
      videoFramesDecoded,
      sameVideoInbound ? last.lastVideoTotalDecodeTime : undefined,
      sameVideoInbound ? last.lastVideoFramesDecoded : undefined,
    );
    const videoFpsFromDecoded = calcFps(
      videoFramesDecoded,
      sameVideoInbound ? last.lastVideoFramesDecoded : undefined,
    );
    const videoFpsFromReceived = calcFps(
      videoFramesReceived,
      sameVideoInbound ? last.lastVideoFramesReceived : undefined,
    );
    const videoFps = videoFpsFromDecoded ?? videoFpsFromReceived ?? inboundVideoFps;
    const videoPlayoutDelayMs = calcPlayoutDelayMs(videoInbound);
    const audioPlayoutDelayMs = calcPlayoutDelayMs(audioInbound);

    this.statsState = {
      lastTimestampMs: now,
      lastVideoInboundId: videoInboundId,
      lastAudioInboundId: audioInboundId,
      lastVideoBytes: videoBytes,
      lastAudioBytes: audioBytes,
      lastVideoJitterBufferDelay: videoJitterBufferDelay,
      lastVideoJitterBufferEmittedCount: videoJitterBufferEmittedCount,
      lastAudioJitterBufferDelay: audioJitterBufferDelay,
      lastAudioJitterBufferEmittedCount: audioJitterBufferEmittedCount,
      lastVideoTotalDecodeTime: videoTotalDecodeTime,
      lastVideoFramesDecoded: videoFramesDecoded,
      lastVideoFramesReceived: videoFramesReceived,
    };

    return {
      videoBitrateKbps: videoBitrate ? Math.max(0, videoBitrate) : undefined,
      audioBitrateKbps: audioBitrate ? Math.max(0, audioBitrate) : undefined,
      videoFps,
      packetsLost,
      roundTripTimeMs: rttMs,
      videoBytesReceived: videoBytes,
      audioBytesReceived: audioBytes,
      videoPacketsReceived: videoPackets,
      audioPacketsReceived: audioPackets,
      videoFramesReceived,
      videoFramesDecoded,
      videoFramesDropped,
      videoDecodeMs,
      videoJitterMs,
      audioJitterMs,
      videoJitterBufferMs,
      audioJitterBufferMs,
      videoPlayoutDelayMs,
      audioPlayoutDelayMs,
      videoCodec,
      audioCodec,
      candidatePair,
    };
  }
}
