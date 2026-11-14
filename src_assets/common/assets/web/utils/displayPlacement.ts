export type DisplayEdge = 'left' | 'right' | 'above' | 'below';
export type DisplayAlignment = 'start' | 'center' | 'end';
export type DisplayAnchorKind = 'physical' | 'client';
export interface DisplayPlacement {
  anchor_kind: DisplayAnchorKind;
  anchor_id: string;
  edge: DisplayEdge;
  alignment: DisplayAlignment;
  gap_px: number;
  primary: boolean;
  preserve?: boolean;
  automatic_fallback?: boolean;
}

// Selecting a client must never create a rule. Explicit preserve also takes
// precedence over an application's isolated placement, without losing the
// directional rule the user can return to later.
export function setDisplayArrangement(
  placements: Record<string, DisplayPlacement>,
  client: string,
  edge: DisplayEdge | 'preserve' | 'automatic',
  fallback: DisplayPlacement,
) {
  if (!client) return;
  if (edge === 'automatic') { delete placements[client]; return; }
  placements[client] = { ...(placements[client] ?? fallback), preserve: edge === 'preserve' };
  if (placements[client].automatic_fallback || (!placements[client].anchor_id && edge !== 'preserve')) {
    // A detached placement has no usable anchor; choose a current one when
    // returning to a directional rule, while retaining its primary preference.
    placements[client].anchor_kind = fallback.anchor_kind;
    placements[client].anchor_id = fallback.anchor_id;
    delete placements[client].automatic_fallback;
  }
  if (edge !== 'preserve') placements[client].edge = edge;
}
