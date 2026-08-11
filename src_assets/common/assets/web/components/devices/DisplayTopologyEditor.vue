<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';

import { apiGet, apiRequest, type ApiPayload } from '@/api/client';
import { AppButton, InlineAlert, StatusBadge } from '@/components/ui';

type Edge = 'left' | 'right' | 'above' | 'below';
type Alignment = 'start' | 'center' | 'end';
type AnchorKind = 'physical' | 'client';
interface Placement { anchor_kind: AnchorKind; anchor_id: string; edge: Edge; alignment: Alignment; gap_px: number; primary: boolean }
interface Node { id: string; label: string; kind: AnchorKind; active: boolean; primary: boolean; desired_position: { x: number; y: number }; mode: { width: number; height: number; refresh_hz: number } }
interface Response { layout: { version: number; placements: Record<string, Placement> }; nodes: Node[]; clients: Array<{ uuid: string; name: string }>; runtime?: Record<string, { lifecycle: string; ready: boolean; retryable: boolean; warning?: string; plaintext_rtsp_warning?: string }>; capacity?: { max: number; used: number }; warnings?: string[] }

const props = withDefaults(defineProps<{ clientUuid?: string; compact?: boolean }>(), { clientUuid: '', compact: false });
const data = ref<Response | null>(null);
const draft = ref<Record<string, Placement>>({});
const selected = ref('');
const error = ref('');
const saving = ref(false);

const clientNodes = computed(() => (data.value?.nodes ?? []).filter((node) => node.kind === 'client'));
const knownClients = computed(() => data.value?.clients ?? []);
const anchors = computed(() => (data.value?.nodes ?? []).filter((node) => node.id !== selected.value));
const focusClients = computed(() => props.clientUuid ? knownClients.value.filter((client) => client.uuid === props.clientUuid) : knownClients.value);
const selectedPlacement = computed<Placement | null>(() => selected.value ? draft.value[selected.value] ?? null : null);
const headingId = computed(() => `display-topology-title-${(props.clientUuid || 'all').replace(/[^a-zA-Z0-9_-]/g, '-')}`);

function defaultPlacement(client: string): Placement {
  const physical = (data.value?.nodes ?? []).find((node) => node.kind === 'physical');
  const fallback = physical ?? clientNodes.value.find((node) => node.id !== client);
  return { anchor_kind: fallback?.kind ?? 'physical', anchor_id: fallback?.id ?? '', edge: 'right', alignment: 'center', gap_px: 0, primary: false };
}
function choose(client: string) { selected.value = client; if (!draft.value[client]) draft.value[client] = defaultPlacement(client); }
function validation(): string {
  const all = Object.entries(draft.value);
  if (all.filter(([, placement]) => placement.primary).length > 1) return 'Only one client display may be primary.';
  for (const [client, placement] of all) {
    if (!placement.anchor_id || placement.anchor_id === client) return 'Choose a different known anchor for every placement.';
    if (!Number.isInteger(placement.gap_px) || placement.gap_px < 0 || placement.gap_px > 100000) return 'Gap must be a nonnegative whole number.';
    const seen = new Set<string>(); let current = client;
    while (draft.value[current]?.anchor_kind === 'client') { if (seen.has(current)) return 'Client anchors cannot form a cycle.'; seen.add(current); current = draft.value[current].anchor_id; }
  }
  return '';
}
const validationError = computed(validation);
function rebuildDraft(response: Response) { data.value = response; draft.value = structuredClone(response.layout?.placements ?? {}); if (props.clientUuid) choose(props.clientUuid); }
async function load() { error.value = ''; try { rebuildDraft(await apiGet<Response>('/api/clients/display-layout')); } catch { error.value = 'Unable to load the display layout.'; } }
async function save() { if (validationError.value) return; saving.value = true; error.value = ''; try { const response = await apiRequest<Response>('/api/clients/display-layout', { method: 'PUT', json: { version: 1, placements: draft.value } as ApiPayload }); rebuildDraft(response); } catch { error.value = 'The layout was not saved. No active topology was changed.'; } finally { saving.value = false; } }
function revert() { if (data.value) draft.value = structuredClone(data.value.layout.placements ?? {}); }
function scaled(node: Node) { const width = Math.max(80, Math.round(node.mode.width / 18)); const height = Math.max(50, Math.round(node.mode.height / 18)); return { width: `${width}px`, height: `${height}px`, transform: `translate(${Math.round(node.desired_position.x / 18)}px, ${Math.round(node.desired_position.y / 18)}px)` }; }
function keyboardPlace(edge: Edge) { if (!selectedPlacement.value) return; selectedPlacement.value.edge = edge; }
function dragPlace(event: PointerEvent, client: Node) {
  const canvas = (event.currentTarget as HTMLElement).closest('.topology-canvas');
  if (!canvas || client.kind !== 'client') return;
  choose(client.id);
  const rect = canvas.getBoundingClientRect();
  const point = { x: event.clientX - rect.left + canvas.scrollLeft, y: event.clientY - rect.top + canvas.scrollTop };
  const candidates = (data.value?.nodes ?? []).filter((node) => node.id !== client.id);
  const anchor = candidates.sort((left, right) => Math.hypot(point.x - left.desired_position.x / 18, point.y - left.desired_position.y / 18) - Math.hypot(point.x - right.desired_position.x / 18, point.y - right.desired_position.y / 18))[0];
  const placement = selectedPlacement.value;
  if (!anchor || !placement) return;
  const dx = point.x - anchor.desired_position.x / 18; const dy = point.y - anchor.desired_position.y / 18;
  placement.anchor_id = anchor.id; placement.anchor_kind = anchor.kind;
  placement.edge = Math.abs(dx) >= Math.abs(dy) ? (dx < 0 ? 'left' : 'right') : (dy < 0 ? 'above' : 'below');
  placement.alignment = 'center'; placement.gap_px = 0;
}
onMounted(load);
</script>

