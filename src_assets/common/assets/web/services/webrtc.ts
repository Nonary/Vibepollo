import { ApiError, apiDelete, apiGet, apiPost } from '@/api/client';
import type { EncodingType, StreamConfig } from '@/types/webrtc';

export interface WebRtcCodecCapability {
  supported: boolean;
  hdr: boolean;
}

export interface WebRtcStreamLimits {
  min_dimension: number;
  max_dimension: number;
  min_fps: number;
  max_fps: number;
  min_bitrate_kbps: number;
  max_bitrate_kbps: number;
}

export interface WebRtcHostCapabilities {
  enabled: boolean;
  availability: {
    state: 'disabled' | 'ready' | 'unverified' | string;
    reason?: string;
  };
  codecs: Record<EncodingType, WebRtcCodecCapability>;
  hdr_policy: 'automatic' | 'force_on' | 'force_off' | string;
  hdr_policy_allows: boolean;
  limits: WebRtcStreamLimits;
}

export interface BrowserVideoCapabilities {
  h264: WebRtcCodecCapability;
  hevc: WebRtcCodecCapability;
  av1: WebRtcCodecCapability;
}

export interface WebRtcConnectionCallbacks {
  onConnectionState?: (state: RTCPeerConnectionState) => void;
  onInputState?: (state: RTCDataChannelState) => void;
  onRemoteStream?: (stream: MediaStream) => void;
  onVideoPlayoutDelay?: (delayMs: number | undefined) => void;
}

interface VideoJitterStatsState {
  delay?: number;
  emitted?: number;
  id?: string;
}

interface WebRtcSessionResponse {
  status?: boolean;
  session?: { id?: string };
  cert_fingerprint?: string;
  cert_pem?: string;
  ice_servers?: RTCIceServer[];
}

interface WebRtcAnswerResponse {
  answer_ready?: boolean;
  error?: string;
  sdp?: string;
  type?: RTCSdpType;
}

interface WebRtcIceCandidateResponse {
  candidates?: Array<{
    candidate?: string;
    index?: number;
    sdpMid?: string | null;
    sdpMLineIndex?: number | null;
  }>;
  next_since?: number;
}

interface CreatedWebRtcSession {
  iceServers: RTCIceServer[];
  id: string;
}

export class WebRtcConnectionCanceledError extends Error {
  constructor() {
    super('Browser stream connection was canceled.');
    this.name = 'WebRtcConnectionCanceledError';
  }
}

const videoMimeTypes: Record<EncodingType, readonly string[]> = {
  h264: ['video/h264'],
  hevc: ['video/h265', 'video/hevc'],
  av1: ['video/av1'],
};

const defaultLimits: WebRtcStreamLimits = {
  min_dimension: 64,
  max_dimension: 16384,
  min_fps: 1,
  max_fps: 1000,
  min_bitrate_kbps: 0,
  max_bitrate_kbps: 500000,
};

const receiverHintRefreshMs = 250;
const videoStatsPollMs = 250;

function normalizedLatencyTargetMs(value: number | undefined, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value)
    ? Math.max(0, Math.min(100, value))
    : fallback;
}

function applyReceiverLatencyHints(receiver: RTCRtpReceiver | undefined, targetMs: number): void {
  if (!receiver) return;
  const receiverWithHints = receiver as RTCRtpReceiver & {
    jitterBufferTarget?: number;
    playoutDelayHint?: number | null;
  };
  try {
    if ('playoutDelayHint' in receiverWithHints) {
      receiverWithHints.playoutDelayHint = targetMs / 1000;
    }
  } catch {
    // Receiver latency hints are optional and vary across browser versions.
  }
  try {
    if (typeof receiverWithHints.jitterBufferTarget === 'number') {
      receiverWithHints.jitterBufferTarget = targetMs;
    }
  } catch {
    // Continue with the browser's default jitter buffer when hints are read-only.
  }
}

type PeerConnectionConstructor = new (configuration?: RTCConfiguration) => RTCPeerConnection;

function peerConnectionConstructor(): PeerConnectionConstructor | null {
  const rtcGlobal = globalThis as typeof globalThis & {
    webkitRTCPeerConnection?: PeerConnectionConstructor;
  };
  if (typeof rtcGlobal.RTCPeerConnection === 'function') return rtcGlobal.RTCPeerConnection;
  if (typeof rtcGlobal.webkitRTCPeerConnection === 'function') {
    return rtcGlobal.webkitRTCPeerConnection;
  }
  return null;
}

