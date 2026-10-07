import type { SessionEvent } from '../../types/sessions';

export interface ChartTimelinePoint {
  timestamp: number;
  segment?: string;
}

export interface ChartEvent {
  id: string;
  timestamp: number;
  eventType: string;
  payload: string;
  sessionId?: string;
}

/** Session history stores Unix seconds; chart coordinates are Unix milliseconds. */
export function eventTimestampMs(timestamp: unknown): number | null {
  const value = typeof timestamp === 'number' ? timestamp : Number(timestamp);
  if (!Number.isFinite(value)) return null;
  // Unix seconds are currently ~1e9 and milliseconds are ~1e12. Keep this
  // threshold generous so old and future retained records remain readable.
  return Math.abs(value) < 100_000_000_000 ? value * 1000 : value;
}

function boundarySlackMs(points: ChartTimelinePoint[]): number {
  if (points.length < 2) return 5_000;
  const gaps = points
    .slice(1)
    .map((point, index) => point.timestamp - (points[index]?.timestamp ?? point.timestamp))
    .filter((gap) => gap > 0 && gap <= 60_000)
    .sort((left, right) => left - right);
  if (!gaps.length) return 5_000;
  const median = gaps[Math.floor(gaps.length / 2)] ?? 2_000;
  return Math.max(5_000, Math.min(60_000, median * 2));
}

function eventKey(event: SessionEvent, index: number): string {
  return `${event.session_uuid || 'event'}:${event.timestamp_unix}:${event.event_type}:${event.payload || ''}:${index}`;
}

/**
 * Keep events in the same timestamp domain as the plotted samples. For a
 * grouped history, an event must belong to a plotted source segment; this
 * prevents a reconnect's marker from appearing on its neighbour's line.
 */
export function eventsForChart(events: SessionEvent[], points: ChartTimelinePoint[]): ChartEvent[] {
  const validPoints = points.filter((point) => Number.isFinite(point.timestamp));
  if (!validPoints.length || !events.length) return [];
  const allMin = Math.min(...validPoints.map((point) => point.timestamp));
  const allMax = Math.max(...validPoints.map((point) => point.timestamp));
  const globalSlack = boundarySlackMs(validPoints);
  const hasSegments = validPoints.some((point) => point.segment != null && point.segment !== '');
  return events.flatMap((event, index) => {
    const timestamp = eventTimestampMs(event.timestamp_unix);
    if (timestamp == null) return [];
    const segmentPoints =
      hasSegments && event.session_uuid
        ? validPoints.filter((point) => point.segment === event.session_uuid)
        : validPoints;
    if (hasSegments && event.session_uuid && !segmentPoints.length) return [];
    const minimum = segmentPoints.length
      ? Math.min(...segmentPoints.map((point) => point.timestamp))
      : allMin;
    const maximum = segmentPoints.length
      ? Math.max(...segmentPoints.map((point) => point.timestamp))
      : allMax;
    const slack = segmentPoints.length ? boundarySlackMs(segmentPoints) : globalSlack;
    if (timestamp < minimum - slack || timestamp > maximum + slack) return [];
    return [
      {
        id: eventKey(event, index),
        timestamp,
        eventType: event.event_type || 'event',
        payload: event.payload || '',
        sessionId: event.session_uuid || undefined,
      },
    ];
  });
}
