import type { GamepadFeedbackMessage, InputMessage } from '@/types/webrtc';

/**
 * Browser gamepad capture for the v2 stream surface.
 *
 * Keyboard, pointer, and touch capture deliberately stay in BrowserStreamView.
 * This helper owns only the Gamepad API polling and hot-plug listeners so that
 * adding controller support cannot install a second set of pointer listeners.
 */

export interface BrowserGamepadCapture {
  release: () => void;
  stop: () => void;
}

interface GamepadCaptureOptions {
  enabled?: () => boolean;
}

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
  connected: boolean;
  needsResync: boolean;
  lastStateSentAt?: number;
  lastGyro?: GamepadVector;
  lastAccel?: GamepadVector;
  lastGyroAt?: number;
  lastAccelAt?: number;
}

const MAX_GAMEPADS = 16;
const AXIS_DEADZONE = 0.08;
const MOTION_SEND_INTERVAL_MS = 16;
const MOTION_DIFF_THRESHOLD = 0.1;
const GAMEPAD_STATE_HEARTBEAT_MS = 500;

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

const activeGamepads = new Map<number, Gamepad>();
const motionRequestState = new Map<number, { gyro: boolean; accel: boolean }>();

function browserGamepadApi(): (() => (Gamepad | null)[]) | null {
  if (typeof navigator === 'undefined') return null;
  if (typeof navigator.getGamepads === 'function') return () => navigator.getGamepads();
  const webkit = (
    navigator as Navigator & {
      webkitGetGamepads?: () => (Gamepad | null)[];
    }
  ).webkitGetGamepads;
  return typeof webkit === 'function' ? webkit.bind(navigator) : null;
}

function getGamepads(): (Gamepad | null)[] {
  const read = browserGamepadApi();
  if (!read) return [];
  try {
    const pads = read();
    return Array.isArray(pads) ? pads : Array.from(pads);
  } catch {
    return [];
  }
}

function gamepadConnected(gamepad: Gamepad): boolean {
  return typeof gamepad.connected === 'boolean' ? gamepad.connected : true;
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
  if (id.includes('xbox')) return GAMEPAD_TYPE.xbox;
  if (id.includes('wireless controller')) return GAMEPAD_TYPE.playstation;
  return GAMEPAD_TYPE.unknown;
}

function resolveButtonMap(gamepad: Gamepad, type: number): Map<number, number> {
  const map = new Map(STANDARD_BUTTON_MAP);
  // Standard Gamepad mapping has no touchpad bit. Use the extra button only
  // when the browser exposes one, and advertise motion only when data exists.
  if (type === GAMEPAD_TYPE.playstation && gamepad.buttons.length > 17) {
    map.set(17, GAMEPAD_BUTTONS.touchpadButton);
  }
  if (gamepad.buttons.length > 18) map.set(18, GAMEPAD_BUTTONS.paddle1);
  if (gamepad.buttons.length > 19) map.set(19, GAMEPAD_BUTTONS.paddle2);
  if (gamepad.buttons.length > 20) map.set(20, GAMEPAD_BUTTONS.paddle3);
  if (gamepad.buttons.length > 21) map.set(21, GAMEPAD_BUTTONS.paddle4);
  return map;
}

function applyDeadzone(value: number, deadzone: number): number {
  const abs = Math.abs(value);
  if (abs <= deadzone) return 0;
  return Math.min(1, Math.max(0, (abs - deadzone) / (1 - deadzone))) * Math.sign(value);
}

function toInt16(value: number): number {
  return Math.round(Math.min(1, Math.max(-1, value)) * 32767);
}

function toUint8(value: number): number {
  return Math.round(Math.min(1, Math.max(0, value)) * 255);
}

function readButtons(gamepad: Gamepad, buttonMap: Map<number, number>): number {
  let mask = 0;
  buttonMap.forEach((bit, index) => {
    if (gamepad.buttons[index]?.pressed) mask |= bit;
  });
  return mask;
}