export const unavailableHostCapabilities: WebRtcHostCapabilities = {
  enabled: false,
  availability: { state: 'unverified', reason: 'WebRTC capabilities are unavailable.' },
  codecs: {
    h264: { supported: false, hdr: false },
    hevc: { supported: false, hdr: false },
    av1: { supported: false, hdr: false },
  },
  hdr_policy: 'automatic',
  hdr_policy_allows: false,
  limits: defaultLimits,
};

function browserSupportsWebRtc(): boolean {
  return peerConnectionConstructor() !== null;
}

function videoRtpCapabilities(): RTCRtpCapabilities | null {
  try {
    const receiver =
      typeof RTCRtpReceiver !== 'undefined' ? RTCRtpReceiver.getCapabilities?.('video') : null;
    if (Array.isArray(receiver?.codecs) && receiver.codecs.length) return receiver;
  } catch {
    // Browsers are allowed to omit capability reporting.
  }
  try {
    const sender =
      typeof RTCRtpSender !== 'undefined' ? RTCRtpSender.getCapabilities?.('video') : null;
    if (Array.isArray(sender?.codecs) && sender.codecs.length) return sender;
  } catch {
    // Browsers are allowed to omit capability reporting.
  }
  return null;
}

function codecMimeType(codec: unknown): string {
  if (!codec || typeof codec !== 'object') return '';
  const mimeType = (codec as { mimeType?: unknown }).mimeType;
  return typeof mimeType === 'string' ? mimeType.toLocaleLowerCase() : '';
}

function supportsEncoding(
  capabilities: RTCRtpCapabilities | null,
  encoding: EncodingType,
): boolean {
  // H.264 Constrained Baseline is mandatory for WebRTC browsers. In
  // particular, WebKit has shipped versions where getCapabilities() was
  // unavailable or incomplete even though H.264 negotiation worked. Treat
  // the standard codec as the compatibility floor and use capability
  // reporting only for optional codecs.
  if (encoding === 'h264') return true;
  if (!capabilities?.codecs?.length) return false;
  const mimeTypes = videoMimeTypes[encoding];
  return capabilities.codecs.some((codec) => mimeTypes.includes(codecMimeType(codec)));
}

function parseFmtpValue(fmtp: string | undefined, key: string): string | null {
  if (typeof fmtp !== 'string' || !fmtp) return null;
  const entry = fmtp
    .split(';')
    .map((value) => value.trim())
    .find((value) => value.toLocaleLowerCase().startsWith(`${key.toLocaleLowerCase()}=`));
  return entry ? entry.slice(entry.indexOf('=') + 1).trim() : null;
}

function parseFmtpDecimal(value: string | null, defaultValue: number): number {
  if (value === null) return defaultValue;
  return /^\d+$/.test(value) ? Number(value) : Number.NaN;
}

function isHevcMain10Profile(profileId: string | null, profileSpace: string | null): boolean {
  // RFC 7798 Main10 is profile-space 0, profile-id 2. A missing profile-space
  // defaults to zero; a missing profile-id defaults to Main (1), not Main10.
  const parsedProfileId = parseFmtpDecimal(profileId, 1);
  const parsedProfileSpace = parseFmtpDecimal(profileSpace, 0);
  return parsedProfileSpace === 0 && parsedProfileId === 2;
}

function hasHevcMain10Capability(capabilities: RTCRtpCapabilities | null): boolean {
  if (!capabilities?.codecs?.length) return false;
  return capabilities.codecs.some((codec) => {
    if (!videoMimeTypes.hevc.includes(codecMimeType(codec))) return false;
    const profileId = parseFmtpValue(codec.sdpFmtpLine, 'profile-id');
    const profileSpace = parseFmtpValue(codec.sdpFmtpLine, 'profile-space');
    return isHevcMain10Profile(profileId, profileSpace);
  });
}

function browserHasHdrOutput(): boolean {
  try {
    return typeof window !== 'undefined' && window.matchMedia('(dynamic-range: high)').matches;
  } catch {
    return false;
  }
}

interface VideoDecodeRequest {
  bitrateKbps?: number;
  fps?: number;
  height?: number;
  width?: number;
}

function boundedPositive(value: number | undefined, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) && value > 0 ? value : fallback;
}

