#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_ROOT = path.resolve(SCRIPT_DIR, '..');
const DEFAULT_ALLOWLIST = path.join(SCRIPT_DIR, 'l10n_static_audit.allowlist.json');
const DEFAULT_BASELINE = path.join(SCRIPT_DIR, 'l10n_static_audit.baseline.json');
const SOURCE_EXTENSIONS = new Set(['.vue', '.ts', '.js']);
const TEMPLATE_ATTRS = [
  'aria-label',
  'content',
  'description',
  'label',
  'message',
  'negative-text',
  'placeholder',
  'positive-text',
  'tab',
  'text',
  'title',
];
const SCRIPT_PROPS = [
  'ariaLabel',
  'content',
  'description',
  'label',
  'message',
  'negativeText',
  'placeholder',
  'positiveText',
  'subtitle',
  'text',
  'title',
];

function parseArgs(argv) {
  const options = {
    root: DEFAULT_ROOT,
    allowlist: DEFAULT_ALLOWLIST,
    baseline: DEFAULT_BASELINE,
    format: 'text',
    strict: false,
    updateBaseline: false,
  };

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--strict') {
      options.strict = true;
    } else if (arg === '--update-baseline') {
      options.updateBaseline = true;
    } else if (arg === '--format') {
      options.format = argv[++i] ?? 'text';
    } else if (arg.startsWith('--format=')) {
      options.format = arg.slice('--format='.length);
    } else if (arg === '--root') {
      options.root = argv[++i] ?? options.root;
    } else if (arg.startsWith('--root=')) {
      options.root = arg.slice('--root='.length);
    } else if (arg === '--allowlist') {
      options.allowlist = argv[++i] ?? options.allowlist;
    } else if (arg.startsWith('--allowlist=')) {
      options.allowlist = arg.slice('--allowlist='.length);
    } else if (arg === '--baseline') {
      options.baseline = argv[++i] ?? options.baseline;
    } else if (arg.startsWith('--baseline=')) {
      options.baseline = arg.slice('--baseline='.length);
    } else if (arg === '--help' || arg === '-h') {
      options.help = true;
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }

  options.root = path.resolve(options.root);
  options.allowlist = path.resolve(options.allowlist);
  options.baseline = path.resolve(options.baseline);
  if (!['text', 'json'].includes(options.format)) {
    throw new Error(`Unsupported --format value: ${options.format}`);
  }
  return options;
}

function helpText() {
  return [
    'Usage: node scripts/l10n_static_audit.mjs [options]',
    '',
    'Options:',
    '  --strict                 fail on unbaselined issues',
    '  --format text|json       output format',
    '  --root <path>            web root to audit',
    '  --allowlist <path>       allowlist JSON file',
    '  --baseline <path>        baseline JSON file',
    '  --update-baseline        rewrite baseline with current issues',
  ].join('\n');
}

function readJson(filePath, fallback = null) {
  if (!fs.existsSync(filePath)) return fallback;
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function stableJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function normalizePath(value) {
  return value.replace(/\\/g, '/');
}

function relativePath(root, filePath) {
  return normalizePath(path.relative(root, filePath));
}

function lineForIndex(text, index) {
  let line = 1;
  for (let i = 0; i < index; i += 1) {
    if (text.charCodeAt(i) === 10) line += 1;
  }
  return line;
}

function flattenMessages(value, prefix = '', out = {}) {
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    for (const [key, child] of Object.entries(value)) {
      const next = prefix ? `${prefix}.${key}` : key;
      flattenMessages(child, next, out);
    }
  } else {
    out[prefix] = value;
  }
  return out;
}

function messageNodeTypes(value, prefix = '', out = {}) {
  const kind = value && typeof value === 'object' && !Array.isArray(value) ? 'object' : 'scalar';
  if (prefix) out[prefix] = kind;
  if (kind === 'object') {
    for (const [key, child] of Object.entries(value)) {
      messageNodeTypes(child, prefix ? `${prefix}.${key}` : key, out);
    }
  }
  return out;
}

function extractPlaceholders(value) {
  if (typeof value !== 'string') return [];
  return Array.from(value.matchAll(/\{([A-Za-z_][\w.-]*)\}/g), (match) => match[1]).sort();
}

