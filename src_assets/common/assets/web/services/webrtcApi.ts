import { http } from '@/http';
import {
  StreamConfig,
  WebRtcIceCandidate,
  WebRtcAnswer,
  WebRtcOffer,
  WebRtcSessionInfo,
  WebRtcSessionState,
} from '@/types/webrtc';

export interface WebRtcApi {
  createSession(config: StreamConfig): Promise<WebRtcSessionInfo>;
  getSessionState(sessionId: string): Promise<WebRtcSessionFetchResult>;
  sendOffer(sessionId: string, offer: WebRtcOffer): Promise<WebRtcAnswer | null>;
  sendIceCandidates(sessionId: string, candidates: RTCIceCandidateInit[]): Promise<void>;
  sendIceCandidate(sessionId: string, candidate: RTCIceCandidateInit): Promise<void>;
  subscribeRemoteCandidates(
    sessionId: string,
    onCandidate: (candidate: RTCIceCandidateInit) => void,
  ): () => void;
  endSession(sessionId: string, options?: WebRtcSessionEndOptions): Promise<void>;
}

export interface WebRtcSessionFetchResult {
  status: number;
  session: WebRtcSessionState | null;
  error?: string;
}

interface WebRtcSessionResponse {
  status?: boolean;
  session?: {
    id: string;
  };
  cert_fingerprint?: string;
  cert_pem?: string;
  ice_servers?: RTCIceServer[];
}

interface WebRtcOfferResponse {
  status?: boolean;
  answer_ready?: boolean;
  sdp?: string;
  type?: RTCSdpType;
  error?: string;
}

interface WebRtcSessionStateResponse {
  session?: WebRtcSessionState;
  error?: string;
}

interface WebRtcIceResponse {
  status?: boolean;
  candidates?: WebRtcIceCandidate[];
  next_since?: number;
  error?: string;
}

export interface WebRtcSessionEndOptions {
  keepalive?: boolean;
}

export class WebRtcHttpApi implements WebRtcApi {
  async createSession(config: StreamConfig): Promise<WebRtcSessionInfo> {
    const muteHostAudio = config.muteHostAudio ?? true;
    const payload = {
      audio: true,
      host_audio: !muteHostAudio,
      video: true,
      encoded: true,
      width: config.width,
      height: config.height,
      fps: config.fps,
      bitrate_kbps: config.bitrateKbps,
      codec: config.encoding,
      hdr: config.hdr,
      audio_channels: config.audioChannels,
      audio_codec: config.audioCodec,
      profile: config.profile,
      app_id: config.appId,
      resume: config.resume,
      video_pacing_mode: config.videoPacingMode,
      video_pacing_slack_ms: config.videoPacingSlackMs,
      video_max_frame_age_ms: config.videoMaxFrameAgeMs,
    };
    const r = await http.post<WebRtcSessionResponse>('/api/webrtc/sessions', payload, {
      validateStatus: () => true,
    });
    if (r.status !== 200 || !r.data?.session?.id) {
      const detail = r.data ? JSON.stringify(r.data) : 'no response body';
      throw new Error(`Failed to create WebRTC session (HTTP ${r.status}): ${detail}`);
    }
    return {
      sessionId: r.data.session.id,
      iceServers: r.data.ice_servers ?? [],
      certFingerprint: r.data.cert_fingerprint,
      certPem: r.data.cert_pem,
    };
  }

  async getSessionState(sessionId: string): Promise<WebRtcSessionFetchResult> {
    const r = await http.get<WebRtcSessionStateResponse>(
      `/api/webrtc/sessions/${encodeURIComponent(sessionId)}`,
      { validateStatus: () => true },
    );
    if (r.status !== 200) {
      const error = r.data?.error ? String(r.data.error) : undefined;
      return { status: r.status, session: null, error };
    }
    return { status: r.status, session: r.data?.session ?? null, error: r.data?.error };
  }

  async sendOffer(sessionId: string, offer: WebRtcOffer): Promise<WebRtcAnswer | null> {
    const r = await http.post<WebRtcOfferResponse>(
      `/api/webrtc/sessions/${encodeURIComponent(sessionId)}/offer`,
      offer,
      { validateStatus: () => true },
    );
    if (r.status !== 200) {
      const detail = r.data ? JSON.stringify(r.data) : 'no response body';
      throw new Error(`Failed to post WebRTC offer (HTTP ${r.status}): ${detail}`);
    }
    if (r.data?.error && r.data.error !== 'Answer not ready') {
      throw new Error(`Failed to post WebRTC offer: ${r.data.error}`);
    }
    if (r.data?.answer_ready && r.data.sdp) {
      return { type: r.data.type ?? 'answer', sdp: r.data.sdp };
    }
    return this.waitForAnswer(sessionId);
  }

  async sendIceCandidate(sessionId: string, candidate: RTCIceCandidateInit): Promise<void> {
    await this.sendIceCandidates(sessionId, [candidate]);
  }

