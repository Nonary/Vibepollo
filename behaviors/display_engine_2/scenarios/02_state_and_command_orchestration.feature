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
      When its outcome is successful, invalid-request, verification-failed, HDR-state-failed, retryable, or fatal
      Then the observable result preserves that outcome category until the one documented HDR-to-SDR fallback takes ownership
      And caller-side helper unavailability remains a separate outcome of failing to start or communicate with the helper rather than a fabricated helper Apply status
      And a replacement request cannot receive the prior request's category or result identity

    Scenario Outline: Apply maps each display-preparation outcome to its required public status
      Given a current Apply request has reached <stage>
      When <condition>
      Then the Apply outcome is <public-status>
      And <next-behavior>

      Examples:
        | stage                         | condition                                                        | public-status      | next-behavior                                                        |
        | obtaining display context     | no usable display context is available                           | retryable          | the request reports once without starting a second mutation            |
        | preflighting requested topology | the requested topology is rejected                             | invalid-request    | the request stops before the mutation boundary                         |
        | preparing settings            | temporary unavailability prevents settings preparation           | retryable          | the request reports once without blindly repeating the transaction     |
        | preparing device, primary, or mode state | requested device, primary, or mode preparation fails       | verification-failed | the request reaches terminal handling                                  |
        | preparing HDR state           | requested HDR preparation fails                                  | HDR-state-failed   | only an eligible virtual-display HDR request may perform one SDR fallback |
        | retaining SettingsManager state | persistence cannot be saved                                    | retryable          | the request reports once without blindly repeating the transaction     |
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
        | Refresh-rate change cancelled or completed stale    | its client result cannot decide the current session                                     | only the current request may retain a successful rate                       |

  Rule: Source default timing profiles are tunable envelopes, not weaker safety rules

    Scenario Outline: Default timing and retry profiles retain their outcome invariant when tuned
      Given the source default <profile> is <default-envelope>
      When a clean-room implementation tunes an operational duration for its environment
      Then it retains every stated retry cap, candidate order, deadline or grace boundary applicable to that profile
      And it must still preserve <invariant>

      Examples:
        | profile                                | default-envelope                                                                 | invariant                                                                                  |
        | pre-Apply baseline capture             | 3 capture/save attempts, with 50 ms between failed attempts                    | Apply continues after failure; no failed capture replaces a known-good baseline             |
        | ordinary Apply transaction             | one preflight and one SettingsManager transaction                                | ordinary Apply never enters global display-stack recovery or a blind retry loop               |
        | restore topology activation            | up to 2 attempts; each waits up to 5 s for a usable accepted topology           | a successful restore call alone is insufficient; the required topology must be usable         |
        | restore recovery between attempts      | 500 ms settling after display-stack recovery                                    | cancellation stops later restore stages and a possible mutation remains recovery-protected    |
        | initial Apply verification             | two matching observations separated by 250 ms                                   | only the current request can publish verification; failed or unknown remains nonverified while callers retain their documented fallback, and an older result never opens the current gate |
        | initial Apply mismatch                 | one immediate repair transaction followed by one new two-sample verification    | capture remains closed until the current request is verified or reported failed              |
        | virtual identity replacement          | at most 2 event-driven identity discoveries and 2 autonomous repair transactions per client Apply | events coalesce to the latest usable identity while the mutation fence is active, then stop instead of sustaining a query or Apply feedback loop |
        | recent-disconnect settlement           | 5 s from Apply start; then 250 ms and 750 ms verify-before-repair checks        | it is the only delayed verification lane; restoration is avoided if the change stuck            |
        | explicit Revert grace                  | 5 s before the first restore, or 0 ms for immediate restore mode                | a later Apply or protected unconfirmed restore retains its documented precedence             |
        | recovery retry window                  | 2 min primary window; events reopen at least 30 s; backoff 0,1,3,5,10,15,20,30 s | expiry pauses attempts until another display event; it never declares an unconfirmed desktop restored |
        | heartbeat recovery                     | 30 s optional period, 30 s missed-ping period, a 2 min recovery deadline, then 5 s replacement grace | a valid ping or replacement connection cancels the old deadline; restore starts only after the current mutation boundary |

    Scenario: A default duration cannot weaken command precedence or desktop protection
      Given any default retry, grace, verification, or recovery duration has been changed
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
      Then the engine verifies that the requested result is active before sending its successful Apply result
      And the client receives one successful Apply result and one successful verification result for that request
      And a successful initial verification establishes a live, protected session

    Scenario: A failed initial verification uses the bounded initial repair envelope
      Given a valid apply request has not yet passed its initial verification gate
      When an initial verification reports that the requested result is not active
      Then the engine makes at most one immediate repair transaction
      And it verifies that repaired result before treating the session as confirmed
      And the client receives failed Apply and verification results if that one repair does not confirm the request

    Scenario: The former final-HDR-repair option remains wire-compatible without creating work
      Given an HDR apply request carries the compatibility option to omit a final initial HDR reapply
      When the engine processes that request
      Then it accepts the option without adding or removing an Apply transaction
      And the ordinary one-repair capture-readiness boundary remains unchanged

    Scenario: A retryable apply outcome does not blindly repeat display mutation
      Given a valid apply request is current
      When the display operation reports a retryable outcome or a verification-related failure
      Then it reports the terminal Apply and verification outcomes for that transaction
      And it does not schedule another full Apply solely from that status

    Scenario: A virtual HDR request may perform one distinct SDR fallback
      Given a virtual-display apply request asks for HDR
      And its SettingsManager transaction reports the specific HDR-state failure
      When the request is still eligible for the one-time HDR fallback
      Then the engine performs one new display request with effective SDR
      And generic retryable or verification failures do not masquerade as HDR capability failure
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

    Scenario: A successful verified session starts no proactive settling mutation
      Given an apply request has already passed its verification gate
      When the initial capture-readiness boundary completes
      Then no 750 ms, 2,500 ms, or 5,500 ms verification or repair staircase is scheduled
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

    Scenario: A refresh-rate change does not create an ordinary settling staircase
      Given a verified apply request has no proactive post-Apply stabilization work
      When a valid, non-conflicting refresh-rate change targets that request's display scope
      Then the accepted rate becomes part of the active request
      And success or failure does not schedule an unconditional verification staircase

  Rule: Post-apply monitoring does not turn generic Windows events into Apply feedback

    Scenario: A confirmed session receives no unconditional settling checks
      Given an apply request has passed its initial verification gate
      When the capture-ready result is published
      Then the engine performs no timed 750 ms, 2,500 ms, or 5,500 ms follow-up checks
      And generic same-identity display, device, or power events start neither verification nor Apply

    Scenario: A recent disconnect uses the transient settlement path instead of an ordinary repair race
      Given a connection breaks immediately after an apply request begins
      And the session is eligible to defer disconnect recovery
      When the in-progress apply or its initial verification reaches a decision point
      Then the engine checks whether the requested display result stuck before restoring the desktop
      And it repairs only when the 250 ms then 750 ms checks show that the result did not stick
      And the transient checks do not race ordinary post-apply work

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
        | the same managed identity        | identity discovery adds no topology planning, verification, or repair                   |
        | no currently usable identity     | no stale-target Apply is queued; supervision waits for a concrete replacement identity  |
        | a different managed identity     | one follow-up intent waits behind the active mutation and rechecks the latest identity before retargeting |

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
        | a queued Apply, Revert, or Disarm       | an autonomous changed-identity event             | the explicit control intent remains queued and the autonomous repair is discarded                 |
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
        | a delayed disconnect-check completion | Apply, Revert, Disarm, Reset, Refresh Rate, or Ping | those commands are handled in arrival order before the disconnect-check result         |
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

    Scenario: A refresh change outside the active Apply scope is rejected
      Given a verified Apply owns an explicit or resolved duplicate-group scope
      When a valid refresh-rate request names a display outside that Apply's explicit or resolved duplicate-group scope
      Then the engine returns a failed refresh-rate result for that request
      And it leaves the verified display session unchanged without mutating the unrelated display

    Scenario: Refresh rate is serialized and correlated when it is permitted
      Given the display engine is not applying or restoring
      When a client requests a valid refresh-rate change
      Then the refresh change is serialized as a display mutation
      And the client receives its success or failure result with the originating request identity
      And a successful change in the active request's scope becomes part of any later changed-identity repair

    Scenario Outline: A completed active-session refresh starts no settling checks
      Given a verified Apply request has an active-session refresh-rate mutation in progress
      When that refresh-rate mutation completes <outcome>
      Then it schedules no timed verification or repair staircase
      And only a successful in-scope result updates the retained request rate
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
      And a completed superseded Apply cannot publish capture readiness while Reset blocks its replacement
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
