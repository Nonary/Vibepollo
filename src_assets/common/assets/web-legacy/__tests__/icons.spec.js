import { describe, it, expect } from 'vitest';
import { createApp } from 'vue';
import Shell from '@/layout/Shell.vue';
import '@/main.js';

// Smoke test ensuring Font Awesome CSS (and thus icon classes) are present after bundling.
// JSDOM cannot verify font network requests, but class names should render.

describe('Font Awesome integration', () => {
  it('mounts root app and contains an FA icon element', () => {
    const app = createApp(Shell);
    const el = document.createElement('div');
    app.mount(el);
    const found = el.querySelector('i.fa-solid, i.fas');
    expect(found).toBeTruthy();
  });
});
