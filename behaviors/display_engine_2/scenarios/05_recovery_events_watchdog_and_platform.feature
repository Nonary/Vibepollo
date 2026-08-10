@display-engine-v2
Feature: Recovery, events, watchdogs, and Windows platform safety
  The display engine protects the user's desktop while a streamed display
  configuration may be active, then restores it safely when a session or the
  operating system becomes unhealthy.
  Durations in this feature are current suggested defaults and may be tuned, but
  the stated ordering, attempt ceilings, attribution, and terminal outcomes are fixed.

  Rule: A desktop that may have changed remains recoverable

    Scenario: An explicit revert protects the desktop and restores it
      Given a session baseline or golden baseline is available
      And a stream configuration may have changed the desktop
      When the client explicitly requests a revert
      Then recovery protection remains armed before restoration begins
      And the helper gives a replacement session the configured non-immediate revert grace opportunity to take control
      And restoration proceeds immediately for a restore-startup request
      And the protection is removed only after the restored desktop is confirmed

    Scenario: Restore startup does not leave an orphaned recovery attempt
      Given the helper starts in restore mode after a reboot or later logon
      When a usable saved baseline is available
      Then it restores and validates that baseline without waiting for a control connection
      And a confirmed restore reaches a safe terminal state

    Scenario: Restore startup without a baseline finishes cleanly
      Given the helper starts in restore mode after a reboot or later logon
      And no current, previous, or golden baseline exists
      When the helper evaluates available restore candidates
      Then it clears the in-process no-op recovery state and requests durable-safeguard cleanup
      And it exits instead of waiting indefinitely for a candidate that does not exist
      But a cleanup failure may leave the replaceable safeguard present and does not prevent this no-baseline launch from exiting

    Scenario: Ordinary recovery has a bounded grace and primary opportunity
      Given a protected desktop needs recovery after an ordinary client revert or lost control connection
      When recovery is not an immediate restore-startup request
      Then it gives a replacement session the default 5-second grace opportunity before beginning restoration
      And the default 2-minute primary recovery window begins when recovery is armed rather than after that grace
      And ordinary recovery therefore has roughly 115 seconds of that original window remaining after the default grace
      And an immediate restore-startup request begins without that grace
      And tuning either duration preserves the replacement opportunity, a finite primary retry window, cancellation responsiveness, and the requirement for confirmed restoration

    Scenario: A replacement during revert grace prevents an obsolete restore mutation
      Given a non-immediate revert is waiting for a replacement session opportunity
      When a newer Apply or disarm supersedes the revert before restoration begins
      Then the old recovery does not change the desktop
      And the newer session intent becomes responsible for the next safe outcome

    Scenario: A canceled recovery keeps protection for an uncertain desktop
      Given recovery has passed the point at which topology, modes, HDR, primary display, or layout may change
      And a durable safeguard was successfully created or already exists
      When recovery or its validation is canceled or superseded
      Then no obsolete completion may declare the desktop restored
      And recovery protection remains armed because the exact mutation boundary is unknown
      And the durable safeguard remains until a confirmed recovery, verified replacement session, or explicit safe disarm owns the result
      And an ordered Reset of staged persistence does not by itself own or prove a safe desktop

    Scenario: A canceled apply retains protection when it may have touched the desktop
      Given an apply is superseded after it may have changed the desktop
      When its late completion is observed
      Then the old apply cannot control the replacement session
      And the desktop remains protected for recovery
      And a newly verified replacement may take over that protection without an unsafe gap

    Scenario: A disarm does not discard an unconfirmed restore boundary
      Given a restore attempt has changed or may have changed the desktop but is not yet confirmed
      When a later disarm is requested
      Then the helper does not falsely claim that recovery is complete
      And recovery protection remains until a safe terminal confirmation is available

  Rule: Recovery applies a candidate in a safe, stable sequence

    Scenario: Restore waits for a complete usable desktop before later restore stages
      Given a recovery candidate contains topology, display settings, and optional layout data
      When recovery activates the candidate topology
      Then it waits until the candidate's required display paths are usable before applying later settings
      And it restores display modes and HDR before primary-display and origin settings
      And it applies recorded rotation or layout only after the preceding restore stages succeed
      And failure at any stage does not report the desktop as restored

    Scenario: A partially applicable saved layout still fails the candidate as a whole
      Given a recovery candidate contains saved layout or rotation changes for multiple displays
      And one saved identity or rotation cannot be prepared while another can be changed
      When recovery applies the optional layout stage
      Then it may apply the compatible saved change while reporting the layout stage unsuccessful as a whole
      And the partially changed desktop remains protected and unconfirmed for retry or fallback

    Scenario: A restore candidate is confirmed only after stable and quiet observations
      Given a recovery candidate appears to match the current desktop at one observation
      When recovery decides whether that candidate is already restored or has just been restored
      Then it requires two equal desktop observations whose topology and modes are not both empty, separated by the default 150 ms sampling interval
      And it acquires that pair within a default window of up to 2 seconds
      And it requires any recorded layout or rotation to match only during recovery confirmation
      And it then requires a default 750 ms quiet period whose repeated snapshots match a newly captured stable quiet-period baseline
      And it does not compare that fresh quiet baseline back to the candidate or recheck layout during the quiet period
      And drift between the candidate check and quiet baseline may pass if the newly observed desktop remains stable
      And those defaults may change only when they preserve the repeated-observation gate, the same comparison boundaries, cancellation responsiveness, and the same terminal recovery result

    Scenario: Recovery uses at most two safe restore applications for one candidate
      Given a recovery candidate has not already been stably confirmed
      When recovery attempts that candidate
      Then it may apply the candidate once and validate it
      And before a second and final safe restore application it waits the default 700 ms double-check interval and checks whether the desktop already matches
      And it does not apply that candidate more than twice in the same recovery attempt
      And cancellation during either application, confirmation, or double-check leaves recovery armed and preserves any successfully created or already existing durable safeguard

    Scenario: Transient OS validation does not discard a structurally valid restore candidate
      Given a saved recovery candidate has valid topology structure
      But an operating-system topology probe is temporarily unavailable while displays are transitioning
      When recovery evaluates that candidate
      Then it retains the candidate for the actual two-application restore path
      And it reports success only if a later topology, settings, and stable-confirmation sequence succeeds

    Scenario: Recovery validation failure returns to protected retry behavior
      Given a restore operation completed with a candidate that appeared successful
      But final recovery validation finds that the desktop no longer matches the candidate
      When the helper handles the validation failure
      Then it keeps recovery protection and preserves any successfully created or already existing durable safeguard
      And it returns to protected retry behavior in the current active recovery window rather than claiming a safe terminal state
      And a later relevant display event may open or extend its default 30-second event window

    Scenario: Final recovery validation waits for the desktop to settle and remains cancellable
      Given recovery has stably confirmed a candidate and must make its final recovery decision
      When final validation starts
      Then it rechecks the current desktop after the default 250 ms settling delay
      And cancellation before that check prevents a restored terminal result
      And tuning that delay may not bypass the final match check, convert cancellation into success, or remove recovery protection before confirmation

    Scenario: Final recovery validation does not recheck saved layout or rotation
      Given candidate-local confirmation matched the saved topology, settings, and any saved layout or rotation
      And the candidate then reached final recovery validation
      When layout or rotation drifts after its candidate-local check
      But the snapshot topology, modes, HDR, primary display, and origins still match after the default 250 ms final settle
      Then final recovery may still be accepted
      And the contract does not guarantee that saved layout or rotation remained correct through final acceptance

  Rule: Lost control connections respect the session's restore policy

    Scenario: A broken connection restores a protected session when restoration is enabled
      Given a verified session has changed the display and restore-on-disconnect is enabled
      When the control connection breaks and its reconnect grace expires
      Then the helper begins recovery in the current primary or event recovery window
      And a replacement control connection cancels the earlier disconnect path before it restores the old session

    Scenario: A deliberately retained stream is not restored merely because its connection breaks
      Given a live session has explicitly disabled restore-on-disconnect
      And no explicit revert is pending
      When its control connection breaks or its heartbeat is lost
      Then the helper does not autonomously restore the paused session's desktop
      And it disarms the disconnected session rather than scheduling further display mutations
      But an explicit revert remains authoritative despite that opt-out

    Scenario: A recent post-apply disconnect settles before deciding to restore
      Given a configuration apply has just started and the connection breaks during its default 5-second startup-churn period
      When the initial apply or verification completes
      Then the helper verifies the requested configuration through its defined disconnected settlement checks
      And it repairs a failed check only while the session remains recoverable
      And a healthy settled configuration keeps its recovery protection without requiring an unreachable client response

    Scenario: Recent post-apply disconnect uses a bounded verify-before-repair ladder
      Given the connection breaks during the default 5-second post-Apply startup-churn period
      And the session remains eligible for disconnect recovery
      When the Apply reaches a point at which its disconnected configuration can be checked
      Then disconnected settlement uses checks after the default 250 ms and 750 ms slots
      And a failed check repairs the current requested configuration before the next applicable slot
      And no ordinary post-Apply check may race or override that settlement decision
      And tuning those slots preserves bounded checks, repair only for the current recoverable session, and cancellation safety

    Scenario: The final disconnected repair is accepted without another observation
      Given the default 250 ms disconnected check failed and its repair did not settle the requested configuration
      And the default 750 ms disconnected check also failed
      When the final repair reapplies the current requested configuration and its Apply result is successful
      Then the session is accepted as steady and remains protected for recovery
      And no additional post-repair observation is required before that acceptance
      But a failed or cancelled final repair is not converted into this accepted result

  Rule: The heartbeat detects a lost helper client without racing a healthy one

    Scenario: Heartbeat monitoring honors the optional startup window
      Given a verified connected session is protected by heartbeat monitoring
      When the initial optional heartbeat window has not elapsed
      Then no recovery is triggered solely because a ping has not yet arrived

    Scenario: Heartbeat monitoring applies missed-ping and recovery grace windows
      Given a verified connected session is protected by heartbeat monitoring
      And its optional startup window has elapsed without a qualifying ping
      When the missed-ping window expires
      Then the helper allows the configured recovery grace window before treating the client as lost
      And a ping during that grace window makes the session healthy again
      And the source default windows are 30 seconds optional startup, 30 seconds without a ping, and 2 minutes of recovery grace
      And tuning those windows may not restore a session before both missed-ping and grace conditions have occurred, or ignore a ping that reestablishes health

    Scenario: A confirmed heartbeat timeout follows the disconnect policy
      Given a protected session has missed its heartbeat beyond the recovery deadline
      When the timeout is confirmed
      Then the stale control connection is retired before recovery is considered
      And the helper starts recovery in the current primary or event recovery window when restoration is enabled or explicitly required
      And it leaves an opted-out paused session un-restored
      And a heartbeat cannot interrupt a recovery or a newer replacement intent that already owns the desktop

  Rule: Windows display events reopen 30-second recovery opportunities

    Scenario Outline: Windows signals a changed display environment
      Given recovery is armed after a failed or unconfirmed restore
      And the ordinary recovery window has expired or is waiting for another opportunity
      When Windows reports <signal>
      Then the event opens a default 30-second event recovery window for the current recovery
      And the next eligible recovery attempt starts without carrying an old backoff delay
      And each supported signal enters the same coalesced changed-display opportunity rather than selecting a different recovery policy
      And each notification delivered inside the one-second quiet period extends that quiet edge so a debounced restore-generated burst cannot wake its own restore
      And a generic display-change or device-nodes notification does not reopen recovery because the preceding restore may have generated it
      And an identity or power event inside an already active recovery window does not erase that window's bounded retry backoff
      And changing that window may not let an event restore an unarmed or stale session, bypass cancellation, or declare an unconfirmed desktop restored

      Examples:
        | signal |
        | a monitor-interface device arrival |
        | a complete monitor-interface device removal |
        | automatic system resume |
        | monitor power changing to on |

    Scenario: Unavailable Windows event sources do not manufacture recovery opportunities
      Given the helper can otherwise continue Apply, verification, and recovery work
      When the Windows display-event listener cannot start
      Then helper operation continues without synthetic display events
      And event-driven recovery and virtual-display repair opportunities are unavailable until helper restart because listener startup is attempted only once per helper lifetime
      And if only device or monitor-power notification registration is unavailable, the corresponding event category is unavailable without disabling unrelated event categories

    Scenario: Recovery retries only within its allowed windows
      Given recovery is armed and a restore attempt cannot be validated
      When repeated attempts fail within the primary or event recovery window
      Then subsequent attempts use the progressive default backoff sequence of 0, 1, 3, 5, 10, 15, 20, then 30 seconds
      And the helper stops retrying when that window is exhausted
      And it preserves recovery protection while waiting for an eligible new event
      And a later display event can open a new 30-second event window
      And tuning the backoff intervals preserves bounded, progressive retries and lets a relevant event start fresh only after the prior window is exhausted without bypassing protection or confirmation

    Scenario: Irrelevant or stale display events do not mutate the current desktop
      Given a newer session or cancellation intent has replaced an earlier one
      When a delayed display event associated with the earlier session arrives
      Then it is ignored
      And a display event outside an armed recovery or virtual-display session does not start an unsolicited restore

    Scenario: An event belongs only to the current recovery or virtual-display session
      Given a device arrival, device removal, automatic-resume, or monitor-on event arrives
      When it is not stale and the current session is eligible for recovery or virtual-display supervision
      Then it may reopen recovery or begin the applicable current-session virtual-display repair
      But a generic display configuration or device-nodes event cannot reopen EventLoop recovery from feedback produced by its preceding restore
      And an irrelevant, unarmed, superseded, or completed session treats the event as a no-op

    Scenario: A Windows display-event burst retains its notification-time session ownership
      Given a current recovery or virtual-display session is eligible to receive Windows display events
      When one or more applicable Windows display events arrive before the default 500 ms event quiet window ends
      Then the burst is handled as one eligible event opportunity after that window
      And that opportunity retains the recovery or session identity that owned the notification when it arrived
      And a device arrival or removal remains identifiable if a generic display or power notification follows it in the same burst
      And changing the quiet window may not relabel an old event for a newer Apply, start a stale restore, or bypass cancellation and target safety

    Scenario: A changed virtual identity during Apply is retargeted after the delivered event opportunity
      Given a current virtual-display Apply or verification observes a changed usable virtual identity
      When the outer 500-millisecond event quiet window delivers the applicable event opportunity
      Then the reapply first retargets the current virtual identity
      And an active mutation reaches its result boundary before a queued replacement Apply begins
      And the queued repair re-observes identity at that boundary and disappears if the id is empty or healthy again
      And an explicit queued Apply, Revert, or Disarm always outranks an autonomous identity repair
      But without an active mutation the replacement Apply begins immediately after the delivered event opportunity
      And cancellation, a stale event, a same-identity event, or a newer session cannot use that restart to mutate another session's desktop

  Rule: Durable recovery survives helper loss but is cleaned up safely

    Scenario: Apply attempts a durable safeguard before its first mutation
      Given an Apply is about to alter topology or display settings
      When Windows may first observe that mutation
      Then the engine attempts to create the durable restore safeguard and records its result before the mutation boundary
      And only reported successful creation is recorded as durable protection for a later restore-mode launch
      And a failed worker-boundary attempt is not retried synchronously by completion handling or a settings-only repair
      But that recorded success does not itself prove the registered definition can launch the helper correctly
      And failed creation is known before mutation but does not itself prevent the mutation or make the desktop safe

    Scenario: Candidate recovery relies on an existing safeguard rather than refreshing one
      Given a recovery candidate is about to restore topology, settings, or layout
      When recovery reaches its first candidate mutation
      Then it relies on the already armed recovery lease and any already existing durable safeguard
      And it does not create or refresh a durable safeguard at that candidate mutation boundary

    Scenario: The durable safeguard has one replaceable restore definition
      Given durable recovery is required for a possibly changed desktop
      When the safeguard is created or refreshed
      Then it uses the single compatibility identity "VibeshineDisplayRestore" whose existing definition is replaced or updated idempotently
      And it attempts author text "Sunshine Display Helper", description text "Automatically restores display settings after reboot", and logon-trigger identity "SunshineDisplayHelperLogonTrigger"
      And it attempts to start the helper path returned by the fixed-capacity executable-path query with the sole restore argument "--restore"
      And it attempts ordinary user privilege, hidden execution, start-when-available behavior, no execution time limit, and continued eligibility across battery transitions
      And it attempts a logon trigger for the identity resolved at safeguard creation time
      But a reported registration success does not guarantee that optional definition sections were obtained or that every requested property was retained because those acquisition and property-write failures are not surfaced
      And executable paths that reach the fixed path-capacity boundary are not rejected as truncated before registration

    Scenario: Durable safeguard creation failures remain visible at their boundary
      Given the helper creates or updates the durable restore safeguard
      When the durable scheduling facility is unavailable, required trigger or launch objects cannot be constructed at a checked boundary, the current helper executable cannot be resolved, or the assembled safeguard cannot be registered
      Then create or update reports failure rather than claiming durable protection
      And a failed create does not prove that the desktop is safe or that no stale safeguard exists
      But failure to obtain unchecked registration information, settings, logon-trigger, or principal sections, or failure while writing their unchecked properties, launch path, or arguments, can still be followed by reported registration success

    Scenario: Durable safeguard deletion is idempotent but reports checked failures
      Given the helper deletes the durable restore safeguard
      When the durable scheduling facility is unavailable or deletion reports an unexpected error
      Then deletion reports failure
      But deleting an already missing safeguard succeeds idempotently

    Scenario: Cancellation before the durable mutation boundary may clear provisional recovery
      Given durable recovery was prepared before a possible desktop mutation
      When cancellation arrives before Windows can observe that mutation
      Then the provisional durable safeguard may be cleared because the desktop was not changed

    Scenario: Cancellation after the durable mutation boundary retains recovery
      Given recovery ownership was armed before a possible desktop mutation
      And Windows may already have observed that mutation
      When cancellation arrives before a safe terminal outcome
      Then recovery remains armed until a confirmed recovery, verified replacement, or explicit safe disarm owns the desktop
      And any successfully created or already existing durable safeguard remains available for that recovery
      And an ordered Reset may clean staged persistence but does not release live recovery ownership
      And a restore task that cannot be armed never permits a claim that the desktop is safe merely because in-process recovery remains available

    Scenario: Failure to arm durable recovery does not masquerade as a safe desktop
      Given a display mutation may have occurred
      When the durable restore safeguard cannot be created
      Then the helper retains in-process recovery protection and its recovery-required state
      And it does not treat the session as safely restored or safely disarmed solely because task creation failed

    Scenario: Durable restore derives its logon identity at safeguard creation time
      Given a durable restore safeguard is required at a later logon
      When no explicit affected-user identity is supplied by the production display lifecycle
      Then identity resolution first uses the active console-session user, then the helper process user, then a resolvable current account identity
      And the first non-service SID found becomes the requested trigger and principal identity
      And the contract does not claim that this derived identity tracks a separately recorded affected user

    Scenario: Durable restore has a safe no-user fallback
      Given a durable restore safeguard is required at a later logon
      When the active-console, process, and account-resolution sequence yields no non-service SID
      Then creation leaves the logon trigger's user identity unscoped, binds the principal group to SID "S-1-5-32-545", and registers with group-logon semantics without localized account-name text
      And it attempts next-ordinary-logon, start-when-available, and battery eligibility
      But unchecked trigger, principal, or settings failures mean reported registration success does not guarantee those fallback behaviors were retained

    Scenario: Durable safeguard cleanup is idempotent only after absence is reached
      Given recovery has reached a safe terminal outcome, the durable scheduling facility is reachable, and no named safeguard exists
      When cleanup is requested again
      Then a missing-safeguard deletion result succeeds as a no-op
      And it does not recreate recovery work or change the confirmed desktop

    Scenario: A confirmed recovery cleans up only the safeguards it no longer needs
      Given recovery has restored a saved baseline and validation succeeds
      When the desktop is confirmed to match the restored baseline
      Then heartbeat monitoring and retry scheduling are disarmed
      And the helper first refreshes the Windows shell so the restored desktop is visible coherently
      And removal of the durable restore safeguard is requested after that shell refresh
      But an unsuccessful removal is not rechecked and may leave the safeguard present even though the confirmed recovery proceeds

    Scenario: Durable cleanup preserves the recovery fallback ordering until confirmation
      Given recovery has eligible Current, Previous, and Golden fallback candidates
      When a candidate fails or remains unconfirmed
      Then recovery preserves the configured candidate order and any successfully created or already existing durable safeguard for the next eligible fallback or event-driven retry
      And it does not delete such a durable safeguard, Current, or Previous state merely because an earlier candidate was attempted

    Scenario: A confirmed golden restoration retires session fallbacks before final recovery validation
      Given golden recovery has restored and stably confirmed a golden baseline
      And current and previous session baselines were available as fallback candidates
      When the helper proceeds to final recovery validation
      Then it retires the obsolete current and previous session baselines before that validation completes
      And a later validation failure keeps recovery protection but does not recreate those retired fallbacks

    Scenario: Candidate-local session cleanup precedes final recovery validation
      Given a Golden or session candidate has passed its candidate-local confirmation
      And staged session state remains from an earlier Apply
      When recovery processes that candidate result
      Then it attempts to reset the staged session state before dispatching final recovery validation
      And a successful reset remains in effect even if final recovery validation later fails
      But a failed reset is recorded while final recovery validation still proceeds

    Scenario: A successful retry of candidate-local cleanup permits a fresh Apply
      Given candidate-local staged-state cleanup failed but final recovery validation confirmed the desktop is restored
      When the helper retries that cleanup and it succeeds
      Then a later Apply starts without stale session display state

    Scenario: Repeated candidate-local cleanup failure cannot serve another Apply
      Given candidate-local staged-state cleanup failed and final recovery validation confirmed the desktop is restored
      When the cleanup retry cannot be completed
      Then the helper does not run a subsequent Apply against that stale state
      And it finishes the current helper lifecycle so a fresh session can own the next Apply

  Rule: Virtual display changes are recovered without losing the physical desktop

    Scenario: Generic and same-identity virtual display events are non-actionable
      Given a verified virtual-display session is being monitored
      And the virtual display continues to resolve to the session's current device identity
      When display-change, device, or monitor-power events arrive for that unchanged identity
      Then generic display and power events do not start virtual-device discovery
      And a device arrival or removal may perform one identity discovery but starts no topology planning, verification, display reset, or repair Apply when the stable identity is unchanged
      And repeated Windows notifications cannot form an Apply feedback loop after capture starts

    Scenario: A temporarily unavailable virtual identity does not reapply stale intent
      Given a verified virtual-display session is being monitored
      And virtual display discovery temporarily resolves no usable identity
      When display-change, device, or monitor-power events arrive during that gap
      Then the helper starts no topology query, verification, display reset, or repair Apply against the prior identity
      And it waits for a later event that resolves a concrete replacement identity

    Scenario: A changed-identity virtual display event retargets before reapplying
      Given a verified virtual-display session is being monitored
      And the virtual driver now resolves to a different usable device identity
      When a device-arrival or device-removal event arrives
      Then the helper rediscovers and retargets the session to that virtual display identity
      And any explicit topology member or monitor-position override that named the previous virtual identity follows the new identity
      And it reapplies the requested configuration only to the rediscovered virtual target
      And a late event from an earlier session cannot retarget the newer desktop session

    Scenario: Changed-identity repair is bounded for one client session
      Given a virtual-display session repeatedly resolves different usable identities
      When device events overlap an active identity repair
      Then one queued repair retains and rechecks the latest usable identity at the mutation fence
      And the helper performs at most two event-driven identity discoveries for that client Apply
      And the helper performs at most two autonomous changed-identity repair transactions for that client Apply
      And later identity notifications start neither discovery nor Apply, so they cannot sustain a query or mutation feedback loop
      But a new explicit client Apply establishes a fresh bounded repair allowance

    Scenario: A matching virtual identity is recognized conservatively
      Given virtual-display supervision needs to resolve a Windows display identity
      When an available display has the friendly name "SudoMaker Virtual Display Adapter" or EDID manufacturer "SMK" with product code "D1CE", compared case-insensitively
      Then it recognizes that display as a compatible virtual display
      And it prefers an active primary compatible identity when more than one compatible identity is available
      And otherwise it prefers an active compatible identity over an inactive one
      And it returns only the selected display's stable device identifier
      But a display-name-only transitional observation remains unavailable until Windows publishes that identifier

    Scenario: No matching virtual identity does not create a target
      Given virtual-display supervision needs to resolve a Windows display identity
      When no compatible display is found or display discovery is unavailable or fails
      Then it does not invent a virtual identity or retarget a physical display
      And it yields no virtual identity rather than falling back to a physical display
      And recovery-side supervision treats the unresolved virtual target as unavailable rather than healthy

  Rule: Platform-visible cleanup happens only for confirmed display outcomes

    Scenario: Successful verification refreshes shell and conditionally blanks HDR
      Given an Apply has been verified successful for the active, current session
      When it completes its platform-visible cleanup
      Then the shell is refreshed after that verified display change
      And if the session requested HDR blanking, a serialized workaround attempts the default 1-second operation over every active display observed HDR-enabled rather than only the resolved target or every intermediate Apply attempt
      And if the session did not request HDR blanking, it does not blank HDR states
      And topology or HDR-state discovery failure may end the launched workaround without changing any display
      And failures inside a successfully launched workaround leave the verified Apply result unchanged
      But failure to launch the workaround execution itself is not caught at this boundary and has no unchanged-result guarantee
      And a confirmed restore refreshes the shell only after it validates the restored desktop
      And shell refresh is best effort and does not hold the verified Apply or restore result open
      But stale, cancelled, failed, or unverified Apply and restore completions do not blank HDR or refresh the shell

    Scenario: Shell refresh uses its ordered best-effort compatibility notifications
      Given a verified Apply or confirmed recovery requests platform-visible shell refresh
      When Windows is notified of the display change
      Then association-change notification occurs before icon-setting reset
      And setting-change broadcasts for "ShellState" then "IconMetrics" occur before the display-change broadcast
      And each explicit broadcast uses the default 100-millisecond abort-if-hung allowance without making its result authoritative
      And the display-change broadcast carries current screen dimensions and observed color depth, using 32 bits per pixel when that depth cannot be observed
      And any failed notification remains best effort and does not revise the verified display outcome

    Scenario: Consecutive HDR blanking requests are serialized but restoration is best effort
      Given a verified current session is within a requested temporary HDR-blanking interval
      When a later verified current session also requests HDR blanking
      Then the later workaround does not overlap the earlier blanking interval
      And queuing it does not join the earlier one on the Apply-result path
      And a newer live Apply, Revert, Disarm, or recovery start clears any older coalesced request that has not started
      And an accepted blanking operation remains owned until its temporary interval completes
      And only after the complete disable request succeeds does it wait and attempt to restore every HDR state it changed
      But a failed blank or restore operation is swallowed and may leave one or more affected displays in the state Windows last accepted
      And failure of either workaround does not revise either verified Apply result
