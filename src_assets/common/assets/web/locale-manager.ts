import { http } from '@/http';

let _i18n: any = null;

export function setI18nGlobal(i18n: any) {
  _i18n = i18n;
}

export function getI18nGlobal() {
  return _i18n;
}

export async function ensureLocaleLoaded(locale: string): Promise<void> {
  if (!_i18n) return;
  try {
    // Short-circuit if we already have messages for this locale
    const has = _i18n.global.availableLocales?.includes(locale);
    if (!has) {
      const r = await http
        .get(`/assets/locale/${locale}.json`, { validateStatus: () => true })
        .then((r) => (r.status === 200 ? r.data : null));
      if (r) {
        _i18n.global.setLocaleMessage(locale, r);
      }
    }
    _i18n.global.locale = locale;
    document.querySelector('html')?.setAttribute('lang', locale);
  } catch (e) {
    console.error('ensureLocaleLoaded failed', e);
  }
}