async function supportsTenBitDecode(
  contentType: string,
  request: VideoDecodeRequest = {},
): Promise<boolean> {
  if (!browserHasHdrOutput() || typeof navigator === 'undefined') return false;
  const mediaCapabilities = (
    navigator as Navigator & {
      mediaCapabilities?: {
        decodingInfo?: (configuration: Record<string, unknown>) => Promise<{ supported?: boolean }>;
      };
    }
  ).mediaCapabilities;
  if (!mediaCapabilities?.decodingInfo) return false;

  try {
    const result = await mediaCapabilities.decodingInfo({
      type: 'media-source',
      video: {
        bitrate: boundedPositive(request.bitrateKbps, 20_000) * 1000,
        contentType,
        framerate: boundedPositive(request.fps, 60),
        height: boundedPositive(request.height, 1080),
        width: boundedPositive(request.width, 1920),
      },
    });
    return result.supported === true;
  } catch {
    return false;
  }
}

export async function browserSupportsHdrStream(config: StreamConfig): Promise<boolean> {
  if (!browserSupportsWebRtc() || !browserHasHdrOutput()) return false;

  const capabilities = videoRtpCapabilities();
  if (!supportsEncoding(capabilities, config.encoding)) return false;

  if (config.encoding === 'hevc') {
    if (!hasHevcMain10Capability(capabilities)) return false;
    return supportsTenBitDecode('video/mp4; codecs="hvc1.2.4.L153.B0"', config);
  }
  if (config.encoding === 'av1') {
    return supportsTenBitDecode('video/webm; codecs="av01.0.08M.10"', config);
  }
  return false;
}

export async function detectBrowserVideoCapabilities(): Promise<BrowserVideoCapabilities> {
  const PeerConnection = peerConnectionConstructor();
  if (!PeerConnection) {
    return {
      h264: { supported: false, hdr: false },
      hevc: { supported: false, hdr: false },
      av1: { supported: false, hdr: false },
    };
  }

  let probe: RTCPeerConnection | null = null;
  try {
    // WebKit has shipped getCapabilities() returning null until its WebRTC
    // backend is initialized by a peer connection. Warm it up first, then use
    // the actual receive-only offer as the authoritative SDR codec list.
    probe = new PeerConnection();
    probe.addTransceiver('video', { direction: 'recvonly' });
    const capabilities = videoRtpCapabilities();
    let offerSdp = '';
    try {
      offerSdp = (await probe.createOffer({ offerToReceiveVideo: true })).sdp ?? '';
    } catch {
      // Capability inspection is still useful when an isolated offer probe is
      // unavailable. Normal connection negotiation remains the final test.
    }

    const offered = (encoding: EncodingType): boolean | null =>
      offerSdp ? encodingOfferedInSdp(offerSdp, encoding) : null;
    const hevc = offered('hevc') ?? supportsEncoding(capabilities, 'hevc');
    const av1 = offered('av1') ?? supportsEncoding(capabilities, 'av1');
    const [hevcDecode, av1Decode] = await Promise.all([
      hevc ? supportsTenBitDecode('video/mp4; codecs="hvc1.2.4.L153.B0"') : Promise.resolve(false),
      av1 ? supportsTenBitDecode('video/webm; codecs="av01.0.08M.10"') : Promise.resolve(false),
    ]);

    return {
      // H.264 Constrained Baseline is mandatory for WebRTC browsers. Treat a
      // missing Safari introspection result as unknown, never unsupported.
      h264: { supported: true, hdr: false },
      hevc: { supported: hevc, hdr: hevcDecode && hasHevcMain10Capability(capabilities) },
      av1: { supported: av1, hdr: av1Decode },
    };
  } catch {
    // Optional codec introspection must never disable the mandatory H.264
    // path. Safari has shipped partial capability objects across releases.
    return {
      h264: { supported: true, hdr: false },
      hevc: { supported: false, hdr: false },
      av1: { supported: false, hdr: false },
    };
  } finally {
    probe?.close();
  }
}

