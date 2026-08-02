module.exports = {
  root: true,
  env: {
    browser: true,
    node: true,
    es2021: true,
  },
  parser: 'vue-eslint-parser',
  parserOptions: {
    parser: '@typescript-eslint/parser',
    // Enable type-aware rules
    project: ['./tsconfig.json'],
    tsconfigRootDir: __dirname,
    ecmaVersion: 2021,
    sourceType: 'module',
    extraFileExtensions: ['.vue'],
  },
  extends: [
    'eslint:recommended',
    'plugin:vue/vue3-recommended',
    'plugin:@typescript-eslint/recommended',
    'plugin:@typescript-eslint/recommended-requiring-type-checking',
    'plugin:prettier/recommended',
    // Keep prettier last to turn off conflicting rules (plugin adds "prettier/prettier" rule)
    'prettier',
  ],
  plugins: ['vue', '@typescript-eslint', 'prettier', 'eslint-comments'],
  // Relax rules that would produce errors across the existing codebase
  // (turn into warnings or disable to avoid blocking lint runs)
  rules: Object.assign(
    {
      'no-empty': 'warn',
      'no-prototype-builtins': 'off',
      '@typescript-eslint/ban-types': 'off',
      '@typescript-eslint/no-explicit-any': 'off',
    },
    {
      // Project-specific overrides (kept after the relaxations)
      'no-unused-vars': 'off',
      'prettier/prettier': 'error',
      '@typescript-eslint/no-unused-vars': ['warn', { argsIgnorePattern: '^_' }],
      'vue/multi-word-component-names': 'off',
    },
  ),
  overrides: [
    {
      files: ['*.vue'],
      rules: {
        'no-undef': 'off',
      },
    },
    {
      files: ['**/*.ts', '**/*.tsx', '**/*.vue'],
      rules: {
        // 1) No "any" or unsafe flows
        '@typescript-eslint/no-explicit-any': 'error',
        '@typescript-eslint/no-unsafe-assignment': 'error',
        '@typescript-eslint/no-unsafe-member-access': 'error',
        '@typescript-eslint/no-unsafe-call': 'error',
        '@typescript-eslint/no-unsafe-return': 'error',
        '@typescript-eslint/no-unsafe-argument': 'error',

        // 2) No assertion or comment “cheats”
        '@typescript-eslint/no-non-null-assertion': 'error',
        '@typescript-eslint/no-unnecessary-type-assertion': 'error',
        '@typescript-eslint/consistent-type-assertions': [
          'error',
          {
            assertionStyle: 'as',
            objectLiteralTypeAssertions: 'never',
          },
        ],
        '@typescript-eslint/ban-ts-comment': [
          'error',
          {
            'ts-ignore': true,
            'ts-nocheck': true,
            'ts-check': false,
            'ts-expect-error': true,
            minimumDescriptionLength: 8,
          },
        ],
        'eslint-comments/no-unlimited-disable': 'error',
        'eslint-comments/no-unused-disable': 'error',
        'eslint-comments/require-description': ['error', { ignore: [] }],

        // 3) Prefer precise types & safe indexing
        '@typescript-eslint/ban-types': [
          'error',
          {
            extendDefaults: true,
            types: {
              '{}': {
                message: 'Use a specific object shape or Record<string, unknown>.',
              },
              object: { message: 'Use a specific object type.' },
              null: {
                message: 'Avoid `null` types; model absence with optional props or ADTs.',
              },
              undefined: {
                message: 'Avoid `undefined` types; use optionals or domain types.',
              },
            },
          },
        ],
        '@typescript-eslint/consistent-indexed-object-style': ['error', 'record'],

        // 4) Disallow `| null` / `| undefined` in unions
        'no-restricted-syntax': [
          'error',
          {
            selector: 'TSUnionType > TSNullKeyword',
            message:
              'Do not use `null` in union types. Prefer optional props or a domain sum type (e.g., Option).',
          },
          {
            selector: 'TSUnionType > TSUndefinedKeyword',
            message: 'Do not use `undefined` in union types. Prefer optional props (foo?: T).',
          },
          {
            selector: 'TSAsExpression > TSAnyKeyword',
            message: 'Do not cast to `any`.',
          },
          {
            selector:
              "TSAsExpression[right.type='TSTypeReference'] > TSAsExpression > TSUnknownKeyword",
            message: 'Do not double-assert via `unknown`. Fix the types instead.',
          },
        ],

        // 5) Tighten common weak spots
        '@typescript-eslint/restrict-plus-operands': 'error',
        '@typescript-eslint/restrict-template-expressions': [
          'error',
          { allowNumber: true, allowBoolean: false, allowAny: false, allowNullish: false },
        ],
        '@typescript-eslint/no-base-to-string': 'error',
      },
    },
    // Generated types & ambient declarations often need unions and `any`.
    {
      files: ['**/*.d.ts', '**/generated/**', '**/*.gen.ts', '**/*.gen.tsx'],
      rules: {
        '@typescript-eslint/no-explicit-any': 'off',
        '@typescript-eslint/ban-types': 'off',
        'no-restricted-syntax': 'off',
      },
    },
  ],
};
