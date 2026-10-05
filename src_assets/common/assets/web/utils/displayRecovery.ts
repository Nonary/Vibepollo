export interface DisplayRecoveryResult {
  status?: boolean;
  virtual_displays_removed?: boolean;
  topology_restored?: boolean;
  physical_display_recovered?: boolean;
  restore_dispatched?: boolean;
  database_restore_applied?: boolean;
  error?: string;
}

const introductionKey = 'vibeshine.display-recovery-introduction-dismissed';

export function showDisplayRecoveryIntroduction(): boolean {
  try {
    return localStorage.getItem(introductionKey) !== 'true';
  } catch {
    return true;
  }
}

export function dismissDisplayRecoveryIntroduction(): void {
  try {
    localStorage.setItem(introductionKey, 'true');
  } catch {
    // Recovery remains available when browser storage is disabled.
  }
}

export function displayRecoverySupported(platform: string): boolean {
  return ['windows', 'linux'].includes(platform.trim().toLowerCase());
}

export function displayRecoveryNotice(result: DisplayRecoveryResult): string {
  if (result.status !== true || result.virtual_displays_removed === false) {
    return 'index.display_recovery.failed';
  }
  if (result.physical_display_recovered === false) return 'index.display_recovery.without_restore';
  if (result.topology_restored === true) return 'index.display_recovery.restored';
  if (result.physical_display_recovered === true) return 'index.display_recovery.recovered';
  if (result.topology_restored === false) {
    return 'index.display_recovery.without_restore';
  }
  // A Windows helper dispatch alone cannot confirm physical recovery.
  return 'index.display_recovery.requested';
}
