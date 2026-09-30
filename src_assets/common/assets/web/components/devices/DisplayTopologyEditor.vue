<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, toRaw } from 'vue';

import { apiGet, apiRequest, type ApiPayload } from '@/api/client';
import { AppButton, InlineAlert, StatusBadge } from '@/components/ui';
import { setDisplayArrangement, type DisplayEdge as Edge, type DisplayPlacement as Placement, type DisplayAnchorKind as AnchorKind } from '@/utils/displayPlacement';

interface Node { id: string; label: string; kind: AnchorKind; active: boolean; primary: boolean; desired_position: { x: number; y: number }; mode: { width: number; height: number; refresh_hz: number } }
interface Response { layout: { version: number; placements: Record<string, Placement> }; nodes: Node[]; clients: Array<{ uuid: string; name: string; display_mode?: string }>; runtime?: Record<string, { lifecycle: string; ready: boolean; retryable: boolean; warning?: string; plaintext_rtsp_warning?: string }>; capacity?: { max: number; used: number }; warnings?: string[] }

const props = withDefaults(defineProps<{ clientUuid?: string; compact?: boolean }>(), { clientUuid: '', compact: false });
const data = ref<Response | null>(null);
const draft = ref<Record<string, Placement>>({});
const selected = ref('');
const error = ref('');
const saving = ref(false);

const knownClients = computed(() => data.value?.clients ?? []);
const focusClients = computed(() => props.clientUuid ? knownClients.value.filter((client) => client.uuid === props.clientUuid) : knownClients.value);
const selectedPlacement = computed<Placement | null>(() => selected.value ? draft.value[selected.value] ?? null : null);
const headingId = computed(() => `display-topology-title-${(props.clientUuid || 'all').replace(/[^a-zA-Z0-9_-]/g, '-')}`);

function pairedClientMode(displayMode?: string) {
  const match = displayMode?.match(/^(\d+)x(\d+)(?:@(\d+))?/i);
  return match ? { width: Number(match[1]), height: Number(match[2]), refresh_hz: Number(match[3] ?? 60) } : { width: 1920, height: 1080, refresh_hz: 60 };
}
const previewNodes = computed<Node[]>(() => {
  if (!data.value) return [];
  const nodes = structuredClone(toRaw(data.value.nodes ?? []));
  let rightmost = nodes.reduce((edge, node) => Math.max(edge, node.desired_position.x + node.mode.width), 0);
  for (const client of knownClients.value) {
    if (nodes.some((node) => node.id === client.uuid)) continue;
    const mode = pairedClientMode(client.display_mode);
    nodes.push({ id: client.uuid, label: client.name || client.uuid, kind: 'client', active: false, primary: false, desired_position: { x: rightmost, y: 0 }, mode });
    rightmost += mode.width;
  }

  const byId = new Map(nodes.map((node) => [node.id, node]));
  const placed = new Set(nodes.filter((node) => node.kind === 'physical').map((node) => node.id));
  const visiting = new Set<string>();
  const place = (id: string) => {
    if (placed.has(id)) return;
    const node = byId.get(id); const placement = draft.value[id];
    if (!node || !placement || placement.preserve || visiting.has(id)) { if (node) placed.add(id); return; }
    visiting.add(id);
    if (placement.anchor_kind === 'client') place(placement.anchor_id);
    const anchor = byId.get(placement.anchor_id);
    if (anchor) {
      const gap = placement.gap_px || 0;
      if (placement.edge === 'left') node.desired_position.x = anchor.desired_position.x - node.mode.width - gap;
      if (placement.edge === 'right') node.desired_position.x = anchor.desired_position.x + anchor.mode.width + gap;
      if (placement.edge === 'above') node.desired_position.y = anchor.desired_position.y - node.mode.height - gap;
      if (placement.edge === 'below') node.desired_position.y = anchor.desired_position.y + anchor.mode.height + gap;
      if (placement.edge === 'left' || placement.edge === 'right') node.desired_position.y = placement.alignment === 'start' ? anchor.desired_position.y : placement.alignment === 'end' ? anchor.desired_position.y + anchor.mode.height - node.mode.height : anchor.desired_position.y + (anchor.mode.height - node.mode.height) / 2;
      if (placement.edge === 'above' || placement.edge === 'below') node.desired_position.x = placement.alignment === 'start' ? anchor.desired_position.x : placement.alignment === 'end' ? anchor.desired_position.x + anchor.mode.width - node.mode.width : anchor.desired_position.x + (anchor.mode.width - node.mode.width) / 2;
      node.primary = placement.primary;
    }
    visiting.delete(id); placed.add(id);
  };
  [...knownClients.value].sort((left, right) => left.uuid.localeCompare(right.uuid)).forEach((client) => place(client.uuid));
  return nodes;
});
const clientNodes = computed(() => previewNodes.value.filter((node) => node.kind === 'client'));
const anchors = computed(() => previewNodes.value.filter((node) => node.id !== selected.value));
const previewOrigin = computed(() => ({
  x: Math.min(0, ...previewNodes.value.map((node) => node.desired_position.x)),
  y: Math.min(0, ...previewNodes.value.map((node) => node.desired_position.y)),
}));

