import { InputMessage } from '@/types/webrtc';

function modifiersFromEvent(event: KeyboardEvent | MouseEvent | WheelEvent) {
  return {
    alt: event.altKey,
    ctrl: event.ctrlKey,
    shift: event.shiftKey,
    meta: event.metaKey,
  };
}

function normalizePoint(event: MouseEvent | WheelEvent, element: HTMLElement) {
  const rect = element.getBoundingClientRect();
  const x = rect.width ? (event.clientX - rect.left) / rect.width : 0;
  const y = rect.height ? (event.clientY - rect.top) / rect.height : 0;
  return {
    x: Math.min(1, Math.max(0, x)),
    y: Math.min(1, Math.max(0, y)),
  };
}

export function attachInputCapture(
  element: HTMLElement,
  send: (payload: string) => void,
): () => void {
  let queuedMove: InputMessage | null = null;
  let rafId = 0;

  const flushMove = () => {
    rafId = 0;
    if (!queuedMove) return;
    send(JSON.stringify(queuedMove));
    queuedMove = null;
  };

  const onMouseMove = (event: MouseEvent) => {
    const { x, y } = normalizePoint(event, element);
    queuedMove = {
      type: 'mouse_move',
      x,
      y,
      buttons: event.buttons,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    if (!rafId) rafId = requestAnimationFrame(flushMove);
  };

  const onMouseDown = (event: MouseEvent) => {
    const { x, y } = normalizePoint(event, element);
    const payload: InputMessage = {
      type: 'mouse_down',
      button: event.button,
      x,
      y,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    element.focus();
    send(JSON.stringify(payload));
  };

  const onMouseUp = (event: MouseEvent) => {
    const { x, y } = normalizePoint(event, element);
    const payload: InputMessage = {
      type: 'mouse_up',
      button: event.button,
      x,
      y,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    send(JSON.stringify(payload));
  };

  const onWheel = (event: WheelEvent) => {
    const { x, y } = normalizePoint(event, element);
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
    if (event.repeat) return;
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

  element.addEventListener('mousemove', onMouseMove);
  element.addEventListener('mousedown', onMouseDown);
  element.addEventListener('mouseup', onMouseUp);
  element.addEventListener('wheel', onWheel);
  element.addEventListener('keydown', onKeyDown);
  element.addEventListener('keyup', onKeyUp);

  return () => {
    if (rafId) cancelAnimationFrame(rafId);
    element.removeEventListener('mousemove', onMouseMove);
    element.removeEventListener('mousedown', onMouseDown);
    element.removeEventListener('mouseup', onMouseUp);
    element.removeEventListener('wheel', onWheel);
    element.removeEventListener('keydown', onKeyDown);
    element.removeEventListener('keyup', onKeyUp);
  };
}
