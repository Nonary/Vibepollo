import { inject, Ref } from 'vue';

class PlatformMessageI18n {
  platform: string;

  constructor(platform: string) {
    this.platform = platform;
  }

  getPlatformKey(key: string, platform: string): string {
    return key + '_' + platform;
  }

  getMessageUsingPlatform(key: string, defaultMsg?: string): string {
    const realKey = this.getPlatformKey(key, this.platform);
    const i18n = inject<{ t: (k: string) => string } | undefined>('i18n');
    if (!i18n || typeof i18n.t !== 'function') return defaultMsg ?? realKey;
    let message = i18n.t(realKey);

    if (message !== realKey) {
      // We got a message back, return early
      return message;
    }

    // If on Windows, we don't fallback to unix, so return early
    if (this.platform === 'windows') {
      return defaultMsg ? defaultMsg : message;
    }

    // there's no message for key, check for unix version
    const unixKey = this.getPlatformKey(key, 'unix');
    message = i18n.t(unixKey);

    if (message === unixKey && defaultMsg) {
      // there's no message for unix key, return defaultMsg
      return defaultMsg;
    }
    return message;
  }
}

export function usePlatformI18n(platform?: string): PlatformMessageI18n {
  // Resolve platform from injected ref if not explicitly passed.
  if (!platform) {
    const injected = inject('platform', null) as string | Ref<string> | null;
    if (injected) {
      // Support either a ref or plain value
      platform = typeof injected === 'object' && 'value' in injected ? injected.value : injected;
    }
  }

  // Fallback defensively instead of throwing to avoid hard render errors
  if (!platform) {
    // Default to "windows" (platform with no unix fallback) to keep behavior predictable.
    platform = 'windows';
  }

  return inject('platformMessage', () => new PlatformMessageI18n(platform), true);
}

export function $tp(key: string, defaultMsg?: string): string {
  try {
    const pm = usePlatformI18n();
    // Guard i18n injection absence (early render before init) inside message getter
    return pm.getMessageUsingPlatform(key, defaultMsg);
  } catch (e) {
    return defaultMsg || key;
  }
}
