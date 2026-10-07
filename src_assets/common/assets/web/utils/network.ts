export const NETWORK_LISTENER_MIN = 1024;
export const NETWORK_LISTENER_MAX = 65535;
export const NETWORK_MOONLIGHT_PORT = 47989;

// The configured port is the base used by the streaming listeners. Keep every
// derived listener in the unprivileged, valid TCP/UDP port range.
export const NETWORK_PORT_MIN = NETWORK_LISTENER_MIN + 5;
export const NETWORK_PORT_MAX = NETWORK_LISTENER_MAX - 21;

export type NetworkPortError =
  | 'required'
  | 'integer'
  | 'dependent-low'
  | 'dependent-high'
  | 'range';

export interface NetworkPortRow {
  protocol: 'tcp' | 'udp';
  ports: string;
  note?: 'moonlight' | 'web-ui';
}

export function parseNetworkPort(value: unknown): number | null {
  if (typeof value === 'number') {
    return Number.isSafeInteger(value) ? value : null;
  }
  if (typeof value !== 'string' || !/^\d+$/.test(value.trim())) return null;
  const parsed = Number(value.trim());
  return Number.isSafeInteger(parsed) ? parsed : null;
}

export function networkPortError(value: unknown): NetworkPortError | undefined {
  if (value == null || (typeof value === 'string' && value.trim() === '')) return 'required';
  const port = parseNetworkPort(value);
  if (port === null) return 'integer';
  if (port - 5 < NETWORK_LISTENER_MIN) return 'dependent-low';
  if (port + 21 > NETWORK_LISTENER_MAX) return 'dependent-high';
  if (port < NETWORK_PORT_MIN || port > NETWORK_PORT_MAX) return 'range';
  return undefined;
}

export function networkPortRows(value: unknown): NetworkPortRow[] {
  if (networkPortError(value)) return [];
  const port = parseNetworkPort(value);
  if (port === null) return [];
  return [
    { protocol: 'tcp', ports: String(port - 5) },
    { protocol: 'tcp', ports: String(port), note: 'moonlight' },
    { protocol: 'tcp', ports: String(port + 1), note: 'web-ui' },
    { protocol: 'tcp', ports: String(port + 21) },
    { protocol: 'udp', ports: `${port + 9} - ${port + 11}` },
  ];
}

export function networkPortErrorKey(error: NetworkPortError | undefined): string | undefined {
  if (!error) return undefined;
  return `ui.settings.validation.port_${error.replace('-', '_')}`;
}
