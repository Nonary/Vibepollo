import {
  APP_DISCONNECT_BEHAVIOR_VALUES,
  disconnectBehaviorPayload,
  effectiveAppDisconnectBehavior,
  normalizeGlobalDisconnectBehavior,
  parseAppDisconnectBehavior,
  shouldShowSuspendWarning,
} from '@web/components/app-edit/disconnectBehavior';

describe('application disconnect behavior', () => {
  test('offers inherit, keep-running, suspend, and close per application', () => {
    expect(APP_DISCONNECT_BEHAVIOR_VALUES).toEqual([
      'inherit',
      'keep_running',
      'suspend',
      'terminate',
    ]);
  });

  test('loads all explicit per-application choices', () => {
    expect(parseAppDisconnectBehavior('inherit', true)).toBe('inherit');
    expect(parseAppDisconnectBehavior('keep_running', true)).toBe('keep_running');
    expect(parseAppDisconnectBehavior('suspend', true)).toBe('suspend');
    expect(parseAppDisconnectBehavior('terminate', false)).toBe('terminate');
  });

  test('migrates the legacy terminate-on-pause field', () => {
    expect(parseAppDisconnectBehavior(undefined, true)).toBe('terminate');
    expect(parseAppDisconnectBehavior(undefined, false)).toBe('inherit');
  });

  test('explicit inherit overrides legacy termination', () => {
    expect(parseAppDisconnectBehavior('inherit', true)).toBe('inherit');
  });

  test('inherit is omitted from the saved payload', () => {
    expect(disconnectBehaviorPayload('inherit')).toEqual({});
    expect(disconnectBehaviorPayload('terminate')).toEqual({
      'disconnect-behavior': 'terminate',
    });
  });

  test('invalid global values fall back to keep running', () => {
    expect(normalizeGlobalDisconnectBehavior('keep_running')).toBe('keep_running');
    expect(normalizeGlobalDisconnectBehavior('suspend')).toBe('suspend');
    expect(normalizeGlobalDisconnectBehavior('terminate')).toBe('keep_running');
    expect(normalizeGlobalDisconnectBehavior('invalid')).toBe('keep_running');
  });

  test('shows the warning for explicit and inherited suspend only', () => {
    expect(shouldShowSuspendWarning('suspend', 'keep_running')).toBe(true);
    expect(shouldShowSuspendWarning('inherit', 'suspend')).toBe(true);
    expect(shouldShowSuspendWarning('keep_running', 'suspend')).toBe(false);
    expect(shouldShowSuspendWarning('terminate', 'suspend')).toBe(false);
    expect(effectiveAppDisconnectBehavior('inherit', 'suspend')).toBe('suspend');
  });
});
