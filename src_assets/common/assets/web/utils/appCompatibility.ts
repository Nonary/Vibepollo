export type AppExtras = Record<string, unknown>;

export function parseAppExtras(json: string): AppExtras | null {
  try {
    const value: unknown = JSON.parse(json || '{}');
    return value !== null && typeof value === 'object' && !Array.isArray(value)
      ? (value as AppExtras)
      : null;
  } catch {
    return null;
  }
}

// Keep absent fields absent, and retain unknown fields and legacy representations
// until the user actually edits that particular option.
export function setAppOption(source: AppExtras, key: string, value: unknown): AppExtras {
  const next = { ...source };
  if (value === undefined) delete next[key];
  else next[key] = value;
  return next;
}

export function appBooleanChoice(value: unknown): string {
  if (value === undefined || value === null) return '';
  const text = String(value).toLowerCase();
  if (['true', 'enabled', '1'].includes(text)) return 'true';
  if (['false', 'disabled', '0'].includes(text)) return 'false';
  return String(value);
}

export function setAppStateCommand(
  source: AppExtras,
  index: number,
  key: string,
  value: unknown,
): AppExtras {
  const commands = Array.isArray(source['state-cmd']) ? [...source['state-cmd']] : [];
  if (index < 0 || index >= commands.length) return source;
  const original = commands[index];
  const command =
    original && typeof original === 'object' && !Array.isArray(original)
      ? (original as AppExtras)
      : {};
  commands[index] = { ...command, [key]: value };
  return setAppOption(source, 'state-cmd', commands);
}