function extractHtmlTags(value) {
  if (typeof value !== 'string') return [];
  return Array.from(value.matchAll(/<\/?([A-Za-z][A-Za-z0-9-]*)\b[^>]*>/g), (match) =>
    match[1].toLowerCase(),
  ).sort();
}

function looksMojibake(value, enValue, key) {
  if (typeof value !== 'string') return false;
  if (/\uFFFD|\?{2,}|Ã[\u0080-\u00ff]|Â[\u0080-\u00ff\s]|â[€\u0080-\u00ff]/.test(value)) {
    return true;
  }

  const namespace = String(key ?? '').split('.', 1)[0];
  const settingsNamespace = ['apps', 'config', 'playnite', 'rtss'].includes(namespace);
  return settingsNamespace && value.includes('?') && !String(enValue ?? '').includes('?');
}

function normalizeFingerprint(fingerprint) {
  if (typeof fingerprint !== 'string') return fingerprint;
  const parts = fingerprint.split('|');
  // Baselines created before this change include a volatile source line number
  // between the file and locale fields. Keep accepting them as a migration path.
  if (parts.length === 8) parts.splice(2, 1);
  return parts.join('|');
}

function issueFingerprint(issue) {
  return [
    issue.rule,
    issue.file ?? '',
    issue.locale ?? '',
    issue.key ?? '',
    issue.value ?? '',
    issue.expected ?? '',
    issue.actual ?? '',
  ].join('|');
}

function makeIssue(issue) {
  const normalized = { severity: 'error', ...issue };
  normalized.fingerprint = issueFingerprint(normalized);
  return normalized;
}

function compileAllowlist(allowlist) {
  return {
    literalValues: new Set(allowlist?.literalValues ?? []),
    literalPatterns: (allowlist?.literalPatterns ?? []).map((pattern) => new RegExp(pattern)),
    pathPatterns: (allowlist?.pathPatterns ?? []).map((pattern) => new RegExp(pattern)),
    issueFingerprints: new Set((allowlist?.issueFingerprints ?? []).map(normalizeFingerprint)),
  };
}

function isAllowedPath(compiledAllowlist, relPath) {
  return compiledAllowlist.pathPatterns.some((pattern) => pattern.test(relPath));
}

function isAllowedLiteral(compiledAllowlist, value) {
  const normalized = value.trim();
  if (!normalized) return true;
  if (compiledAllowlist.literalValues.has(normalized)) return true;
  return compiledAllowlist.literalPatterns.some((pattern) => pattern.test(normalized));
}

