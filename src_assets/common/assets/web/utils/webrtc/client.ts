import { WebRtcApi } from '@/services/webrtcApi';
import { GamepadFeedbackMessage, StreamConfig, WebRtcStatsSnapshot } from '@/types/webrtc';

export interface WebRtcClientCallbacks {
  onRemoteStream?: (stream: MediaStream) => void;
  onConnectionState?: (state: RTCPeerConnectionState) => void;
  onIceState?: (state: RTCIceConnectionState) => void;
  onInputChannelState?: (state: RTCDataChannelState) => void;
  onStats?: (stats: WebRtcStatsSnapshot) => void;
  onInputMessage?: (message: GamepadFeedbackMessage) => void;
  onError?: (error: Error) => void;
}

export interface WebRtcDisconnectOptions {
  keepalive?: boolean;
}

interface StatsState {
  lastVideoBytes?: number;
  lastAudioBytes?: number;
  lastTimestampMs?: number;
}

const ENCODING_MIME: Record<string, string[]> = {
  h264: ['video/h264'],
  hevc: ['video/h265', 'video/hevc'],
  av1: ['video/av1'],
};
const AUDIO_JITTER_BUFFER_TARGET_MS = 20;

function resolveEncodingPreference(encoding: string): string {
  if (typeof RTCRtpSender === 'undefined') return encoding;
  const mimes = ENCODING_MIME[encoding.toLowerCase()];
  if (!mimes) return encoding;
  const caps = RTCRtpSender.getCapabilities('video');
  if (!caps?.codecs) return encoding;
  const supported = caps.codecs.some((codec) => mimes.includes(codec.mimeType.toLowerCase()));
  return supported ? encoding : 'h264';
}

