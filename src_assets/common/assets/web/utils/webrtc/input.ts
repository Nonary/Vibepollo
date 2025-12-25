import { InputMessage } from '@/types/webrtc';

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
}

function modifiersFromEvent(event: KeyboardEvent | MouseEvent | WheelEvent | PointerEvent) {
  return {
    alt: event.altKey,
    ctrl: event.ctrlKey,
    shift: event.shiftKey,
    meta: event.metaKey,
  };
}

function shouldPreventDefaultKey(event: KeyboardEvent): boolean {
  if (event.code === 'Space' || event.key === ' ' || event.key === 'Spacebar') return true;
  if (event.code === 'Tab' || event.key === 'Tab') return true;
  if (event.code === 'MetaLeft' || event.code === 'MetaRight') return true;
  if (event.code === 'OSLeft' || event.code === 'OSRight') return true;
  if (event.key === 'Meta' || event.key === 'OS') return true;
  if (event.key === 'Alt' || event.key === 'AltGraph' || event.key === 'Control') return true;
  if (event.metaKey || event.altKey || event.ctrlKey) return true;
  return false;
}

function resolveInputRect(
  element: HTMLElement,
  video?: HTMLVideoElement | null,
): { rect: DOMRect; contentRect: DOMRect } {
  const rect = element.getBoundingClientRect();
  if (!video || !video.videoWidth || !video.videoHeight || rect.width <= 0 || rect.height <= 0) {
    return { rect, contentRect: rect };
  }

  const elementAspect = rect.width / rect.height;
  const videoAspect = video.videoWidth / video.videoHeight;
  let contentWidth = rect.width;
  let contentHeight = rect.height;
  let offsetX = 0;
  let offsetY = 0;

  if (videoAspect > elementAspect) {
    contentHeight = rect.width / videoAspect;
    offsetY = (rect.height - contentHeight) / 2;
  } else if (videoAspect < elementAspect) {
    contentWidth = rect.height * videoAspect;
    offsetX = (rect.width - contentWidth) / 2;
  }

  const contentRect = new DOMRect(
    rect.left + offsetX,
    rect.top + offsetY,
    contentWidth,
    contentHeight,
  );
  return { rect, contentRect };
}

function normalizePoint(
  event: MouseEvent | WheelEvent | PointerEvent,
  element: HTMLElement,
  video?: HTMLVideoElement | null,
) {
  const { contentRect } = resolveInputRect(element, video);
  const x = contentRect.width ? (event.clientX - contentRect.left) / contentRect.width : 0;
  const y = contentRect.height ? (event.clientY - contentRect.top) / contentRect.height : 0;
  return {
    x: Math.min(1, Math.max(0, x)),
    y: Math.min(1, Math.max(0, y)),
  };
}