export async function fetchWebRtcHostCapabilities(): Promise<WebRtcHostCapabilities> {
  const payload = await apiGet<Partial<WebRtcHostCapabilities>>('/api/webrtc/capabilities');
  const codec = (encoding: EncodingType): WebRtcCodecCapability => ({
    supported: payload.codecs?.[encoding]?.supported === true,
    hdr: payload.codecs?.[encoding]?.hdr === true,
  });
  return {
    enabled: payload.enabled === true,
    availability: {
      state: payload.availability?.state ?? 'unverified',
      reason: payload.availability?.reason,
    },
    codecs: {
      h264: codec('h264'),
      hevc: codec('hevc'),
      av1: codec('av1'),
    },
    hdr_policy: payload.hdr_policy ?? 'automatic',
    hdr_policy_allows: payload.hdr_policy_allows === true,
    limits: { ...defaultLimits, ...payload.limits },
  };
}

function describeApiError(error: unknown, fallback: string): Error {
  if (error instanceof ApiError) {
    const payload = error.payload as { error?: unknown; message?: unknown } | null;
    const detail =
      typeof payload?.error === 'string'
        ? payload.error
        : typeof payload?.message === 'string'
          ? payload.message
          : '';
    return new Error(detail || `${fallback} (HTTP ${error.status})`);
  }
  return error instanceof Error ? error : new Error(fallback);
}

function encodingOfferedInSdp(sdp: string, encoding: EncodingType): boolean {
  const wanted = encoding === 'hevc' ? new Set(['h265', 'hevc']) : new Set([encoding]);
  let inVideo = false;
  for (const line of sdp.split(/\r?\n/)) {
    if (line.startsWith('m=')) {
      inVideo = line.startsWith('m=video');
      continue;
    }
    if (!inVideo || !line.startsWith('a=rtpmap:')) continue;
    const codecName = line
      .slice(line.indexOf(' ') + 1)
      .split('/')[0]
      ?.trim()
      .toLocaleLowerCase();
    if (codecName && wanted.has(codecName)) return true;
  }
  return false;
}

function preferredCodecs(encoding: EncodingType, hdr: boolean): RTCRtpCodec[] {
  const capabilities = videoRtpCapabilities();
  if (!capabilities?.codecs?.length) return [];
  let preferred = capabilities.codecs.filter((codec) =>
    videoMimeTypes[encoding].includes(codecMimeType(codec)),
  );
  if (hdr && encoding === 'hevc') {
    preferred = preferred.filter((codec) => {
      const profileId = parseFmtpValue(codec.sdpFmtpLine, 'profile-id');
      const profileSpace = parseFmtpValue(codec.sdpFmtpLine, 'profile-space');
      return isHevcMain10Profile(profileId, profileSpace);
    });
  }
  if (encoding === 'h264') {
    const packetizationMode1 = preferred.filter((codec) =>
      /(?:^|;)\s*packetization-mode=1(?:;|$)/i.test(codec.sdpFmtpLine ?? ''),
    );
    if (packetizationMode1.length) preferred = packetizationMode1;
  }
  if (!preferred.length) return [];

  // Keep the complete capability list in the offer. Firefox can leave orphaned
  // fmtp/rtcp-fb lines when every non-selected codec is removed, while the host
  // still enforces the selected codec when it accepts the offer.
  const preferredSet = new Set(preferred);
  return [...preferred, ...capabilities.codecs.filter((codec) => !preferredSet.has(codec))];
}

async function createSession(config: StreamConfig): Promise<CreatedWebRtcSession> {
  try {
    const payload = await apiPost<WebRtcSessionResponse>('/api/webrtc/sessions', {
      app_id: config.appId,
      audio: true,
      audio_channels: config.audioChannels ?? 2,
      audio_codec: config.audioCodec ?? 'opus',
      bitrate_kbps: config.bitrateKbps,
      codec: config.encoding,
      encoded: true,
      fps: config.fps,
      hdr: config.hdr === true,
      host_audio: !(config.muteHostAudio ?? true),
      height: config.height,
      profile: config.profile,
      resume: config.resume === true,
      video: true,
      video_max_frame_age_ms: config.videoMaxFrameAgeMs,
      video_pacing_mode: config.videoPacingMode,
      video_pacing_slack_ms: config.videoPacingSlackMs,
      width: config.width,
    });
    const id = payload.session?.id;
    if (!id) throw new Error('The host did not return a WebRTC session identifier.');
    return { id, iceServers: payload.ice_servers ?? [] };
  } catch (error) {
    throw describeApiError(error, 'Unable to create the browser stream.');
  }
}

