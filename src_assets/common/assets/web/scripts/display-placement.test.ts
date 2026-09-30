import assert from 'node:assert/strict';
import test from 'node:test';
import { setDisplayArrangement, type DisplayPlacement } from '../utils/displayPlacement.ts';

const below: DisplayPlacement = { anchor_kind: 'physical', anchor_id: 'screen', edge: 'below', alignment: 'center', gap_px: 20, primary: false };

test('manual arrangement preserves the selected rule and survives persistence', () => {
  const placements = { tablet: { ...below }, peer: { ...below, edge: 'left' as const } };
  setDisplayArrangement(placements, 'tablet', 'preserve', below);
  const persisted = JSON.parse(JSON.stringify({ version: 1, placements }));
  assert.equal(persisted.placements.tablet.preserve, true);
  assert.equal(persisted.placements.tablet.edge, 'below');
  assert.equal(persisted.placements.peer.edge, 'left');
});

test('each direction retains the chosen anchor, alignment, gap and primary policy', () => {
  const placements: Record<string, DisplayPlacement> = {};
  for (const direction of ['above', 'below', 'left', 'right'] as const) {
    setDisplayArrangement(placements, 'tablet', direction, below);
    assert.deepEqual(placements.tablet, { ...below, edge: direction, preserve: false });
  }
  setDisplayArrangement(placements, '', 'right', below);
  assert.equal(placements[''], undefined);
});

test('automatic arrangement clears only the selected client override', () => {
  const placements = { tablet: { ...below, preserve: true }, peer: { ...below } };
  setDisplayArrangement(placements, 'tablet', 'automatic', below);
  assert.equal(placements.tablet, undefined);
  assert.deepEqual(placements.peer, below);
});
