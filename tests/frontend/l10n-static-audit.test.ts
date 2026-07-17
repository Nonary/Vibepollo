import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const scriptPath = resolve(repoRoot, 'src_assets/common/assets/web/scripts/l10n_static_audit.mjs');
const allowlistPath = resolve(
  repoRoot,
  'src_assets/common/assets/web/scripts/l10n_static_audit.allowlist.json',
);

function writeJson(path: string, value: unknown) {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, `${JSON.stringify(value, null, 2)}\n`, 'utf8');
}

function fixture(files: Record<string, string>, localeOverrides: Record<string, unknown> = {}) {
  const root = mkdtempSync(resolve(tmpdir(), 'l10n-audit-'));
  writeJson(resolve(root, 'public/assets/locale/en.json'), {
    common: {
      ok: 'OK',
      greeting: 'Hello {name}',
      rich: 'Click <b>Apply</b>',
    },
    ...localeOverrides.en,
  });
  writeJson(resolve(root, 'public/assets/locale/fr.json'), {
    common: {
      ok: 'OK',
      greeting: 'Bonjour {name}',
      rich: 'Cliquez <b>Appliquer</b>',
    },
    ...localeOverrides.fr,
  });
  for (const [relative, content] of Object.entries(files)) {
    const fullPath = resolve(root, relative);
    mkdirSync(dirname(fullPath), { recursive: true });
    writeFileSync(fullPath, content, 'utf8');
  }
  return root;
}

function audit(root: string) {
  const output = execFileSync(
    process.execPath,
    [
      scriptPath,
      '--root',
      root,
      '--allowlist',
      allowlistPath,
      '--baseline',
      resolve(root, 'baseline.json'),
      '--format',
      'json',
    ],
    { encoding: 'utf8' },
  );
  return JSON.parse(output);
}

describe('l10n static audit', () => {
  it('flags missing literal i18n keys', () => {
    const root = fixture({
      'Example.vue': `<template><button>{{ $t('common.missing') }}</button></template>`,
    });
    expect(audit(root).issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'missing-en-key', key: 'common.missing' }),
      ]),
    );
  });

  it('flags hard-coded template text and UI props', () => {
    const root = fixture({
      'Example.vue': `<template><section title="Apply changes">Save Changes</section></template>`,
    });
    const issues = audit(root).issues;
    expect(issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'hardcoded-template-text', value: 'Save Changes' }),
      ]),
    );
    expect(issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'hardcoded-ui-attr', value: 'Apply changes' }),
      ]),
    );
  });

  it('keeps legacy baseline entries valid when source lines move', () => {
    const root = fixture({
      'Example.vue': `<template><section>Save Changes</section></template>`,
    });
    const initial = audit(root);
    const issue = initial.issues.find(
      (candidate: { rule: string; file?: string; value?: string }) =>
        candidate.rule === 'hardcoded-template-text' &&
        candidate.file === 'Example.vue' &&
        candidate.value === 'Save Changes',
    );
    if (!issue) throw new Error('Expected hard-coded text issue');

    const [rule, file, ...rest] = issue.fingerprint.split('|');
    writeJson(resolve(root, 'baseline.json'), {
      issues: [{ fingerprint: [rule, file, '999', ...rest].join('|') }],
    });

    expect(
      audit(root).unbaselined.some(
        (candidate: { rule: string; file?: string; value?: string }) =>
          candidate.rule === 'hardcoded-template-text' &&
          candidate.file === 'Example.vue' &&
          candidate.value === 'Save Changes',
      ),
    ).toBe(false);
  });

  it('flags script UI properties and fallback misuse', () => {
    const root = fixture({
      'example.ts': `const button = { label: 'Start Stream' }; const value = t('common.ok') || 'OK'; translate('common.ok', 'Okay');`,
    });
    const issues = audit(root).issues;
    expect(issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'hardcoded-ui-prop', value: 'Start Stream' }),
      ]),
    );
    expect(
      issues.filter((issue: { rule: string }) => issue.rule === 'fallback-literal').length,
    ).toBeGreaterThan(0);
  });

  it('flags placeholder mismatches and stale extra locale keys', () => {
    const root = fixture(
      {},
      {
        fr: {
          common: {
            greeting: 'Bonjour',
          },
          stale: 'Ancien',
        },
      },
    );
    const issues = audit(root).issues;
    expect(issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'locale-placeholder-mismatch', key: 'common.greeting' }),
      ]),
    );
    expect(issues).toEqual(
      expect.arrayContaining([expect.objectContaining({ rule: 'locale-extra-key', key: 'stale' })]),
    );
  });

  it('flags HTML tag mismatches and missing locale keys', () => {
    const root = fixture(
      {},
      {
        en: {
          extra: {
            key: 'Extra',
          },
        },
        fr: {
          common: {
            rich: 'Cliquez <strong>Appliquer</strong>',
          },
        },
      },
    );
    const issues = audit(root).issues;
    expect(issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'locale-html-tag-mismatch', key: 'common.rich' }),
      ]),
    );
    expect(issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'locale-missing-key', key: 'extra.key' }),
      ]),
    );
  });

  it('flags mojibake in locale values', () => {
    const root = fixture(
      {},
      {
        fr: {
          common: {
            greeting: 'Bonjour ?? {name}',
          },
        },
      },
    );
    expect(audit(root).issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'locale-mojibake', key: 'common.greeting' }),
      ]),
    );
  });

  it('flags isolated question-mark corruption in settings translations', () => {
    const root = fixture(
      {},
      {
        en: {
          config: {
            saving: 'Saving…',
          },
        },
        fr: {
          config: {
            saving: 'Enregistrement?',
          },
        },
      },
    );
    expect(audit(root).issues).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ rule: 'locale-mojibake', key: 'config.saving' }),
      ]),
    );
  });

  it('allows technical literals', () => {
    const root = fixture({
      'options.ts': `export const options = [{ label: 'NVIDIA NVENC' }, { label: 'IPv4' }];`,
    });
    expect(
      audit(root).issues.filter(
        (issue: { rule: string; value?: string }) =>
          issue.rule === 'hardcoded-ui-prop' &&
          (issue.value === 'NVIDIA NVENC' || issue.value === 'IPv4'),
      ),
    ).toEqual([]);
  });

  it('allows untranslated locale values when the value is allowlisted', () => {
    const root = fixture(
      {},
      {
        en: {
          brand: {
            playnite: 'Playnite',
          },
        },
        fr: {
          brand: {
            playnite: 'Playnite',
          },
        },
      },
    );
    expect(
      audit(root).issues.filter(
        (issue: { rule: string; key?: string }) =>
          issue.rule === 'locale-untranslated-value' && issue.key === 'brand.playnite',
      ),
    ).toEqual([]);
  });
});