async function waitForAnswer(
  sessionId: string,
  canceled: () => boolean,
): Promise<RTCSessionDescriptionInit> {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    if (canceled()) throw new WebRtcConnectionCanceledError();
    try {
      const result = await apiGet<WebRtcAnswerResponse>(
        `/api/webrtc/sessions/${encodeURIComponent(sessionId)}/answer`,
      );
      if (result.answer_ready && result.sdp) {
        return { type: result.type ?? 'answer', sdp: result.sdp };
      }
      if (result.error && result.error !== 'Answer not ready') throw new Error(result.error);
    } catch (error) {
      if (error instanceof ApiError && error.status === 404)
        throw describeApiError(error, 'WebRTC session disappeared.');
      if (error instanceof Error && error.message !== 'Answer not ready') throw error;
    }
    await new Promise<void>((resolve) => window.setTimeout(resolve, 250));
  }
  throw new Error('Timed out waiting for the WebRTC answer.');
}

async function deleteSessionQuietly(sessionId: string): Promise<void> {
  if (!sessionId) return;
  try {
    await apiDelete(`/api/webrtc/sessions/${encodeURIComponent(sessionId)}`);
  } catch {
    // The host also clears an already-ended session; no retry is useful here.
  }
}

export class BrowserWebRtcSession {
  private activeGeneration = 0;
  private dataChannel: RTCDataChannel | null = null;
  private eventSource: EventSource | null = null;
  private generation = 0;
  private localCandidateTimer: number | undefined;
  private localCandidates: RTCIceCandidateInit[] = [];
  private pendingRemoteCandidates: RTCIceCandidateInit[] = [];
  private receiverHintTimer: number | undefined;
  private remoteCandidateIndex = 0;
  private remoteCandidatePollTimer: number | undefined;
  private remoteStream: MediaStream | null = null;
  private sessionId = '';
  private videoJitterStats: VideoJitterStatsState = {};
  private videoLatencyTargetMs = 0;
  private videoStatsTimer: number | undefined;
  private peerConnection: RTCPeerConnection | null = null;

  get connected(): boolean {
    return this.peerConnection?.connectionState === 'connected';
  }

  sendInput(message: Record<string, unknown>): boolean {
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return false;
    try {
      this.dataChannel.send(JSON.stringify(message));
      return true;
    } catch {
      return false;
    }
  }

  requestLatencyResync(): boolean {
    return this.sendInput({ type: 'latency_resync' });
  }

