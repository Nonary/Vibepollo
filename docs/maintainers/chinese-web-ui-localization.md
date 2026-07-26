# Chinese Web UI Localization Review

This note records the Web UI Chinese fluency pass completed and reviewed on 2026-06-19.
It covers `zh` and `zh_TW` strings as rendered in static and production-build Web UI review surfaces.

## Scope

The review focused on high-traffic Web UI pages and modal flows:

- Dashboard, applications, settings, changelog, stats, and troubleshooting pages
- Application edit modal basics, configuration overrides, RTX HDR, frame generation, and Lossless Scaling sections
- Capture and display-device configuration helpers

The pass intentionally kept product names, executable names, codecs, vendor names,
environment variables, URLs, and sample paths untranslated.

## Changes

- Polished Simplified and Traditional Chinese wording for common labels, settings descriptions, hotkey text,
  changelog labels, update checks, troubleshooting logs, and application-management copy.
- Replaced stale Sunshine branding in user-facing Vibepollo strings while preserving Sunshine compatibility names
  where the UI describes environment variables or upstream integrations.
- Localized hardcoded English labels and helper text in the application edit modal and selected configuration panels.
- Added guarded translation handling for checkbox labels and configuration override descriptions so already-rendered text
  is not treated as an i18n key.
- Added focused tests for Chinese string fluency, locale option labels, hardcoded English regressions, and session number
  formatting.

## Verification

Repository-local checks:

```bash
npm exec -- vitest run ../../../../tests/frontend/chinese-localization-fluency.test.ts ../../../../tests/frontend/session-formatting.test.ts
npm exec -- vitest run ../../../../tests/frontend/checkbox.test.ts ../../../../tests/frontend/intl-locale.test.ts
npm run build
```

Run those commands from `src_assets/common/assets/web`.

The locale audit and browser review for this pass were run from the surrounding localization workspace that contains
the custom `tools/` and `reports/` directories:

```bash
node tools/audit-locales.mjs --repo worktrees/chinese-webui
playwright-cli --raw run-code --filename=tools/browser-review-run-code.js
```

The locale audit result was:

```text
zh: 0 missing, 0 identical, 0 likely English, 0 placeholder mismatches
zh_TW: 0 missing, 0 identical, 0 likely English, 0 placeholder mismatches
```

The 2026-06-19 follow-up production browser review covered dashboard, settings, applications, clients, tokens, and stats
for both `zh` and `zh_TW`. It reported:

- no visible hits for the reviewed English phrases
- `document.lang` normalized to `zh-CN` and `zh-TW`
- 0 page errors
- 0 failed requests in the targeted rerun

The broader Web UI test suite still has unrelated failures in `navbar.test.ts` and `http-csrf.test.ts`; track those
separately from Chinese localization unless they are required by the merge gate.

## Review Guidance

When updating Chinese Web UI translations:

- Review `zh` and `zh_TW` in the rendered UI, not only in JSON files.
- Keep Simplified and Traditional terminology separate; do not mechanically convert one file into the other.
- Preserve brand names, executable names, codecs, environment variables, URLs, and sample paths.
- Add or update focused tests when localizing hardcoded component text so regressions are visible without a full
  browser pass.
