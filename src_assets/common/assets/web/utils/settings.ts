import type { SettingsField } from '../configs/settingsSchema.ts';

export function acknowledgeSettings(
  original: Record<string, unknown>,
  submitted: Record<string, unknown>,
): Record<string, unknown> {
  return { ...original, ...submitted };
}

export function settingError(field: SettingsField | undefined, value: unknown): string | undefined {
  if (!field || value === '' || value == null) return;
  if (field.kind === 'number' || field.kind === 'duration') {
    const number = Number(value);
    if (
      !Number.isFinite(number) ||
      (field.min != null && number < field.min) ||
      (field.max != null && number > field.max)
    )
      return 'ui.settings.validation.number';
  }
  if (field.key === 'dd_manual_resolution' && !/^[1-9]\d*x[1-9]\d*$/i.test(String(value).trim()))
    return 'ui.settings.validation.resolution';
  if (['keybindings', 'dd_snapshot_exclude_devices'].includes(field.key)) {
    try {
      if (!Array.isArray(typeof value === 'string' ? JSON.parse(value) : value))
        return 'ui.settings.validation.array';
    } catch {
      return 'ui.settings.validation.array';
    }
  }
}

export function configBoolean(value: unknown, fallback = false): boolean {
  if (value == null) return fallback;
  return (
    value === true || ['1', 'true', 'enabled', 'yes', 'on'].includes(String(value).toLowerCase())
  );
}