function applyCodecPreferences(
  transceiver: RTCRtpTransceiver | null,
  encoding: string,
): void {
  if (!transceiver || typeof RTCRtpSender === 'undefined') return;
  const caps = RTCRtpSender.getCapabilities('video');
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

function applyAudioReceiverHints(receiver?: RTCRtpReceiver): void {
  if (!receiver) return;
  const receiverAny = receiver as any;
  try {
    if (typeof receiverAny.jitterBufferTarget === 'number') {
      receiverAny.jitterBufferTarget = AUDIO_JITTER_BUFFER_TARGET_MS;
    }
  } catch {
    /* ignore */
  }
  try {
    if ('playoutDelayHint' in receiverAny) {
      receiverAny.playoutDelayHint = AUDIO_JITTER_BUFFER_TARGET_MS / 1000;
    }
  } catch {
    /* ignore */
  }
  try {
    if (typeof receiverAny.getParameters === 'function' && typeof receiverAny.setParameters === 'function') {
      const parameters = receiverAny.getParameters();
      if (parameters && typeof parameters === 'object' && 'jitterBufferTarget' in parameters) {
        parameters.jitterBufferTarget = AUDIO_JITTER_BUFFER_TARGET_MS;
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
  private statsState: StatsState = {};
  private pendingRemoteCandidates: RTCIceCandidateInit[] = [];
  private autoDisconnectTimer?: number;
  private disconnecting = false;
  private pendingInput: string[] = [];
  private maxPendingInput = 256;

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

  async connect(config: StreamConfig, callbacks: WebRtcClientCallbacks = {}): Promise<string> {
    await this.disconnect();
    this.clearAutoDisconnectTimer();
    this.disconnecting = false;
    const resolvedEncoding = resolveEncodingPreference(config.encoding);
    const sessionConfig =
      resolvedEncoding === config.encoding ? config : { ...config, encoding: resolvedEncoding };
    const session = await this.api.createSession(sessionConfig);
    this.sessionId = session.sessionId;
    this.pendingRemoteCandidates = [];
    this.pc = new RTCPeerConnection({
      iceServers: session.iceServers,
      bundlePolicy: 'max-bundle',
      rtcpMuxPolicy: 'require',
    });

    const videoTransceiver = this.pc.addTransceiver('video', { direction: 'recvonly' });
    this.pc.addTransceiver('audio', { direction: 'recvonly' });
    applyCodecPreferences(videoTransceiver, sessionConfig.encoding);

    this.inputChannel = this.pc.createDataChannel('input', {
      ordered: false,
      maxRetransmits: 0,
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
        applyAudioReceiverHints(event.receiver);
      }
      callbacks.onRemoteStream?.(this.remoteStream);
    };

    this.pc.onconnectionstatechange = () => {
      if (!this.pc) return;
      const state = this.pc.connectionState;
      callbacks.onConnectionState?.(state);
      if (state === 'connected') {
        this.clearAutoDisconnectTimer();
      } else if (state === 'failed' || state === 'closed') {
        this.scheduleAutoDisconnect(0);
      } else if (state === 'disconnected') {
        this.scheduleAutoDisconnect(5000);
      }
    };

    this.pc.oniceconnectionstatechange = () => {
      if (!this.pc) return;
      callbacks.onIceState?.(this.pc.iceConnectionState);
    };

    this.pc.onicecandidate = (event) => {
      if (!event.candidate || !this.sessionId) return;
      void this.api.sendIceCandidate(this.sessionId, event.candidate.toJSON());
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
      await this.pc.setLocalDescription(mungedOffer);
      const answer = await this.api.sendOffer(session.sessionId, {
        type: mungedOffer.type,
        sdp: mungedOffer.sdp ?? '',
      });
      if (!answer?.sdp) {
        throw new Error('WebRTC answer not received');
      }
      await this.pc.setRemoteDescription(answer);
      await this.flushPendingCandidates();
    } catch (error) {
      callbacks.onError?.(error as Error);
      await this.disconnect();
      throw error;
    }

    this.startStatsPolling(callbacks);
    return session.sessionId;
  }

  async disconnect(options: WebRtcDisconnectOptions = {}): Promise<void> {
    if (this.disconnecting) return;
    this.disconnecting = true;
    this.clearAutoDisconnectTimer();
    if (this.statsTimer) {
      window.clearInterval(this.statsTimer);
      this.statsTimer = undefined;
    }
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
    this.remoteStream = new MediaStream();
    this.pendingRemoteCandidates = [];
    this.pc = undefined;
    this.sessionId = undefined;
    this.inputChannel = undefined;
    this.pendingInput = [];
    this.statsState = {};
    this.disconnecting = false;
  }

  sendInput(payload: string): boolean {
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

  private queueInput(payload: string): void {
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
    this.statsTimer = window.setInterval(async () => {
      if (!this.pc) return;
      try {
        const stats = await this.pc.getStats();
        const snapshot = this.extractStats(stats);
        callbacks.onStats?.(snapshot);
      } catch {
        /* ignore */
      }
    }, 1000);
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
    let videoBytes: number | undefined;
    let audioBytes: number | undefined;
    let videoFps: number | undefined;
    let packetsLost: number | undefined;
    let rttMs: number | undefined;
    let videoPackets: number | undefined;
    let audioPackets: number | undefined;
    let videoFramesReceived: number | undefined;
    let videoFramesDecoded: number | undefined;
    let videoFramesDropped: number | undefined;
    let videoTotalDecodeTime: number | undefined;
    let videoJitterMs: number | undefined;
    let audioJitterMs: number | undefined;
    let videoJitterBufferDelay: number | undefined;
    let videoJitterBufferEmittedCount: number | undefined;
    let audioJitterBufferDelay: number | undefined;
    let audioJitterBufferEmittedCount: number | undefined;
    let videoCodecId: string | undefined;
    let audioCodecId: string | undefined;
    let selectedPair: any | undefined;
    const candidates = new Map<string, any>();

    report.forEach((item) => {
      if (item.type === 'inbound-rtp' && item.kind === 'video') {
        videoBytes = (item as any).bytesReceived ?? videoBytes;
        videoFps = (item as any).framesPerSecond ?? videoFps;
        packetsLost = (item as any).packetsLost ?? packetsLost;
        videoPackets = (item as any).packetsReceived ?? videoPackets;
        videoFramesReceived = (item as any).framesReceived ?? videoFramesReceived;
        videoFramesDecoded = (item as any).framesDecoded ?? videoFramesDecoded;
        videoFramesDropped = (item as any).framesDropped ?? videoFramesDropped;
        videoTotalDecodeTime = (item as any).totalDecodeTime ?? videoTotalDecodeTime;
        if (typeof (item as any).jitter === 'number') {
          videoJitterMs = (item as any).jitter * 1000;
        }
        videoJitterBufferDelay = (item as any).jitterBufferDelay ?? videoJitterBufferDelay;
        videoJitterBufferEmittedCount =
          (item as any).jitterBufferEmittedCount ?? videoJitterBufferEmittedCount;
        videoCodecId = (item as any).codecId ?? videoCodecId;
      }
      if (item.type === 'inbound-rtp' && item.kind === 'audio') {
        audioBytes = (item as any).bytesReceived ?? audioBytes;
        audioPackets = (item as any).packetsReceived ?? audioPackets;
        if (typeof (item as any).jitter === 'number') {
          audioJitterMs = (item as any).jitter * 1000;
        }
        audioJitterBufferDelay = (item as any).jitterBufferDelay ?? audioJitterBufferDelay;
        audioJitterBufferEmittedCount =
          (item as any).jitterBufferEmittedCount ?? audioJitterBufferEmittedCount;
        audioCodecId = (item as any).codecId ?? audioCodecId;
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
    const calcRate = (bytes?: number, lastBytes?: number) => {
      if (bytes == null || lastBytes == null || !deltaMs) return undefined;
      return Math.round(((bytes - lastBytes) * 8) / deltaMs);
    };
    const videoBitrate = calcRate(videoBytes, last.lastVideoBytes);
    const audioBitrate = calcRate(audioBytes, last.lastAudioBytes);
    const calcJitterBufferMs = (delay?: number, emitted?: number) => {
      if (delay == null || emitted == null || emitted <= 0) return undefined;
      return (delay / emitted) * 1000;
    };
    const calcDecodeMs = (totalDecodeTime?: number, framesDecoded?: number) => {
      if (totalDecodeTime == null || !framesDecoded) return undefined;
      return (totalDecodeTime / framesDecoded) * 1000;
    };
    const videoJitterBufferMs = calcJitterBufferMs(videoJitterBufferDelay, videoJitterBufferEmittedCount);
    const audioJitterBufferMs = calcJitterBufferMs(audioJitterBufferDelay, audioJitterBufferEmittedCount);
    const videoDecodeMs = calcDecodeMs(videoTotalDecodeTime, videoFramesDecoded);

    this.statsState = {
      lastTimestampMs: now,
      lastVideoBytes: videoBytes,
      lastAudioBytes: audioBytes,
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
      videoCodec,
      audioCodec,
      candidatePair,
    };
  }
}