function defaultPlacement(client: string): Placement {
  const physical = previewNodes.value.find((node) => node.kind === 'physical');
  const fallback = physical ?? clientNodes.value.find((node) => node.id !== client);
  return { anchor_kind: fallback?.kind ?? 'physical', anchor_id: fallback?.id ?? '', edge: 'right', alignment: 'center', gap_px: 0, primary: false };
}
function choose(client: string) { selected.value = client; }
const selectedArrangement = computed<Edge | 'preserve' | 'automatic'>({
  get: () => !selectedPlacement.value ? 'automatic' : selectedPlacement.value.preserve ? 'preserve' : selectedPlacement.value.edge,
  set: (edge) => setDisplayArrangement(draft.value, selected.value, edge, defaultPlacement(selected.value)),
});
function validation(): string {
  const all = Object.entries(draft.value);
  if (all.filter(([, placement]) => placement.primary && !placement.preserve).length > 1) return 'Only one client display may be primary.';
  for (const [client, placement] of all) {
    if (placement.preserve) continue;
    if (!placement.anchor_id || placement.anchor_id === client) return 'Choose a different known anchor for every placement.';
    if (!Number.isInteger(placement.gap_px) || placement.gap_px < 0 || placement.gap_px > 100000) return 'Gap must be a nonnegative whole number.';
    const seen = new Set<string>(); let current = client;
    while (draft.value[current]?.anchor_kind === 'client' && !draft.value[current]?.preserve) { if (seen.has(current)) return 'Client anchors cannot form a cycle.'; seen.add(current); current = draft.value[current].anchor_id; }
  }
  return '';
}
const validationError = computed(validation);
function rebuildDraft(response: Response) { data.value = response; draft.value = structuredClone(response.layout?.placements ?? {}); if (props.clientUuid) choose(props.clientUuid); }
async function load() { error.value = ''; try { rebuildDraft(await apiGet<Response>('/api/clients/display-layout')); } catch { error.value = 'Unable to load the display layout.'; } }
async function save() { if (validationError.value) return; saving.value = true; error.value = ''; try { const response = await apiRequest<Response>('/api/clients/display-layout', { method: 'PUT', json: { version: 1, placements: draft.value } as ApiPayload }); rebuildDraft(response); } catch { error.value = 'The layout was not saved. No active topology was changed.'; } finally { saving.value = false; } }
function revert() { if (data.value) draft.value = structuredClone(toRaw(data.value.layout.placements ?? {})); }
function scaled(node: Node) { const width = Math.max(80, Math.round(node.mode.width / 18)); const height = Math.max(50, Math.round(node.mode.height / 18)); return { width: `${width}px`, height: `${height}px`, transform: `translate(${18 + Math.round((node.desired_position.x - previewOrigin.value.x) / 18)}px, ${18 + Math.round((node.desired_position.y - previewOrigin.value.y) / 18)}px)` }; }
interface DragGesture { pointerId: number; clientId: string; startX: number; startY: number; moved: boolean; element: HTMLElement }
let dragGesture: DragGesture | null = null;
const dragThresholdPx = 5;
function ownsDrag(event: PointerEvent, client: Node) {
  return dragGesture?.pointerId === event.pointerId && dragGesture.clientId === client.id && dragGesture.element === event.currentTarget;
}
function clearDrag() {
  const gesture = dragGesture;
  dragGesture = null;
  if (gesture?.element.hasPointerCapture(gesture.pointerId)) gesture.element.releasePointerCapture(gesture.pointerId);
}
function startDrag(event: PointerEvent, client: Node) {
  if (client.kind !== 'client' || !event.isPrimary || event.button !== 0 || dragGesture) return;
  choose(client.id);
  const element = event.currentTarget as HTMLElement;
  dragGesture = { pointerId: event.pointerId, clientId: client.id, startX: event.clientX, startY: event.clientY, moved: false, element };
  element.setPointerCapture(event.pointerId);
}
function moveDrag(event: PointerEvent, client: Node) {
  if (!ownsDrag(event, client) || !dragGesture) return;
  dragGesture.moved ||= Math.hypot(event.clientX - dragGesture.startX, event.clientY - dragGesture.startY) > dragThresholdPx;
}
function finishDrag(event: PointerEvent, client: Node) {
  if (!ownsDrag(event, client) || !dragGesture) return;
  const moved = dragGesture.moved || Math.hypot(event.clientX - dragGesture.startX, event.clientY - dragGesture.startY) > dragThresholdPx;
  clearDrag();
  if (moved) dragPlace(event, client);
}
function cancelDrag(event: PointerEvent, client: Node) {
  if (ownsDrag(event, client)) clearDrag();
}
function keyboardPlace(edge: Edge, client: Node) {
  if (client.kind !== 'client') return;
  choose(client.id);
  selectedArrangement.value = edge;
}
function dragPlace(event: PointerEvent, client: Node) {
  const canvas = (event.currentTarget as HTMLElement).closest('.topology-canvas');
  if (!canvas || client.kind !== 'client') return;
  choose(client.id);
  const rect = canvas.getBoundingClientRect();
  const point = { x: event.clientX - rect.left + canvas.scrollLeft, y: event.clientY - rect.top + canvas.scrollTop };
  const candidates = previewNodes.value.filter((node) => node.id !== client.id);
  const canvasPoint = (node: Node) => ({ x: 18 + (node.desired_position.x - previewOrigin.value.x) / 18, y: 18 + (node.desired_position.y - previewOrigin.value.y) / 18 });
  const anchor = candidates.sort((left, right) => { const l = canvasPoint(left); const r = canvasPoint(right); return Math.hypot(point.x - l.x, point.y - l.y) - Math.hypot(point.x - r.x, point.y - r.y); })[0];
  if (!anchor) return;
  selectedArrangement.value = 'right';
  const placement = selectedPlacement.value;
  if (!placement) return;
  const anchorPoint = canvasPoint(anchor); const dx = point.x - anchorPoint.x; const dy = point.y - anchorPoint.y;
  placement.anchor_id = anchor.id; placement.anchor_kind = anchor.kind;
  placement.edge = Math.abs(dx) >= Math.abs(dy) ? (dx < 0 ? 'left' : 'right') : (dy < 0 ? 'above' : 'below');
  placement.alignment = 'center'; placement.gap_px = 0;
}
onBeforeUnmount(clearDrag);
onMounted(load);
</script>