  async connect(config: StreamConfig, callbacks: WebRtcConnectionCallbacks = {}): Promise<void> {
    const generation = ++this.generation;
    await this.disposeCurrentSession();
    this.throwIfCanceled(generation);

    const PeerConnection = peerConnectionConstructor();
    if (!PeerConnection) {
      throw new Error(
        'This browser does not provide the WebRTC APIs required for browser streaming.',
      );
    }

    const session = await createSession(config);
    if (!this.isCurrent(generation)) {
      await deleteSessionQuietly(session.id);
      throw new WebRtcConnectionCanceledError();
    }
    this.sessionId = session.id;
    this.activeGeneration = generation;
    this.videoLatencyTargetMs = normalizedLatencyTargetMs(
      config.videoMaxFrameAgeMs,
      Math.max(5, Math.min(100, 1000 / Math.max(1, config.fps))),
    );
    try {
      const remoteStream = typeof MediaStream === 'function' ? new MediaStream() : null;
      this.remoteStream = remoteStream;
      const connection = new PeerConnection({ iceServers: session.iceServers });
      this.peerConnection = connection;
      const video = connection.addTransceiver('video', { direction: 'recvonly' });
      connection.addTransceiver('audio', { direction: 'recvonly' });
      const codecs = preferredCodecs(config.encoding, config.hdr === true);
      if (codecs.length) {
        try {
          video.setCodecPreferences(codecs);
        } catch {
          // Safari versions with partial setCodecPreferences support can still
          // negotiate their default receive codecs. The SDP offer below is the
          // authoritative check, so do not reject before it exists.
        }
      }
      if (config.hdr) {
        const supportsHdr = await browserSupportsHdrStream(config);
        this.throwIfCanceled(generation);
        if (!supportsHdr) {
          throw new Error(
            `This browser cannot confirm HDR ${config.encoding.toUpperCase()} decoding at the selected stream settings.`,
          );
        }
      }

      // Abandon lost packets instead of retransmitting them, but preserve
      // event order so a delayed iPad pointer move cannot arrive after a tap
      // and pull the host cursor back to an older coordinate.
      this.dataChannel = connection.createDataChannel('input', {
        maxRetransmits: 0,
        ordered: true,
        priority: 'high',
      } as RTCDataChannelInit & { priority: 'high' });
      this.dataChannel.onopen = () => {
        if (this.isActiveGeneration(generation)) callbacks.onInputState?.('open');
      };
      this.dataChannel.onclose = () => {
        if (this.isActiveGeneration(generation)) callbacks.onInputState?.('closed');
      };
      this.dataChannel.onerror = () => {
        if (this.isActiveGeneration(generation)) callbacks.onInputState?.('closing');
      };

      connection.onconnectionstatechange = () => {
        if (this.isActiveGeneration(generation))
          callbacks.onConnectionState?.(connection.connectionState);
      };
      connection.ontrack = (event) => {
        if (!this.isActiveGeneration(generation)) return;
        if (event.track.kind === 'video') {
          try {
            event.track.contentHint = 'motion';
          } catch {
            // contentHint is advisory and unavailable in some Safari releases.
          }
          applyReceiverLatencyHints(event.receiver, this.videoLatencyTargetMs);
        } else if (event.track.kind === 'audio') {
          applyReceiverLatencyHints(event.receiver, 20);
        }
        const stream = remoteStream ?? event.streams[0];
        if (!stream) return;
        this.remoteStream = stream;
        if (remoteStream) {
          for (const track of stream.getTracks()) {
            if (track.kind === event.track.kind) stream.removeTrack(track);
          }
          stream.addTrack(event.track);
        }
        callbacks.onRemoteStream?.(stream);
      };
      this.startReceiverHintRefresh(generation);
      this.startVideoStatsPolling(connection, callbacks, generation);
      connection.onicecandidate = (event) => {
        if (event.candidate && this.isActiveGeneration(generation)) {
          this.queueLocalCandidate(event.candidate.toJSON(), generation);
        }
      };

      this.startRemoteCandidateSubscription(generation);
      const offer = await connection.createOffer({
        offerToReceiveAudio: true,
        offerToReceiveVideo: true,
      });
      this.throwIfCanceled(generation);
      if (!encodingOfferedInSdp(offer.sdp ?? '', config.encoding)) {
        throw new Error(
          `The browser did not offer ${config.encoding.toUpperCase()} for this stream.`,
        );
      }
      await connection.setLocalDescription(offer);
      this.throwIfCanceled(generation);
      const answer = await apiPost<WebRtcAnswerResponse>(
        `/api/webrtc/sessions/${encodeURIComponent(this.sessionId)}/offer`,
        { sdp: offer.sdp ?? '', type: offer.type },
      );
      this.throwIfCanceled(generation);
      if (answer.error && answer.error !== 'Answer not ready') {
        throw new Error(answer.error);
      }
      const remoteDescription =
        answer.answer_ready && answer.sdp
          ? { type: answer.type ?? 'answer', sdp: answer.sdp }
          : await waitForAnswer(this.sessionId, () => !this.isCurrent(generation));
      this.throwIfCanceled(generation);
      await connection.setRemoteDescription(remoteDescription);
      await this.flushRemoteCandidates(generation);
    } catch (error) {
      await this.disposeGeneration(generation);
      if (error instanceof WebRtcConnectionCanceledError || !this.isCurrent(generation)) {
        throw new WebRtcConnectionCanceledError();
      }
      throw describeApiError(error, 'Unable to negotiate the browser stream.');
    }
  }

  async disconnect(): Promise<void> {
    ++this.generation;
    await this.disposeCurrentSession();
  }

  private isCurrent(generation: number): boolean {
    return this.generation === generation;
  }

  private isActiveGeneration(generation: number): boolean {
    return this.generation === generation && this.activeGeneration === generation;
  }

  private throwIfCanceled(generation: number): void {
    if (!this.isCurrent(generation)) throw new WebRtcConnectionCanceledError();
  }

  private async disposeGeneration(generation: number): Promise<void> {
    if (this.activeGeneration === generation) await this.disposeCurrentSession();
  }

