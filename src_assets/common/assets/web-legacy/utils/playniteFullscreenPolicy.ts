interface PlayniteFullscreenApplication {
  'playnite-fullscreen'?: unknown;
}

// The host launches Playnite's fullscreen helper only for apps carrying this
// flag. The editor has no control for it, and saves replace the whole stored
// entry, so carry it over or the app silently becomes a plain desktop session.
export function preservePlayniteFullscreen(
  source: PlayniteFullscreenApplication | null | undefined,
  payload: Record<string, unknown>,
): void {
  if (!source || !Object.prototype.hasOwnProperty.call(source, 'playnite-fullscreen')) return;
  payload['playnite-fullscreen'] = source['playnite-fullscreen'];
}
