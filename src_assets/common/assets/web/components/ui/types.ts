// Public type contracts for the Vibepollo UI primitives.
export type UiIconName =
  | 'activity'
  | 'alert-triangle'
  | 'check'
  | 'check-circle'
  | 'chevron-down'
  | 'chevron-left'
  | 'chevron-right'
  | 'copy'
  | 'devices'
  | 'download'
  | 'edit'
  | 'external-link'
  | 'gamepad'
  | 'help'
  | 'info'
  | 'integrations'
  | 'key'
  | 'library'
  | 'logs'
  | 'menu'
  | 'minus'
  | 'more'
  | 'overview'
  | 'play'
  | 'plus'
  | 'refresh'
  | 'search'
  | 'sessions'
  | 'settings'
  | 'stop'
  | 'trash'
  | 'upload'
  | 'user'
  | 'warning'
  | 'x'
  | 'x-circle';

export type StatusTone = 'neutral' | 'info' | 'success' | 'warning' | 'danger';
export type AlertTone = Exclude<StatusTone, 'neutral'>;
export type ButtonVariant = 'primary' | 'secondary' | 'tertiary' | 'danger';
export type ButtonSize = 'compact' | 'default' | 'touch';
