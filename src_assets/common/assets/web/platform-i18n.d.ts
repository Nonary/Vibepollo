declare class PlatformMessageI18n {
  platform: string;
  constructor(platform: string);
  getPlatformKey(key: string, platform: string): string;
  getMessageUsingPlatform(key: string, defaultMsg?: string): string;
}

export declare function usePlatformI18n(platform?: string): PlatformMessageI18n;
export declare function $tp(key: string, defaultMsg?: string): string;
