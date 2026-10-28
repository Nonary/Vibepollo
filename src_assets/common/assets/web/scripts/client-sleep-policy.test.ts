import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import ts from 'typescript';

// Execute the production editor's pure normalization/merge/payload helpers.
// Vue rendering and API authentication are separate integration boundaries.
const component = readFileSync(new URL('../views/DevicesView.vue', import.meta.url), 'utf8');
const script = component.split('<script setup lang="ts">')[1]!.split('</script>')[0]!;
const parsed = ts.createSourceFile('DevicesView.ts', script, ts.ScriptTarget.Latest, true);
const declarations = parsed.statements
  .filter(
    (statement) =>
      ts.isFunctionDeclaration(statement) ||
      (ts.isVariableStatement(statement) &&
        statement.declarationList.declarations.some(
          (declaration) =>
            ts.isIdentifier(declaration.name) &&
            ['draftScalarKeys', 'PERMISSION_ALL'].includes(declaration.name.text),
        )),
  )
  .map((statement) => statement.getText(parsed))
  .join('\n');
const code = ts.transpileModule(declarations, {
  compilerOptions: { target: ts.ScriptTarget.ES2022 },
}).outputText;
const context = vm.createContext({ structuredClone, toRaw: (value: unknown) => value });
vm.runInContext(code, context);
const call = (name: string, ...arguments_: unknown[]) => context[name](...arguments_);
const device = (allow_sleep?: unknown) =>
  call('normalizeDevice', { uuid: 'paired', name: 'Client', perm: 0x071f1f00, allow_sleep }, 0);

test('sleep authority is default off and accepts only an explicit boolean grant', () => {
  for (const value of [undefined, null, false, 'true', '1', 1]) {
    assert.equal(device(value).allow_sleep, false);
    assert.equal(call('draftFromDevice', device(value)).allowSleep, false);
  }
  assert.equal(device(true).allow_sleep, true);
});

test('sleep checkbox persists explicitly without changing streaming or command preferences', () => {
  const current = device(false);
  const draft = call('draftFromDevice', current);
  const before = call('updatePayload', current, draft);
  draft.allowSleep = true;
  const after = call('updatePayload', current, draft);
  assert.equal(before.allow_sleep, false);
  assert.equal(after.allow_sleep, true);
  assert.equal(JSON.stringify({ ...before, allow_sleep: true }), JSON.stringify(after));
});

test('unrelated edits preserve a concurrent sleep grant or revocation', () => {
  for (const allowed of [false, true]) {
    const origin = call('draftFromDevice', device(!allowed));
    const local = call('cloneDraft', origin);
    local.name = 'Renamed locally';
    const latest = call('draftFromDevice', device(allowed));
    const merged = call('mergeLatestDraft', origin, local, latest);
    assert.equal(merged.conflict, false);
    assert.equal(merged.draft.allowSleep, allowed);
    assert.equal(merged.draft.name, 'Renamed locally');
    assert.equal(call('updatePayload', device(allowed), merged.draft).allow_sleep, allowed);
  }
});
