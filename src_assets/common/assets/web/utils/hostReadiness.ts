export interface ReadinessMetadata {
  platform?: string;
  encoder_status?: {
    state?: 'ready' | 'failed' | 'unknown';
    h264?: boolean;
    hevc?: boolean;
    av1?: boolean;
  };
  capture_status?: {
    configured_backend?: string;
    observed_backend?: string;
    managed_event_driven?: boolean;
    virtual_display_configured?: boolean;
  };
  virtual_display?: {
    capable?: boolean;
    ready?: boolean;
    reason?: string;
    hdr?: string;
    modes?: string[];
    layouts?: string[];
  };
  linux?: { session_role?: 'desktop' | 'greeter' | 'unknown' };
}

export function hostReadiness(
  metadata: ReadinessMetadata | null,
  streaming: boolean,
  failed: boolean,
): 'healthy' | 'streaming' | 'warning' | 'unknown' {
  if (failed) return 'warning';
  if (streaming) return 'streaming';
  if (!metadata) return 'unknown';
  if (metadata.encoder_status?.state === 'failed') return 'warning';
  if (metadata.platform?.includes('linux') && metadata.capture_status?.virtual_display_configured) {
    if (metadata.virtual_display?.ready === false || metadata.virtual_display?.capable === false)
      return 'warning';
    if (metadata.virtual_display?.ready !== true) return 'unknown';
  }
  return metadata.encoder_status?.state === 'ready' ? 'healthy' : 'unknown';
}

export function linuxCaptureState(
  metadata: ReadinessMetadata,
  virtualMode?: string,
): 'active' | 'configured' | 'unavailable' | 'physical' | 'unknown' {
  if (
    metadata.capture_status?.managed_event_driven === true &&
    metadata.capture_status.observed_backend === 'kms'
  )
    return 'active';
  if (
    virtualMode === 'disabled' ||
    (virtualMode === undefined && metadata.capture_status?.virtual_display_configured === false)
  )
    return 'physical';
  if (metadata.virtual_display?.capable === false || metadata.virtual_display?.ready === false)
    return 'unavailable';
  if (
    metadata.virtual_display?.ready === true &&
    metadata.capture_status?.configured_backend === 'kms'
  )
    return 'configured';
  return 'unknown';
}
