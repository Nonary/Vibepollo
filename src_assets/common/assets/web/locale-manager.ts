import { http } from '@/http';
import { toIntlLocale } from '@/utils/intlLocale';

type LocaleMessages = Record<string, unknown>;
type I18nInstance = {
  global: {
    availableLocales?: string[];
    setLocaleMessage(locale: string, messages: LocaleMessages): void;
    locale: {
      value: string;
    };
  };
};

const i18nState: { current?: I18nInstance } = {};

export function setI18nGlobal(i18n: I18nInstance) {
  i18nState.current = i18n;
}

export function getI18nGlobal() {
  return i18nState.current;
}

export async function ensureLocaleLoaded(locale: string): Promise<void> {
  const i18n = i18nState.current;
  if (!i18n) return;
  try {
    // Short-circuit if we already have messages for this locale
    const has = i18n.global.availableLocales?.includes(locale);
    if (!has) {
      const r = await http
        .get<LocaleMessages>(`/assets/locale/${locale}.json`, { validateStatus: () => true })
        .then((r) => (r.status === 200 ? r.data : null));
      if (r) {
        i18n.global.setLocaleMessage(locale, r);
      }
    }
    i18n.global.locale.value = locale;
    document.querySelector('html')?.setAttribute('lang', toIntlLocale(locale) ?? locale);
  } catch (e) {
    console.error('ensureLocaleLoaded failed', e);
  }
}
