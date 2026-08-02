# Vibepollo requirements traceability

This matrix turns the UX specification into implementation ownership and observable acceptance conditions. Priorities are **Critical**, **High**, and **Later**.

## Cross-cutting foundations

| ID | Priority | Requirement | Owned artifact or pattern | Acceptance condition |
|---|---|---|---|---|
| FND-01 | Critical | One canonical primitive → semantic → component token graph | `design/tokens.json`, token generator | Product styles consume `--vs-*` semantic/component variables; raw primitive values do not appear in page styles. |
| FND-02 | Critical | Dark-first UI with a complete light theme | Theme color tokens, `[data-theme]`, generator contrast gate | Dark and light preserve the same hierarchy and semantic state meanings; ordinary text and controls meet WCAG AA contrast, while essential boundaries and focus indicators meet non-text contrast requirements. |
| FND-03 | Critical | Medium default density with local compact support | `[data-density="compact"]`, row/control tokens | Default controls are 40 px and normal rows 52 px; compact data controls/rows become 32/44 px without shrinking touch targets where touch use is expected. |
| FND-04 | Critical | Shared responsive layout | Page padding, content widths, grid, 480/768/1024/1440 breakpoints | Content reflows without loss at desktop, tablet, mobile, 200% text, and 400% zoom. Only intrinsically wide logs/data grids scroll horizontally. |
| FND-05 | Critical | Accessible interaction primitives | Focus, target, color, type, and control tokens | Every action has native semantics, accessible name, visible 2 px focus ring with 2 px offset, keyboard operation, and non-color state cue. |
| FND-06 | Critical | Shared state catalogue | Status badge/indicator, alert, live-region policy | Connected, disconnected, streaming, pending, blocked, warning, error, saving, saved, stale, loading, empty, partial, and unavailable each define text, icon, color, announcement, and action. |
| FND-07 | Critical | Shared asynchronous-state treatment | Skeleton, spinner/progress, inline alert, retry action | Loading geometry resembles final content; errors remain inline and actionable; empty states explain what happened and offer a relevant next step. |
| FND-08 | High | Restrained depth and shape | Border, radius, shadow, raised-surface tokens | Ordinary groups use borders/surface shifts; shadows are limited to raised transient elements and overlays; pills are limited to tags, status, and segmented controls. |
| FND-09 | Critical | Purposeful motion with a reduced-motion replacement | Motion tokens and media query | Controls/overlays use 80/160/240 ms durations; 320 ms is exceptional; reduced motion removes spatial transitions and traveling skeletons. |
| FND-10 | High | Stable live data | Status row, metric cell, virtualized collection/viewer | Metrics reserve width, live updates do not reorder rows, and selection/filter/scroll context survives refreshes. |

## Page and shell matrix

