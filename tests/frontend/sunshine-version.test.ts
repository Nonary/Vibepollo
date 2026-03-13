import VibeshineVersion from '@web/sunshine_version';

describe('VibeshineVersion', () => {
  test('treats stable respins as newer than the plain release', () => {
    const base = new VibeshineVersion('1.14.14');
    const stableRespin = new VibeshineVersion('1.14.14-stable.1');

    expect(stableRespin.isGreater(base)).toBe(true);
    expect(base.isGreater(stableRespin)).toBe(false);
  });

  test('keeps alpha beta and rc builds below the plain release', () => {
    const base = new VibeshineVersion('1.14.14');

    expect(base.isGreater('1.14.14-alpha.1')).toBe(true);
    expect(base.isGreater('1.14.14-beta.1')).toBe(true);
    expect(base.isGreater('1.14.14-rc.1')).toBe(true);
  });

  test('still orders prerelease identifiers correctly inside each channel', () => {
    expect(new VibeshineVersion('1.14.14-beta.2').isGreater('1.14.14-beta.1')).toBe(true);
    expect(new VibeshineVersion('1.14.14-stable.2').isGreater('1.14.14-stable.1')).toBe(true);
    expect(new VibeshineVersion('1.14.14-stable.1').isGreater('1.14.14-rc.9')).toBe(true);
  });
});