  private async disposeCurrentSession(): Promise<void> {
    const sessionId = this.sessionId;
    this.stopRemoteCandidateSubscription();
    this.stopLatencyMonitoring();
    if (this.localCandidateTimer) window.clearTimeout(this.localCandidateTimer);
    this.localCandidateTimer = undefined;
    this.localCandidates = [];
    this.pendingRemoteCandidates = [];
    this.remoteCandidateIndex = 0;

    try {
      this.dataChannel?.close();
    } catch {
      // Closing a completed channel is harmless.
    }
    this.dataChannel = null;
    try {
      this.peerConnection?.close();
    } catch {
      // Closing a completed connection is harmless.
    }
    this.peerConnection = null;
    this.remoteStream?.getTracks().forEach((track) => track.stop());
    this.remoteStream = null;
    this.videoJitterStats = {};
    this.sessionId = '';
    this.activeGeneration = 0;
    await deleteSessionQuietly(sessionId);
  }

  private startReceiverHintRefresh(generation: number): void {
    if (this.receiverHintTimer) return;
    this.receiverHintTimer = window.setInterval(() => {
      const connection = this.peerConnection;
      if (!connection || !this.isActiveGeneration(generation)) return;
      for (const receiver of connection.getReceivers()) {
        if (receiver.track?.kind === 'video') {
          applyReceiverLatencyHints(receiver, this.videoLatencyTargetMs);
        } else if (receiver.track?.kind === 'audio') {
          applyReceiverLatencyHints(receiver, 20);
        }
      }
    }, receiverHintRefreshMs);
  }

  private startVideoStatsPolling(
    connection: RTCPeerConnection,
    callbacks: WebRtcConnectionCallbacks,
    generation: number,
  ): void {
    if (this.videoStatsTimer) return;
    const poll = async (): Promise<void> => {
      if (this.peerConnection !== connection || !this.isActiveGeneration(generation)) return;
      try {
        const report = await connection.getStats();
        let inbound: RTCStats | undefined;
        report.forEach((entry) => {
          const candidate = entry as RTCStats & {
            isRemote?: boolean;
            kind?: string;
            mediaType?: string;
          };
          if (
            candidate.type === 'inbound-rtp' &&
            candidate.isRemote !== true &&
            (candidate.kind === 'video' || candidate.mediaType === 'video')
          ) {
            inbound = candidate;
          }
        });

        const sample = inbound as
          | (RTCStats & {
              jitterBufferDelay?: number;
              jitterBufferEmittedCount?: number;
            })
          | undefined;
        const delay = sample?.jitterBufferDelay;
        const emitted = sample?.jitterBufferEmittedCount;
        const previous = this.videoJitterStats;
        let delayMs: number | undefined;
        if (
          sample &&
          previous.id === sample.id &&
          typeof delay === 'number' &&
          typeof emitted === 'number' &&
          typeof previous.delay === 'number' &&
          typeof previous.emitted === 'number'
        ) {
          const deltaDelay = delay - previous.delay;
          const deltaEmitted = emitted - previous.emitted;
          if (deltaDelay >= 0 && deltaEmitted > 0) delayMs = (deltaDelay / deltaEmitted) * 1000;
        }
        this.videoJitterStats = { delay, emitted, id: sample?.id };
        callbacks.onVideoPlayoutDelay?.(delayMs);
      } catch {
        // Stats are diagnostic input only; streaming must continue without them.
      }

      if (this.peerConnection !== connection || !this.isActiveGeneration(generation)) return;
      this.videoStatsTimer = window.setTimeout(() => {
        this.videoStatsTimer = undefined;
        void poll();
      }, videoStatsPollMs);
    };
    void poll();
  }

  private stopLatencyMonitoring(): void {
    if (this.receiverHintTimer) window.clearInterval(this.receiverHintTimer);
    if (this.videoStatsTimer) window.clearTimeout(this.videoStatsTimer);
    this.receiverHintTimer = undefined;
    this.videoStatsTimer = undefined;
  }

  private queueLocalCandidate(candidate: RTCIceCandidateInit, generation: number): void {
    if (!this.isActiveGeneration(generation)) return;
    this.localCandidates.push(candidate);
    if (this.localCandidateTimer) return;
    this.localCandidateTimer = window.setTimeout(() => {
      this.localCandidateTimer = undefined;
      void this.flushLocalCandidates(generation);
    }, 75);
  }

