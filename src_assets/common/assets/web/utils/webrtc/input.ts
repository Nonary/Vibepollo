import { GamepadFeedbackMessage, InputMessage } from '@/types/webrtc';

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

const WHEEL_STEP_PIXELS = 120;
const SYSTEM_KEY_CODES = [
  'AltLeft',
  'AltRight',
  'ControlLeft',
  'ControlRight',
  'MetaLeft',
  'MetaRight',
  'OSLeft',
  'OSRight',
];

function getKeyboardLockApi(): {
  lock?: (keys?: string[]) => Promise<void>;
  unlock?: () => void;
} | null {
  if (typeof navigator === 'undefined') return null;
  const anyNavigator = navigator as Navigator & {
    keyboard?: { lock?: (keys?: string[]) => Promise<void>; unlock?: () => void };
  };
  return anyNavigator.keyboard ?? null;
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

function normalizeWheelDelta(delta: number, deltaMode: number): number {
  if (deltaMode === WheelEvent.DOM_DELTA_PIXEL) {
    return delta / WHEEL_STEP_PIXELS;
  }
  return delta;
}

const MAX_GAMEPADS = 16;
const AXIS_DEADZONE = 0.08;
const MOTION_SEND_INTERVAL_MS = 16;
const MOTION_DIFF_THRESHOLD = 0.1;
const GAMEPAD_STATE_HEARTBEAT_MS = 500;

const activeGamepads = new Map<number, Gamepad>();
const motionRequestState = new Map<number, { gyro: boolean; accel: boolean }>();

const GAMEPAD_TYPE = {
  unknown: 0,
  xbox: 1,
  playstation: 2,
  nintendo: 3,
} as const;

const GAMEPAD_CAPS = {
  analogTriggers: 0x01,
  touchpad: 0x08,
  accel: 0x10,
  gyro: 0x20,
} as const;

const GAMEPAD_BUTTONS = {
  dpadUp: 0x0001,
  dpadDown: 0x0002,
  dpadLeft: 0x0004,
  dpadRight: 0x0008,
  start: 0x0010,
  back: 0x0020,
  leftStick: 0x0040,
  rightStick: 0x0080,
  leftButton: 0x0100,
  rightButton: 0x0200,
  home: 0x0400,
  a: 0x1000,
  b: 0x2000,
  x: 0x4000,
  y: 0x8000,
  paddle1: 0x010000,
  paddle2: 0x020000,
  paddle3: 0x040000,
  paddle4: 0x080000,
  touchpadButton: 0x100000,
  miscButton: 0x200000,
} as const;

const STANDARD_BUTTON_MAP = new Map<number, number>([
  [0, GAMEPAD_BUTTONS.a],
  [1, GAMEPAD_BUTTONS.b],
  [2, GAMEPAD_BUTTONS.x],
  [3, GAMEPAD_BUTTONS.y],
  [4, GAMEPAD_BUTTONS.leftButton],
  [5, GAMEPAD_BUTTONS.rightButton],
  [8, GAMEPAD_BUTTONS.back],
  [9, GAMEPAD_BUTTONS.start],
  [10, GAMEPAD_BUTTONS.leftStick],
  [11, GAMEPAD_BUTTONS.rightStick],
  [12, GAMEPAD_BUTTONS.dpadUp],
  [13, GAMEPAD_BUTTONS.dpadDown],
  [14, GAMEPAD_BUTTONS.dpadLeft],
  [15, GAMEPAD_BUTTONS.dpadRight],
  [16, GAMEPAD_BUTTONS.home],
  [17, GAMEPAD_BUTTONS.miscButton],
]);

type GamepadVector = [number, number, number];

interface GamepadSnapshot {
  buttons: number;
  lt: number;
  rt: number;
  lsX: number;
  lsY: number;
  rsX: number;
  rsY: number;
}

interface GamepadMeta {
  buttonMap: Map<number, number>;
  supportedButtons: number;
  capabilities: number;
  type: number;
  lastGyro?: GamepadVector;
  lastAccel?: GamepadVector;
  lastGyroAt?: number;
  lastAccelAt?: number;
  connected: boolean;
  needsResync: boolean;
  lastStateSentAt?: number;
}

function resolveGamepadType(gamepad: Gamepad): number {
  const id = (gamepad.id || '').toLowerCase();
  if (id.includes('nintendo') || id.includes('switch') || id.includes('joy-con')) {
    return GAMEPAD_TYPE.nintendo;
  }
  if (
    id.includes('playstation') ||
    id.includes('dualshock') ||
    id.includes('dualsense') ||
    id.includes('ps4') ||
    id.includes('ps5')
  ) {
    return GAMEPAD_TYPE.playstation;
  }
  if (id.includes('xbox')) {
    return GAMEPAD_TYPE.xbox;
  }
  if (id.includes('wireless controller')) {
    return GAMEPAD_TYPE.playstation;
  }
  return GAMEPAD_TYPE.unknown;
}

function resolveButtonMap(gamepad: Gamepad, type: number): Map<number, number> {
  const map = new Map(STANDARD_BUTTON_MAP);
  if (type === GAMEPAD_TYPE.playstation) {
    map.set(17, GAMEPAD_BUTTONS.touchpadButton);
  }
  if (gamepad.buttons.length > 17) {
    map.set(18, GAMEPAD_BUTTONS.paddle1);
  }
  if (gamepad.buttons.length > 18) {
    map.set(19, GAMEPAD_BUTTONS.paddle2);
  }
  if (gamepad.buttons.length > 19) {
    map.set(20, GAMEPAD_BUTTONS.paddle3);
  }
  if (gamepad.buttons.length > 20) {
    map.set(21, GAMEPAD_BUTTONS.paddle4);
  }
  return map;
}

function applyDeadzone(value: number, deadzone: number): number {
  const abs = Math.abs(value);
  if (abs <= deadzone) return 0;
  const scaled = (abs - deadzone) / (1 - deadzone);
  return Math.min(1, Math.max(0, scaled)) * Math.sign(value);
}

function toInt16(value: number): number {
  const clamped = Math.min(1, Math.max(-1, value));
  return Math.round(clamped * 32767);
}

function toUint8(value: number): number {
  const clamped = Math.min(1, Math.max(0, value));
  return Math.round(clamped * 255);
}

function readButtons(gamepad: Gamepad, buttonMap: Map<number, number>): number {
  let mask = 0;
  buttonMap.forEach((bit, index) => {
    const button = gamepad.buttons[index];
    if (button?.pressed) {
      mask |= bit;
    }
  });
  return mask;
}

function readGamepadState(gamepad: Gamepad, buttonMap: Map<number, number>): GamepadSnapshot {
  const axes = gamepad.axes || [];
  const lx = applyDeadzone(axes[0] ?? 0, AXIS_DEADZONE);
  const ly = applyDeadzone(-(axes[1] ?? 0), AXIS_DEADZONE);
  const rx = applyDeadzone(axes[2] ?? 0, AXIS_DEADZONE);
  const ry = applyDeadzone(-(axes[3] ?? 0), AXIS_DEADZONE);
  const lt = toUint8(gamepad.buttons[6]?.value ?? 0);
  const rt = toUint8(gamepad.buttons[7]?.value ?? 0);
  return {
    buttons: readButtons(gamepad, buttonMap),
    lt,
    rt,
    lsX: toInt16(lx),
    lsY: toInt16(ly),
    rsX: toInt16(rx),
    rsY: toInt16(ry),
  };
}

function readMotionVector(value: unknown): GamepadVector | undefined {
  if (!value || typeof value !== 'object') return undefined;
  const array = Array.isArray(value)
    ? value
    : (value as { length?: number; [index: number]: number });
  if (typeof array.length !== 'number' || array.length < 3) return undefined;
  const x = Number(array[0]);
  const y = Number(array[1]);
  const z = Number(array[2]);
  if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return undefined;
  return [x, y, z];
}

function readGamepadMotion(gamepad: Gamepad): { gyro?: GamepadVector; accel?: GamepadVector } {
  const pose = (
    gamepad as { pose?: { angularVelocity?: unknown; linearAcceleration?: unknown } | null }
  ).pose;
  const motion = (
    gamepad as { motion?: { angularVelocity?: unknown; linearAcceleration?: unknown } }
  ).motion;
  const motionData = (
    gamepad as { motionData?: { angularVelocity?: unknown; linearAcceleration?: unknown } }
  ).motionData;
  const source = motion ?? motionData ?? pose ?? null;
  if (!source) return {};
  const gyro = readMotionVector(source.angularVelocity);
  const accel = readMotionVector(source.linearAcceleration);
  return { gyro, accel };
}

function motionChanged(previous: GamepadVector | undefined, next: GamepadVector): boolean {
  if (!previous) return true;
  return (
    Math.abs(previous[0] - next[0]) > MOTION_DIFF_THRESHOLD ||
    Math.abs(previous[1] - next[1]) > MOTION_DIFF_THRESHOLD ||
    Math.abs(previous[2] - next[2]) > MOTION_DIFF_THRESHOLD
  );
}

function getHapticActuator(gamepad: Gamepad): GamepadHapticActuator | null {
  const direct = (gamepad as { vibrationActuator?: GamepadHapticActuator }).vibrationActuator;
  if (direct?.playEffect) {
    return direct;
  }
  const haptics = (gamepad as { hapticActuators?: GamepadHapticActuator[] }).hapticActuators;
  if (haptics?.length && haptics[0]?.playEffect) {
    return haptics[0];
  }
  return null;
}

function clampMagnitude(value: number): number {
  if (!Number.isFinite(value)) return 0;
  return Math.min(1, Math.max(0, value));
}

function setMotionRequest(id: number, motionType: number, enabled: boolean): void {
  const state = motionRequestState.get(id) ?? { gyro: true, accel: true };
  if (motionType === 2) {
    state.gyro = enabled;
  } else if (motionType === 1) {
    state.accel = enabled;
  }
  motionRequestState.set(id, state);
}

function getGamepads(): (Gamepad | null)[] {
  if (typeof navigator === 'undefined') return [];
  const fallback = (navigator as Navigator & { webkitGetGamepads?: () => (Gamepad | null)[] })
    .webkitGetGamepads;
  const pads = navigator.getGamepads?.() ?? fallback?.() ?? [];
  return Array.isArray(pads) ? pads : Array.from(pads);
}

function isGamepadConnected(gamepad: Gamepad): boolean {
  if (typeof gamepad.connected === 'boolean') return gamepad.connected;
  return true;
}

export function applyGamepadFeedback(message: GamepadFeedbackMessage | unknown): void {
  if (!message || typeof message !== 'object') return;
  const payload = message as GamepadFeedbackMessage;
  if (payload.type !== 'gamepad_feedback') return;
  const id = Number(payload.id);
  if (!Number.isFinite(id)) return;

  if (payload.event === 'motion_event_state') {
    const motionType = Number(payload.motionType);
    const reportRate = Number(payload.reportRate);
    if (Number.isFinite(motionType)) {
      setMotionRequest(id, motionType, reportRate > 0);
    }
    return;
  }

  if (payload.event !== 'rumble' && payload.event !== 'rumble_triggers') {
    return;
  }

  const gamepad = activeGamepads.get(id) ?? getGamepads()[id];
  if (!gamepad) return;
  const actuator = getHapticActuator(gamepad);
  if (!actuator) return;

  let strong = clampMagnitude((payload.lowfreq ?? 0) / 65535);
  let weak = clampMagnitude((payload.highfreq ?? 0) / 65535);
  if (payload.event === 'rumble_triggers') {
    strong = clampMagnitude((payload.left ?? 0) / 65535);
    weak = clampMagnitude((payload.right ?? 0) / 65535);
  }

  try {
    void actuator.playEffect('dual-rumble', {
      duration: 100,
      strongMagnitude: strong,
      weakMagnitude: weak,
    });
  } catch {
    /* ignore */
  }
}

export function attachInputCapture(
  element: HTMLElement,
  send: (payload: string | ArrayBuffer) => boolean | void,
  options: InputCaptureOptions = {},
): () => void {
  const video = options.video ?? null;
  const onMetrics = options.onMetrics;
  const gamepadEnabled = options.gamepad ?? true;
  const shouldDrop = options.shouldDrop;
  let queuedMove: InputMessage | null = null;
  let queuedMoveAt = 0;
  let rafId = 0;
  let mouseMoveSeq = 0;
  const pressedKeys = new Map<string, { key: string; code: string }>();
  const supportsPointer = typeof window !== 'undefined' && 'PointerEvent' in window;
  const supportsGamepad =
    gamepadEnabled &&
    typeof navigator !== 'undefined' &&
    (typeof navigator.getGamepads === 'function' ||
      typeof (navigator as Navigator & { webkitGetGamepads?: () => (Gamepad | null)[] })
        .webkitGetGamepads === 'function');
  const metrics: InputCaptureMetrics = {};
  let moveDelaySum = 0;
  let moveDelaySamples = 0;
  let moveEventLagSum = 0;
  let moveEventLagSamples = 0;
  let moveRateWindowStart = performance.now();
  let moveRateCount = 0;
  let moveSendRateCount = 0;
  let lastMetricsEmitAt = 0;
  const toU16Unit = (value: number) => Math.round(Math.min(1, Math.max(0, value)) * 65535);
  const encodeMouseMove = (payload: InputMessage & { type: 'mouse_move' }) => {
    const out = new ArrayBuffer(7);
    const view = new DataView(out);
    view.setUint8(0, 1);
    view.setUint16(1, mouseMoveSeq, true);
    view.setUint16(3, toU16Unit(payload.x), true);
    view.setUint16(5, toU16Unit(payload.y), true);
    mouseMoveSeq = (mouseMoveSeq + 1) & 0xffff;
    return out;
  };
  const sendPayload = (payload: InputMessage) => {
    if (shouldDrop?.(payload)) {
      return false;
    }
    if (payload.type === 'mouse_move') {
      return send(encodeMouseMove(payload)) !== false;
    }
    return send(JSON.stringify(payload)) !== false;
  };
  const keyboardLockApi = getKeyboardLockApi();
  let keyboardLocked = false;

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
    sendPayload(queuedMove);
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
      sendPayload(payload);
    }
    pressedKeys.clear();
  };

  const requestKeyboardLock = () => {
    if (!keyboardLockApi?.lock || keyboardLocked) return;
    keyboardLockApi.lock(SYSTEM_KEY_CODES).then(
      () => {
        keyboardLocked = true;
      },
      () => {
        /* ignore */
      },
    );
  };

  const releaseKeyboardLock = () => {
    if (!keyboardLocked) return;
    try {
      keyboardLockApi?.unlock?.();
    } catch {
      /* ignore */
    }
    keyboardLocked = false;
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
    sendPayload(payload);
  };

  const onWheel = (event: WheelEvent) => {
    event.preventDefault();
    const { x, y } = normalizePoint(event, element, video);
    const dx = normalizeWheelDelta(event.deltaX, event.deltaMode);
    const dy = normalizeWheelDelta(event.deltaY, event.deltaMode);
    const payload: InputMessage = {
      type: 'wheel',
      dx,
      dy,
      x,
      y,
      modifiers: modifiersFromEvent(event),
      ts: performance.now(),
    };
    sendPayload(payload);
  };

  const onKeyDown = (event: KeyboardEvent) => {
    requestKeyboardLock();
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
    sendPayload(payload);
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
    sendPayload(payload);
  };

  const onMouseMove = (event: MouseEvent) => queueMove(event);
  const onMouseDown = (event: MouseEvent) => {
    element.focus();
    requestKeyboardLock();
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
    requestKeyboardLock();
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
  const onBlur = () => {
    releaseAllKeys();
    releaseKeyboardLock();
  };
  const onVisibilityChange = () => {
    if (document.hidden) {
      releaseAllKeys();
      releaseKeyboardLock();
    }
  };

  const gamepadStates = new Map<number, GamepadSnapshot>();
  const gamepadMeta = new Map<number, GamepadMeta>();
  let gamepadRaf = 0;

  const ensureGamepadMeta = (gamepad: Gamepad) => {
    const existing = gamepadMeta.get(gamepad.index);
    if (existing) return existing;
    const type = resolveGamepadType(gamepad);
    const buttonMap = resolveButtonMap(gamepad, type);
    let supportedButtons = 0;
    buttonMap.forEach((bit) => {
      supportedButtons |= bit;
    });
    const motion = readGamepadMotion(gamepad);
    const hasGyro = Boolean(motion.gyro);
    const hasAccel = Boolean(motion.accel);
    let capabilities = 0;
    if (gamepad.buttons.length > 6 || gamepad.buttons.length > 7) {
      capabilities |= GAMEPAD_CAPS.analogTriggers;
    }
    if (hasAccel || type === GAMEPAD_TYPE.playstation) {
      capabilities |= GAMEPAD_CAPS.accel;
    }
    if (hasGyro || type === GAMEPAD_TYPE.playstation) {
      capabilities |= GAMEPAD_CAPS.gyro;
    }
    const meta: GamepadMeta = {
      buttonMap,
      supportedButtons,
      capabilities,
      type,
      connected: false,
      needsResync: true,
    };
    gamepadMeta.set(gamepad.index, meta);
    return meta;
  };

  const sendGamepadConnect = (gamepad: Gamepad, meta: GamepadMeta) => {
    const payload: InputMessage = {
      type: 'gamepad_connect',
      id: gamepad.index,
      gamepadType: meta.type,
      capabilities: meta.capabilities,
      supportedButtons: meta.supportedButtons,
      ts: performance.now(),
    };
    return sendPayload(payload);
  };

  const sendGamepadDisconnect = (index: number, activeMask: number) => {
    const payload: InputMessage = {
      type: 'gamepad_disconnect',
      id: index,
      activeMask,
      ts: performance.now(),
    };
    return sendPayload(payload);
  };

  const maybeSendMotion = (
    index: number,
    meta: GamepadMeta,
    motion: { gyro?: GamepadVector; accel?: GamepadVector },
    now: number,
  ) => {
    const motionState = motionRequestState.get(index);
    const gyroEnabled = motionState ? motionState.gyro : true;
    const accelEnabled = motionState ? motionState.accel : true;
    if (motion.gyro && gyroEnabled) {
      const lastAt = meta.lastGyroAt ?? 0;
      if (now - lastAt >= MOTION_SEND_INTERVAL_MS && motionChanged(meta.lastGyro, motion.gyro)) {
        meta.lastGyroAt = now;
        meta.lastGyro = motion.gyro;
        const payload: InputMessage = {
          type: 'gamepad_motion',
          id: index,
          motionType: 2,
          x: (motion.gyro[0] * 180) / Math.PI,
          y: (motion.gyro[1] * 180) / Math.PI,
          z: (motion.gyro[2] * 180) / Math.PI,
          ts: now,
        };
        sendPayload(payload);
      }
    }
    if (motion.accel && accelEnabled) {
      const lastAt = meta.lastAccelAt ?? 0;
      if (now - lastAt >= MOTION_SEND_INTERVAL_MS && motionChanged(meta.lastAccel, motion.accel)) {
        meta.lastAccelAt = now;
        meta.lastAccel = motion.accel;
        const payload: InputMessage = {
          type: 'gamepad_motion',
          id: index,
          motionType: 1,
          x: motion.accel[0],
          y: motion.accel[1],
          z: motion.accel[2],
          ts: now,
        };
        sendPayload(payload);
      }
    }
  };

  const pollGamepads = () => {
    gamepadRaf = 0;
    const pads = getGamepads();
    let activeMask = 0;
    const seen = new Set<number>();
    for (const [padIndex, pad] of pads.entries()) {
      if (!pad) continue;
      if (!isGamepadConnected(pad)) continue;
      const index = Number.isFinite(pad.index) ? pad.index : padIndex;
      if (index < 0 || index >= MAX_GAMEPADS) continue;
      activeMask |= 1 << index;
      seen.add(index);
      activeGamepads.set(index, pad);
      const meta = ensureGamepadMeta(pad);
      if (!meta.connected) {
        if (sendGamepadConnect(pad, meta)) {
          meta.connected = true;
          meta.needsResync = true;
        } else {
          meta.needsResync = true;
        }
      }
      const snapshot = readGamepadState(pad, meta.buttonMap);
      const previous = gamepadStates.get(index);
      const stateChanged =
        !previous ||
        previous.buttons !== snapshot.buttons ||
        previous.lt !== snapshot.lt ||
        previous.rt !== snapshot.rt ||
        previous.lsX !== snapshot.lsX ||
        previous.lsY !== snapshot.lsY ||
        previous.rsX !== snapshot.rsX ||
        previous.rsY !== snapshot.rsY;
      const now = performance.now();
      const shouldHeartbeat =
        !meta.lastStateSentAt || now - meta.lastStateSentAt >= GAMEPAD_STATE_HEARTBEAT_MS;
      if (stateChanged || meta.needsResync || shouldHeartbeat) {
        const payload: InputMessage = {
          type: 'gamepad_state',
          id: index,
          activeMask,
          buttons: snapshot.buttons,
          gamepadType: meta.type,
          capabilities: meta.capabilities,
          supportedButtons: meta.supportedButtons,
          lt: snapshot.lt,
          rt: snapshot.rt,
          lsX: snapshot.lsX,
          lsY: snapshot.lsY,
          rsX: snapshot.rsX,
          rsY: snapshot.rsY,
          ts: now,
        };
        const sent = sendPayload(payload);
        if (sent) {
          gamepadStates.set(index, snapshot);
          meta.needsResync = false;
          meta.lastStateSentAt = now;
          meta.connected = true;
        } else {
          meta.needsResync = true;
        }
      }
      const motion = readGamepadMotion(pad);
      if (motion.gyro || motion.accel) {
        maybeSendMotion(index, meta, motion, now);
      }
    }
    if (gamepadMeta.size) {
      const missing: number[] = [];
      gamepadMeta.forEach((_value, index) => {
        if (!seen.has(index)) {
          missing.push(index);
        }
      });
      if (missing.length) {
        missing.forEach((index) => {
          gamepadMeta.delete(index);
          gamepadStates.delete(index);
          activeGamepads.delete(index);
          motionRequestState.delete(index);
          sendGamepadDisconnect(index, activeMask);
        });
      }
    }
    activeGamepads.forEach((_pad, index) => {
      if (!seen.has(index)) {
        activeGamepads.delete(index);
      }
    });
    if (supportsGamepad) {
      gamepadRaf = requestAnimationFrame(pollGamepads);
    }
  };

  const onGamepadConnected = () => {
    if (!supportsGamepad) return;
    if (!gamepadRaf) {
      gamepadRaf = requestAnimationFrame(pollGamepads);
    }
  };

  const onGamepadDisconnected = () => {
    if (!supportsGamepad) return;
    if (!gamepadRaf) {
      gamepadRaf = requestAnimationFrame(pollGamepads);
    }
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

  if (supportsGamepad) {
    gamepadRaf = requestAnimationFrame(pollGamepads);
    window.addEventListener('gamepadconnected', onGamepadConnected);
    window.addEventListener('gamepaddisconnected', onGamepadDisconnected);
  }

  return () => {
    if (rafId) cancelAnimationFrame(rafId);
    if (gamepadRaf) cancelAnimationFrame(gamepadRaf);
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
    releaseKeyboardLock();
    if (supportsGamepad) {
      window.removeEventListener('gamepadconnected', onGamepadConnected);
      window.removeEventListener('gamepaddisconnected', onGamepadDisconnected);
    }
    releaseAllKeys();
    if (gamepadMeta.size) {
      let activeMask = 0;
      gamepadMeta.forEach((_value, index) => {
        if (index < 0 || index >= MAX_GAMEPADS) return;
        activeMask |= 1 << index;
      });
      gamepadMeta.forEach((_value, index) => {
        if (index < 0 || index >= MAX_GAMEPADS) return;
        sendGamepadDisconnect(index, activeMask & ~(1 << index));
      });
      gamepadMeta.clear();
      gamepadStates.clear();
    }
    activeGamepads.clear();
    motionRequestState.clear();
  };
}
