export function toIntlLocale(locale?: string) {
  const value = String(locale ?? '').trim();
  if (!value) return undefined;
  if (value === 'zh') return 'zh-CN';
  if (value === 'zh_TW') return 'zh-TW';
  return value.replaceAll('_', '-');
}
