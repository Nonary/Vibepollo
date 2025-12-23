import { WebRtcApi } from '@/services/webrtcApi';
import { StreamConfig, WebRtcStatsSnapshot } from '@/types/webrtc';

export interface WebRtcClientCallbacks {
  onRemoteStream?: (stream: MediaStream) => void;
  onConnectionState?: (state: RTCPeerConnectionState) => void;
  onIceState?: (state: RTCIceConnectionState) => void;
  onInputChannelState?: (state: RTCDataChannelState) => void;
  onStats?: (stats: WebRtcStatsSnapshot) => void;
  onError?: (error: Error) => void;
}

interface StatsState {
  lastVideoBytes?: number;
  lastAudioBytes?: number;
  lastTimestampMs?: number;
}

const ENCODING_MIME: Record<string, string> = {
  h264: 'video/h264',
  hevc: 'video/h265',
  av1: 'video/av1',
};

function applyCodecPreferences(
  transceiver: RTCRtpTransceiver | null,
  encoding: string,
): void {
  if (!transceiver || typeof RTCRtpSender === 'undefined') return;
  const caps = RTCRtpSender.getCapabilities('video');
  if (!caps?.codecs) return;
  const mime = ENCODING_MIME[encoding.toLowerCase()];
  if (!mime) return;
  const preferred = caps.codecs.filter((codec) => codec.mimeType.toLowerCase() === mime);
  if (!preferred.length) return;
  const rest = caps.codecs.filter((codec) => codec.mimeType.toLowerCase() !== mime);
  try {
    transceiver.setCodecPreferences([...preferred, ...rest]);
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

  constructor(api: WebRtcApi) {
    this.api = api;
  }

  get connectionState(): RTCPeerConnectionState | undefined {
    return this.pc?.connectionState;
  }

  get inputChannelState(): RTCDataChannelState | undefined {
    return this.inputChannel?.readyState;
  }

  async connect(config: StreamConfig, callbacks: WebRtcClientCallbacks = {}): Promise<void> {
    await this.disconnect();
    const session = await this.api.createSession(config);
    this.sessionId = session.sessionId;
    this.pc = new RTCPeerConnection({
      iceServers: session.iceServers,
      bundlePolicy: 'max-bundle',
      rtcpMuxPolicy: 'require',
    });

    const videoTransceiver = this.pc.addTransceiver('video', { direction: 'recvonly' });
    this.pc.addTransceiver('audio', { direction: 'recvonly' });
    applyCodecPreferences(videoTransceiver, config.encoding);

    this.inputChannel = this.pc.createDataChannel('input', {
      ordered: false,
      maxRetransmits: 0,
    });
    this.inputChannel.onopen = () => callbacks.onInputChannelState?.('open');
    this.inputChannel.onclose = () => callbacks.onInputChannelState?.('closed');
    this.inputChannel.onerror = () => callbacks.onInputChannelState?.('closing');

    this.pc.ontrack = (event) => {
      this.remoteStream.addTrack(event.track);
      callbacks.onRemoteStream?.(this.remoteStream);
    };

    this.pc.onconnectionstatechange = () => {
      if (!this.pc) return;
      callbacks.onConnectionState?.(this.pc.connectionState);
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
        void this.pc.addIceCandidate(candidate);
      },
    );

    try {
      const offer = await this.pc.createOffer({
        offerToReceiveAudio: true,
        offerToReceiveVideo: true,
      });
      await this.pc.setLocalDescription(offer);
      const answer = await this.api.sendOffer(session.sessionId, {
        type: offer.type,
        sdp: offer.sdp ?? '',
      });
      if (!answer?.sdp) {
        throw new Error('WebRTC answer not received');
      }
      await this.pc.setRemoteDescription(answer);
    } catch (error) {
      callbacks.onError?.(error as Error);
      throw error;
    }

    this.startStatsPolling(callbacks);
  }

  async disconnect(): Promise<void> {
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
      await this.api.endSession(this.sessionId);
    }
    this.remoteStream = new MediaStream();
    this.pc = undefined;
    this.sessionId = undefined;
    this.inputChannel = undefined;
    this.statsState = {};
  }

  sendInput(payload: string): void {
    if (!this.inputChannel || this.inputChannel.readyState !== 'open') return;
    try {
      this.inputChannel.send(payload);
    } catch {
      /* ignore */
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

  private extractStats(report: RTCStatsReport): WebRtcStatsSnapshot {
    let videoBytes: number | undefined;
    let audioBytes: number | undefined;
    let videoFps: number | undefined;
    let packetsLost: number | undefined;
    let rttMs: number | undefined;

    report.forEach((item) => {
      if (item.type === 'inbound-rtp' && item.kind === 'video') {
        videoBytes = (item as any).bytesReceived ?? videoBytes;
        videoFps = (item as any).framesPerSecond ?? videoFps;
        packetsLost = (item as any).packetsLost ?? packetsLost;
      }
      if (item.type === 'inbound-rtp' && item.kind === 'audio') {
        audioBytes = (item as any).bytesReceived ?? audioBytes;
      }
      if (item.type === 'candidate-pair' && (item as any).state === 'succeeded') {
        rttMs = (item as any).currentRoundTripTime
          ? (item as any).currentRoundTripTime * 1000
          : rttMs;
      }
    });

    const now = Date.now();
    const last = this.statsState;
    const deltaMs = last.lastTimestampMs ? Math.max(1, now - last.lastTimestampMs) : 0;
    const calcRate = (bytes?: number, lastBytes?: number) => {
      if (bytes == null || lastBytes == null || !deltaMs) return undefined;
      return Math.round(((bytes - lastBytes) * 8) / deltaMs);
    };
    const videoBitrate = calcRate(videoBytes, last.lastVideoBytes);
    const audioBitrate = calcRate(audioBytes, last.lastAudioBytes);

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
    };
  }
}