<template>
  <section class="topology-editor vs-surface" :class="{ 'topology-editor--compact': compact }" :aria-labelledby="headingId">
    <div class="topology-editor__heading">
      <div><h3 :id="headingId">Remote Monitor layout</h3><p>Changes apply on the next activation. They never rearrange an active topology.</p></div>
      <StatusBadge v-if="data?.capacity" :label="`${data.capacity.used} of ${data.capacity.max} client identities`" tone="neutral" compact />
    </div>
    <InlineAlert v-if="error || validationError" tone="danger">{{ error || validationError }}</InlineAlert>
    <InlineAlert v-for="warning in data?.warnings ?? []" :key="warning" tone="warning">{{ warning }}</InlineAlert>
    <div class="topology-canvas" role="group" aria-label="Display topology. Select a display rectangle to edit its placement.">
      <button v-for="node in data?.nodes ?? []" :key="node.id" type="button" class="topology-node" :class="[`topology-node--${node.kind}`, { 'is-selected': selected === node.id }]" :style="scaled(node)" :aria-pressed="selected === node.id" @click="node.kind === 'client' && choose(node.id)" @pointerup="dragPlace($event, node)" @keydown.left.prevent="keyboardPlace('left')" @keydown.right.prevent="keyboardPlace('right')" @keydown.up.prevent="keyboardPlace('above')" @keydown.down.prevent="keyboardPlace('below')">
        <strong>{{ node.kind === 'physical' ? 'Physical' : 'Client' }} · {{ node.label || node.id }}</strong><span>{{ node.mode.width }} × {{ node.mode.height }} @ {{ node.mode.refresh_hz }} Hz</span><span v-if="node.primary">Primary</span>
      </button>
    </div>
    <div v-if="selectedPlacement" class="topology-controls">
      <label>Display<select v-model="selected"><option v-for="client in focusClients" :key="client.uuid" :value="client.uuid">{{ client.name || client.uuid }}</option></select></label>
      <label>Anchor<select v-model="selectedPlacement.anchor_id" @change="selectedPlacement.anchor_kind = (anchors.find((anchor) => anchor.id === selectedPlacement?.anchor_id)?.kind ?? 'physical')"><option v-for="anchor in anchors" :key="anchor.id" :value="anchor.id">{{ anchor.kind === 'physical' ? 'Physical: ' : 'Client: ' }}{{ anchor.label || anchor.id }}</option></select></label>
      <label>Place<select v-model="selectedPlacement.edge"><option value="left">Left of</option><option value="right">Right of</option><option value="above">Above</option><option value="below">Below</option></select></label>
      <label>Align<select v-model="selectedPlacement.alignment"><option value="start">Start</option><option value="center">Center</option><option value="end">End</option></select></label>
      <label>Gap (px)<input v-model.number="selectedPlacement.gap_px" min="0" max="100000" step="1" type="number" /></label>
      <label class="topology-primary"><input v-model="selectedPlacement.primary" type="checkbox" /> Use as primary while active</label>
    </div>
    <div class="topology-status" v-for="client in focusClients" :key="`status-${client.uuid}`"><strong>{{ client.name || client.uuid }}</strong><StatusBadge :label="data?.runtime?.[client.uuid]?.lifecycle ?? 'inactive'" :tone="data?.runtime?.[client.uuid]?.retryable ? 'warning' : 'neutral'" compact /><span v-if="data?.runtime?.[client.uuid]?.warning">{{ data?.runtime?.[client.uuid]?.warning }}</span><span v-if="data?.runtime?.[client.uuid]?.plaintext_rtsp_warning">{{ data?.runtime?.[client.uuid]?.plaintext_rtsp_warning }}</span></div>
    <div class="topology-actions"><AppButton variant="tertiary" label="Revert" :disabled="saving" @click="revert" /><AppButton variant="primary" label="Save layout" :disabled="Boolean(validationError)" :busy="saving" @click="save" /></div>
  </section>