  private async flushLocalCandidates(generation: number): Promise<void> {
    if (!this.isActiveGeneration(generation) || !this.sessionId || !this.localCandidates.length)
      return;
    const candidates = this.localCandidates.splice(0);
    try {
      await apiPost(`/api/webrtc/sessions/${encodeURIComponent(this.sessionId)}/ice`, {
        candidates: candidates
          .filter((candidate) => Boolean(candidate.candidate))
          .map((candidate) => ({
            candidate: candidate.candidate,
            sdpMid: candidate.sdpMid ?? '',
            sdpMLineIndex: candidate.sdpMLineIndex ?? -1,
          })),
      });
    } catch {
      // ICE gathering continues; a later candidate batch can still establish the session.
    }
  }

  private startRemoteCandidateSubscription(generation: number): void {
    if (!this.isActiveGeneration(generation) || !this.sessionId) return;
    try {
      const source = new EventSource(
        `/api/webrtc/sessions/${encodeURIComponent(this.sessionId)}/ice/stream?since=${this.remoteCandidateIndex}`,
      );
      source.addEventListener('candidate', (event) => {
        if (!this.isActiveGeneration(generation)) return;
        try {
          const candidate = JSON.parse((event as MessageEvent<string>).data) as NonNullable<
            WebRtcIceCandidateResponse['candidates']
          >[number];
          this.receiveRemoteCandidate(candidate, generation);
          const index = Number((event as MessageEvent<string>).lastEventId);
          if (Number.isFinite(index))
            this.remoteCandidateIndex = Math.max(this.remoteCandidateIndex, index);
        } catch {
          // Ignore malformed server-sent events and retain the polling fallback.
        }
      });
      source.onerror = () => {
        if (!this.isActiveGeneration(generation)) return;
        source.close();
        if (this.eventSource === source) this.eventSource = null;
        this.scheduleRemoteCandidatePoll(generation);
      };
      this.eventSource = source;
    } catch {
      this.scheduleRemoteCandidatePoll(generation);
    }
  }

  private scheduleRemoteCandidatePoll(generation: number): void {
    if (!this.isActiveGeneration(generation) || this.remoteCandidatePollTimer || !this.sessionId)
      return;
    this.remoteCandidatePollTimer = window.setTimeout(() => {
      this.remoteCandidatePollTimer = undefined;
      void this.pollRemoteCandidates(generation);
    }, 250);
  }

  private async pollRemoteCandidates(generation: number): Promise<void> {
    if (!this.isActiveGeneration(generation) || !this.sessionId) return;
    try {
      const result = await apiGet<WebRtcIceCandidateResponse>(
        `/api/webrtc/sessions/${encodeURIComponent(this.sessionId)}/ice?since=${this.remoteCandidateIndex}`,
      );
      for (const candidate of result.candidates ?? [])
        this.receiveRemoteCandidate(candidate, generation);
      if (typeof result.next_since === 'number') {
        this.remoteCandidateIndex = Math.max(this.remoteCandidateIndex, result.next_since);
      }
    } catch {
      // Keep polling while the session remains open.
    }
    this.scheduleRemoteCandidatePoll(generation);
  }

  private receiveRemoteCandidate(
    candidate: NonNullable<WebRtcIceCandidateResponse['candidates']>[number],
    generation: number,
  ): void {
    if (!this.isActiveGeneration(generation)) return;
    if (!candidate.candidate) return;
    if (typeof candidate.index === 'number') {
      this.remoteCandidateIndex = Math.max(this.remoteCandidateIndex, candidate.index);
    }
    const next = {
      candidate: candidate.candidate,
      sdpMid: candidate.sdpMid ?? null,
      sdpMLineIndex: candidate.sdpMLineIndex ?? null,
    } satisfies RTCIceCandidateInit;
    if (!this.peerConnection?.remoteDescription) {
      this.pendingRemoteCandidates.push(next);
      return;
    }
    void this.peerConnection.addIceCandidate(next).catch(() => undefined);
  }

  private async flushRemoteCandidates(generation: number): Promise<void> {
    const candidates = this.pendingRemoteCandidates.splice(0);
    for (const candidate of candidates) {
      if (this.isActiveGeneration(generation) && this.peerConnection) {
        await this.peerConnection.addIceCandidate(candidate).catch(() => undefined);
      }
    }
  }

  private stopRemoteCandidateSubscription(): void {
    this.eventSource?.close();
    this.eventSource = null;
    if (this.remoteCandidatePollTimer) window.clearTimeout(this.remoteCandidatePollTimer);
    this.remoteCandidatePollTimer = undefined;
  }
}