function isUiEnglishLiteral(value) {
  const normalized = value.trim();
  if (normalized.length < 2) return false;
  if (!/[A-Za-z]/.test(normalized)) return false;
  if (/^[\w.-]+$/.test(normalized) && !/[A-Z]/.test(normalized)) return false;
  if (/^https?:\/\//i.test(normalized)) return false;
  if (/^#[0-9A-Fa-f]{3,8}$/.test(normalized)) return false;
  return /[a-z]/.test(normalized) || /\s/.test(normalized);
}

function stripComments(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, (match) => ' '.repeat(match.length))
    .replace(
      /(^|[^:])\/\/.*$/gm,
      (match, prefix) => `${prefix}${' '.repeat(match.length - prefix.length)}`,
    );
}

function splitTopLevelArgs(argsText) {
  const args = [];
  let start = 0;
  let depth = 0;
  let quote = '';
  let escape = false;
  for (let i = 0; i < argsText.length; i += 1) {
    const char = argsText[i];
    if (quote) {
      if (escape) {
        escape = false;
      } else if (char === '\\') {
        escape = true;
      } else if (char === quote) {
        quote = '';
      }
      continue;
    }
    if (char === '"' || char === "'" || char === '`') {
      quote = char;
    } else if (char === '(' || char === '[' || char === '{') {
      depth += 1;
    } else if (char === ')' || char === ']' || char === '}') {
      depth = Math.max(0, depth - 1);
    } else if (char === ',' && depth === 0) {
      args.push(argsText.slice(start, i).trim());
      start = i + 1;
    }
  }
  args.push(argsText.slice(start).trim());
  return args;
}

function readStringLiteral(raw) {
  const value = raw.trim();
  const quote = value[0];
  if (!['"', "'", '`'].includes(quote) || value[value.length - 1] !== quote) return null;
  if (quote === '`' && /\$\{/.test(value)) return null;
  try {
    return JSON.parse(
      quote === '"' ? value : `"${value.slice(1, -1).replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`,
    );
  } catch {
    return value.slice(1, -1);
  }
}

function findMatchingParen(text, openIndex) {
  let depth = 0;
  let quote = '';
  let escape = false;
  for (let i = openIndex; i < text.length; i += 1) {
    const char = text[i];
    if (quote) {
      if (escape) {
        escape = false;
      } else if (char === '\\') {
        escape = true;
      } else if (char === quote) {
        quote = '';
      }
      continue;
    }
    if (char === '"' || char === "'" || char === '`') {
      quote = char;
    } else if (char === '(') {
      depth += 1;
    } else if (char === ')') {
      depth -= 1;
      if (depth === 0) return i;
    }
  }
  return -1;
}

function extractCallArgs(text, calleePattern) {
  const calls = [];
  const regex = new RegExp(`(?<![\\w$])(${calleePattern})\\s*\\(`, 'g');
  let match;
  while ((match = regex.exec(text))) {
    const openIndex = text.indexOf('(', match.index);
    const closeIndex = findMatchingParen(text, openIndex);
    if (closeIndex < 0) continue;
    calls.push({
      name: match[1],
      index: match.index,
      args: splitTopLevelArgs(text.slice(openIndex + 1, closeIndex)),
      raw: text.slice(match.index, closeIndex + 1),
      end: closeIndex + 1,
    });
    regex.lastIndex = closeIndex + 1;
  }
  return calls;
}

function scanI18nCalls(root, relPath, text, enKeys) {
  const issues = [];
  const source = stripComments(text);
  const calls = extractCallArgs(source, '\\$t|t|translate|translateOr');
  for (const call of calls) {
    const keyArgIndex = call.name === 'translateOr' ? 1 : 0;
    const fallbackArgIndex = call.name === 'translateOr' ? 2 : 1;
    const key = readStringLiteral(call.args[keyArgIndex] ?? '');
    if (key && !enKeys.has(key)) {
      issues.push(
        makeIssue({
          rule: 'missing-en-key',
          file: relPath,
          line: lineForIndex(text, call.index),
          key,
          message: `Literal i18n key is not present in en.json: ${key}`,
        }),
      );
    }

    const fallback = readStringLiteral(call.args[fallbackArgIndex] ?? '');
    if (fallback && isUiEnglishLiteral(fallback)) {
      issues.push(
        makeIssue({
          rule: 'fallback-literal',
          file: relPath,
          line: lineForIndex(text, call.index),
          key: key ?? '',
          value: fallback,
          message: `I18n fallback literal should be represented in en.json: ${fallback}`,
        }),
      );
    }
  }

  for (const call of extractCallArgs(source, '\\$tp')) {
    const baseKey = readStringLiteral(call.args[0] ?? '');
    if (!baseKey) continue;
    const candidates = [
      `${baseKey}_windows`,
      `${baseKey}_linux`,
      `${baseKey}_macos`,
      `${baseKey}_unix`,
    ];
    if (!candidates.some((key) => enKeys.has(key))) {
      issues.push(
        makeIssue({
          rule: 'missing-en-key',
          file: relPath,
          line: lineForIndex(text, call.index),
          key: baseKey,
          message: `Platform i18n base key has no platform-specific en.json entries: ${baseKey}`,
        }),
      );
    }
  }

  const fallbackRegex =
    /(?:\$t|t)\s*\(\s*(['"`])([^'"`]+)\1\s*\)\s*(?:\|\||\?\?)\s*(['"`])([\s\S]*?)\3/g;
  let fallbackMatch;
  while ((fallbackMatch = fallbackRegex.exec(source))) {
    const fallback = fallbackMatch[4];
    if (!isUiEnglishLiteral(fallback)) continue;
    issues.push(
      makeIssue({
        rule: 'fallback-literal',
        file: relPath,
        line: lineForIndex(text, fallbackMatch.index),
        key: fallbackMatch[2],
        value: fallback,
        message: `I18n expression uses an English fallback literal: ${fallback}`,
      }),
    );
  }

  return issues;
}

function scanVueTemplateLiterals(relPath, text, compiledAllowlist) {
  if (!relPath.endsWith('.vue')) return [];
  const issues = [];
  const openingMatch = /<template\b[^>]*>/i.exec(text);
  if (!openingMatch) return issues;
  const templateOffset = openingMatch.index + openingMatch[0].length;
  const tagRegex = /<\/?template\b[^>]*>/gi;
  tagRegex.lastIndex = templateOffset;
  let depth = 1;
  let closingIndex = -1;
  let tagMatch;
  while ((tagMatch = tagRegex.exec(text))) {
    if (/^<\/template/i.test(tagMatch[0])) {
      depth -= 1;
      if (depth === 0) {
        closingIndex = tagMatch.index;
        break;
      }
    } else {
      depth += 1;
    }
  }
  if (closingIndex < 0) return issues;
  const template = text.slice(templateOffset, closingIndex);

  const textRegex = />([^<>{}]*[A-Za-z][^<>{}]*)</g;
  let match;
  while ((match = textRegex.exec(template))) {
    const value = match[1].replace(/\s+/g, ' ').trim();
    if (!isUiEnglishLiteral(value) || isAllowedLiteral(compiledAllowlist, value)) continue;
    issues.push(
      makeIssue({
        rule: 'hardcoded-template-text',
        file: relPath,
        line: lineForIndex(text, templateOffset + match.index),
        value,
        message: `Hard-coded template text should use i18n: ${value}`,
      }),
    );
  }

  for (const attr of TEMPLATE_ATTRS) {
    const attrRegex = new RegExp(
      `(?<![:@\\w-])${attr}\\s*=\\s*(['"])([^'"{}]*[A-Za-z][^'"{}]*)\\1`,
      'g',
    );
    let attrMatch;
    while ((attrMatch = attrRegex.exec(template))) {
      const value = attrMatch[2].trim();
      if (!isUiEnglishLiteral(value) || isAllowedLiteral(compiledAllowlist, value)) continue;
      issues.push(
        makeIssue({
          rule: 'hardcoded-ui-attr',
          file: relPath,
          line: lineForIndex(text, templateOffset + attrMatch.index),
          value,
          message: `Hard-coded UI attribute "${attr}" should use i18n: ${value}`,
        }),
      );
    }
  }

  return issues;
}

function scanScriptUiLiterals(relPath, text, compiledAllowlist) {
  const issues = [];
  const source = stripComments(text);

  for (const prop of SCRIPT_PROPS) {
    const propRegex = new RegExp(`\\b${prop}\\s*:\\s*(['"\`])([^'"\`]*[A-Za-z][^'"\`]*)\\1`, 'g');
    let match;
    while ((match = propRegex.exec(source))) {
      const value = match[2].trim();
      if (!isUiEnglishLiteral(value) || isAllowedLiteral(compiledAllowlist, value)) continue;
      issues.push(
        makeIssue({
          rule: 'hardcoded-ui-prop',
          file: relPath,
          line: lineForIndex(text, match.index),
          value,
          message: `Hard-coded UI property "${prop}" should use i18n: ${value}`,
        }),
      );
    }
  }

  const messageRegex =
    /\b(?:message|notification)\.(?:success|error|warning|info|create)\s*\(\s*(['"`])([^'"`]*[A-Za-z][^'"`]*)\1/g;
  let messageMatch;
  while ((messageMatch = messageRegex.exec(source))) {
    const value = messageMatch[2].trim();
    if (!isUiEnglishLiteral(value) || isAllowedLiteral(compiledAllowlist, value)) continue;
    issues.push(
      makeIssue({
        rule: 'hardcoded-ui-message',
        file: relPath,
        line: lineForIndex(text, messageMatch.index),
        value,
        message: `Hard-coded message text should use i18n: ${value}`,
      }),
    );
  }

  return issues;
}

function collectSourceFiles(root, compiledAllowlist) {
  const files = [];
  const visit = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const fullPath = path.join(dir, entry.name);
      const relPath = relativePath(root, fullPath);
      if (isAllowedPath(compiledAllowlist, relPath)) continue;
      if (entry.isDirectory()) {
        visit(fullPath);
      } else if (SOURCE_EXTENSIONS.has(path.extname(entry.name))) {
        files.push(fullPath);
      }
    }
  };
  visit(root);
  return files.sort();
}

function compareLocaleFiles(root, enJson, allowlist) {
  const localeDir = path.join(root, 'public', 'assets', 'locale');
  const issues = [];
  const enFlat = flattenMessages(enJson);
  const enTypes = messageNodeTypes(enJson);

  for (const fileName of fs
    .readdirSync(localeDir)
    .filter((name) => name.endsWith('.json'))
    .sort()) {
    if (fileName === 'en.json') continue;
    const filePath = path.join(localeDir, fileName);
    const locale = path.basename(fileName, '.json');
    const messages = readJson(filePath);
    const flat = flattenMessages(messages);
    const types = messageNodeTypes(messages);

    for (const [key, expectedType] of Object.entries(enTypes)) {
      if (types[key] && types[key] !== expectedType) {
        issues.push(
          makeIssue({
            rule: 'locale-type-mismatch',
            locale,
            key,
            file: relativePath(root, filePath),
            expected: expectedType,
            actual: types[key],
            message: `${fileName} has object/scalar mismatch at ${key}`,
          }),
        );
      }
    }

    for (const [key, enValue] of Object.entries(enFlat)) {
      if (!Object.prototype.hasOwnProperty.call(flat, key)) {
        issues.push(
          makeIssue({
            rule: 'locale-missing-key',
            locale,
            key,
            file: relativePath(root, filePath),
            expected: String(enValue ?? ''),
            message: `${fileName} is missing key ${key}`,
          }),
        );
        continue;
      }

      const value = flat[key];
      if (typeof value === 'string' && value.trim() === '') {
        issues.push(
          makeIssue({
            rule: 'locale-empty-value',
            locale,
            key,
            file: relativePath(root, filePath),
            message: `${fileName} has an empty value for ${key}`,
          }),
        );
      }

      if (looksMojibake(value, enValue, key)) {
        issues.push(
          makeIssue({
            rule: 'locale-mojibake',
            locale,
            key,
            file: relativePath(root, filePath),
            value: String(value),
            message: `${fileName} appears to contain mojibake at ${key}`,
          }),
        );
      }

      const expectedPlaceholders = extractPlaceholders(enValue);
      const actualPlaceholders = extractPlaceholders(value);
      if (expectedPlaceholders.join('\0') !== actualPlaceholders.join('\0')) {
        issues.push(
          makeIssue({
            rule: 'locale-placeholder-mismatch',
            locale,
            key,
            file: relativePath(root, filePath),
            expected: expectedPlaceholders.join(','),
            actual: actualPlaceholders.join(','),
            message: `${fileName} placeholder mismatch at ${key}`,
          }),
        );
      }

      const expectedTags = extractHtmlTags(enValue);
      const actualTags = extractHtmlTags(value);
      if (expectedTags.join('\0') !== actualTags.join('\0')) {
        issues.push(
          makeIssue({
            rule: 'locale-html-tag-mismatch',
            locale,
            key,
            file: relativePath(root, filePath),
            expected: expectedTags.join(','),
            actual: actualTags.join(','),
            message: `${fileName} HTML tag mismatch at ${key}`,
          }),
        );
      }

      if (
        typeof enValue === 'string' &&
        typeof value === 'string' &&
        value === enValue &&
        !/^en(?:_|$)/.test(locale) &&
        isUiEnglishLiteral(value) &&
        !isAllowedLiteral(allowlist, value)
      ) {
        issues.push(
          makeIssue({
            rule: 'locale-untranslated-value',
            locale,
            key,
            file: relativePath(root, filePath),
            value,
            message: `${fileName} value is identical to English at ${key}`,
          }),
        );
      }
    }

    for (const key of Object.keys(flat)) {
      if (!Object.prototype.hasOwnProperty.call(enFlat, key)) {
        issues.push(
          makeIssue({
            rule: 'locale-extra-key',
            locale,
            key,
            file: relativePath(root, filePath),
            message: `${fileName} has stale extra key ${key}`,
          }),
        );
      }
    }
  }

  return issues;
}

export function runAudit(rawOptions = {}) {
  const root = path.resolve(rawOptions.root ?? DEFAULT_ROOT);
  const allowlistPath = path.resolve(rawOptions.allowlist ?? DEFAULT_ALLOWLIST);
  const baselinePath = path.resolve(rawOptions.baseline ?? DEFAULT_BASELINE);
  const allowlist = compileAllowlist(readJson(allowlistPath, {}));
  const baseline = readJson(baselinePath, { issues: [] });
  const baselineFingerprints = new Set(
    (baseline.issues ?? []).map((issue) => normalizeFingerprint(issue.fingerprint ?? issue)),
  );
  const localeDir = path.join(root, 'public', 'assets', 'locale');
  const enPath = path.join(localeDir, 'en.json');
  const enJson = readJson(enPath);
  const enKeys = new Set(Object.keys(flattenMessages(enJson)));
  const issues = [];

  issues.push(...compareLocaleFiles(root, enJson, allowlist));

  for (const filePath of collectSourceFiles(root, allowlist)) {
    const relPath = relativePath(root, filePath);
    const text = fs.readFileSync(filePath, 'utf8');
    issues.push(...scanI18nCalls(root, relPath, text, enKeys));
    issues.push(...scanVueTemplateLiterals(relPath, text, allowlist));
    issues.push(...scanScriptUiLiterals(relPath, text, allowlist));
  }

  const filtered = issues.filter((issue) => !allowlist.issueFingerprints.has(issue.fingerprint));
  const unbaselined = filtered.filter((issue) => !baselineFingerprints.has(issue.fingerprint));
  const countsByRule = {};
  for (const issue of filtered) countsByRule[issue.rule] = (countsByRule[issue.rule] ?? 0) + 1;

  return {
    root,
    allowlistPath,
    baselinePath,
    issueCount: filtered.length,
    unbaselinedCount: unbaselined.length,
    countsByRule,
    issues: filtered,
    unbaselined,
  };
}

function formatText(result) {
  const lines = [
    `Frontend l10n static audit`,
    `Root: ${result.root}`,
    `Issues: ${result.issueCount}`,
    `Unbaselined: ${result.unbaselinedCount}`,
  ];
  for (const [rule, count] of Object.entries(result.countsByRule).sort()) {
    lines.push(`  ${rule}: ${count}`);
  }
  const sample = result.unbaselined.slice(0, 50);
  if (sample.length) {
    lines.push('', 'Unbaselined issues:');
    for (const issue of sample) {
      const location = [issue.file, issue.line].filter(Boolean).join(':');
      const subject = issue.key || issue.value || '';
      lines.push(`- ${issue.rule} ${location} ${subject}`);
      lines.push(`  ${issue.message}`);
    }
    if (result.unbaselined.length > sample.length) {
      lines.push(`... ${result.unbaselined.length - sample.length} more unbaselined issues`);
    }
  }
  return lines.join('\n');
}

function writeBaseline(result) {
  const baseline = {
    version: 1,
    generatedAt: new Date().toISOString(),
    issueCount: result.issueCount,
    issues: result.issues
      .map((issue) => ({
        fingerprint: issue.fingerprint,
        rule: issue.rule,
        file: issue.file ?? null,
        locale: issue.locale ?? null,
        key: issue.key ?? null,
        value: issue.value ?? null,
        message: issue.message,
      }))
      .sort((a, b) => a.fingerprint.localeCompare(b.fingerprint)),
  };
  fs.writeFileSync(result.baselinePath, stableJson(baseline));
}

function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) {
    console.log(helpText());
    return;
  }

  const result = runAudit(options);
  if (options.updateBaseline) {
    writeBaseline(result);
  }

  if (options.format === 'json') {
    console.log(stableJson(result));
  } else {
    console.log(formatText(result));
  }

  if (options.strict && result.unbaselinedCount > 0) {
    process.exitCode = 1;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main();
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  }
}
