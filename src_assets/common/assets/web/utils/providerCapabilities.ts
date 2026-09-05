/** Provider actions require explicit backend support; platform alone is insufficient. */
export function providerSupported(metadata: unknown, provider: string): boolean {
  if (!metadata || typeof metadata !== 'object') return false;
  const providers = (metadata as { providers?: unknown }).providers;
  return Boolean(
    providers &&
      typeof providers === 'object' &&
      (providers as Record<string, unknown>)[provider] === true,
  );
}

export function supportsManagedLinuxDisplay(metadata: unknown): boolean {
  if (!metadata || typeof metadata !== 'object') return false;
  const display = (metadata as { virtual_display?: { backend?: string } }).virtual_display;
  return display?.backend === 'kscreen-vkms';
}
export function settingsCapabilitySupported(key: string, metadata: unknown): boolean {
  if (!metadata || typeof metadata !== 'object') return true;
  const platform = String((metadata as { platform?: string }).platform ?? '').toLowerCase();
  if (!platform.includes('linux')) return true;
  if (/^(virtual_display|dd_)/.test(key)) return supportsManagedLinuxDisplay(metadata);
  if (/^(frame_limiter_|mangohud_)/.test(key)) return providerSupported(metadata, 'mangohud');
  return true;
}
