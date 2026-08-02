import { readFile, writeFile } from "node:fs/promises";

const sourceUrl = new URL("../design/tokens.json", import.meta.url);
const cssUrl = new URL("../styles/tokens.css", import.meta.url);
const typescriptUrl = new URL("../generated/tokens.ts", import.meta.url);
const checkOnly = process.argv.includes("--check");
const printTarget = process.argv.find((argument) => argument.startsWith("--print="))?.slice(8);

const source = JSON.parse(await readFile(sourceUrl, "utf8"));
const tokenIndex = new Map();

function indexTokens(node, path = [], inheritedType) {
  if (!node || typeof node !== "object" || Array.isArray(node)) {
    return;
  }

  const type = node.$type ?? inheritedType;
  if (Object.hasOwn(node, "$value")) {
    tokenIndex.set(path.join("."), { path, type, value: node.$value });
    return;
  }

  for (const [key, value] of Object.entries(node)) {
    if (!key.startsWith("$")) {
      indexTokens(value, [...path, key], type);
    }
  }
}

indexTokens(source);

const aliasPattern = /^\{([^}]+)\}$/;

function resolveToken(tokenPath, stack = []) {
  if (stack.includes(tokenPath)) {
    throw new Error(`Circular token alias: ${[...stack, tokenPath].join(" -> ")}`);
  }

  const token = tokenIndex.get(tokenPath);
  if (!token) {
    throw new Error(`Unknown token alias: ${tokenPath}`);
  }

  if (typeof token.value === "string") {
    const alias = token.value.match(aliasPattern);
    if (alias) {
      return resolveToken(alias[1], [...stack, tokenPath]);
    }
    return token.value;
  }

  if (typeof token.value === "number") {
    return String(token.value);
  }

  if (
    token.value &&
    typeof token.value === "object" &&
    Object.hasOwn(token.value, "value") &&
    Object.hasOwn(token.value, "unit")
  ) {
    return `${token.value.value}${token.value.unit}`;
  }

  throw new Error(`Unsupported value for token: ${tokenPath}`);
}

const themeBackgrounds = ["canvas", "surface", "subtle", "raised"];
const themeTextColors = [
  "textPrimary",
  "textSecondary",
  "textMuted",
  "accentDefault",
  "accentHover",
  "success",
  "warning",
  "danger",
  "info",
  "dataAccent",
];
const minimumTextContrast = 4.5;
const minimumNonTextContrast = 3;

function parseHexColor(value, tokenPath) {
  const match = /^#([\dA-F]{2})([\dA-F]{2})([\dA-F]{2})$/i.exec(value);
  if (!match) {
    throw new Error(`${tokenPath} must be a six-digit hex color for contrast validation`);
  }
  return match.slice(1).map((channel) => Number.parseInt(channel, 16) / 255);
}

function relativeLuminance(value, tokenPath) {
  const [red, green, blue] = parseHexColor(value, tokenPath).map((channel) =>
    channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4,
  );
  return 0.2126 * red + 0.7152 * green + 0.0722 * blue;
}

function contrastRatio(foreground, background, foregroundPath, backgroundPath) {
  const lighter = Math.max(
    relativeLuminance(foreground, foregroundPath),
    relativeLuminance(background, backgroundPath),
  );
  const darker = Math.min(
    relativeLuminance(foreground, foregroundPath),
    relativeLuminance(background, backgroundPath),
  );
  return (lighter + 0.05) / (darker + 0.05);
}

function assertContrast(themeName, foregroundName, backgroundName, minimum) {
  const foregroundPath = `primitive.color.${themeName}.${foregroundName}`;
  const backgroundPath = `primitive.color.${themeName}.${backgroundName}`;
  const foreground = resolveToken(foregroundPath);
  const background = resolveToken(backgroundPath);
  const ratio = contrastRatio(foreground, background, foregroundPath, backgroundPath);
  if (ratio + Number.EPSILON < minimum) {
    throw new Error(
      `${foregroundPath} on ${backgroundPath} has ${ratio.toFixed(2)}:1 contrast; expected at least ${minimum}:1`,
    );
  }
}

function validateThemeContrast() {
  // WCAG 2.2 SC 1.4.3 and 1.4.11. Subtle borders are decorative; essential
  // control boundaries use borderStrong and are validated here.
  for (const themeName of ["dark", "light"]) {
    for (const foregroundName of themeTextColors) {
      for (const backgroundName of themeBackgrounds) {
        assertContrast(themeName, foregroundName, backgroundName, minimumTextContrast);
      }
    }

    for (const accentName of ["accentDefault", "accentHover"]) {
      assertContrast(themeName, "onAccent", accentName, minimumTextContrast);
    }

    for (const backgroundName of themeBackgrounds) {
      assertContrast(themeName, "borderStrong", backgroundName, minimumNonTextContrast);
      assertContrast(themeName, "focus", backgroundName, minimumNonTextContrast);
    }
  }
}

validateThemeContrast();

function collect(node, sourcePath, outputPath = [], inheritedType, output = []) {
  if (!node || typeof node !== "object" || Array.isArray(node)) {
    return output;
  }

  const type = node.$type ?? inheritedType;
  if (Object.hasOwn(node, "$value")) {
    output.push({
      path: outputPath,
      type,
      value: resolveToken(sourcePath.join(".")),
    });
    return output;
  }

  for (const [key, value] of Object.entries(node)) {
    if (!key.startsWith("$")) {
      collect(value, [...sourcePath, key], [...outputPath, key], type, output);
    }
  }
  return output;
}

