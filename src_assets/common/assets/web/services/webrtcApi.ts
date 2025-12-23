import {
  StreamConfig,
  WebRtcAnswer,
  WebRtcOffer,
  WebRtcSessionInfo,
} from '@/types/webrtc';

export interface WebRtcApi {
  createSession(config: StreamConfig): Promise<WebRtcSessionInfo>;
  sendOffer(sessionId: string, offer: WebRtcOffer): Promise<WebRtcAnswer>;
  sendIceCandidate(sessionId: string, candidate: RTCIceCandidateInit): Promise<void>;
  subscribeRemoteCandidates(
    sessionId: string,
    onCandidate: (candidate: RTCIceCandidateInit) => void,
  ): () => void;
  endSession(sessionId: string): Promise<void>;
}

interface MockSession {
  id: string;
  pc: RTCPeerConnection;
  close: () => void;
  candidateHandlers: Set<(candidate: RTCIceCandidateInit) => void>;
}

const DEFAULT_ICE: RTCIceServer[] = [{ urls: ['stun:stun.l.google.com:19302'] }];

function randomId(): string {
  return Math.random().toString(36).slice(2, 10);
}

function createMockVideoStream(config: StreamConfig) {
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(320, config.width);
  canvas.height = Math.max(180, config.height);
  const ctx = canvas.getContext('2d');
  let running = true;

  const draw = () => {
    if (!ctx || !running) return;
    const { width, height } = canvas;
    const now = new Date();
    const gradient = ctx.createLinearGradient(0, 0, width, height);
    gradient.addColorStop(0, '#111827');
    gradient.addColorStop(1, '#2563eb');
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, width, height);

    ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
    ctx.font = '24px "Space Mono", monospace';
    ctx.fillText('Sunshine WebRTC Mock Stream', 24, 48);
    ctx.font = '16px "Space Mono", monospace';
    ctx.fillText(
      `${config.width}x${config.height} @ ${config.fps}fps (${config.encoding.toUpperCase()})`,
      24,
      80,
    );
    ctx.fillText(now.toLocaleTimeString(), 24, 108);

    ctx.fillStyle = 'rgba(15, 23, 42, 0.85)';
    ctx.fillRect(width - 220, height - 70, 196, 46);
    ctx.fillStyle = '#e2e8f0';
    ctx.font = '14px "Space Mono", monospace';
    ctx.fillText('Mock stream', width - 200, height - 42);

    requestAnimationFrame(draw);
  };
  draw();

  const stream = canvas.captureStream(Math.max(1, config.fps));
  return {
    stream,
    stop: () => {
      running = false;
      stream.getTracks().forEach((track) => track.stop());
    },
  };
}

function createMockAudioStream() {
  const audioCtx = new AudioContext();
  const oscillator = audioCtx.createOscillator();
  const gain = audioCtx.createGain();
  const destination = audioCtx.createMediaStreamDestination();
  oscillator.type = 'sine';
  oscillator.frequency.value = 440;
  gain.gain.value = 0.03;
  oscillator.connect(gain);
  gain.connect(destination);
  oscillator.start();

  return {
    stream: destination.stream,
    stop: () => {
      try {
        oscillator.stop();
      } catch {
        /* ignore */
      }
      try {
        audioCtx.close();
      } catch {
        /* ignore */
      }
      destination.stream.getTracks().forEach((track) => track.stop());
    },
  };
}

function applyEncodingPreference(pc: RTCPeerConnection, encoding: string): void {
  const sender = pc.getSenders().find((s) => s.track?.kind === 'video');
  if (!sender || typeof RTCRtpSender === 'undefined') return;
  const caps = RTCRtpSender.getCapabilities('video');
  if (!caps?.codecs) return;
  const preferred = encoding.toLowerCase();
  const preferredMime = {
    h264: 'video/h264',
    hevc: 'video/h265',
    av1: 'video/av1',
  }[preferred];
  if (!preferredMime) return;
  const preferredCodecs = caps.codecs.filter(
    (codec) => codec.mimeType.toLowerCase() === preferredMime,
  );
  if (!preferredCodecs.length) return;
  const others = caps.codecs.filter(
    (codec) => codec.mimeType.toLowerCase() !== preferredMime,
  );
  try {
    sender.setCodecPreferences([...preferredCodecs, ...others]);
  } catch {
    /* ignore */
  }
}

export class MockWebRtcApi implements WebRtcApi {
  private sessions = new Map<string, MockSession>();

  async createSession(config: StreamConfig): Promise<WebRtcSessionInfo> {
    const id = randomId();
    const pc = new RTCPeerConnection({
      iceServers: DEFAULT_ICE,
      bundlePolicy: 'max-bundle',
      rtcpMuxPolicy: 'require',
    });
    const video = createMockVideoStream(config);
    const audio = createMockAudioStream();
    video.stream.getTracks().forEach((track) => pc.addTrack(track, video.stream));
    audio.stream.getTracks().forEach((track) => pc.addTrack(track, audio.stream));

    const candidateHandlers = new Set<(candidate: RTCIceCandidateInit) => void>();
    pc.onicecandidate = (event) => {
      if (!event.candidate) return;
      candidateHandlers.forEach((handler) => handler(event.candidate.toJSON()));
    };

    pc.ondatachannel = (event) => {
      const channel = event.channel;
      channel.onmessage = () => {
        /* placeholder: input messages would be handled on the server */
      };
    };

    const close = () => {
      video.stop();
      audio.stop();
      pc.close();
    };

    this.sessions.set(id, { id, pc, close, candidateHandlers });
    applyEncodingPreference(pc, config.encoding);
    return { sessionId: id, iceServers: DEFAULT_ICE };
  }

  async sendOffer(sessionId: string, offer: WebRtcOffer): Promise<WebRtcAnswer> {
    const session = this.sessions.get(sessionId);
    if (!session) {
      throw new Error('Session not found');
    }
    await session.pc.setRemoteDescription(offer);
    const answer = await session.pc.createAnswer();
    await session.pc.setLocalDescription(answer);
    return { type: answer.type, sdp: answer.sdp ?? '' };
  }

  async sendIceCandidate(sessionId: string, candidate: RTCIceCandidateInit): Promise<void> {
    const session = this.sessions.get(sessionId);
    if (!session || !candidate) return;
    try {
      await session.pc.addIceCandidate(candidate);
    } catch {
      /* ignore */
    }
  }

  subscribeRemoteCandidates(
    sessionId: string,
    onCandidate: (candidate: RTCIceCandidateInit) => void,
  ): () => void {
    const session = this.sessions.get(sessionId);
    if (!session) return () => {};
    session.candidateHandlers.add(onCandidate);
    return () => session.candidateHandlers.delete(onCandidate);
  }

  async endSession(sessionId: string): Promise<void> {
    const session = this.sessions.get(sessionId);
    if (!session) return;
    session.close();
    this.sessions.delete(sessionId);
  }
}
