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
    ecmaVersion: 2021,
    sourceType: 'module',
    extraFileExtensions: ['.vue'],
  },
  extends: [
    'eslint:recommended',
    'plugin:vue/vue3-recommended',
    'plugin:@typescript-eslint/recommended',
    'plugin:prettier/recommended',
    // Keep prettier last to turn off conflicting rules (plugin adds "prettier/prettier" rule)
    'prettier',
  ],
  plugins: ['vue', '@typescript-eslint', 'prettier'],
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
  ],
};