function readState(gamepad: Gamepad, buttonMap: Map<number, number>): GamepadSnapshot {
  const axes = gamepad.axes || [];
  return {
    buttons: readButtons(gamepad, buttonMap),
    lt: toUint8(gamepad.buttons[6]?.value ?? 0),
    rt: toUint8(gamepad.buttons[7]?.value ?? 0),
    lsX: toInt16(applyDeadzone(axes[0] ?? 0, AXIS_DEADZONE)),
    lsY: toInt16(applyDeadzone(-(axes[1] ?? 0), AXIS_DEADZONE)),
    rsX: toInt16(applyDeadzone(axes[2] ?? 0, AXIS_DEADZONE)),
    rsY: toInt16(applyDeadzone(-(axes[3] ?? 0), AXIS_DEADZONE)),
  };
}

function readVector(value: unknown): GamepadVector | undefined {
  if (!value || typeof value !== 'object') return undefined;
  const array = value as { length?: number; [index: number]: unknown };
  if (typeof array.length !== 'number' || array.length < 3) return undefined;
  const vector = [Number(array[0]), Number(array[1]), Number(array[2])] as GamepadVector;
  return vector.every(Number.isFinite) ? vector : undefined;
}

function readMotion(gamepad: Gamepad): { gyro?: GamepadVector; accel?: GamepadVector } {
  // The standard browser Gamepad API does not expose motion. Support browser
  // extensions that provide it, but never claim motion from controller type.
  const candidate = gamepad as Gamepad & {
    motion?: { angularVelocity?: unknown; linearAcceleration?: unknown };
    motionData?: { angularVelocity?: unknown; linearAcceleration?: unknown };
    pose?: { angularVelocity?: unknown; linearAcceleration?: unknown } | null;
  };
  const source = candidate.motion ?? candidate.motionData ?? candidate.pose;
  if (!source) return {};
  return {
    gyro: readVector(source.angularVelocity),
    accel: readVector(source.linearAcceleration),
  };
}

function vectorChanged(previous: GamepadVector | undefined, next: GamepadVector): boolean {
  return (
    !previous ||
    Math.abs(previous[0] - next[0]) > MOTION_DIFF_THRESHOLD ||
    Math.abs(previous[1] - next[1]) > MOTION_DIFF_THRESHOLD ||
    Math.abs(previous[2] - next[2]) > MOTION_DIFF_THRESHOLD
  );
}

function ensureMeta(gamepad: Gamepad, metadata: Map<number, GamepadMeta>): GamepadMeta {
  const existing = metadata.get(gamepad.index);
  if (existing) return existing;
  const type = resolveGamepadType(gamepad);
  const buttonMap = resolveButtonMap(gamepad, type);
  let supportedButtons = 0;
  buttonMap.forEach((bit) => {
    supportedButtons |= bit;
  });
  const motion = readMotion(gamepad);
  let capabilities = 0;
  if (gamepad.buttons.length > 6) capabilities |= GAMEPAD_CAPS.analogTriggers;
  if (motion.accel) capabilities |= GAMEPAD_CAPS.accel;
  if (motion.gyro) capabilities |= GAMEPAD_CAPS.gyro;
  if (type === GAMEPAD_TYPE.playstation && gamepad.buttons.length > 17) {
    capabilities |= GAMEPAD_CAPS.touchpad;
  }
  const meta: GamepadMeta = {
    buttonMap,
    supportedButtons,
    capabilities,
    type,
    connected: false,
    needsResync: true,
  };
  metadata.set(gamepad.index, meta);
  return meta;
}

function sent(send: (payload: Record<string, unknown>) => boolean, payload: InputMessage): boolean {
  return send(payload as unknown as Record<string, unknown>);
}

function now(): number {
  return typeof performance !== 'undefined' ? performance.now() : Date.now();
}

