@display-engine-v2
Feature: Display engine v2 clean-room public contract and defaults
  Implementations may vary their internal design while preserving public
  vocabulary, default timing profiles, and the safety properties those defaults protect.

  Rule: Public outcomes remain distinguishable

    Scenario: A verified gate outcome is explicit verified success
      Given a caller receives a verified display-gate outcome
      When it chooses its documented next action
      Then it may treat only that current outcome as verified success
      And it does not transfer that verification to another request or session

    Scenario Outline: A nonverified caller result is not upgraded to verified success
      Given a caller observes <caller result> for a display operation or gate
      When it chooses its documented next action
      Then it can distinguish <caller result> from verified success
      And it does not label <caller result> as verified success

      Examples:
        | caller result |
        | failed        |
        | unknown       |
        | unavailable   |
        | cancelled     |
        | gave-up       |

    Scenario: Compatibility selection is learned from the live reply, not presumed
      Given a helper's response compatibility is unknown at the start of a live session
      When the session receives an attributable apply response
      Then Sunshine uses the observed compatible response form for later compatible requests
      And it never lets an untagged legacy reply satisfy successor work

  Rule: Default timing profiles may be tuned only with equivalent safety

    Scenario Outline: A tunable default retains its externally visible safety invariant
      Given an implementation changes the operational default for <profile>
      When a caller reaches the corresponding deadline, retry boundary, or throttle
      Then it still produces <terminal outcome>
      And it still preserves <invariant>

      Examples:
        | profile                                  | terminal outcome                                      | invariant                                                     |
        | normal helper readiness and accepted liveness delivery: 5 seconds | ready or unavailable by the caller deadline | no late work certifies a retired helper                 |
        | ordinary control connection: 2 seconds      | connected or unavailable by the caller deadline      | unavailable work does not claim current-session ownership      |
        | ordinary control send: 5 seconds            | dispatched or unavailable by the caller deadline     | a cancelled send cannot later publish success                  |
        | shutdown control connection or send: 500 milliseconds | dispatched or unavailable promptly          | teardown cannot wait indefinitely or extend old ownership      |
        | legacy Apply result: 5 seconds              | Apply result or unsuccessful result                  | an untagged late reply never satisfies successor work          |
        | Refresh Rate connection and result: 600 milliseconds and 12 seconds | result or unsuccessful result | result remains attributable to the requested display and current request |
        | Snapshot Current stage: 3 seconds           | result, dispatch-only compatibility, or failure      | snapshot work cannot consume all later Apply budget            |
        | stale-result retirement: 250 milliseconds   | prior authority is retired promptly                  | a late result cannot satisfy later work                        |
        | restore per-attempt topology readiness: 5 seconds | usable restored topology or the defined recovery retry | incomplete restoration is never published as confirmed success |
        | fallback target-activation observation: 6 seconds | observed target or best-effort continuation after core success | an unobserved target does not overturn that fallback's core success |
        | normal helper listener renewal: 15 seconds   | listener availability is renewed                     | pending safety work continues while no client is connected     |
        | helper heartbeat: 30-second initial and missed-ping windows, then 2-minute recovery grace | recovery only after the grace boundary | a timely ping cancels an obsolete missed-ping recovery         |
        | existing-helper reuse separation: 200 milliseconds | second and final liveness delivery or unavailable       | ordinary reuse does not kill a possibly restoring helper       |
        | hard-restart cache-only liveness: 100 milliseconds | reuse or begin bounded replacement                    | a healthy live helper is not killed solely because restart was requested |
        | hard-restart exit observation and settle: 2 seconds then 100 milliseconds | replacement preparation continues or is cancelled | retired ownership cannot answer successor work          |
        | hard-restart overlap retry interval: 150 milliseconds | next bounded launch attempt or unavailable              | overlap cannot create an unlimited launch loop                 |
        | new-helper early-exit observation: 50 milliseconds per check | early failure or readiness preparation                  | only singleton conflict receives its one special retry         |
        | singleton-conflict cleanup and settle: 1 second then 300 milliseconds | one retry or unavailable                         | no repeated singleton retry loop                               |
        | new-helper initialization and readiness: 200 milliseconds then 5 seconds at 100-millisecond checks | ready or unavailable | no late readiness adopts expired ownership             |
        | helper supervision outside the zero-RTSP-session plus launched-app condition: 5 seconds | next liveness check is due | a failed check cannot reapply an obsolete display request |
        | helper supervision with zero counted RTSP sessions and a launched app identifier: 20 seconds | next liveness check is due | WebRTC-only activity does not imply the 5-second cadence |
        | shutdown liveness delivery: 250 milliseconds | accepted or unavailable promptly                   | teardown does not extend an earlier ownership                  |
        | prompt disarm budget and throttle: 150 milliseconds | delivered, throttled, or unavailable promptly | repeated requests do not permit uncontrolled recent restore    |
        | disarm grace: 5 seconds                  | recent restore may be stopped; older restore may finish | an older restore is not overwritten by the prompt path       |
        | forced-stop wait: 2 seconds              | cleanup continues after the observation wait          | obsolete helper results never own successor work               |
        | deferred initial delay: 2 seconds        | first attempt occurs after readiness delay            | no ended stream receives a later display change                |
        | deferred retry: 500 milliseconds to 10 seconds | retry or removal after the allowed attempts      | retries remain finite and session-owned                        |
        | helper cooldown after its fixed failure threshold: 30 seconds | unavailable during cooldown                  | one failure remains eligible for its retry                     |
        | deferred output pending-Apply allowance: 8 seconds at 250-millisecond checks | capture retarget or cancellation              | Apply is attempted before retarget and cancellation prevents late retarget |
        | one preferred virtual capture-readiness check: 3 seconds | desired target, existing safe selection, or unavailable evidence | one check is bounded and never claims a missing target; its enclosing capture cycle remains separately defined |
        | one ordinary reinitialization or synchronous capture-open invocation: 200 then 300 milliseconds | display or return to the enclosing reopen cycle after two attempts | one invocation remains bounded while its outer cycle is not |
        | one recent-Apply reinitialization or synchronous capture-open invocation: 300 through 700 milliseconds in 100-millisecond increments | display or return to the enclosing reopen cycle after five attempts | one invocation gets finite extra settling opportunity while its outer cycle is not |
        | virtual teardown settle: 5 seconds       | settled removal or continued restoration attempt      | timeout cannot suppress required restoration fallback          |
        | recovered-display reapply waits: 250-millisecond increments | success, cancellation, or exhaustion after five attempts | reinitialization never fabricates Apply success          |

    Scenario: A changed timer cannot weaken result attribution or verification ownership
      Given an implementation uses a different operational timing value
      When a reply, verification, restoration, or cleanup arrives after its owner was cancelled or superseded
      Then that event cannot publish a current session result
      And it cannot open capture, mark capture-stable, or change active-session ownership

    Scenario: A changed operational duration cannot turn a bounded caller path into an unlimited one
      Given an implementation tunes a documented operational duration for bounded helper readiness, deferred resolution, or display verification
      When that bounded operation keeps failing
      Then callers still reach the documented finite retry cap and safe terminal result
      And the separately documented initial, reinitialization, and synchronous capture-open outer cycles remain shutdown-owned rather than acquiring invented attempt caps

  Rule: Logical capacities and retry counts remain exact

    Scenario Outline: A fixed logical bound retains its current cap
      Given an implementation preserves <bounded behavior>
      When activity reaches <exact cap>
      Then it produces <boundary result>
      And tuning operational durations does not change that count or capacity

      Examples:
        | bounded behavior                         | exact cap                         | boundary result                                      |
        | ordinary existing-helper liveness        | two delivery attempts             | reuse or unavailable without forced termination      |
        | hard-restart launch overlap              | five attempts after the initial attempt | unavailable if no launch is accepted              |
        | new-helper early-exit observation        | six checks                         | readiness preparation or early-exit handling          |
        | singleton-conflict restart               | one retry                          | ready or unavailable without another retry            |
        | helper-start cooldown threshold           | two consecutive failures           | the second failure arms cooldown and success clears it |
        | deferred resolution Apply                | six attempts                       | pending work is removed after the sixth failure        |
        | recreated virtual-display Apply          | five attempts                      | reinitialize capture without claiming failed Apply     |
        | one ordinary reinitialization or synchronous capture-open invocation | two attempts | that invocation opens or returns failure to its enclosing cycle |
        | one recent-Apply reinitialization or synchronous capture-open invocation | five attempts | that invocation returns to its enclosing cycle without a sixth attempt |
        | recognized result-frame retention        | 32 frames per live connection      | oldest retained frame is evicted even when a newer recognized frame is malformed or unattributable |
        | process-global successfully sent Apply identity history | 64 identifiers            | oldest identity loses unknown-protocol stale classification evidence |

    Scenario Outline: A fixed policy constant preserves its observable meaning
      Given a clean-room implementation represents <policy>
      When the policy is applied
      Then it preserves <default value>
      And a tunable representation must retain the same ordering and safety outcome

      Examples:
        | policy                                  | default value                                      |
        | highest-refresh operating-system hint  | exact 10000 over 1                                 |
        | isolated pointer-space anchor           | 64000 with saturating coordinate arithmetic       |
        | default EDID refresh targets            | 120, 180, 240, and 288 in that order              |
        | EDID target allowance                   | 0.5 hertz                                         |
        | golden snapshot latest schema           | version 2                                         |
        | permanent virtual-display count maximum | 4                                                 |