</template>

<style scoped>
.topology-editor { display:grid; gap:1rem; padding:1rem; }.topology-editor__heading,.topology-status,.topology-actions { display:flex; align-items:center; justify-content:space-between; gap:.75rem; flex-wrap:wrap; }.topology-editor h3,.topology-editor p { margin:0; }.topology-editor p { color:var(--vs-color-text-muted); margin-top:.25rem; }.topology-canvas { position:relative; min-height:230px; overflow:auto; border:1px solid var(--vs-color-border-subtle); border-radius:var(--vs-radius-card); background:linear-gradient(90deg, color-mix(in srgb,var(--vs-color-bg-surface) 94%,var(--vs-color-text-primary) 6%) 1px,transparent 1px),linear-gradient(color-mix(in srgb,var(--vs-color-bg-surface) 94%,var(--vs-color-text-primary) 6%) 1px,transparent 1px); background-size:20px 20px; }.topology-node { position:absolute; display:grid; align-content:center; gap:.25rem; padding:.5rem; text-align:left; color:var(--vs-color-text-primary); background:var(--vs-color-bg-raised); border:2px solid var(--vs-color-border-strong); border-radius:var(--vs-radius-subtle); cursor:pointer; }.topology-node--client { border-color:var(--vs-color-accent-default); }.topology-node.is-selected { outline:3px solid color-mix(in srgb,var(--vs-color-accent-default) 42%,transparent); }.topology-node span { font-size:.75rem; color:var(--vs-color-text-muted); }.topology-controls { display:grid; grid-template-columns:repeat(auto-fit,minmax(140px,1fr)); gap:.75rem; }.topology-controls label { display:grid; gap:.3rem; font-size:.875rem; }.topology-controls select,.topology-controls input { min-width:0; padding:.45rem; color:var(--vs-color-text-primary); background:var(--vs-color-bg-surface); border:1px solid var(--vs-color-border-strong); border-radius:var(--vs-radius-control); }.topology-primary { align-content:end; grid-template-columns:auto 1fr; }.topology-status { padding:.5rem .75rem; border-left:3px solid var(--vs-color-border-strong); background:var(--vs-color-bg-surface); }.topology-status span { color:var(--vs-color-text-muted); font-size:.875rem; }.topology-actions { justify-content:flex-end; } @media (max-width:640px) { .topology-canvas { min-height:180px; }.topology-status { align-items:flex-start; flex-direction:column; } }
</style>