  async sendIceCandidates(sessionId: string, candidates: RTCIceCandidateInit[]): Promise<void> {
    const payload = candidates
      .filter((candidate) => Boolean(candidate.candidate))
      .slice(0, 256)
      .map((candidate) => ({
        sdpMid: candidate.sdpMid,
        sdpMLineIndex: candidate.sdpMLineIndex,
        candidate: candidate.candidate,
      }));
    if (!payload.length) return;
    await http.post(
      `/api/webrtc/sessions/${encodeURIComponent(sessionId)}/ice`,
      { candidates: payload },
      { validateStatus: () => true },
    );
  }

  subscribeRemoteCandidates(
    sessionId: string,
    onCandidate: (candidate: RTCIceCandidateInit) => void,
  ): () => void {
    let stopped = false;
    let lastIndex = 0;
    let pollTimer: number | undefined;
    let eventSource: EventSource | null = null;

    const stopPolling = () => {
      if (pollTimer) {
        window.clearTimeout(pollTimer);
        pollTimer = undefined;
      }
    };

    const poll = async () => {
      if (stopped) return;
      try {
        const r = await http.get<WebRtcIceResponse>(
          `/api/webrtc/sessions/${encodeURIComponent(sessionId)}/ice`,
          { params: { since: lastIndex }, validateStatus: () => true },
        );
        if (r.status === 200 && Array.isArray(r.data?.candidates)) {
          for (const candidate of r.data.candidates) {
            onCandidate({
              sdpMid: candidate.sdpMid,
              sdpMLineIndex: candidate.sdpMLineIndex,
              candidate: candidate.candidate,
            });
            if (typeof candidate.index === 'number') {
              lastIndex = Math.max(lastIndex, candidate.index);
            }
          }
          if (typeof r.data.next_since === 'number') {
            lastIndex = Math.max(lastIndex, r.data.next_since);
          }
        }
      } catch {
        /* ignore */
      }
      if (!stopped) {
        pollTimer = window.setTimeout(poll, 1000);
      }
    };

    const startPolling = () => {
      if (pollTimer || stopped) return;
      poll();
    };

    try {
      eventSource = new EventSource(
        `/api/webrtc/sessions/${encodeURIComponent(sessionId)}/ice/stream?since=${lastIndex}`,
      );
      eventSource.addEventListener('candidate', (event) => {
        if (stopped) return;
        try {
          const payload = JSON.parse((event as MessageEvent).data) as WebRtcIceCandidate;
          onCandidate({
            sdpMid: payload.sdpMid,
            sdpMLineIndex: payload.sdpMLineIndex,
            candidate: payload.candidate,
          });
          const id = (event as MessageEvent).lastEventId;
          if (id) {
            const parsed = Number.parseInt(id, 10);
            if (!Number.isNaN(parsed)) {
              lastIndex = Math.max(lastIndex, parsed);
            }
          }
        } catch {
          /* ignore */
        }
      });
      eventSource.addEventListener('keepalive', () => {
        /* no-op */
      });
      eventSource.onerror = () => {
        if (stopped) return;
        eventSource?.close();
        eventSource = null;
        startPolling();
      };
    } catch {
      startPolling();
    }

    return () => {
      stopped = true;
      stopPolling();
      if (eventSource) {
        eventSource.close();
        eventSource = null;
      }
    };
  }

  async endSession(sessionId: string, options?: WebRtcSessionEndOptions): Promise<void> {
    if (options?.keepalive && typeof fetch === 'function') {
      try {
        await fetch(`/api/webrtc/sessions/${encodeURIComponent(sessionId)}`, {
          method: 'DELETE',
          keepalive: true,
          credentials: 'include',
          headers: {
            'X-Requested-With': 'XMLHttpRequest',
          },
        });
        return;
      } catch {
        /* ignore */
      }
    }
    await http.delete(`/api/webrtc/sessions/${encodeURIComponent(sessionId)}`, {
      validateStatus: () => true,
    });
  }

  private async waitForAnswer(sessionId: string): Promise<WebRtcAnswer | null> {
    const start = Date.now();
    const timeoutMs = 30000;
    while (Date.now() - start < timeoutMs) {
      try {
        const r = await http.get<WebRtcOfferResponse>(
          `/api/webrtc/sessions/${encodeURIComponent(sessionId)}/answer`,
          { validateStatus: () => true },
        );
        if (r.status === 200 && r.data?.error && r.data.error !== 'Answer not ready') {
          throw new Error(`Failed to fetch WebRTC answer: ${r.data.error}`);
        }
        if (r.status === 200 && r.data?.sdp) {
          return { type: r.data.type ?? 'answer', sdp: r.data.sdp };
        }
        if (r.status === 400 && r.data?.error && r.data.error !== 'Answer not ready') {
          throw new Error(`Failed to fetch WebRTC answer: ${r.data.error}`);
        }
      } catch {
        /* ignore */
      }
      await new Promise((resolve) => window.setTimeout(resolve, 300));
    }
    return null;
  }
}