<template>
  <section class="topology-editor vs-surface" :class="{ 'topology-editor--compact': compact }" :aria-labelledby="headingId">
    <div class="topology-editor__heading">
      <div><h3 :id="headingId">Client display arrangement</h3><p>Saved directions apply to extended application streams and Remote Monitor on their next activation. Verify only preserves the current Windows arrangement; choose an automatic display preparation option to apply a saved direction.</p></div>
      <StatusBadge v-if="data?.capacity" :label="`${data.capacity.used} of ${data.capacity.max} client identities`" tone="neutral" compact />
    </div>
    <InlineAlert v-if="error || validationError" tone="danger">{{ error || validationError }}</InlineAlert>
    <InlineAlert v-for="warning in data?.warnings ?? []" :key="warning" tone="warning">{{ warning }}</InlineAlert>
    <label v-if="focusClients.length" class="topology-client-picker">Client display<select v-model="selected" @change="choose(selected)"><option disabled value="">Choose a paired client</option><option v-for="client in focusClients" :key="client.uuid" :value="client.uuid">{{ client.name || client.uuid }}</option></select></label>
    <div class="topology-canvas" role="group" aria-label="Display topology. Select a display rectangle to edit its placement.">
      <button v-for="node in previewNodes" :key="node.id" type="button" class="topology-node" :class="[`topology-node--${node.kind}`, { 'is-selected': selected === node.id, 'is-inactive': !node.active }]" :style="scaled(node)" :aria-pressed="selected === node.id" @click="node.kind === 'client' && choose(node.id)" @pointerdown="startDrag($event, node)" @pointermove="moveDrag($event, node)" @pointerup="finishDrag($event, node)" @pointercancel="cancelDrag($event, node)" @lostpointercapture="cancelDrag($event, node)" @keydown.left.prevent="keyboardPlace('left', node)" @keydown.right.prevent="keyboardPlace('right', node)" @keydown.up.prevent="keyboardPlace('above', node)" @keydown.down.prevent="keyboardPlace('below', node)">
        <strong>{{ node.kind === 'physical' ? 'Physical' : 'Client' }} · {{ node.label || node.id }}</strong><span>{{ node.mode.width }} × {{ node.mode.height }} @ {{ node.mode.refresh_hz }} Hz</span><span v-if="node.primary">Primary</span><span v-else-if="node.kind === 'client' && !node.active">Inactive preview</span>
      </button>
    </div>
    <label v-if="selected" class="topology-client-picker">Arrangement<select v-model="selectedArrangement"><option value="automatic">Use application layout / automatic</option><option value="preserve">Preserve / manual (Windows layout)</option><option value="left">Left of</option><option value="right">Right of</option><option value="above">Above</option><option value="below">Below</option></select></label>
    <p v-if="selected && (selectedArrangement === 'preserve' || selectedArrangement === 'automatic')">Application streams keep their Windows positions unless their layout requests isolation. Explicit Preserve / manual also overrides isolation. Remote Monitor uses automatic placement when no direction is saved.</p>
    <div v-if="selectedPlacement && !selectedPlacement.preserve" class="topology-controls">
      <label>Display<select v-model="selected"><option v-for="client in focusClients" :key="client.uuid" :value="client.uuid">{{ client.name || client.uuid }}</option></select></label>
      <label>Anchor<select v-model="selectedPlacement.anchor_id" @change="selectedPlacement.anchor_kind = (anchors.find((anchor) => anchor.id === selectedPlacement?.anchor_id)?.kind ?? 'physical')"><option v-for="anchor in anchors" :key="anchor.id" :value="anchor.id">{{ anchor.kind === 'physical' ? 'Physical: ' : 'Client: ' }}{{ anchor.label || anchor.id }}</option></select></label>
      <label>Align<select v-model="selectedPlacement.alignment"><option value="start">Start</option><option value="center">Center</option><option value="end">End</option></select></label>
      <label>Gap (px)<input v-model.number="selectedPlacement.gap_px" min="0" max="100000" step="1" type="number" /></label>
      <label class="topology-primary"><input v-model="selectedPlacement.primary" type="checkbox" /> Use as primary while active</label>
    </div>
    <div class="topology-status" v-for="client in focusClients" :key="`status-${client.uuid}`"><strong>{{ client.name || client.uuid }}</strong><StatusBadge :label="data?.runtime?.[client.uuid]?.lifecycle ?? 'inactive'" :tone="data?.runtime?.[client.uuid]?.retryable ? 'warning' : 'neutral'" compact /><span v-if="data?.runtime?.[client.uuid]?.warning">{{ data?.runtime?.[client.uuid]?.warning }}</span><span v-if="data?.runtime?.[client.uuid]?.plaintext_rtsp_warning">{{ data?.runtime?.[client.uuid]?.plaintext_rtsp_warning }}</span></div>
    <div class="topology-actions"><AppButton variant="tertiary" label="Revert" :disabled="saving" @click="revert" /><AppButton variant="primary" label="Save layout" :disabled="Boolean(validationError)" :busy="saving" @click="save" /></div>
  </section>
