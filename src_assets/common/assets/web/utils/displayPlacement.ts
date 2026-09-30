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
  if (edge !== 'preserve') placements[client].edge = edge;
}
