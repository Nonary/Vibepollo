/** Provider actions require explicit backend support; platform alone is insufficient. */
export function providerSupported(metadata: unknown, provider: string): boolean {
  if (!metadata || typeof metadata !== 'object') return false;
  const providers = (metadata as { providers?: unknown }).providers;
  return Boolean(providers && typeof providers === 'object' &&
    (providers as Record<string, unknown>)[provider] === true);
}