/** Route host feedback to a browser controller's vibration actuator. */
export function applyGamepadFeedback(message: GamepadFeedbackMessage | unknown): void {
  if (!message || typeof message !== 'object') return;
  const payload = message as GamepadFeedbackMessage;
  const id = Number(payload.id);
  if (payload.type !== 'gamepad_feedback' || !Number.isFinite(id)) return;
  if (payload.event === 'motion_event_state') {
    const motionType = Number(payload.motionType);
    const state = motionRequestState.get(id) ?? { gyro: true, accel: true };
    if (motionType === 1) state.accel = Number(payload.reportRate) > 0;
    if (motionType === 2) state.gyro = Number(payload.reportRate) > 0;
    motionRequestState.set(id, state);
    return;
  }
  if (payload.event !== 'rumble' && payload.event !== 'rumble_triggers') return;
  const gamepad = activeGamepads.get(id) ?? getGamepads()[id];
  if (!gamepad) return;
  const candidate = gamepad as Gamepad & {
    vibrationActuator?: GamepadHapticActuator;
    hapticActuators?: GamepadHapticActuator[];
  };
  const actuator = candidate.vibrationActuator ?? candidate.hapticActuators?.[0];
  if (!actuator) return;
  const scale = (value: number | undefined): number =>
    Number.isFinite(value) ? Math.min(1, Math.max(0, Number(value) / 65535)) : 0;
  const strong = payload.event === 'rumble_triggers' ? scale(payload.left) : scale(payload.lowfreq);
  const weak = payload.event === 'rumble_triggers' ? scale(payload.right) : scale(payload.highfreq);
  try {
    void actuator.playEffect('dual-rumble', {
      duration: 100,
      strongMagnitude: strong,
      weakMagnitude: weak,
    });
  } catch {
    // Haptics are optional and can disappear while a controller is unplugged.
  }
}