</template>

<style scoped>
.topology-editor { display:grid; gap:1rem; padding:1rem; }.topology-editor__heading,.topology-status,.topology-actions { display:flex; align-items:center; justify-content:space-between; gap:.75rem; flex-wrap:wrap; }.topology-editor h3,.topology-editor p { margin:0; }.topology-editor p { color:var(--vs-color-text-muted); margin-top:.25rem; }.topology-client-picker { display:grid; gap:.3rem; max-width:28rem; font-size:.875rem; }.topology-client-picker select { padding:.45rem; color:var(--vs-color-text-primary); background:var(--vs-color-bg-surface); border:1px solid var(--vs-color-border-strong); border-radius:var(--vs-radius-control); }.topology-canvas { position:relative; min-height:230px; overflow:auto; border:1px solid var(--vs-color-border-subtle); border-radius:var(--vs-radius-card); background:linear-gradient(90deg, color-mix(in srgb,var(--vs-color-bg-surface) 94%,var(--vs-color-text-primary) 6%) 1px,transparent 1px),linear-gradient(color-mix(in srgb,var(--vs-color-bg-surface) 94%,var(--vs-color-text-primary) 6%) 1px,transparent 1px); background-size:20px 20px; }.topology-node { position:absolute; display:grid; align-content:center; gap:.25rem; padding:.5rem; text-align:left; color:var(--vs-color-text-primary); background:var(--vs-color-bg-raised); border:2px solid var(--vs-color-border-strong); border-radius:var(--vs-radius-subtle); cursor:pointer; }.topology-node--client { border-color:var(--vs-color-accent-default); touch-action:none; user-select:none; }.topology-node.is-inactive { opacity:.72; border-style:dashed; }.topology-node.is-selected { opacity:1; outline:3px solid color-mix(in srgb,var(--vs-color-accent-default) 42%,transparent); }.topology-node span { font-size:.75rem; color:var(--vs-color-text-muted); }.topology-controls { display:grid; grid-template-columns:repeat(auto-fit,minmax(140px,1fr)); gap:.75rem; }.topology-controls label { display:grid; gap:.3rem; font-size:.875rem; }.topology-controls select,.topology-controls input { min-width:0; padding:.45rem; color:var(--vs-color-text-primary); background:var(--vs-color-bg-surface); border:1px solid var(--vs-color-border-strong); border-radius:var(--vs-radius-control); }.topology-primary { align-content:end; grid-template-columns:auto 1fr; }.topology-status { padding:.5rem .75rem; border-left:3px solid var(--vs-color-border-strong); background:var(--vs-color-bg-surface); }.topology-status span { color:var(--vs-color-text-muted); font-size:.875rem; }.topology-actions { justify-content:flex-end; } @media (max-width:640px) { .topology-canvas { min-height:180px; }.topology-status { align-items:flex-start; flex-direction:column; } }
</style>
