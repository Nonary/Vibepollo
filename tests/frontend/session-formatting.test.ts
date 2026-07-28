import { formatBytes, formatNumber } from '@/components/session/sessionFormatting';

describe('session formatting', () => {
  it('renders missing numeric values as placeholders instead of undefined text', () => {
    expect(formatNumber(undefined)).toBe('--');
    expect(formatNumber(null)).toBe('--');
    expect(formatNumber(Number.NaN)).toBe('--');
  });

  it('keeps compact formatting for real numeric values', () => {
    expect(formatNumber(0)).toBe('0');
    expect(formatNumber(999)).toBe('999');
    expect(formatNumber(1_500)).toBe('1.5K');
    expect(formatNumber(1_250_000)).toBe('1.3M');
  });

  it('renders missing byte values as placeholders', () => {
    expect(formatBytes(undefined)).toBe('--');
    expect(formatBytes(null)).toBe('--');
    expect(formatBytes(Number.NaN)).toBe('--');
  });

  it('keeps byte units for real numeric values', () => {
    expect(formatBytes(512)).toBe('512 B');
    expect(formatBytes(2_048)).toBe('2.0 KB');
    expect(formatBytes(3_145_728)).toBe('3.0 MB');
  });
});
