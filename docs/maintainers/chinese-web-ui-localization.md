# Chinese Localization Reference

The browser frontend and the `zh` and `zh_TW` JSON catalogs live under
`src_assets/common/assets/web`. The browser interface loads the established
catalog first, then overlays its `ui` catalog from
`public/assets/locale/ui/<locale>.json`. Missing overlay messages fall back to
English until their CrowdIn translation is returned; established keys continue
to use the existing translated terminology.

## Scope

The 2026-06-19 language pass remains useful as a terminology reference for
common labels, settings descriptions, changelog text, troubleshooting messages,
and application-management copy. It intentionally keeps product names,
executable names, codecs, vendor names, environment variables, URLs, and sample
paths untranslated.

## Review Guidance

When updating the retained Chinese catalogs:

- Review `zh.json` and `zh_TW.json` directly.
- Review `ui/zh.json` and `ui/zh_TW.json` when those overlay catalogs are present.
- Keep Simplified and Traditional terminology separate; do not mechanically
  convert one file into the other.
- Preserve brand names, executable names, codecs, environment variables, URLs,
  and sample paths.
- Keep interface wording concise enough for narrow settings rows and mobile
  layouts, and verify that placeholders and interpolation keys remain intact.
