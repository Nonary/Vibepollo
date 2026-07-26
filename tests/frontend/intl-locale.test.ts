import { describe, expect, it } from 'vitest';
import { toIntlLocale } from '@web/utils/intlLocale';

describe('intl locale mapping', () => {
  it('maps Chinese app locales to browser Intl locales', () => {
    expect(toIntlLocale('zh')).toBe('zh-CN');
    expect(toIntlLocale('zh_TW')).toBe('zh-TW');
  });

  it('normalizes underscore locale IDs and leaves empty locale unset', () => {
    expect(toIntlLocale('pt_BR')).toBe('pt-BR');
    expect(toIntlLocale('')).toBeUndefined();
    expect(toIntlLocale()).toBeUndefined();
  });
});