export function attachBrowserGamepadCapture(
  send: (payload: Record<string, unknown>) => boolean,
  options: GamepadCaptureOptions = {},
): BrowserGamepadCapture | null {
  const read = browserGamepadApi();
  if (!read || typeof window === 'undefined') return null;
  const enabled = options.enabled ?? (() => true);
  const states = new Map<number, GamepadSnapshot>();
  const metadata = new Map<number, GamepadMeta>();
  let rafId: number | undefined;
  let stopped = false;

  const sendConnect = (pad: Gamepad, meta: GamepadMeta): boolean =>
    sent(send, {
      type: 'gamepad_connect',
      id: pad.index,
      gamepadType: meta.type,
      capabilities: meta.capabilities,
      supportedButtons: meta.supportedButtons,
      ts: now(),
    });

  const sendDisconnect = (index: number, activeMask: number): boolean =>
    sent(send, { type: 'gamepad_disconnect', id: index, activeMask, ts: now() });

  const sendNeutral = (index: number, meta: GamepadMeta, activeMask: number): boolean =>
    sent(send, {
      type: 'gamepad_state',
      id: index,
      activeMask,
      buttons: 0,
      gamepadType: meta.type,
      capabilities: meta.capabilities,
      supportedButtons: meta.supportedButtons,
      lt: 0,
      rt: 0,
      lsX: 0,
      lsY: 0,
      rsX: 0,
      rsY: 0,
      ts: now(),
    });

  const maybeMotion = (
    index: number,
    meta: GamepadMeta,
    motion: { gyro?: GamepadVector; accel?: GamepadVector },
    timestamp: number,
  ): void => {
    const requested = motionRequestState.get(index) ?? { gyro: true, accel: true };
    if (motion.gyro && requested.gyro) {
      if (
        timestamp - (meta.lastGyroAt ?? 0) >= MOTION_SEND_INTERVAL_MS &&
        vectorChanged(meta.lastGyro, motion.gyro)
      ) {
        meta.lastGyroAt = timestamp;
        meta.lastGyro = motion.gyro;
        sent(send, {
          type: 'gamepad_motion',
          id: index,
          motionType: 2,
          x: (motion.gyro[0] * 180) / Math.PI,
          y: (motion.gyro[1] * 180) / Math.PI,
          z: (motion.gyro[2] * 180) / Math.PI,
          ts: timestamp,
        });
      }
    }
    if (motion.accel && requested.accel) {
      if (
        timestamp - (meta.lastAccelAt ?? 0) >= MOTION_SEND_INTERVAL_MS &&
        vectorChanged(meta.lastAccel, motion.accel)
      ) {
        meta.lastAccelAt = timestamp;
        meta.lastAccel = motion.accel;
        sent(send, {
          type: 'gamepad_motion',
          id: index,
          motionType: 1,
          x: motion.accel[0],
          y: motion.accel[1],
          z: motion.accel[2],
          ts: timestamp,
        });
      }
    }
  };

  const poll = (): void => {
    rafId = undefined;
    if (stopped) return;
    const pads = getGamepads();
    const seen = new Set<number>();
    let activeMask = 0;
    if (enabled()) {
      const connectedPads: Array<{ index: number; pad: Gamepad }> = [];
      for (const [position, pad] of pads.entries()) {
        if (!pad || !gamepadConnected(pad)) continue;
        const index = Number.isFinite(pad.index) ? pad.index : position;
        if (index < 0 || index >= MAX_GAMEPADS) continue;
        seen.add(index);
        activeMask |= 1 << index;
        connectedPads.push({ index, pad });
      }
      for (const { index, pad } of connectedPads) {
        activeGamepads.set(index, pad);
        const meta = ensureMeta(pad, metadata);
        if (!meta.connected && sendConnect(pad, meta)) {
          meta.connected = true;
          meta.needsResync = true;
        }
        if (!meta.connected) continue;
        const snapshot = readState(pad, meta.buttonMap);
        const previous = states.get(index);
        const changed =
          !previous ||
          previous.buttons !== snapshot.buttons ||
          previous.lt !== snapshot.lt ||
          previous.rt !== snapshot.rt ||
          previous.lsX !== snapshot.lsX ||
          previous.lsY !== snapshot.lsY ||
          previous.rsX !== snapshot.rsX ||
          previous.rsY !== snapshot.rsY;
        const timestamp = now();
        if (
          changed ||
          meta.needsResync ||
          timestamp - (meta.lastStateSentAt ?? 0) >= GAMEPAD_STATE_HEARTBEAT_MS
        ) {
          const didSend = sent(send, {
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
            ts: timestamp,
          });
          if (didSend) {
            states.set(index, snapshot);
            meta.connected = true;
            meta.needsResync = false;
            meta.lastStateSentAt = timestamp;
          }
        }
        const motion = readMotion(pad);
        if (motion.gyro || motion.accel) maybeMotion(index, meta, motion, timestamp);
      }
    }

    for (const [index, meta] of metadata) {
      if (seen.has(index)) continue;
      if (meta.connected) sendDisconnect(index, activeMask);
      metadata.delete(index);
      states.delete(index);
      activeGamepads.delete(index);
      motionRequestState.delete(index);
    }
    if (enabled()) {
      for (let index = 0; index < MAX_GAMEPADS; index += 1) {
        if (!seen.has(index)) activeGamepads.delete(index);
      }
    }
    rafId = window.requestAnimationFrame(poll);
  };

  const onGamepadChange = (): void => {
    if (!rafId && !stopped) rafId = window.requestAnimationFrame(poll);
  };
  const release = (): void => {
    let activeMask = 0;
    metadata.forEach((_meta, index) => {
      if (index >= 0 && index < MAX_GAMEPADS) activeMask |= 1 << index;
    });
    metadata.forEach((meta, index) => {
      if (!meta.connected) return;
      if (sendNeutral(index, meta, activeMask)) {
        states.set(index, { buttons: 0, lt: 0, rt: 0, lsX: 0, lsY: 0, rsX: 0, rsY: 0 });
        meta.needsResync = true;
      }
    });
  };
  const stop = (): void => {
    if (stopped) return;
    stopped = true;
    if (rafId !== undefined) window.cancelAnimationFrame(rafId);
    rafId = undefined;
    window.removeEventListener('gamepadconnected', onGamepadChange);
    window.removeEventListener('gamepaddisconnected', onGamepadChange);
    let activeMask = 0;
    metadata.forEach((_meta, index) => {
      if (index >= 0 && index < MAX_GAMEPADS) activeMask |= 1 << index;
    });
    metadata.forEach((meta, index) => {
      if (meta.connected) sendDisconnect(index, activeMask & ~(1 << index));
    });
    metadata.clear();
    states.clear();
    activeGamepads.clear();
    motionRequestState.clear();
  };

  window.addEventListener('gamepadconnected', onGamepadChange);
  window.addEventListener('gamepaddisconnected', onGamepadChange);
  rafId = window.requestAnimationFrame(poll);
  return { release, stop };
}