| ID | Priority | Surface | Requirement | Reused foundations and patterns | Acceptance condition |
|---|---|---|---|---|---|
| SHL-01 | Critical | Shell | Persistent navigation for Overview, Library, Devices, Sessions, Integrations, Logs, and Settings; Settings/Help remain low in the rail | Navigation, page header, semantic status | ≥1024 px supports 264 px expanded and 64 px collapsed modes with labels in the normal state. Tablet uses rail or drawer; mobile uses a modal drawer. |
| SHL-02 | Critical | Shell | Contextual title, supporting context, and page actions | Page header, overflow menu | Desktop holds one row; tablet moves secondary actions to overflow; mobile stacks while retaining one visible primary action. |
| SHL-03 | High | Shell | Global health is discoverable without constant noise | Status control, alert/live region | Active or degraded health is persistent and actionable; healthy state may be visually quiet but the Overview/status destination remains reachable. |
| SHL-04 | Critical | Shell | Predictable keyboard/focus behavior | Native navigation, focus tokens, dialog/drawer focus management | Tab order follows visual order; Escape closes only the top transient layer; focus returns to its invoker; icon-only collapsed navigation retains accessible names and tooltips. |
| DSH-01 | Critical | Dashboard | Readiness and failures dominate | Status summary, alert, quick action | No more than two or three summary regions appear above the fold; each warning identifies consequence and next action; decorative statistics are absent. |
| DSH-02 | High | Dashboard | Active sessions, paired-device summary, and recent games provide direct routes to detail | Session row, device summary, game card | Live values update in place without row movement; selecting an item reaches its canonical detail/management surface. |
| DSH-03 | High | Dashboard | Urgency determines responsive order | Responsive grid | Layout is 2–3 columns desktop, 2 tablet, 1 mobile; warnings/readiness precede recency content in the single-column order. |
| LIB-01 | Critical | Library | Cover grid is default with persistent list alternative | Filter bar, segmented view control, game card/list row | View changes retain filters, selection, result count, and scroll context. List view exposes metadata useful for comparison and bulk work. |
| LIB-02 | Critical | Library | Search, filter, sort, collections/saved filters, and Add Game remain available | Search, filter sheet, page header | Query state persists in the URL where practical. Desktop keeps filters visible; tablet collapses them; mobile opens a full-height filter sheet. |
| LIB-03 | Critical | Library | Large libraries remain responsive | Virtualized collection, artwork fallback, skeleton | Grid supports 176/148/128 px minimum cards and 16/12 px gaps; missing/loading/error artwork does not shift card geometry; only visible ranges render at large scale. |
| LIB-04 | Critical | Library | Keyboard selection matches pointer capability | Selectable grid, context menu | Arrows move card focus; Enter opens details; Space toggles selection in selection mode; Shift+Arrow extends; Ctrl/Cmd+A selects filtered results; Shift+F10 exposes the same actions. |
| LIB-05 | High | Library workflows | Add Game and Game Details remain structured, safe workflows | Settings groups, save bar, danger zone | Add Game covers source, identity, executable, artwork, overrides, and advanced options. Details defaults to read mode; Test Launch is separate from Save; destructive actions are isolated. |
| SET-01 | Critical | Settings | Searchable local categories within a 960 px reading width | Settings navigation, search, settings group | Categories remain addressable and searchable; settings are grouped in flat bordered sections rather than one card per row. |
| SET-02 | Critical | Settings | Descriptive rows adapt without hiding labels | Settings row, field/control primitives | Rows are at least 64 px; label/description sits left and control right at ≥720 px, then stacks with full-width controls below it. Labels are persistent and programmatic. |
| SET-03 | Critical | Settings | Save semantics are explicit | Inline saved state, validation summary, sticky save bar | Independent low-risk toggles may autosave; related technical fields use explicit Save. A page never mixes models without a visible boundary/explanation. |
| SET-04 | Critical | Settings | Dependencies, unavailable controls, restart requirements, and danger operations are explained | Dependent expander, inline alert, restart banner, danger zone | Disabled labels remain readable with adjacent rationale; validation describes correction; restart state persists; reset/recovery actions are isolated and confirmed. |
| DEV-01 | Critical | Devices | Discovered/pending, paired, and active devices are distinct | Device list/table, section heading, status badge | Rows use explicit Streaming, Connected, Offline, Awaiting approval, Blocked, or Error text plus icon/color and show an appropriate approve/repair/manage action. |
| DEV-02 | Critical | Devices | Name/state and primary action remain visible as space decreases | Responsive row, overflow, details drawer | Desktop/tablet reduce secondary columns before essential content; mobile stacks rows and retains real buttons—no swipe-only operation. Preview/diagnostics drawer becomes full-screen below 768 px. |
| DEV-03 | High | Devices | Search/filter and live changes preserve context | Filter bar, stable live list | Device values update without reordering; focused/selected device and expanded details remain stable unless the item is removed. |
| SES-01 | Critical | Sessions | Active session essentials and Stop are immediately available | Session row/table, confirmation | Every session shows game, client, duration, resolution, frame rate, quality, state, and visible Stop. Stop has an explicit consequence and critical completion announcement when appropriate. |
| SES-02 | High | Sessions | Diagnostics are secondary but reachable | Expandable detail, diagnostic drawer | Summary remains human-readable; technical metrics/payloads are collapsed initially; live numbers reserve width and do not shift or reorder the row. |
| LOG-01 | High | Logs | Full-width, virtualized technical viewer | Log toolbar/viewer, monospace type | Lines have stable numbers; severity/source filters, search, pause/autoscroll, copy, and export are keyboard operable; high volume does not render the entire dataset. |
| LOG-02 | High | Logs | Human explanation precedes raw data | Summary panel, expandable raw payload | Errors present impact and likely next action before raw detail. Receiving lines do not trigger animation or assistive announcements. |
| LOG-03 | High | Logs | Narrow layouts contain horizontal overflow | Full-screen mobile viewer | Raw lines may scroll horizontally inside the viewer while the page itself remains single-axis; controls wrap or move to overflow without becoming icon-only mysteries. |
| MNT-01 | High | Maintenance | Update status and ordinary service actions are distinct from recovery | Version/update panel, progress, restart banner | Current/available version, release notes, progress, retry, and restart remain together; continuing operations survive toast dismissal and expose persistent status. |
| MNT-02 | Critical | Maintenance | Backup/restore/reset communicate risk | Danger zone, confirmation dialog, progress/error state | Recovery is separated from normal updates; confirmations name the affected data/service and irreversible consequence; failure preserves a retry or support/log route. |

## Required blueprint states

| Surface | Desktop | Tablet | Mobile | Loading | Empty | Error/degraded |
|---|---|---|---|---|---|---|
| Shell | Expanded/collapsible sidebar | Rail or temporary drawer | Modal drawer | Preserve shell geometry | Not applicable | Persistent global route to status/recovery |
| Dashboard | 2–3 columns | 2 columns | 1 urgency-ordered column | Geometric summaries | Explain that no sessions/warnings exist without celebratory clutter | Inline cause and next action |
| Library | 4–8 grid columns or list | 3–5 columns | 2–3 columns | Artwork-shaped skeletons | Explain no games or no filtered results; Add/Clear action | Stable collection with retry; do not discard filters |
| Settings | Split rows | Split where viable | Stacked controls | Disable affected group with readable status | Category may explain unavailable capability | Field correction plus group/page summary as appropriate |
| Devices | Structured table/rows | Reduced columns | Stacked rows | Row skeletons | Explain discovery/pairing and offer refresh/help | State label, cause, repair/retry action |
| Sessions | Full metrics table/rows | Reduced columns + expansion | Stacked essentials | Stable placeholders | Clearly state no active streams | Degraded/ended state remains long enough to understand and act |
| Logs | Viewer + optional detail pane | Viewer + drawer | Full-screen viewer | Low-motion progress | Explain filters/time range and offer clear filters | Preserve available lines and expose retry/export |
| Maintenance | Constrained status sections | Single column | Single column + safe-area action bar | Persistent progress | No-update state reports current version | Failure remains inline with retry and logs/support route |

## Definition of done for each implemented row

1. The requirement uses public semantic/component tokens and a documented shared pattern.
2. Dark/light, default density, keyboard, screen-reader naming, focus, forced-colors, reduced-motion, and high-zoom behavior are accounted for.
3. Responsive behavior is demonstrated with realistic long strings, not only placeholder copy.
4. Loading, empty, partial, unavailable, and failure states preserve context and provide the next useful action.
5. Live updates do not cause unexpected focus movement, row reordering, layout shift, or excessive announcements.