function toKebab(value) {
  return value
    .replace(/([A-Z]+)([A-Z][a-z])/g, "$1-$2")
    .replace(/([a-z0-9])([A-Z])/g, "$1-$2")
    .replace(/[_\s]+/g, "-")
    .toLowerCase();
}

function cssName(path) {
  return `--vs-${path.map(toKebab).join("-")}`;
}

function mergeTokenSets(...sets) {
  const merged = new Map();
  for (const set of sets) {
    for (const token of set) {
      merged.set(cssName(token.path), token);
    }
  }
  return [...merged.entries()]
    .map(([name, token]) => ({ ...token, name }))
    .sort((left, right) => (left.name < right.name ? -1 : left.name > right.name ? 1 : 0));
}

function groupTokens(rootPath) {
  let node = source;
  for (const segment of rootPath) {
    node = node?.[segment];
  }
  if (!node) {
    throw new Error(`Missing token group: ${rootPath.join(".")}`);
  }
  return collect(node, rootPath);
}

const semanticShared = groupTokens(["semantic", "shared"]);
const semanticDark = groupTokens(["semantic", "dark"]);
const semanticLight = groupTokens(["semantic", "light"]);
const semanticDensityDefault = groupTokens(["semantic", "density", "default"]);
const semanticDensityCompact = groupTokens(["semantic", "density", "compact"]);
const componentShared = groupTokens(["component", "shared"]);
const componentDark = groupTokens(["component", "dark"]);
const componentLight = groupTokens(["component", "light"]);
const componentDensityDefault = groupTokens(["component", "density", "default"]);
const componentDensityCompact = groupTokens(["component", "density", "compact"]);

const darkTokens = mergeTokenSets(
  semanticDark,
  semanticShared,
  semanticDensityDefault,
  componentDark,
  componentShared,
  componentDensityDefault,
);
const lightThemeTokens = mergeTokenSets(semanticLight, componentLight);
const lightTokens = mergeTokenSets(
  semanticLight,
  semanticShared,
  semanticDensityDefault,
  componentLight,
  componentShared,
  componentDensityDefault,
);
const compactTokens = mergeTokenSets(semanticDensityCompact, componentDensityCompact);

function renderDeclarations(tokens) {
  return tokens.map((token) => `  ${token.name}: ${token.value};`).join("\n");
}

const css = `/* Generated by scripts/generate-tokens.mjs from design/tokens.json. Do not edit. */
:root,
[data-theme="dark"] {
  color-scheme: dark;
${renderDeclarations(darkTokens)}
}

[data-theme="light"] {
  color-scheme: light;
${renderDeclarations(lightThemeTokens)}
}

[data-density="compact"] {
${renderDeclarations(compactTokens)}
}

@media (prefers-reduced-motion: reduce) {
  :root {
    --vs-motion-duration-immediate: 0ms;
    --vs-motion-duration-control: 0ms;
    --vs-motion-duration-overlay: 0ms;
    --vs-motion-duration-layout: 0ms;
  }
}
`;

function identifierFor(path) {
  const words = path.flatMap((segment) =>
    segment
      .replace(/([A-Z]+)([A-Z][a-z])/g, "$1 $2")
      .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
      .replace(/[^a-zA-Z0-9]+/g, " ")
      .trim()
      .split(/\s+/),
  );
  return words
    .map((word, index) => {
      const normalized = word.toLowerCase();
      return index === 0 ? normalized : normalized[0].toUpperCase() + normalized.slice(1);
    })
    .join("");
}

const allTokens = mergeTokenSets(darkTokens, lightTokens, compactTokens);
const tokenEntries = allTokens
  .map((token) => ({ key: identifierFor(token.path), name: token.name }))
  .sort((left, right) => (left.key < right.key ? -1 : left.key > right.key ? 1 : 0));

const seenIdentifiers = new Set();
for (const token of tokenEntries) {
  if (seenIdentifiers.has(token.key)) {
    throw new Error(`Generated TypeScript identifier collision: ${token.key}`);
  }
  seenIdentifiers.add(token.key);
}

const nameLines = tokenEntries.map(({ key, name }) => `  ${key}: "${name}",`).join("\n");
const valueLines = tokenEntries.map(({ key, name }) => `  ${key}: "var(${name})",`).join("\n");

const typescript = `/* Generated by scripts/generate-tokens.mjs from design/tokens.json. Do not edit. */

export const tokenNames = {
${nameLines}
} as const;

export const tokens = {
${valueLines}
} as const;

export const themes = ["dark", "light"] as const;
export const densities = ["default", "compact"] as const;

export type TokenKey = keyof typeof tokenNames;
export type TokenName = (typeof tokenNames)[TokenKey];
export type ThemeName = (typeof themes)[number];
export type DensityName = (typeof densities)[number];

export function cssVar(name: TokenName): \`var(\${TokenName})\` {
  return \`var(\${name})\`;
}
`;

async function emit(url, contents) {
  if (checkOnly) {
    const current = await readFile(url, "utf8").catch(() => "");
    if (current !== contents) {
      process.exitCode = 1;
      console.error(`${url.pathname} is out of date`);
    }
    return;
  }
  await writeFile(url, contents, "utf8");
}

if (printTarget) {
  if (printTarget !== "css" && printTarget !== "typescript") {
    throw new Error("--print must be either css or typescript");
  }
  process.stdout.write(printTarget === "css" ? css : typescript);
} else {
  await Promise.all([emit(cssUrl, css), emit(typescriptUrl, typescript)]);
}
