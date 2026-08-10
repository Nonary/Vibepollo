@display-engine-v2
Feature: Display engine v2 state and command orchestration
  The display engine keeps each display-changing session coherent while newer
  client intent, delayed outcomes, and recovery protection overlap.

  Rule: The externally observable state vocabulary and terminal outcomes are finite

    Scenario Outline: A command exposes only its permitted state progression
      Given the engine is in <start-state>
      When it accepts <command>
      Then it reports transitions only through <permitted-states>
      And its terminal outcome is <terminal-outcome>

      Examples:
        | start-state                         | command                                      | permitted-states                                           | terminal-outcome                                                        |
        | waiting                              | a valid Apply                                | applying, verifying, waiting, or virtual-display monitoring | verified, terminal apply failure, or a protected session awaiting recovery |
        | waiting                              | an explicit Revert with a candidate          | restoring, confirming restoration, waiting, or recovery-waiting | confirmed restore, or armed recovery awaiting a display event        |
        | any live protected session          | Disarm without an unconfirmed restore        | waiting                                                     | protection is removed unless an earlier cancelled mutation remains unknown |
        | waiting or a protected live session | Reset                                        | the existing lifecycle condition followed by its current terminal condition | staged state is cleared, an ordinary failure releases its barrier, or required cleanup failure ends the helper lifecycle |
        | virtual-display monitoring          | a relevant display change                    | verifying or applying, then virtual-display monitoring      | the existing session is retained or its configuration is repaired        |

    Scenario: Reachable helper Apply outcomes keep invalid, retryable, verification, and fatal failures distinct
      Given a request reaches a terminal apply decision
      When its outcome is successful, invalid-request, verification-failed, retryable, or fatal
      Then the observable result preserves that outcome category until a documented retry or SDR fallback takes ownership
      And caller-side helper unavailability remains a separate outcome of failing to start or communicate with the helper rather than a fabricated helper Apply status
      And a replacement request cannot receive the prior request's category or result identity

    Scenario Outline: Apply maps each display-preparation outcome to its required public status
      Given a current Apply request has reached <stage>
      When <condition>
      Then the Apply outcome is <public-status>
      And <next-behavior>

      Examples:
        | stage                         | condition                                                        | public-status      | next-behavior                                                        |
        | obtaining display context     | no usable display context is available                           | retryable          | the request follows its retry schedule without reporting success      |
        | activating requested topology | the requested topology is rejected                               | verification-failed | the request follows verification-failure retry or terminal handling  |
        | activating requested topology | an unexpected platform exception occurs                          | fatal              | the request reaches fatal terminal handling                           |
        | preparing session baseline    | saving the preparation baseline fails                              | retryable          | the request follows its retry schedule without reporting success      |
        | preparing settings            | temporary unavailability prevents settings preparation           | retryable          | the request follows its retry schedule without reporting success      |
        | preparing device, primary, mode, or HDR state | requested device, primary, mode, or HDR preparation fails | verification-failed | the request follows verification-failure retry or terminal handling  |
        | preparing settings            | another settings preparation failure occurs                      | fatal              | the request reaches fatal terminal handling                           |
        | preparing settings            | requested settings are prepared successfully                     | successful         | the request proceeds to its current-request verification gate         |

    Scenario: Superseded request ownership is a boundary rather than a result
      Given a display operation, verification, recovery, refresh change, or delayed event belongs to an earlier request or client session
      When newer control intent becomes current
      Then any later completion from the earlier owner is discarded for state transition and client-result purposes
      And the earlier completion may still report whether it reached the display-mutation boundary
      And the current request or client session alone decides the terminal result

    Scenario Outline: Cancellation preserves the observed desktop-safety outcome
      Given <in-flight-work> has been superseded before its result is known
      When its completion reaches the state engine
      Then <mutation-consequence>
      And <ownership-consequence>

      Examples:
        | in-flight-work                                      | mutation-consequence                                                                    | ownership-consequence                                                       |
        | Apply cancelled before the first display mutation   | the cancelled request cannot mutate the desktop and provisional state is cleared when safe | the newer deferred control intent may proceed                              |
        | Apply cancelled after display mutation may begin    | the desktop may have changed and recovery remains armed                                 | the newer intent waits for that boundary, then owns its own outcome        |
        | Revert cancelled during its initial grace           | restoration was prevented before the desktop was touched                                 | Apply or Disarm may take over                                               |
        | Revert cancelled after restoration begins           | the desktop may have changed and the recovery lease remains armed                       | the stale completion is discarded; later work cannot claim restore success |
        | Recovery confirmation cancelled                     | restoration remains unconfirmed and recovery remains armed                              | the newer intent owns future progress, not the cancelled confirmation      |
        | Refresh-rate change cancelled or completed stale    | its client result and prior settling check cannot decide the current session            | later settling starts only from the current request                         |

  Rule: Source default timing profiles are tunable envelopes, not weaker safety rules

    Scenario Outline: Default timing and retry profiles retain their outcome invariant when tuned
      Given the source default <profile> is <default-envelope>
      When a clean-room implementation tunes an operational duration for its environment
      Then it retains every stated retry cap, candidate order, deadline or grace boundary applicable to that profile
      And it must still preserve <invariant>

      Examples:
        | profile                                | default-envelope                                                                 | invariant                                                                                  |
        | pre-Apply baseline capture             | 3 capture/save attempts, with 50 ms between failed attempts                    | Apply continues after failure; no failed capture replaces a known-good baseline             |
        | Apply retry                             | 3 total attempts for each effective HDR or SDR phase; retries after 500 ms then 1,000 ms | no retry may let an obsolete request or session report success or remove recovery after mutation    |
        | topology activation                    | up to 2 attempts; each waits up to 5 s for a usable accepted topology           | a successful call alone is insufficient; the required target must be usable before success  |
        | topology recovery between attempts     | 500 ms settling after display-stack recovery                                    | cancellation stops later stages and a possible mutation remains recovery-protected           |
        | initial Apply verification             | two matching observations separated by 250 ms                                   | only the current request can publish verification; failed or unknown remains nonverified while callers retain their documented fallback, and an older result never opens the current gate |
        | post-Apply settling                    | immediate repair option, then 750 ms; HDR adds 2,500 ms and 5,500 ms stages     | a verified session stays verified if a later best-effort repair fails                        |
        | recent-disconnect settlement           | 5 s from Apply start; then 250 ms and 750 ms verify-before-repair checks        | it replaces ordinary settling rather than racing it; restoration is avoided if the change stuck |
        | explicit Revert grace                  | 5 s before the first restore, or 0 ms for immediate restore mode                | a later Apply or protected unconfirmed restore retains its documented precedence             |
        | recovery retry window                  | 2 min primary window; events reopen at least 30 s; backoff 0,1,3,5,10,15,20,30 s | expiry pauses attempts until another display event; it never declares an unconfirmed desktop restored |
        | heartbeat recovery                     | 30 s optional period, 30 s missed-ping period, a 2 min recovery deadline, then 5 s replacement grace | a valid ping or replacement connection cancels the old deadline; restore starts only after the current mutation boundary |

    Scenario: A default duration cannot weaken command precedence or desktop protection
      Given any default retry, grace, stabilization, or recovery duration has been changed
      When Apply, Revert, Disarm, Reset, Refresh, Stop, a display event, and a heartbeat overlap
      Then retired-session input remains unable to mutate or answer
      And Reset remains ordered without cancelling an earlier display mutation
      And a possibly changed desktop remains protected until explicit disarm is safe or recovery is confirmed
      And candidate ordering and current-request ownership do not depend on the chosen durations

  Rule: A new apply request establishes the current session intent

    Scenario: An already-dispatched Apply command without a configuration is rejected before display application
      Given an internal Apply command has already reached the state engine
      When that dispatched command has no display configuration
      Then the client receives an invalid-request apply result for that request
      And the engine does not start a display application for that request
      And the engine remains ready for a later valid request

    Scenario: An invalid dispatched Apply still supersedes prior recovery work and publishes exclusions but not recovery policy
      Given an internal Apply command without a display configuration carries an exclusion update and different recovery-policy values
      And earlier operations, restore scheduling, or request-local restore progress may still exist
      When that Apply reaches configuration validation
      Then it has already cleared older explicit recovery intent, cancelled current operations, disarmed restore scheduling, reset request-local restore progress, and armed a fresh heartbeat
      And its exclusion update is already the current baseline-filtering policy
      But its golden-ordering and restore-on-disconnect values do not replace the previously published recovery policy
      And it reports invalid-request without beginning a display application
      But any unresolved earlier desktop-recovery obligation remains live even though its current schedule and request progress were reset
      And a later disconnect or heartbeat may reopen that recovery under the previously published restore policy

    Scenario: A valid apply request supersedes unfinished autonomous recovery
      Given recovery protection is pending from an earlier session
      When a client submits a valid apply request
      Then the new request becomes the current session intent
      And unfinished recovery work cannot restore the desktop in place of that new intent
      And the request's restore-on-disconnect and snapshot-exclusion choices take effect before later recovery decisions

    Scenario: A failed replacement Apply still owns the published recovery policy
      Given a protected session already has a desktop recovery obligation
      And a replacement Apply selects different golden-ordering or restore-on-disconnect behavior
      When that replacement becomes current but later fails before verification
      Then the earlier desktop obligation remains protected when it is still unresolved
      And later automatic recovery decisions use the replacement request's published policy
      And the failed replacement does not silently restore the superseded session's policy

    Scenario: Apply continues when all three fallback baseline captures cannot succeed
      Given a valid Apply request has no current session baseline
      And 3 fallback capture/save attempts separated by 50 ms cannot produce a valid baseline
      When the engine begins the Apply request
      Then it continues to process the Apply request instead of rejecting it solely for the capture failure
      And that Apply request does not guarantee that a later Revert has a current-session baseline to use

    Scenario: A later session intent supersedes an older deferred session intent
      Given a display-changing request is still being completed
      And an apply, revert, or disarm intent is already waiting behind it
      When a client submits a newer apply, revert, or disarm intent
      Then only the newer session intent remains to be honored after the active change reaches a safe boundary
      And an ordered reset request remains a barrier rather than being discarded as an older session intent

    Scenario: Replacement intent waits until a possibly mutating change reports its outcome
      Given a display-changing request may already have changed the desktop
      When a replacement apply, revert, or disarm request arrives
      Then the replacement request cancels obsolete future work where safe
      But it does not take control until the active change reports whether the desktop was touched
      And any required recovery protection is retained before the replacement request begins

    Scenario: A command from a retired session cannot alter the current session
      Given a newer client session owns the display engine
      When a command from an earlier session arrives
      Then the command is ignored
      And it cannot mutate display state, recovery protection, or the current request

  Rule: Apply outcomes are confirmed before a capture-gated client proceeds

    Scenario: A successful apply receives an initial verification gate
      Given a valid apply request is current
      When the requested display change completes successfully
      Then the client receives one successful apply result for its request
      And the engine verifies that the requested result is active before opening that request's verification gate
      And a successful initial verification establishes a live, protected session

    Scenario: A failed initial verification uses the exact initial settling envelope
      Given a valid apply request has not yet passed its initial verification gate
      When an initial verification reports that the requested result is not active
      Then the engine makes an immediate repair and, if needed, a 750 ms repair
      And an HDR request additionally uses 2,500 ms and 5,500 ms repair stages
      And it verifies each repaired result before treating the session as confirmed
      And the client receives a failed verification result if none of those repairs confirms the request

    Scenario: A capture-gated request may omit only the final initial HDR repair
      Given an HDR apply request is waiting to open a capture gate
      And the request asks to omit the final initial HDR reapply
      When earlier apply or verification attempts have not confirmed the result
      Then the engine does not delay the initial gate for that final HDR repair
      And it reports the terminal verification outcome for the originating request
      And later post-confirmation stabilization still uses its 750 ms, HDR 2,500 ms, and HDR 5,500 ms checks when applicable

    Scenario: A retryable apply outcome uses three total attempts with fixed default backoff
      Given a valid apply request is current
      When the display operation reports a retryable outcome or a verification-related failure
      Then it makes at most 3 total Apply attempts with retries after 500 ms and 1,000 ms
      And it reports the terminal apply and verification outcomes when the retry allowance is exhausted

    Scenario: A virtual HDR request may fall back to SDR after the three-attempt HDR budget
      Given a virtual-display apply request asks for HDR
      And the 3 standard Apply attempts have been exhausted without confirmation
      When the request is still eligible for the one-time HDR fallback
      Then the engine retries the display request with effective SDR
      And the effective SDR phase receives its own bounded allowance of at most 3 Apply attempts
      And it still requires confirmation before reporting success

    Scenario: A failure after a possible mutation keeps the desktop protected
      Given an apply attempt may already have changed the desktop
      When the attempt reaches a fatal or exhausted failure outcome
      Then the engine retains or enters recovery protection until a safe terminal outcome is confirmed
      And it does not present the display as an unprotected idle session merely because the request failed

    Scenario: A failure before any display mutation does not leave provisional session state behind
      Given an apply attempt is rejected or fails before changing the desktop
      When no retry will be made
      Then the engine reports the failure for that request
      And it clears provisional session state that could incorrectly affect a later independent apply request

    Scenario: A successful verified session is not revoked by a later best-effort settling repair failure
      Given an apply request has already passed its verification gate
      When a later stabilization repair cannot be confirmed
      Then the original successful verification result remains valid
      And the engine preserves the live session and its recovery protection

  Rule: Retired request or client-session work produces no current-session transition or reply

    Scenario: A stale apply or verification completion cannot open a newer request's gate
      Given a newer apply request has superseded an earlier request
      When a delayed apply or verification completion for the earlier request arrives
      Then the completion cannot change the current session state
      And it cannot send an apply or verification result for the newer request
      And the newer request keeps its own request identity and session ownership

    Scenario: A stale session result is not delivered as a current-session reply
      Given a request belongs to a retired client session or superseded request ownership
      When an asynchronous result for that request becomes available
      Then it produces no current-session transition or reply
      And it cannot be mistaken for a result of the current request

    Scenario: A cancelled apply that may have mutated the desktop still reports its safety boundary
      Given an apply request is cancelled by newer intent
      When its completion shows that the display may have changed
      Then recovery protection is retained for the desktop
      And the newer intent waits for that safety outcome before it is honored

    Scenario: A delayed retired-session result cannot respond through a replacement client session
      Given a request's client session has been replaced
      When a delayed result from the retired session occurs
      Then it cannot transition the active session
      And it cannot open a response gate or deliver a result through the replacement session

    Scenario: A stale display or heartbeat event cannot redirect a newer session
      Given a newer session owns the display engine
      When a display event or heartbeat event from an earlier request or replaced client session arrives
      Then the event is ignored
      And it cannot start recovery, disarm protection, or reapply the older session's display request

    Scenario: A refresh-rate change invalidates older settling checks for its active request
      Given a verified apply request has a pending stabilization check
      When a valid, non-conflicting refresh-rate change targets that request's display scope
      Then older settling checks cannot decide the outcome using the pre-refresh request
      And the engine restarts the applicable settling verification for the refresh result

  Rule: Post-apply stabilization protects a session after its first confirmation

    Scenario: A confirmed session is checked again while Windows settles
      Given an apply request has passed its initial verification gate
      When the display may still be settling after the change
      Then the engine performs its 750 ms stabilization check and, for HDR, its 2,500 ms and 5,500 ms checks
      And a failed stabilization check causes the immediate repair/recheck defined for the next settling stage
      And a successful stabilization check preserves the confirmed session

    Scenario: A recent disconnect uses the transient settlement path instead of an ordinary repair race
      Given a connection breaks immediately after an apply request begins
      And the session is eligible to defer disconnect recovery
      When the in-progress apply or its initial verification reaches a decision point
      Then the engine checks whether the requested display result stuck before restoring the desktop
      And it repairs only when the 250 ms then 750 ms checks show that the result did not stick
      And the transient checks do not race ordinary post-apply stabilization work

    Scenario: The final transient-disconnect repair has no later observation gate
      Given the 250 ms and 750 ms transient-disconnect checks each found that the requested display result did not stick
      And the failed 750 ms check started the final bounded repair
      When that repair operation reports success
      Then the engine retains the session as verified and protected without scheduling another display observation
      And this terminal settlement path does not guarantee that the repaired desktop was observed after the repair

    Scenario Outline: A virtual-display event cannot bypass an active display mutation
      Given a monitored virtual-display session has a display mutation in progress
      When an event resolves to <identity-result>
      Then <active-mutation-result>
      And the active mutation retains its authoritative completion boundary

      Examples:
        | identity-result                  | active-mutation-result                                                                  |
        | the same managed identity        | no separate repair is added because the active result will receive its settling check  |
        | a different managed identity     | the request is retargeted and one repair waits behind the active mutation               |

    Scenario: A same-identity event during virtual Apply yields to current verification
      Given a virtual-display request is applying or being verified
      When a display event still resolves to the request's current managed identity
      Then the engine lets the in-progress result be verified without restarting it solely for that event

  Rule: Autonomous signals yield to current control intent

    Scenario Outline: Simultaneous command classes have observable precedence
      Given <earlier-work> is not yet at a safe completion boundary
      When <later-input> arrives
      Then <required-result>

      Examples:
        | earlier-work                            | later-input                                      | required-result                                                                                 |
        | Apply, Revert, Disarm, or Refresh       | a newer Apply, Revert, or Disarm                 | the newer session intent replaces older deferred session intent but waits for safety reporting  |
        | Apply, Revert, or Refresh               | Reset                                             | Reset is retained in order and does not cancel the earlier mutation or its recovery protection  |
        | an unconfirmed Revert                   | Disarm or Snapshot Current                       | the input cannot cancel, overwrite, or claim completion of the unconfirmed restoration          |
        | a pending current-snapshot request      | a later replacement control command on the same connection | the snapshot request remains an ordering barrier; replacement does not pass it               |
        | a current Apply intent                  | heartbeat timeout                                | the automatic signal cannot replace that newer Apply with autonomous recovery                    |
        | a live recovery                          | duplicate disconnect recovery                    | the duplicate joins the current recovery without extending its grace period                      |
        | a current session                       | a stale display event or stale heartbeat          | the stale event is ignored and cannot redirect the current session                               |
        | any accepted Stop                        | later input                                      | the engine enters terminal lifecycle handling; later input cannot revive the old session         |

    Scenario Outline: Only a contiguous eligible command burst may take precedence over an asynchronous completion
      Given <completion> becomes ready to handle
      And <following-inputs> are the uninterrupted burst received immediately after it
      When the helper establishes processing order
      Then <ordering-result>
      And the first non-priority input remains an ordering boundary that later commands cannot cross

      Examples:
        | completion                         | following-inputs                                      | ordering-result                                                                       |
        | any asynchronous completion        | Apply, Revert, Disarm, or Reset                        | those commands are handled in arrival order before the completion                     |
        | a stabilization-check completion   | Apply, Revert, Disarm, Reset, Refresh Rate, or Ping    | those commands are handled in arrival order before the stabilization result           |
        | any asynchronous completion        | Snapshot Current, Golden export, Stop, or an event     | the completion is handled before that first ineligible input                          |

    Scenario: Continuous input traffic cannot starve lifecycle timers
      Given command, event, or completion messages keep arriving without an idle interval
      When the helper handles each input opportunity
      Then it advances heartbeat and recovery-scheduler timers before handling the next opportunity
      And sustained traffic alone cannot postpone a due liveness or recovery decision indefinitely

    Scenario: Recovery is reopened by events without bypassing the current recovery boundary
      Given recovery is armed and its current retry window has expired
      When a display-change, power-resume, device-arrival, or device-removal event for the current session arrives
      Then the engine reopens a 30-second recovery opportunity and resets the recovery backoff to its first attempt
      And it starts at most the recovery currently permitted by that opportunity
      And a stale event, unarmed recovery, or non-recovery session causes no autonomous restore

    Scenario: An explicit revert remains required despite later automatic recovery policy
      Given a client has explicitly requested Revert while an earlier display mutation is still active
      When a heartbeat timeout occurs before that mutation reaches its safety boundary
      Then the explicit Revert remains the required recovery intent
      And autonomous policy cannot replace that intent with a disarm decision

    Scenario: A duplicate disconnect joins an already-running recovery
      Given a disconnect-triggered recovery is already restoring the desktop
      When the same control loss is reported again
      Then the engine joins the existing recovery instead of starting another one
      And it does not restart the recovery grace or discard the active restore outcome

    Scenario: A heartbeat timeout yields to a newer pending Apply intent
      Given a possibly mutating display operation is active
      And a newer Apply request is pending behind that operation
      When a heartbeat timeout arrives for the older session
      Then the timeout does not replace the pending Apply with autonomous recovery
      And the newer Apply remains the next current control intent after the safety boundary

    Scenario: A heartbeat timeout honors a session that opted out of automatic restoration
      Given a live protected session has disabled restore on disconnect
      And no explicit Revert is pending
      When the session heartbeat times out
      Then the engine does not restore the desktop solely because of that automatic signal
      And it clears the ordinary recovery state for the opted-out session

    Scenario: A live ping invalidates an earlier heartbeat recovery deadline
      Given a protected live session is approaching a heartbeat recovery deadline
      When the client sends a valid Ping
      Then the engine refreshes session liveness
      And the earlier missed-heartbeat deadline cannot later start recovery for that live session

  Rule: Non-apply commands preserve the same mutation and response boundaries

    Scenario: An explicit revert requests recovery even when disconnect restoration is disabled
      Given a live session has disabled automatic restore on disconnect
      When the client explicitly requests a revert
      Then the engine begins recovery after any active display mutation reaches a safe boundary
      And the explicit revert is not discarded as a disconnect-policy decision

    Scenario Outline: Revert updates golden-first policy only when that field is supplied
      Given the current recovery policy has <prior-golden-first>
      When an explicit Revert carries <golden-first-field>
      Then later candidate ordering uses <effective-golden-first>
      And the Revert's separate prefer-Golden-when-Current-is-missing choice is published for that request

      Examples:
        | prior-golden-first     | golden-first-field       | effective-golden-first                    |
        | Golden-first enabled   | no golden-first field    | the enabled value retained from before    |
        | Golden-first disabled  | no golden-first field    | the disabled value retained from before   |
        | either prior value     | explicit enabled         | enabled                                   |
        | either prior value     | explicit disabled        | disabled                                  |

    Scenario: A Revert with no persisted restore tier exits cleanly
      Given the Current, Previous, and Golden persistence paths are all absent
      When a Revert is requested
      Then the engine does not begin a restore attempt
      And it cancels current operations, clears recovery state, and requests a successful helper exit

    Scenario: Any present restore tier can arm recovery even when no usable candidate loads
      Given at least one Current, Previous, or Golden persistence path exists
      When normal recovery completes without confirming a candidate
      Then the engine remains armed in its recovery-waiting state with its retry window and backoff state
      And path presence alone does not claim that any candidate was usable or restored
      And a later current control intent may supersede that waiting recovery safely

    Scenario: Disarm does not abandon an unconfirmed restore
      Given an attempted restore has not yet been confirmed
      When a client requests disarm
      Then the engine keeps the restore protection and does not cancel that unconfirmed restore
      And a later safe recovery outcome determines when protection may be removed

    Scenario: Disarm clears an ordinary protected session
      Given no unconfirmed mutation or restore is pending
      When a client requests disarm
      Then the engine cancels obsolete work
      And it clears recovery protection and returns to a waiting session state

    Scenario: Disarm preserves protection after a cancelled mutation with an unknown desktop outcome
      Given a cancelled display mutation may have changed the desktop
      When a client requests Disarm after that work reaches its completion boundary
      Then the engine cancels obsolete future work without removing the recovery guard
      And its waiting state does not claim that the desktop is safely unprotected

    Scenario: Snapshot and golden export do not overwrite a changing display baseline
      Given an apply or revert is actively changing the display
      When a client requests the current snapshot or golden export
      Then the engine does not capture a baseline from the changing desktop
      And a current-snapshot request receives its correlated failure result

    Scenario: Snapshot current applies requested exclusions and returns a correlated outcome
      Given no display mutation or recovery is active
      When a client requests a current snapshot with an exclusion update
      Then the updated exclusion policy is used for the snapshot
      And the client receives the success or failure result for the same request identity

    Scenario: Golden export is accepted when no tracked Apply or recovery is active
      Given no tracked display mutation or recovery is active
      When a client requests a golden baseline export
      Then the engine runs one Golden export operation using up to 3 complete filtered capture-and-save attempts, waiting 50 ms after each of the first 2 failures, under the current exclusion policy
      And a successful export makes the new golden baseline eligible for future recovery decisions
      And acceptance does not prove that an external actor left the desktop stable across observations

    Scenario: Refresh rate rejects invalid or conflicting mutation requests
      Given a client asks to change a refresh rate
      When the request has no device, a zero rate component, or conflicts with an active display mutation
      Then the engine returns a failed refresh-rate result for that request
      And it does not let the refresh request race the active display transaction

    Scenario: A pending stabilization check rejects refresh changes outside its display scope
      Given a verified Apply has a post-Apply stabilization check pending
      When a valid refresh-rate request names a display outside that Apply's explicit or resolved duplicate-group scope
      Then the engine returns a failed refresh-rate result for that request
      And it leaves the current stabilization decision in control without mutating the unrelated display

    Scenario: Refresh rate is serialized and correlated when it is permitted
      Given the display engine is not applying or restoring
      When a client requests a valid refresh-rate change
      Then the refresh change is serialized as a display mutation
      And the client receives its success or failure result with the originating request identity
      And a successful change in the active request's scope becomes part of later settling behavior

    Scenario Outline: A completed active-session refresh restarts settling checks
      Given a verified Apply request has an active-session refresh-rate mutation in progress
      When that refresh-rate mutation completes <outcome>
      Then the engine retires earlier settling checks that used the pre-refresh request
      And it performs the applicable later settling decision before allowing obsolete verification to control the session
      And an unsuccessful refresh is not treated as proof that the desktop remained unchanged

      Examples:
        | outcome          |
        | successfully     |
        | unsuccessfully   |

    Scenario: Reset waits for a display mutation without cancelling it
      Given a display-changing request is active
      When a client requests reset
      Then reset waits until that display mutation reaches its completion boundary
      And it does not cancel the active display change or discard its recovery protection

    Scenario: Reset may clear staged state while read-only verification continues
      Given Apply has finished its display mutation and its read-only verification is in progress
      When a current-session Reset is accepted
      Then staged-state cleanup may begin without cancelling that verification
      And the verification still owns its request outcome while Reset owns only its cleanup result

    Scenario: A successful ordered reset releases only the current deferred control intent
      Given reset is ordered behind display work
      And a current deferred control command is waiting for that reset
      When reset completes successfully
      Then the engine clears the staged display-session state before honoring the deferred command
      And any superseded deferred control intent is not revived

    Scenario: Every accepted Reset remains an ordered barrier
      Given one Reset is already waiting or being completed
      When another current-session Reset is accepted
      Then the later Reset remains ordered after the earlier one instead of being folded into session intent
      And a following Apply, Revert, or Disarm cannot pass either accepted Reset

    Scenario: A Reset completion remains authoritative after newer cancellation
      Given an accepted Reset has reached its non-cancellable cleanup boundary
      And later control intent advances ordinary display-work cancellation
      When the Reset completion arrives
      Then the engine retires that Reset barrier according to its actual success or failure
      And it does not discard the completion as stale display work

    Scenario: An ordinary Reset failure releases its barrier without claiming cleanup
      Given Reset was requested outside the mandatory cleanup after a confirmed recovery
      And a later control intent is waiting behind it
      When Reset fails
      Then the staged display-session state is not reported as cleared
      But the ordinary Reset barrier is retired and the later intent may proceed
      And only the separate mandatory recovery-cleanup failure path requires a fresh helper lifecycle

    Scenario: A recovery terminal outcome waits for its ordered reset before helper exit
      Given a confirmed recovery needs display-session cleanup
      And no newer control transaction has superseded that recovery
      When the engine orders reset as part of its terminal cleanup
      Then it keeps the helper alive until the ordered reset completes
      And it reaches the recovery terminal lifecycle outcome only after that cleanup succeeds

    Scenario: A failed recovery-ordered reset requires a fresh helper session
      Given confirmed recovery ordered reset to clear staged display-session state before another transaction
      When reset fails
      Then the engine does not start the deferred display mutation against that stale state
      And it ends the current helper lifecycle so a fresh session can start cleanly

    Scenario: Ping maintains the current live session without replacing its intent
      Given a live display session is awaiting liveness confirmation
      When the client sends ping
      Then the session liveness is refreshed
      And the ping does not replace the current display request or its recovery policy

    Scenario: Stop ends the helper lifecycle without accepting further work
      Given the display engine is running
      When the client requests stop
      Then the helper begins a clean exit
      And it does not continue as an active session after exit is requested