export function attachInputCapture(
  element: HTMLElement,
  send: (payload: string) => void,
  options: InputCaptureOptions = {},
): () => void {
  const video = options.video ?? null;
  const onMetrics = options.onMetrics;
  let queuedMove: InputMessage | null = null;
  let queuedMoveAt = 0;
  let rafId = 0;
  const pressedKeys = new Map<string, { key: string; code: string }>();
  const supportsPointer = typeof window !== 'undefined' && 'PointerEvent' in window;
  const metrics: InputCaptureMetrics = {};
  let moveDelaySum = 0;
  let moveDelaySamples = 0;
  let moveEventLagSum = 0;
  let moveEventLagSamples = 0;
  let moveRateWindowStart = performance.now();
  let moveRateCount = 0;
  let moveSendRateCount = 0;
  let lastMetricsEmitAt = 0;

  const emitMetrics = () => {
    if (!onMetrics) return;
    const now = performance.now();
    if (now - lastMetricsEmitAt < 100) return;
    lastMetricsEmitAt = now;
    onMetrics({ ...metrics });
  };

  const flushMove = () => {
    rafId = 0;
    if (!queuedMove) return;
    const now = performance.now();
    const delayMs = Math.max(0, now - queuedMoveAt);
    moveDelaySum += delayMs;
    moveDelaySamples += 1;
    metrics.lastMoveDelayMs = delayMs;
    metrics.avgMoveDelayMs = moveDelaySum / moveDelaySamples;
    metrics.maxMoveDelayMs = Math.max(metrics.maxMoveDelayMs ?? 0, delayMs);
    moveSendRateCount += 1;
    const rateWindowMs = now - moveRateWindowStart;
    if (rateWindowMs >= 1000) {
      metrics.moveRateHz = Math.round((moveRateCount / rateWindowMs) * 1000);
      metrics.moveSendRateHz = Math.round((moveSendRateCount / rateWindowMs) * 1000);
      metrics.moveCoalesceRatio = moveRateCount ? moveSendRateCount / moveRateCount : undefined;
      moveRateWindowStart = now;
      moveRateCount = 0;
      moveSendRateCount = 0;
    }
    send(JSON.stringify(queuedMove));
    queuedMove = null;
    emitMetrics();
  };

  const releaseAllKeys = () => {
    if (!pressedKeys.size) return;
    const ts = performance.now();
    for (const entry of pressedKeys.values()) {
      const payload: InputMessage = {
        type: 'key_up',
        key: entry.key,
        code: entry.code,
        repeat: false,
        modifiers: { alt: false, ctrl: false, shift: false, meta: false },
        ts,
      };
      send(JSON.stringify(payload));
    }
    pressedKeys.clear();
  };

  const queueMove = (event: MouseEvent | PointerEvent) => {
    const { x, y } = normalizePoint(event, element, video);
    const now = performance.now();
    const eventLagMs = Math.max(0, now - event.timeStamp);
    moveEventLagSum += eventLagMs;
    moveEventLagSamples += 1;
    metrics.lastMoveEventLagMs = eventLagMs;
    metrics.avgMoveEventLagMs = moveEventLagSum / moveEventLagSamples;
    metrics.maxMoveEventLagMs = Math.max(metrics.maxMoveEventLagMs ?? 0, eventLagMs);
    queuedMoveAt = performance.now();
    queuedMove = {
      type: 'mouse_move',
      x,
      y,
      buttons: event.buttons,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    moveRateCount += 1;
    if (!rafId) rafId = requestAnimationFrame(flushMove);
  };

  const sendButton = (event: MouseEvent | PointerEvent, type: 'mouse_down' | 'mouse_up') => {
    const { x, y } = normalizePoint(event, element, video);
    const payload: InputMessage = {
      type,
      button: event.button,
      x,
      y,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    send(JSON.stringify(payload));
  };

  const onWheel = (event: WheelEvent) => {
    event.preventDefault();
    const { x, y } = normalizePoint(event, element, video);
    const payload: InputMessage = {
      type: 'wheel',
      dx: event.deltaX,
      dy: event.deltaY,
      x,
      y,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    send(JSON.stringify(payload));
  };

  const onKeyDown = (event: KeyboardEvent) => {
    if (shouldPreventDefaultKey(event)) {
      event.preventDefault();
      event.stopPropagation();
    }
    if (event.repeat) return;
    pressedKeys.set(event.code, { key: event.key, code: event.code });
    const payload: InputMessage = {
      type: 'key_down',
      key: event.key,
      code: event.code,
      repeat: event.repeat,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    send(JSON.stringify(payload));
  };

  const onKeyUp = (event: KeyboardEvent) => {
    if (shouldPreventDefaultKey(event)) {
      event.preventDefault();
      event.stopPropagation();
    }
    pressedKeys.delete(event.code);
    const payload: InputMessage = {
      type: 'key_up',
      key: event.key,
      code: event.code,
      repeat: event.repeat,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    send(JSON.stringify(payload));
  };

  const onMouseMove = (event: MouseEvent) => queueMove(event);
  const onMouseDown = (event: MouseEvent) => {
    element.focus();
    sendButton(event, 'mouse_down');
  };
  const onMouseUp = (event: MouseEvent) => sendButton(event, 'mouse_up');
  const onPointerMove = (event: PointerEvent) => {
    if (event.pointerType === 'touch') return;
    queueMove(event);
  };
  const onPointerDown = (event: PointerEvent) => {
    if (event.pointerType === 'touch') return;
    element.focus();
    try {
      element.setPointerCapture(event.pointerId);
    } catch {
      /* ignore */
    }
    sendButton(event, 'mouse_down');
  };
  const onPointerUp = (event: PointerEvent) => {
    if (event.pointerType === 'touch') return;
    sendButton(event, 'mouse_up');
    try {
      element.releasePointerCapture(event.pointerId);
    } catch {
      /* ignore */
    }
  };
  const onPointerCancel = (event: PointerEvent) => {
    if (event.pointerType === 'touch') return;
    try {
      element.releasePointerCapture(event.pointerId);
    } catch {
      /* ignore */
    }
  };
  const onContextMenu = (event: MouseEvent) => {
    event.preventDefault();
  };
  const onBlur = () => releaseAllKeys();
  const onVisibilityChange = () => {
    if (document.hidden) releaseAllKeys();
  };

  if (supportsPointer) {
    element.addEventListener('pointermove', onPointerMove);
    element.addEventListener('pointerdown', onPointerDown);
    element.addEventListener('pointerup', onPointerUp);
    element.addEventListener('pointercancel', onPointerCancel);
  } else {
    element.addEventListener('mousemove', onMouseMove);
    element.addEventListener('mousedown', onMouseDown);
    element.addEventListener('mouseup', onMouseUp);
  }
  element.addEventListener('wheel', onWheel, { passive: false });
  element.addEventListener('keydown', onKeyDown);
  element.addEventListener('keyup', onKeyUp);
  element.addEventListener('contextmenu', onContextMenu);
  element.addEventListener('blur', onBlur);
  window.addEventListener('blur', onBlur);
  document.addEventListener('visibilitychange', onVisibilityChange);

  return () => {
    if (rafId) cancelAnimationFrame(rafId);
    if (supportsPointer) {
      element.removeEventListener('pointermove', onPointerMove);
      element.removeEventListener('pointerdown', onPointerDown);
      element.removeEventListener('pointerup', onPointerUp);
      element.removeEventListener('pointercancel', onPointerCancel);
    } else {
      element.removeEventListener('mousemove', onMouseMove);
      element.removeEventListener('mousedown', onMouseDown);
      element.removeEventListener('mouseup', onMouseUp);
    }
    element.removeEventListener('wheel', onWheel);
    element.removeEventListener('keydown', onKeyDown);
    element.removeEventListener('keyup', onKeyUp);
    element.removeEventListener('contextmenu', onContextMenu);
    element.removeEventListener('blur', onBlur);
    window.removeEventListener('blur', onBlur);
    document.removeEventListener('visibilitychange', onVisibilityChange);
    releaseAllKeys();
  };
}
