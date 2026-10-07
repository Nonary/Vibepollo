import type { GamepadFeedbackMessage, InputMessage } from '../../types/webrtc';

export interface InputCaptureMetrics {
  lastMoveDelayMs?: number;
  avgMoveDelayMs?: number;
  maxMoveDelayMs?: number;
  lastMoveEventLagMs?: number;
  avgMoveEventLagMs?: number;
  maxMoveEventLagMs?: number;
  moveRateHz?: number;
  moveSendRateHz?: number;
  moveCoalesceRatio?: number;
}

interface InputCaptureOptions {
  video?: HTMLVideoElement | null;
  onMetrics?: (metrics: InputCaptureMetrics) => void;
  gamepad?: boolean;
  shouldDrop?: (payload: InputMessage) => boolean;
}

export declare function requestKeyboardLock(keys?: string[]): Promise<boolean>;
export declare function releaseKeyboardLock(): void;
export declare function applyGamepadFeedback(message: GamepadFeedbackMessage | unknown): void;
export declare function attachInputCapture(
  element: HTMLElement,
  send: (payload: string | ArrayBuffer) => boolean | void,
  options?: InputCaptureOptions,
): () => void;
