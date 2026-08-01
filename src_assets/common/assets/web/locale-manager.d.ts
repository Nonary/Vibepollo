type LocaleMessages = Record<string, unknown>;

interface I18nInstance {
  global: {
    availableLocales?: string[];
    setLocaleMessage(locale: string, messages: LocaleMessages): void;
    locale: { value: string };
  };
}

export declare function setI18nGlobal(i18n: I18nInstance): void;
export declare function getI18nGlobal(): I18nInstance | undefined;
export declare function ensureLocaleLoaded(locale: string): Promise<void>;
