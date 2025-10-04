import { createI18n, I18n } from 'vue-i18n';

// Import only the fallback language files
import en from '@/public/assets/locale/en.json';
import { http } from '@/http';

interface LocaleResponse {
  locale?: string;
}

type MessageSchema = typeof en;

export default async function (): Promise<any> {
  const r: LocaleResponse = await http
    .get('./api/configLocale', { validateStatus: () => true })
    .then((r) => (r.status === 200 ? r.data : {}))
    .catch(() => ({}));
  const locale = r.locale ?? 'en';
  document.querySelector('html')?.setAttribute('lang', locale);
  const messages: Record<string, MessageSchema> = {
    en,
  };
  try {
    if (locale !== 'en') {
      const r = await http
        .get(`/assets/locale/${locale}.json`, { validateStatus: () => true })
        .then((r) => (r.status === 200 ? r.data : null));
      if (r) messages[locale] = r;
    }
  } catch (e) {
    console.error('Failed to download translations', e);
  }
  const i18n = createI18n({
    // Use the Composition API and inject global helpers so `$t` works in templates
    legacy: false,
    globalInjection: true,
    locale: locale, // set locale
    fallbackLocale: 'en', // set fallback locale
    messages: messages,
  });
  return i18n;
}
