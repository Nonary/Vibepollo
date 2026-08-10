@display-engine-v2
Feature: Display engine v2 client integration and stream lifecycle
  Sunshine builds, dispatches, and supervises display-helper work so a stream
  receives a default-deadline, session-correct display outcome.

  Rule: Session requests preserve the intended display behavior

    Scenario: A request marked Skip is not dispatched
      Given a session cannot produce a usable display request
      When Sunshine builds the display-helper request
      Then it does not send a display command
      And the caller receives a safe unsuccessful apply outcome

    Scenario: A failed virtual-display setup does not alter active displays
      Given a session's virtual display could not be initialized
      When Sunshine builds its display request
      Then it declines to dispatch display configuration work
      And the existing desktop remains the capture fallback

    Scenario: A disabled physical display configuration with an output override remains a capture-target choice
      Given a physical session has a display-output override
      And display configuration is disabled for that session
      When Sunshine builds the session request
      Then it does not create a display-helper apply request
      And the output override remains a capture-target choice rather than an unintended display mutation

    Scenario: A configuration-disabled session requests a revert when source behavior requires restoration
      Given a session resolves to the configured restoration behavior instead of an apply configuration
      When Sunshine builds the session request
      Then the request is a revert intent
      And it does not carry a fabricated display configuration

    Scenario: A session-derived request preserves its display choices
      Given a stream session supplies a target configuration and display preferences
      When Sunshine builds an apply request
      Then the request carries the intended target configuration and session overrides
      And it carries applicable virtual arrangement, topology, placement, and physical refresh preferences

    Scenario: An apply request carries safety choices that affect its session
      Given a display apply has a valid configuration
      When Sunshine dispatches it to the selected helper
      Then the request includes applicable HDR handling, golden restore preference, disconnect policy, and snapshot exclusions
      And a stream-start request asks for capture-verification behavior within the 8-second shared default budget

    Scenario: A virtual session keeps its resolved target identity before dispatch
      Given a virtual display session has a resolved virtual display identity
      When Sunshine prepares its apply request
      Then the identity is retained for the session and baseline-exclusion policy
      And later session cleanup can distinguish that virtual target from physical displays

    Scenario: A successful apply publishes only the successful session's display state
      Given a session apply succeeds
      When Sunshine records the active display session
      Then it records that session's effective target, dimensions, frame rate, HDR choice, and virtual-display choice
      And a failed or cancelled apply does not publish those values as the active session

    Scenario: A virtual session enables its requested virtual-display supervision
      Given a successful session apply requests virtual-display supervision
      When Sunshine adopts the session display state
      Then virtual-display supervision is enabled for that session
      And the supervision remains associated with the session that requested it

  Rule: Session-derived virtual requests preserve layout, target, and mode intent

    Scenario Outline: Virtual layout determines activation, primary, and isolation intent
      Given a virtual-display session uses the <layout> layout
      When Sunshine builds its valid virtual Apply request
      Then the request asks to <activation intent>
      And the request <primary intent>
      And physical and virtual pointer space is <pointer-space intent>

      Examples:
        | layout                    | activation intent                 | primary intent                         | pointer-space intent                 |
        | exclusive                 | make the virtual target the only display | does not preserve a physical primary | not extended with physical displays |
        | extended                  | keep the virtual target active    | preserves a physical primary when needed | extended normally                    |
        | extended-primary          | keep the virtual target active    | makes the virtual target primary         | extended normally                    |
        | extended-isolated         | keep the virtual target active    | does not require virtual primary         | isolated safely                      |
        | extended-primary-isolated | keep the virtual target active    | makes the virtual target primary         | isolated safely                      |

    Scenario: A session-specific virtual layout overrides the configured layout
      Given the configured virtual layout differs from a session-specific layout
      When Sunshine builds the session's virtual Apply request
      Then it uses the session-specific layout intent
      And it does not merge activation, primary, or isolation intent from the configured layout

    Scenario Outline: Virtual intent may originate from the session, configured mode, or application metadata
      Given a session has virtual intent from <source>
      When Sunshine builds the display request
      Then it prepares virtual target behavior when a valid virtual configuration can be formed
      And it does not require all three virtual-intent sources to agree

      Examples:
        | source                         |
        | an explicit session request     |
        | a configured per-client mode    |
        | a configured shared mode        |
        | application virtual-screen metadata |

    Scenario: Virtual stable identity follows configured sharing scope
      Given a session requires a virtual display
      When Sunshine derives the stable virtual identity
      Then per-client mode uses the client UUID when nonempty and otherwise uses the session unique identity
      And shared mode uses the persistent shared identity rather than either per-client identity

    Scenario: Invalid saved virtual identity is repaired within its scope
      Given virtual creation needs a parseable stable identity
      When shared mode has a missing or invalid saved shared identity, or a non-client session identity is invalid
      Then Sunshine uses its persistent virtual identity and updates the applicable shared or session identity
      And a nonempty per-client UUID still remains the per-client display identity

    Scenario: Virtual target resolution follows its constrained identity order
      Given a virtual session needs a capture target
      When Sunshine resolves the target identity
      Then it tries the session's known virtual display, the client name, and the configured output hint in that order
      And it does not substitute an arbitrary managed virtual display unless the client name is empty

    Scenario: A resolved virtual target remains the target of the virtual Apply
      Given a virtual session has a resolved active virtual target identity
      When Sunshine creates the virtual Apply request
      Then the request and published session retain that virtual target identity
      And it does not redirect the request to an arbitrary physical target

    Scenario: Unavailable named virtual identity produces no arbitrary display mutation
      Given a named virtual session has no resolved virtual target identity
      And no configured output provides a valid fallback target
      When Sunshine cannot form a valid virtual Apply request
      Then it returns no display request or a safe unsuccessful outcome
      And it does not choose an arbitrary active physical display

    Scenario: A configured physical output is only a constrained fallback target
      Given virtual target resolution cannot identify a live virtual target
      And the configured output names a physical display
      When Sunshine builds the request
      Then it may retain that configured physical output as the explicit fallback target
      And it does not substitute another physical target when that explicit output is unavailable

    Scenario: Virtual creation defaults only missing mode components
      Given a virtual display must be created for a session
      And effective positive dimensions or refresh may be unavailable
      When Sunshine forms its initial creation mode
      Then missing dimensions default to 1920 by 1080 and missing refresh defaults to 60000 millihertz
      And HDR begins as no state change unless source-display policy explicitly resolves enabled or disabled

    Scenario: Virtual creation publishes only a resolved successful identity
      Given virtual creation completes with immediate or later identity evidence
      When Sunshine adopts the creation result
      Then only a successful nonempty identity becomes the runtime output and adapter-verification target
      And failure clears virtual ownership without changing the active display selection

    Scenario: A valid virtual Apply preserves effective dimensions and frame rate
      Given a virtual request has a valid target configuration and positive session dimensions or frame rate
      When Sunshine creates the Apply request
      Then it carries the effective requested dimensions and frame rate for that virtual target
      And published virtual session state uses the same effective values

    Scenario: Virtual frame-generation refresh never reduces the required display rate
      Given a virtual session has an active frame-generation refresh policy
      When the requested display rate is below the policy's required minimum
      Then Sunshine raises the virtual target rate to at least that minimum
      And it retains the session's frame-generation rate as session state

    Scenario: User-disabled virtual resolution or refresh changes are respected
      Given a virtual session has user-disabled resolution or refresh changes
      When Sunshine builds a valid virtual Apply request
      Then it does not add the disabled resolution or refresh override
      And it retains an already valid requested target mode unless another valid policy requires a minimum frame-generation rate

    Scenario: Virtual supervision is enabled only after a valid virtual Apply is built
      Given a session has virtual intent but its configuration cannot be formed
      When Sunshine builds the display request
      Then it does not enable virtual-display supervision or publish virtual Apply state
      And a valid virtual Apply is required before that supervision can belong to the session

  Rule: Standard display requests preserve mode-specific compatibility behavior

    Scenario: An unconfirmed virtual HDR target uses effective SDR
      Given a session captures from a virtual target that did not confirm HDR activation
      And the otherwise effective standard request asks for HDR
      When Sunshine builds the Apply request
      Then it requests SDR for that target while retaining the valid topology and mode intent
      And it does not claim that HDR is active for the session

    Scenario: RTX HDR uses an SDR display request
      Given RTX HDR is enabled for a standard display request
      When Sunshine builds the Apply request
      Then it requests SDR from the display
      And it leaves HDR conversion to the applicable stream behavior rather than claiming display HDR

    Scenario Outline: Non-desktop dummy-plug compatibility uses its limited HDR mode
      Given the dummy-plug HDR compatibility option is enabled
      And the session targets a <session kind>
      When Sunshine builds a standard Apply request without active frame-generation compatibility fixes
      Then <result>

      Examples:
        | session kind | result                                                              |
        | non-desktop  | it requests the limited 30 Hz HDR configuration                    |
        | desktop      | it does not impose that non-desktop limited configuration          |

    Scenario: Dummy-plug HDR with a frame-generation compatibility fix does not impose 30 Hz
      Given a non-desktop request uses dummy-plug HDR and an applicable frame-generation compatibility fix
      When Sunshine builds the standard Apply request
      Then it requests HDR without imposing the limited 30 Hz mode
      And the applicable frame-generation refresh behavior remains eligible

    Scenario Outline: Compatibility refresh uses the 10,000/1 target only for its eligible trigger
      Given a standard request has <eligible trigger>
      And dummy-plug HDR does not suppress that trigger
      When Sunshine builds the Apply request
      Then it requests the 10,000/1 compatibility refresh target
      And it does not add a user-disabled resolution change

      Examples:
        | eligible trigger                                                        |
        | a generation-one frame-generation compatibility fix                     |
        | a generation-two frame-generation compatibility fix                     |
        | VSync disabled with no NVIDIA GPU or unavailable NVIDIA control         |

    Scenario: Dummy-plug HDR suppresses best-effort refresh without a frame-generation fix
      Given dummy-plug HDR is enabled for a standard request
      And no frame-generation compatibility fix applies
      When Sunshine builds the Apply request
      Then it does not request the 10,000/1 compatibility refresh target solely from best-effort refresh conditions
      And it preserves the dummy-plug-specific mode behavior

    Scenario: Standard requests without refresh conditions preserve normal rate selection
      Given a standard request has no frame-generation compatibility fix and no eligible best-effort refresh condition
      When Sunshine builds the Apply request
      Then it uses the requested or configured normal refresh behavior
      And it does not invent a forced compatibility refresh

    Scenario: A physical output override with disabled configuration remains capture-only
      Given display configuration is disabled and a session explicitly selects a physical output
      When Sunshine builds the request
      Then it retains that output only as the capture choice
      And it does not apply, revert, or supervise a display change for that override

    Scenario: Disabled configuration otherwise requests restoration
      Given a standard display request has display configuration disabled
      And there is no physical-output capture-only exception
      And no valid non-desktop dummy-plug compatibility exception applies
      When Sunshine builds the standard request
      Then it requests Revert without a fabricated display configuration
      And it does not enable virtual-display supervision

    Scenario: The valid non-desktop dummy-plug exception builds only its limited configuration
      Given a standard display request has display configuration disabled
      And a non-desktop session meets the dummy-plug HDR compatibility conditions without a frame-generation compatibility fix
      When Sunshine builds the standard request
      Then it creates only the limited target configuration required by that compatibility behavior
      And it does not reinterpret disabled configuration as a general display-configuration request

    Scenario Outline: Dummy-plug desktop classification follows application launch intent
      Given a dummy-plug compatibility decision has <application evidence>
      When Sunshine classifies the session as desktop or non-desktop
      Then it treats the session as <classification>
      And the classification controls whether the non-desktop limited configuration is eligible

      Examples:
        | application evidence                                      | classification |
        | a resolved app with neither command nor Playnite identity | desktop        |
        | a resolved app with a command or Playnite identity        | non-desktop    |
        | no resolved app and a nonpositive app identity            | desktop        |
        | no resolved app and a positive app identity               | non-desktop    |

  Rule: Session topology preserves explicit display relationships safely

    Scenario: A physical ensure-only request names only its explicit physical target
      Given a physical request explicitly asks to deactivate other displays
      And it has a nonempty explicit physical target identity
      When Sunshine supplies topology intent
      Then it names that physical target as the only display
      And it does not use virtual placement behavior for the physical session

    Scenario: An exclusive virtual layout names only its explicit virtual target
      Given a virtual session uses exclusive layout and has a nonempty target identity
      When Sunshine supplies topology intent without a saved extended topology
      Then it names that virtual target as the only display
      And it does not inject unrelated physical displays

    Scenario: Exclusive virtual intent upgrades disabled preparation
      Given virtual intent uses exclusive layout
      And ordinary device preparation is disabled
      When Sunshine builds a valid virtual request
      Then preparation becomes ensure only that virtual display
      And nonexclusive layouts retain their own activation and primary intent

    Scenario: An extended virtual layout merges a saved topology without duplicate virtual membership
      Given a nonexclusive virtual session has a saved topology and a resolved virtual target
      When Sunshine supplies topology intent
      Then it preserves the saved topology and adds the virtual target only if absent
      And it does not duplicate a target already present under case-insensitive identity matching

    Scenario: Extended non-primary layout restores physical primary safety after virtual creation
      Given extended non-primary layout finds that the virtual target became primary
      And physical display evidence is available
      When Sunshine supplies monitor placement intent
      Then it returns the first usable nonvirtual display to the origin and places the virtual target immediately to its right by that display's width
      And it leaves placement unchanged when evidence is unavailable or the virtual target is not observed primary

    Scenario: Extended-primary layout does not impose non-primary physical restoration
      Given a virtual session uses extended-primary layout
      When Sunshine supplies monitor placement intent
      Then it preserves the virtual-primary intent
      And it does not apply the extended non-primary physical-primary correction

    Scenario: Isolated layout preserves physical layout while separating virtual pointer space
      Given a virtual session uses isolated non-primary extended layout
      When display evidence is available
      Then Sunshine preserves physical display positions
      And it places the virtual target at the default 64000 isolation anchor without coordinate overflow

    Scenario: Primary isolated layout keeps the virtual target primary and separates physical pointer space
      Given a virtual session uses extended-primary-isolated layout
      And physical display evidence is available
      When Sunshine supplies monitor placement intent
      Then it keeps the virtual target at the primary origin and finds the minimum physical x and y coordinates
      And it translates every physical origin equally until those minima reach the default 64000 isolation anchor, saturating additions instead of overflowing

    Scenario: Missing topology or display evidence leaves placement safe and partial
      Given required virtual identity, saved topology, or display evidence is unavailable
      When Sunshine cannot derive a safe placement adjustment
      Then it omits that adjustment rather than inventing a target or coordinate
      And it retains only independently valid topology intent

    Scenario: Pre-virtual refresh restoration carries physical target rates only
      Given a virtual session retained refresh rates from before virtual display creation
      When Sunshine builds topology-related refresh restoration intent
      Then a nonisolated layout includes physical target rates while an isolated layout includes none
      And it excludes the resolved virtual target case-insensitively from any retained rates

  Rule: Engine selection and helper availability are safe and default-deadline limited

    Scenario Outline: The selected engine follows the user's setting and release channel
      Given the display-helper setting is <selection>
      And Sunshine is running a <release channel> build
      When Sunshine starts a helper for a display request
      Then it uses the <selected engine> helper for that control session
      And it records that concrete engine choice for a later restore launch

      Examples:
        | selection | release channel | selected engine |
        | v2        | stable          | v2              |
        | legacy    | prerelease      | legacy          |
        | automatic | prerelease      | v2              |
        | automatic | stable          | legacy          |
        | invalid   | prerelease      | v2              |
        | invalid   | stable          | legacy          |

    Scenario: A valid explicit apply requests helper availability
      Given a valid display apply request was built for a session
      When Sunshine dispatches that apply
      Then it attempts to reuse or start the helper needed to service the request
      And the caller receives helper unavailability if the helper cannot become ready

    Scenario: A revert may start a helper even when normal display control is disabled
      Given restoration is explicitly requested
      When Sunshine submits the revert
      Then it may start the helper needed to deliver that restoration intent
      And it returns whether the helper accepted the request

    Scenario: An existing helper with accepted liveness delivery is reused
      Given a compatible helper is already running and accepts a liveness frame
      When Sunshine needs to dispatch a new request
      Then it reuses that helper as the current control service
      And it does not create a competing helper instance

    Scenario: A helper that rejects both ordinary liveness deliveries is not trusted as ready
      Given an existing helper appears available but does not accept either of two ordinary liveness deliveries separated by the default 200 milliseconds
      When Sunshine needs display control
      Then it retires the connection but leaves the existing helper running because restoration may still be in progress
      And it records a start failure and reports the helper unavailable until later recovery succeeds

    Scenario: A stream-start restart avoids interrupting an unconfirmed restore
      Given a stream-start request finds an unconfirmed restore still in progress
      When Sunshine prepares the new apply
      Then it allows the new apply to supersede the restore through the live helper
      And it does not prematurely terminate the helper and strand an uncertain desktop

    Scenario: A stream-start request can restart a stale helper when no restore must be preserved
      Given a stream-start request finds a stale helper with no unconfirmed restore to protect
      When the helper cannot prove prompt liveness
      Then Sunshine attempts a safe replacement of the stale helper
      And it proceeds only if the replacement helper becomes ready within the caller's budget

    Scenario Outline: Apply helper acquisition preserves its outer retry order under one deadline
      Given <apply kind> needs a helper and no acquisition attempt has yet succeeded
      When Sunshine performs the caller-visible acquisition sequence
      Then it attempts <sequence>
      And it checks cancellation between attempts and never extends the original shared Apply deadline
      And the five overlap launches inside one hard-restart attempt do not add another outer acquisition attempt

      Examples:
        | apply kind                                             | sequence                                             |
        | stream start with no unconfirmed restore to protect    | hard restart, then ordinary reuse or start, then one final hard restart |
        | stream start with an unconfirmed restore to protect    | ordinary reuse or start, then one final ordinary reuse or start         |
        | ordinary non-stream Apply                              | ordinary reuse or start, then one final ordinary reuse or start         |

    Scenario: Repeated helper start failures enter a cooldown
      Given helper startup has failed twice consecutively without a successful start
      When another request attempts to start the helper during the default 30-second cooldown
      Then Sunshine returns helper unavailability without a hot restart loop
      And a later successful helper start clears the failure condition

    Scenario: A single recent helper start failure still receives its allowed retry
      Given a previously healthy display-helper path has one start failure
      When Sunshine immediately needs display control again
      Then it allows one immediate retry before cooldown suppression applies
      And only a second consecutive start failure arms the cooldown

    Scenario: A system context without a user session does not apply to an unsafe desktop
      Given Sunshine is running as the system account without an interactive user session
      When an apply request needs display access
      Then Sunshine prefers the helper path appropriate for that context
      And if that path is unavailable it does not use an unsafe direct display fallback

    Scenario: V2-only helper settings are synchronized only with a selected v2 helper
      Given a helper selected to run the v2 engine has become ready
      When Sunshine synchronizes helper verbosity
      Then it sends the v2-only setting only to that v2 helper
      And reconnecting allows the new helper session to receive its applicable setting

  Rule: Client communication keeps replies attributable to the live request

    Scenario: A v2 apply acknowledgement creates a correlated verification ticket
      Given Sunshine dispatches a valid apply to a v2 helper
      When the helper accepts the apply with a matching v2 acknowledgement
      Then Sunshine records a verification ticket for that exact apply and live helper session
      And later verification must match that ticket before it can affect capture

    Scenario: A legacy apply acknowledgement remains compatible but is not verification proof
      Given Sunshine dispatches an apply to a legacy-compatible helper
      When the helper returns its untagged acknowledgement
      Then Sunshine preserves the compatible synchronous result behavior
      And it does not claim that a separately correlated v2 verification is available

    Scenario: The live apply reply determines response compatibility
      Given helper configuration has changed or a helper was reused
      When Sunshine receives the first apply acknowledgement for the live connection
      Then it determines v2 or legacy response compatibility from that reply
      And it does not infer the live response format only from the configured engine preference

    Scenario: A reply with current attribution evidence remains request-specific
      Given a current request is awaiting a correlated result on a known v2 connection
      When a response for a different request becomes available
      Then Sunshine preserves that response only for its matching operation
      And it continues waiting for the current request's own result or deadline

    Scenario: A live connection retains at most 32 recognized result frames
      Given Apply, Verification, Refresh, or Snapshot result frames arrive before the current waiter can consume them
      When the live connection already retains 32 recognized result frames
      Then each additional recognized result frame evicts the oldest retained frame
      And malformed or unattributable recognized results consume the same capacity and can evict a valid awaited result
      And an evicted awaited result becomes unavailable rather than being fabricated

    Scenario: Connection retirement discards every retained reply
      Given a live helper connection has retained result frames
      When Sunshine retires that connection
      Then all of that connection's retained results lose authority together
      And a replacement connection cannot consume them

    Scenario Outline: Unknown apply compatibility is learned from bounded evidence
      Given the live connection's Apply response compatibility is unknown
      When the first applicable Apply result is <reply form>
      Then Sunshine classifies it as <classification>
      And the result follows <current-request effect>

      Examples:
        | reply form                                                        | classification | current-request effect                              |
        | an untagged result                                                 | legacy         | its synchronous success or failure is consumed      |
        | a tagged result matching the current request                       | v2             | its matching success or failure is consumed         |
        | a tagged result among the 64 most recently issued other Apply ids   | v2 traffic     | it is preserved or ignored as stale, not consumed   |
        | a long failure carrying an unknown id not in that recent history    | legacy         | the compatible failure is consumed                  |

    Scenario: Unknown-protocol stale classification has a bounded limitation
      Given more than 64 later Apply identifiers have aged an old issued identifier out of recent history
      And response compatibility is still unknown
      When a long failed tagged response for that old identifier arrives
      Then it may be classified as a legacy-compatible failure
      And only a connection already known as v2 guarantees exact identifier matching without that limitation

    Scenario: Recently issued Apply identity history is process-global and send-qualified
      Given an Apply has a nonzero identifier
      When its frame is sent successfully
      Then the identifier enters a process-global newest-64 history used only while Apply response compatibility is unknown
      And connection retirement does not clear that history
      But a failed send does not add the identifier, and the sixty-fifth newer successful send evicts the oldest identity

    Scenario: A superseding control request cannot inherit an outstanding untagged reply
      Given a helper has not established v2 response correlation
      And an earlier control request may still have an untagged reply outstanding
      When Sunshine sends a superseding control request
      Then it isolates the successor request from the earlier untagged reply
      And the earlier reply cannot be mistaken for the successor's result

    Scenario: A cancelled or expired apply returns without publishing stale success
      Given an apply has a cancellation condition or caller deadline
      When cancellation is requested or the deadline expires before a matching acknowledgement
      Then Sunshine returns an unsuccessful apply outcome
      And it does not publish active-session state or a verification ticket for that obsolete work

    Scenario: A missing deadline-limited apply acknowledgement retires its stale connection
      Given an apply was sent to a v2-capable helper with an explicit shared deadline
      When its matching acknowledgement does not arrive before the caller deadline
      Then Sunshine returns unsuccessful by that deadline
      And it retires the connection so a late acknowledgement cannot answer a newer request

    Scenario: Resetting a client connection invalidates outstanding waits
      Given an apply or verification wait belongs to a cached helper connection
      When Sunshine retires that connection for failure, replacement, or explicit reset
      Then the outstanding wait returns unavailable rather than accepting a stale reply
      And a later request establishes a new connection identity

    Scenario: A normal liveness probe may establish a healthy helper connection
      Given Sunshine needs an ordinary liveness check
      When it sends the probe within its normal caller budget
      Then it may use a healthy connection or establish one if time permits
      And probe success means only that the liveness request was sent along a healthy send path

    Scenario: A fast liveness probe never spends stream-start time reconnecting
      Given a latency-sensitive stream-start or restore-disarm path needs a fast probe
      When no already-live helper connection is available
      Then the probe fails promptly
      And it does not perform a slow connection setup within that fast budget

  Rule: Caller-side operations share default deadlines and compatible semantics

    Scenario: A latency-sensitive stream-start apply never enters an unlimited fallback
      Given a stream-start apply cannot reach a ready helper within its shared deadline
      When helper dispatch fails or is cancelled
      Then Sunshine returns a safe unsuccessful apply outcome
      And it does not enter the potentially blocking in-process fallback path

    Scenario: An ordinary uncancelled apply may use the supported local fallback
      Given a non-stream-start apply has no cancellation condition
      And the helper is unavailable outside the system-without-user-session case
      When both a session, a configuration, and an interactive display context are available
      Then the core display-settings result alone determines fallback success
      And after core success it observes a named target or requested virtual-display activation for up to the default 6 seconds when that evidence is relevant
      And activation observation, optional topology, and position failures after core success remain best effort and do not change the successful result

    Scenario: The local fallback does not promise helper-equivalent follow-up work
      Given an ordinary eligible apply reaches the local fallback
      When its core display-settings operation succeeds
      Then Sunshine may publish the successful session even if target activation verification or optional placement remains unconfirmed
      And physical refresh-restoration overrides are not consumed by this fallback path

    Scenario: A deadline-limited revert shares one caller deadline
      Given a stream-start or shutdown caller requests revert with a deadline
      When helper connection or command dispatch is delayed
      Then Sunshine stops waiting when the shared deadline expires
      And it does not leave the caller in an unlimited revert path

    Scenario: A prompt disarm uses only a live helper connection
      Given a stream-start path must quickly cancel very recent restore activity
      When no cached helper connection is immediately usable
      Then Sunshine reports that disarm was not delivered within the default 150-millisecond prompt budget
      And it does not spend the stream-start budget on connection setup or repeat the prompt intent inside the same default 150-millisecond throttle

    Scenario: A failed prompt disarm does not leave recent restore activity uncontrolled
      Given a restore has been pending for less than the default 5-second grace boundary and prompt disarm could not be delivered
      When intervention is needed to prevent uncontrolled restore activity
      Then Sunshine stops the stale helper rather than allowing uncontrolled restore activity to continue
      And it clears only the restore expectation associated with that intervention

    Scenario: An older restore is allowed to finish instead of being overwritten by prompt disarm
      Given restoration has progressed for the default 5-second grace boundary or longer
      When a later stream-start path attempts prompt disarm
      Then Sunshine does not cancel that unconfirmed restore through the prompt path
      And the next apply or confirmed recovery determines the desktop outcome

    Scenario: Snapshot Current is skipped while an unconfirmed restore is expected
      Given a live helper is expected to be restoring the desktop
      When Sunshine asks to snapshot the current display state
      Then the snapshot request returns unavailable
      And it does not capture a baseline from the potentially unrestored desktop

    Scenario: A known v2 Snapshot Current waits for its matching result
      Given the live helper is known to support v2 responses
      When Sunshine requests Snapshot Current without a caller deadline
      Then it waits for the matching snapshot outcome
      And it returns failure if that matching outcome does not arrive

    Scenario: A legacy or unknown Snapshot Current remains dispatch-only
      Given the live helper has legacy or unknown response compatibility
      When Sunshine requests Snapshot Current without a caller deadline
      Then it returns the dispatch outcome without inventing a correlated completion
      And a later apply retains its own baseline-capture safety path

    Scenario: A deadline-limited Snapshot Current cannot consume the whole stream-start budget
      Given Snapshot Current is part of a stream-start procedure with an explicit shared deadline
      When the helper is slow or unavailable
      Then Sunshine bounds snapshot connection, dispatch, and any v2 completion wait
      And it returns safely so later stream-start work can use the remaining budget

    Scenario: A missing deadline-limited snapshot result cannot contaminate a later request
      Given a deadline-limited v2 Snapshot Current was sent
      When its matching result does not arrive before its explicit wait ends
      Then Sunshine returns failure
      And it retires the stale connection unless the operation was superseded

    Scenario: Refresh-rate control is validated and correlated when supported
      Given a caller requests a nonzero refresh rate for a named display
      When the live helper supports correlated refresh results
      Then Sunshine waits for that refresh request's matching result
      And another display-control request can supersede the wait without receiving its result

    Scenario: Legacy refresh-rate control cannot borrow a v2 result
      Given the live helper uses legacy refresh-rate behavior
      When Sunshine requests a valid refresh-rate change
      Then it uses the compatible untagged result path
      And it does not accept a correlated result intended for another operation

    Scenario: Caller maintenance commands report helper availability without fabricating success
      Given Sunshine requests golden export, persistence reset, or disarm
      When the helper cannot be reached or the command cannot be dispatched
      Then the caller receives failure for that command
      And no command is reported successful merely because a prior helper session existed

    Scenario: Stop remains a callable compatibility operation
      Given a caller explicitly uses the public helper Stop operation
      When the live connection accepts that request
      Then the helper performs its defined clean shutdown behavior
      And ordinary Sunshine stream and watchdog lifecycle does not imply that it invokes Stop

    Scenario: Log-level synchronization is scoped to the live helper connection
      Given Sunshine has already synchronized the requested v2 log level to the live helper connection
      When the same log level is requested again
      Then Sunshine need not repeat the control dispatch for that live connection
      And a later connection replacement makes the setting eligible for synchronization again

  Rule: Deferred resolution work runs only for the intended live session

    Scenario: System-context resolution work is deferred only when a user session is unavailable
      Given an apply includes a session resolution request
      And Sunshine is running as the system account without an interactive user session
      When direct display work is unavailable
      Then Sunshine may defer that eligible resolution apply
      And it does not apply the resolution to an arbitrary system-context desktop

    Scenario: Ordinary unavailable display access defers only resolution work
      Given an apply has no session or no resolution change
      When system-without-user startup or ordinary local display access is unavailable
      Then Sunshine does not create deferred resolution work for that request
      And it returns the immediate caller outcome

    Scenario: Lock-screen helper-send failure may defer broader session intent
      Given a helper was reached for a session Apply
      And delivery fails while the lock screen is active
      When Sunshine preserves work for the interactive desktop
      Then it may defer the session request even when no resolution change is present
      And this lock-screen exception does not make sessionless work eligible

    Scenario: Deferred resolution work retains session intent without retaining a dead session
      Given an eligible resolution apply is deferred
      When the original session object later becomes unavailable
      Then the deferred work retains only the display-relevant session intent
      And it cannot dereference or publish a stale session as active

    Scenario: A lock-screen or temporarily unavailable display path may defer eligible resolution work
      Given an eligible session resolution apply cannot complete because the display path is temporarily unavailable
      When the system context lacks interactive display access or the temporary display condition makes that work deferrable
      Then Sunshine retains one pending resolution request for later readiness
      And a newer pending request replaces an older pending request for the same deferred path

    Scenario: Deferred work waits for an interactive session and a still-live stream
      Given a resolution apply is pending
      When an interactive user session becomes available
      Then Sunshine waits 2 seconds after readiness before its first attempt and retries at 500 milliseconds, doubling no higher than 10 seconds
      And it claims the request only while the associated stream is active or pending

    Scenario: Session teardown removes orphaned deferred work
      Given a resolution apply is pending for a stream that ends
      When session teardown or a superseding normal apply or revert occurs
      Then Sunshine removes the pending work
      And it does not later change the desktop for that ended or superseded session

    Scenario: Cancellation leaves deferred work safe for the owning lifecycle
      Given Sunshine has claimed a deferred resolution apply
      When its owning shutdown path cancels the attempt before a terminal outcome
      Then Sunshine does not publish a stale successful session
      And it preserves or clears the deferred request only according to current session ownership

    Scenario: Deferred resolution retries stop after their allowed attempts
      Given a live deferred resolution apply repeatedly fails after readiness
      When its sixth allowed attempt fails
      Then Sunshine removes the pending request
      And it does not retry indefinitely or resurrect the ended stream's display intent

    Scenario: A successful deferred apply is adopted by the correct active stream
      Given a deferred resolution apply succeeds while its stream is still live
      When Sunshine publishes the successful display session
      Then ongoing supervision is associated with that session's current identity
      And a stale cleanup from an earlier session cannot remove it

  Rule: Stream capture uses verification as a default-deadline soft gate, not an assumed success

    Scenario: A stream-start apply failure preserves the existing-display capture fallback
      Given a stream-start display request cannot be applied through the helper
      When stream startup continues after the unavailable display configuration
      Then Sunshine uses the existing display as the capture fallback
      And it does not report the unavailable display request as applied or verified

    Scenario: A verification ticket is invalidated by a newer display-changing request
      Given a prior apply supplied a verification ticket
      When a newer apply, revert, disarm, or reset changes display-control intent
      Then the earlier ticket cannot verify the newer session
      And any wait for that ticket returns unavailable rather than current-session success

    Scenario: A v2 verification result must match the ticket and live helper session
      Given a stream is waiting on a v2 verification ticket
      When a verification result arrives
      Then capture-gate status changes only if the result matches the ticket's request and live session
      And an unmatched or superseded result is not treated as verification

    Scenario: A legacy, unavailable, or timed-out verification result is explicit unknown
      Given an apply has no attributable v2 verification result
      When Sunshine evaluates its verification ticket
      Then it reports the verification status as unknown rather than verified
      And callers can apply their explicit default-deadline fallback policy

    Scenario: A verified stream-start ticket opens the capture gate
      Given a stream-start apply has a current v2 verification ticket
      When its matching verification succeeds within the stream-start budget
      Then the capture gate reports that capture may proceed
      And the result belongs only to that stream start

    Scenario: A failed stream-start verification is recorded without hard-blocking capture
      Given a stream-start apply has a current v2 verification ticket
      When its matching verification reports failure
      Then the capture gate reports a failed display target
      And the producer resolves the gate immediately so capture need not consume the remaining 8-second shared maximum
      And it does not treat that result as capture-ready

    Scenario Outline: An unknown stream-start verification may resolve before the shared maximum
      Given a stream-start apply has <unknown condition>
      When verification resolves as unknown
      Then the gate records that it gave up waiting rather than that the target was verified
      And the stream-start path proceeds as soon as the gate future resolves
      And only an outstanding attributable result may consume the remaining 8-second shared Apply-plus-verification budget

      Examples:
        | unknown condition                                      |
        | a legacy or otherwise unusable ticket that returns immediately |
        | a superseded request whose ownership check returns early        |
        | an unavailable result that may return before its deadline        |
        | a result still absent when the remaining deadline expires        |

    Scenario: A recent HDR apply delays capture only until HDR is active or its 3-second wait ends
      Given the most recent display apply requested HDR
      When asynchronous capture reinitialization or synchronous opening reaches its Windows stability wait
      Then Sunshine waits for the output to report HDR until 3 seconds after the successful apply
      And it starts capture with the observed SDR fallback if HDR does not become active in time

    Scenario: A recent non-HDR display apply waits for verification or the 1.5-second settle fallback
      Given display topology recently changed without a pending HDR request
      When asynchronous capture reinitialization or synchronous opening reaches its Windows stability wait
      Then Sunshine waits only until the apply is capture-stable or 1.5 seconds after the successful apply
      And it does not extend capture delay indefinitely

    Scenario: Initial asynchronous capture does not run the recent-Apply stability helper
      Given the asynchronous capture path is opening its display for the first time
      When display topology recently changed
      Then it proceeds through its own enumeration, virtual-readiness, open, and retry cycle without the HDR or 1.5-second recent-Apply stability wait
      And later asynchronous reinitialization remains eligible for that Windows-sensitive wait

    Scenario: Capture stability is proof only for the current eligible apply
      Given a display apply has been verified
      When Sunshine evaluates capture stability
      Then it accepts the stable indication only for the current apply request and eligible exclusive virtual SDR session
      And prior or unrelated verification cannot mark the new session stable

    Scenario: Ordinary WebRTC preparation treats unverified completion as nonfatal but not verified
      Given a WebRTC display apply was accepted
      When its default-deadline verification is failed, unknown, or unavailable
      Then Sunshine continues capability preparation
      And it does not report the display target as verified

    Scenario: HTTP capability probing does not turn an unverified display gate into a failed capability response
      Given HTTP capability probing has a virtual-display session with pending, failed, unknown, or timed-out verification
      When it reaches its supplied display-gate deadline
      Then it continues adapter-scoped encoder capability probing without further display-gate delay
      And it does not report a capability failure solely because the display was not verified

  Rule: Helper supervision cadence follows RTSP-session and launched-process evidence

    Scenario Outline: Helper supervision uses the observed active and suspended check cadences
      Given display-helper supervision has <supervision condition>
      When Sunshine schedules its next liveness check
      Then it uses the default <check cadence>
      And a failed check still follows the same current-session ownership and reconnection rules

      Examples:
        | supervision condition                                      | check cadence |
        | no counted RTSP session while a launched application identifier remains, including WebRTC-only activity | 20 seconds |
        | every other combination of counted RTSP sessions and launched-application state                       | 5 seconds  |

    Scenario: Starting supervision reuses an existing watchdog for a newer active stream
      Given helper supervision is already running
      When a newer stream becomes the active display session
      Then supervision adopts the newer stream identity
      And Sunshine does not start a duplicate watchdog

    Scenario: A watchdog liveness failure reconnects without reapplying an old display request
      Given helper supervision detects that its current helper connection is unusable
      When it restores helper communication successfully
      Then it accepts successful delivery of a liveness request as sufficient for ongoing supervision without waiting for its acknowledgement
      And it does not automatically replay a previous stream's display apply

    Scenario: Supervision pauses helper use while display control is disabled
      Given helper supervision is active
      When display control becomes disabled
      Then Sunshine releases the helper connection for that period
      And it does not keep restarting display control until the feature is enabled again

    Scenario: Ordinary watchdog stop preserves only the production active-or-pending predicates
      Given a prior stream asks to stop helper supervision
      When the ordinary stop is processed
      Then any nonzero shared running-session count or active-or-pending WebRTC session leaves supervision running
      And a pending RTSP launch with no running session is not included in that predicate
      And such a pending RTSP launch is protected only when the separate active-session generation mismatch identifies the stop as stale

    Scenario: A stale watchdog stop cannot clean up a newer display session
      Given supervision ownership belongs to a newer active stream
      When a stop request from an older stream arrives
      Then Sunshine ignores that stale stop for the newer session
      And it preserves the newer session's helper connection and display state

    Scenario: Forced watchdog stop completes shutdown or explicit restoration cleanup
      Given product shutdown or explicit user-requested restoration requires forced cleanup
      When Sunshine stops helper supervision
      Then it completes the applicable supervision cleanup even if a normal stream remains visible
      And it clears only session state that belongs to the forced cleanup scope

    Scenario: Watchdog shutdown cleanup is never abandoned
      Given helper supervision is stopping or being replaced
      When its prior cleanup cannot finish immediately
      Then Sunshine retains responsibility for completing that cleanup safely
      And a later lifecycle transition cannot expose stale supervision as a healthy new session

  Rule: Read-only integration queries fail safely

    Scenario: No successful apply is treated as no recent display change
      Given Sunshine has not completed a successful display apply
      When capture preparation asks how recently display configuration changed
      Then it treats the prior apply as not recent
      And it does not add a settling delay for a nonexistent successful apply

    Scenario: Device enumeration returns no device list on query failure
      Given Sunshine cannot read the available display devices
      When a caller requests device enumeration
      Then the integration returns no device list
      And its JSON-facing form returns an empty list rather than partial invalid data

    Scenario: Current topology capture returns unavailable on query failure
      Given Sunshine cannot read the active display topology
      When a caller requests the current topology
      Then the integration returns no topology
      And it does not fabricate a topology for a later display request

    Scenario: Frame-generation refresh support reports unknown when display evidence is unavailable
      Given a caller asks whether a display supports requested frame-generation refresh targets
      When the target cannot be resolved or its display evidence is unavailable
      Then Sunshine returns unavailable or an explicit unknown support result
      And it does not claim unsupported or supported status without applicable evidence

    Scenario: Frame-generation refresh support evaluates each resolved target independently
      Given Sunshine resolves a display and can inspect its refresh evidence
      When a caller requests several target refresh rates
      Then Sunshine reports supported, unsupported, or unknown for each target rate
      And the result identifies the resolved display used for that evaluation

  Rule: Helper selection, readiness, and restart use observed caller defaults

    Scenario Outline: Helper readiness uses the caller kind's default liveness envelope
      Given Sunshine needs a helper for a <caller kind> operation
      When no usable live helper has yet accepted its liveness delivery
      Then Sunshine permits readiness and liveness work for at most <default envelope>
      And cancellation or the caller's earlier deadline returns unavailable without later publishing success

      Examples:
        | caller kind              | default envelope |
        | ordinary apply or probe   | 5 seconds        |
        | helper startup readiness  | 5 seconds        |
        | shutdown or teardown      | 250 milliseconds |
        | stream-start fast disarm  | 150 milliseconds |

    Scenario: Helper readiness failure returns unavailable after the default 5-second envelope
      Given an eligible request needs a helper that cannot become live
      When 5 seconds of readiness work elapse without liveness success
      Then Sunshine returns helper unavailable
      And it does not publish a display session, verification ticket, or successful maintenance result

    Scenario: A restart removes unsafe stale ownership before reuse
      Given a helper is nonresponsive and no protected restoration must continue
      When Sunshine restarts display control for a new request
      Then the earlier helper identity and unresolved replies are retired before the replacement is used
      And a later result from the retired identity cannot publish or verify the new request

    Scenario: A hard restart preserves a helper that accepts the fast probe
      Given a caller requests a hard helper restart
      When the existing helper accepts a cache-only liveness frame within the default 100 milliseconds
      Then Sunshine reuses it instead of terminating it
      And no replacement launch is attempted

    Scenario: A hard restart bounds retirement before replacement
      Given a requested hard restart cannot deliver its default 100-millisecond cache-only liveness frame
      When Sunshine prepares replacement
      Then it retires the old connection, requests termination, observes exit for at most 2 seconds, and allows a default 100-millisecond resource-settle interval
      And cancellation or the caller deadline can end each wait without starting successor work

    Scenario: Prelaunch stale-helper cleanup is cancellation-bound and best effort
      Given Sunshine is about to launch a genuinely new helper
      When it clears known duplicate helper ownership
      Then cancellation or deadline expiry aborts the launch
      And inability to enumerate, open, terminate, or observe some stale instance is recorded but may proceed to launch and singleton resolution

    Scenario: A new helper receives one concrete launch policy
      Given Sunshine is permitted to start display control for the current user context
      When it launches a new helper
      Then it supplies the selected concrete engine and a log level clamped from zero through six and persists that engine choice for restore startup
      And system-context launch fallback is enabled only when no interactive user session is available

    Scenario: Hard-restart launch overlap has five extra attempts
      Given a hard-restart replacement launch initially overlaps old ownership
      When the initial launch attempt fails
      Then Sunshine permits at most five additional launch attempts at default 150-millisecond intervals
      And cancellation or deadline expiry ends the sequence as unavailable

    Scenario: New helper early exit is observed in six short checks
      Given a helper launch was accepted
      When Sunshine observes startup for six default 50-millisecond checks
      Then any ordinary early exit records a start failure and returns unavailable
      And only the established singleton-conflict exit is eligible for its special retry

    Scenario: Singleton-conflict startup receives one delayed retry
      Given a newly launched helper exits with the established singleton-conflict result
      When Sunshine applies the startup race workaround
      Then it waits the default 1-second cleanup interval, launches once more, and allows a default 300-millisecond settle interval
      And a failed retry launch, cancellation, or deadline expiry returns unavailable without another singleton retry

    Scenario: Successful launch still requires bounded control readiness
      Given a new helper survives early-exit observation
      When Sunshine completes the default 200-millisecond initialization interval
      Then it attempts accepted control liveness at a default 100-millisecond cadence for no more than 5 seconds
      And success clears the failure streak while timeout records one start failure

    Scenario: Forced stopping waits no longer than 2 seconds for observed exit
      Given Sunshine must force-stop an obsolete helper
      When the stop is issued
      Then Sunshine waits up to 2 seconds for its exit before completing local cleanup
      And a missed exit observation does not let its late reply own later work

  Rule: Direct callers preserve shared deadline and ownership rules

    Scenario Outline: Apply callers use the source-visible shared budget
      Given a <caller> begins a display apply and optional verification
      When it has no earlier explicit deadline
      Then acknowledgement and verification together share <default budget>
      And success after that budget cannot be published for that caller's session

      Examples:
        | caller                    | default budget |
        | RTSP stream start          | 8 seconds      |
        | RTSP stream resume         | 8 seconds      |
        | WebRTC preparation         | 15 seconds     |
        | ordinary non-stream apply  | 15 seconds     |

    Scenario: The RTSP capture consumer receives only one second beyond its producer budget
      Given RTSP stream start owns an 8-second apply-verification gate
      When capture waits for the gate
      Then capture waits at most 9 seconds from the capture consumer's own wait start rather than from gate creation
      And a late gate result is recorded as gave-up rather than as verified capture readiness

  Rule: Capability probing preserves stream ownership and adapter attribution

    Scenario: HTTP probing does not disrupt stream lifecycle ownership
      Given encoder capability evidence is not already cached for the selected adapter
      When HTTP probing finds stream lifecycle work active, stopping, or unavailable for exclusive use
      Then it returns the currently known capabilities marked incomplete
      And it does not create, replace, or remove a display session for the probe

    Scenario: Encoder capability evidence remains scoped to the observed capture adapter
      Given a display target suggests an adapter but capability probing observes another adapter
      When encoder capabilities are recorded
      Then only the observed adapter may own a new successful capability result
      And a pending or virtual-display hint cannot create a cross-adapter positive result

  Rule: RTSP display preparation preserves first-owner ordering and target precedence

    Scenario: First-session preparation snapshots before display discovery
      Given the first RTSP session has no earlier display owner
      And a very recent restore may still need prompt disarm
      When Sunshine begins display preparation
      Then it prompt-disarms the recent restore and attempts Snapshot Current before any display enumeration or output-existence check
      And only after that snapshot attempt may an application output override become active

    Scenario: First-session snapshot failure is nonfatal and deadline bounded
      Given Snapshot Current is attempted for the first RTSP session
      When snapshot completion fails or remains unavailable within the shared 8-second stream-start deadline
      Then Sunshine continues later display preparation with the remaining deadline
      And a concurrent later session does not replace the shared restore baseline

    Scenario: A virtual-selection application output becomes virtual intent
      Given an application output override names the product's virtual-display selection
      When Sunshine prepares the session target
      Then it treats the value as virtual intent instead of a literal physical output
      And it does not publish that selection text as the capture output

    Scenario: Frame-generation-required virtual display defeats a physical application target
      Given an application selects a physical output
      And effective frame-generation policy requires virtual display
      When Sunshine prepares the session target
      Then virtual intent wins and the application physical override is cleared for that session
      And frame-generation refresh policy is derived for the virtual target

    Scenario: RTSP application output suppresses only lower-priority virtual intent
      Given an application selects a physical output
      And frame generation does not require virtual display
      When RTSP resolves virtual intent
      Then the application target suppresses configured virtual mode only when the client did not explicitly request virtual
      And it does not overwrite an already explicit per-session virtual-mode choice

    Scenario: WebRTC application output has its narrower virtual exception
      Given a WebRTC application selects a physical output
      And frame generation does not require virtual display
      When WebRTC resolves virtual intent
      Then the application target suppresses other virtual intent without a separate client-explicit exception
      And frame-generation-required virtual display remains the higher-priority exception

    Scenario Outline: Configured physical output fallback distinguishes absence from inactivity
      Given no higher-priority virtual intent is active
      And the configured physical output is <output condition>
      When the first RTSP session prepares display selection
      Then Sunshine chooses <result>
      And an application-specific physical target is never silently substituted

      Examples:
        | output condition                                                 | result                                                |
        | known missing with no application target                         | virtual-display fallback                              |
        | known missing with an application target                         | the application target without automatic substitution |
        | present but inactive with no interactive helper session          | virtual-display fallback                              |
        | present but inactive with an available activation path           | helper activation of the configured output            |
        | uncertain because enumeration failed                             | the configured output without manufactured fallback   |

    Scenario: No physical display may trigger automatic virtual intent
      Given no prior target rule requested a virtual display
      And the selected backend reports automatic virtual enablement is eligible because no physical display is available
      When Sunshine completes RTSP display selection
      Then it requests a virtual display
      And the choice remains subject to normal creation success and identity publication rules

    Scenario: A newcomer cannot reconfigure another stream's display ownership
      Given another stream already owns shared display state
      When a new RTSP session prepares its target
      Then a nonvirtual newcomer joins the current capture target without display changes
      And a virtual newcomer may join only a stable-identity match on the configured capture adapter

    Scenario: Invalid shared virtual reuse is an explicit setup failure
      Given another stream owns display state
      And the newcomer requests virtual display but finds no stable-identity match or the existing virtual target does not match the configured capture adapter
      When Sunshine evaluates shared reuse
      Then it marks virtual setup failed and does not claim the target
      And it neither creates nor reconfigures a display for that newcomer

    Scenario: Resume reuses only a validated virtual target
      Given ordinary display changes are disabled for resume
      And the session requests virtual display
      When Sunshine finds an active stable-identity target on the configured capture adapter
      Then it republishes that target and marks it for a refresh Apply
      And an adapter mismatch rejects the target instead of claiming cross-adapter ownership

    Scenario: Resume may recreate a missing required virtual target
      Given ordinary display changes are disabled for resume
      And no validated reusable virtual target exists
      When the session still requires virtual display
      Then Sunshine may recreate it on demand and Apply the refreshed session intent
      And a physical resume override is merely republished without arbitrary display substitution

    Scenario: Nonexclusive virtual creation preserves only available precreation evidence
      Given RTSP is about to create a nonexclusive virtual display
      When Sunshine samples the current physical desktop
      Then it best-effort retains topology and each physical refresh, preserving rational rates and converting decimal rates to thousandths
      And missing topology or mode evidence omits only that restoration metadata while exclusive layout retains no topology snapshot

    Scenario: Recreated RTSP virtual display gets five bounded Apply attempts
      Given owned virtual-display recovery recreates an RTSP target
      When Sunshine rebuilds the session display intent
      Then it prompt-disarms before each of at most five Apply attempts with default waits of 250, 500, 750, 1000, and 1250 milliseconds after failures
      And cancellation stops the sequence while an owned recreated target requests capture reinitialization after success or exhaustion without fabricating Apply success

    Scenario: Recreated WebRTC virtual display uses the same bounded reapply contract
      Given owned virtual-display recovery recreates a WebRTC target
      When Sunshine rebuilds the session display intent
      Then it prompt-disarms before each of at most five Apply attempts with the same progressive default waits
      And capture reinitialization remains eligible after exhaustion while the failed Apply is not reported successful

    Scenario: WebRTC changes displays only when it owns an eligible start
      Given WebRTC is preparing capture
      When RTSP is active or the WebRTC request is resume-only
      Then ordinary display changes are suppressed
      And Apply occurs only for normal exclusive ownership, on-demand virtual recreation, or a required virtual refresh

    Scenario: WebRTC verification is a nonfatal 15-second soft gate
      Given an eligible WebRTC display Apply was accepted
      When verification does not confirm the target within the shared 15-second default budget
      Then WebRTC continues later capability preparation when otherwise possible
      And it records no verified display claim for the failed, unknown, unavailable, or cancelled result

  Rule: Virtual backend and adapter selection never silently cross policy boundaries

    Scenario: The selected virtual backend owns all virtual-display operations
      Given configuration selects one supported virtual-display backend
      When Sunshine creates, enumerates, monitors, or removes virtual displays
      Then those operations remain on the selected backend
      And failure does not silently fall through to the other backend

    Scenario: Configured render-adapter preference must resolve and be accepted exactly
      Given virtual-display creation has a configured adapter name and optional paired device identity
      When Sunshine selects the render adapter
      Then the exact preferred adapter must resolve and the selected backend must accept it
      And an unresolved or rejected preference fails without choosing an arbitrary adapter

    Scenario: Startup may initialize virtual support for a displayless host
      Given the selected backend reports no physical display and automatic enablement is eligible
      When process startup reaches display initialization
      Then Sunshine initializes virtual-display support
      And a shutdown request may abort before the next startup stage

    Scenario: Startup cleans an unowned active virtual display
      Given process startup observes an active virtual display
      And no RTSP or WebRTC session owns it
      When startup recovery runs
      Then Sunshine requests virtual-display cleanup using the configured restoration policy
      And a shutdown request may abort after that cleanup stage

  Rule: Deferred virtual output publication remains lease-owned and capture ordered

    Scenario: Every output publication has distinct rollback ownership
      Given current or deferred runtime output may already be published
      When a later publish or clear changes output ownership
      Then the ownership generation advances even when the later value names the same display
      And conditional rollback can clear only the exact current or deferred publication it owns

    Scenario: Unqualified output clear removes current and deferred choices
      Given current and deferred runtime output choices exist
      When Sunshine performs an unqualified clear
      Then both choices are removed
      And an older conditional rollback cannot remove a later publication

    Scenario Outline: Lock-screen virtual output chooses continuity before deferral
      Given a virtual runtime output is published while the lock screen is active
      And <physical condition>
      When Sunshine decides whether to publish immediately
      Then it <publication result>
      And ownership remains associated with the publication's lease

      Examples:
        | physical condition                         | publication result                                                     |
        | a usable active physical fallback exists  | defers the virtual output and clears the current runtime output         |
        | no usable active physical fallback exists | publishes the virtual output immediately to avoid losing capture        |

    Scenario: Unlock promotes only the latest deferred output
      Given a leased virtual output remains deferred through the lock screen
      When the interactive desktop becomes available
      Then Sunshine atomically promotes that still-current deferred output
      And superseded or cancelled ownership cannot publish or retarget capture

    Scenario: Deferred Apply precedes capture retarget after unlock
      Given unlock has promoted a deferred virtual output
      And display Apply work remains pending
      When Sunshine prepares capture retargeting
      Then it retries pending Apply at default 250-millisecond checks for at most 8 seconds before requesting capture reinitialization
      And timeout permits retarget with Apply still pending while cancellation suppresses retarget entirely

  Rule: Capture target choice preserves physical safety and bounded virtual readiness

    Scenario: Lock screen with an active physical display prevents virtual preference
      Given the lock screen is active and at least one physical display is active
      When capture chooses between physical and virtual targets
      Then it prefers physical capture
      And configured virtual mode or an enumerated virtual display does not override that safety choice

    Scenario: A present runtime output override is authoritative for virtual preference
      Given a runtime output override is present
      When capture evaluates target preference
      Then it prefers virtual only when the nonempty override identifies a virtual display
      And it does not consult lower-priority configured virtual preference for that decision

    Scenario Outline: Capture chooses virtual without a runtime override only from eligible evidence
      Given no runtime output override is present
      And installed virtual-display support and at least one enumerated virtual display are available
      When capture evaluates <evidence>
      Then virtual preference is <preference>
      And absent backend support or absent virtual devices always yields physical preference

      Examples:
        | evidence                                                                      | preference |
        | active output identifies virtual                                              | enabled    |
        | configured mode is per client or shared                                       | enabled    |
        | automatic virtual activation is enabled                                       | enabled    |
        | some virtual display is active and no physical display is active               | enabled    |
        | active physical and virtual displays coexist without another virtual trigger  | disabled   |

    Scenario: Virtual capture name prefers active logical identity
      Given virtual capture is preferred
      When Sunshine resolves the desired capture name
      Then it selects the first active virtual display with a logical name, otherwise the first enumerated virtual display with a logical name
      And a device without a logical capture name is not claimed as the desired capture target

    Scenario: Missing preferred virtual target has a bounded safe fallback
      Given virtual capture is preferred but its logical target is unresolved or absent from the capture list
      When Sunshine waits the default 3 seconds for that target to appear
      Then it uses the existing safe selection after the wait if the target remains unavailable
      And an empty capture list remains unavailable rather than claiming readiness

    Scenario: Initial asynchronous capture retries until success or capture shutdown
      Given asynchronous capture is opening its initial display
      When enumeration, preferred-virtual readiness, or the direct display-open attempt does not produce a usable display
      Then it re-enumerates, reevaluates preferred-virtual readiness, makes one direct open attempt when ready, and waits 50 milliseconds after a failed cycle
      And it repeats without a fixed attempt cap until a display opens or capture shutdown cancels the loop

    Scenario Outline: Each reinitialization or synchronous capture-open invocation uses a bounded progressive attempt group
      Given asynchronous reinitialization or synchronous opening invokes one display reopen <apply recency>
      When every attempt in that invocation fails
      Then it uses <attempt profile>
      And that invocation returns failure to its outer shutdown-owned reopen cycle without adding a cancellation outcome

      Examples:
        | apply recency                                  | attempt profile                                              |
        | no successful Apply within the preceding 5 seconds | 2 attempts with 200 and 300 millisecond waits             |
        | a successful Apply within the preceding 5 seconds   | 5 attempts with 300, 400, 500, 600, and 700 millisecond waits |

    Scenario: Reinitialization and synchronous opening repeat bounded attempt groups until success or shutdown
      Given asynchronous reinitialization or synchronous capture opening remains active
      When one bounded progressive attempt group returns without opening a display
      Then the enclosing reopen cycle refreshes display evidence and may invoke the same bounded group again
      And no fixed number of groups ends that cycle before a display opens or capture shutdown is observed between groups

    Scenario: Asynchronous capture reinitialization preserves teardown and retarget ordering
      Given active asynchronous capture must reinitialize its display
      When it crosses the reinitialization boundary
      Then every encoder device releases its shared capture surfaces before the shared surfaces are released
      And the active display is released before the recent-Apply stability wait and display re-enumeration
      And preferred-virtual readiness is checked before a pending explicit nonnegative display switch is consumed against the refreshed list
      And only then does the bounded progressive reopen profile run
      And capture shutdown is observed between outer reopen cycles
      But shutdown arriving during an invocation that succeeds may still allow the reopened display to be published before the next shutdown boundary

    Scenario Outline: Empty display reenumeration handles stale names by Apply recency
      Given capture reenumeration returns empty after a previously nonempty list
      When <condition>
      Then Sunshine <result>
      And it does not mix the two fallback lists

      Examples:
        | condition                                                            | result                                                        |
        | a successful Apply was within 5 seconds and mapped configured output is nonempty | replaces stale names with only that configured target |
        | no such recent Apply or no nonempty configured target                 | restores the old list and reports refresh failure             |

    Scenario: First empty display enumeration retains the configured candidate
      Given both the prior and reenumerated capture lists are empty
      When Sunshine refreshes capture targets
      Then it retains the mapped configured output as the sole candidate even when that name is empty
      And selection remains unverified until capture can open it

    Scenario: Refreshed capture selection has a strict fallback order
      Given capture has a refreshed nonempty display list
      When Sunshine chooses the current index case-insensitively
      Then it prefers a found nonempty runtime output override, then preserves the prior display, then prefers configured output if the prior disappeared, then uses the first result
      And a missing runtime override target falls through without pretending it matched another identity

  Rule: Authenticated display APIs preserve compatibility and failure distinctions

    Scenario Outline: Display enumeration detail follows query precedence
      Given an authenticated display-enumeration request has <query>
      When Sunshine selects enumeration detail
      Then it uses <detail>
      And presence of detail prevents the legacy full query from overriding it

      Examples:
        | query                                  | detail  |
        | no detail or full query                | minimal |
        | detail=full                            | full    |
        | detail=anything-else and full=true     | minimal |
        | no detail and full=1                   | full    |
        | no detail and full=true                | full    |
        | no detail and full=yes                 | full    |

    Scenario: Minimal and full enumeration expose different device sets
      Given an authenticated caller requests display devices
      When minimal or full detail is selected
      Then minimal detail omits devices without current mode information
      And full detail may retain inactive devices and extended display evidence

    Scenario: Enumeration failure and response failure remain distinct
      Given an authenticated caller requests display devices
      When underlying enumeration fails
      Then the successful JSON value is an empty list
      And an exception while parsing or producing the outer response instead returns status false with an enumeration error

    Scenario: Display enumeration always requires authentication
      Given a display-enumeration request is not authenticated
      When it reaches the display API
      Then authentication failure is returned before enumeration
      And neither minimal nor full display evidence is exposed

    Scenario: Frame-generation EDID lookup chooses the first nonempty hint alias
      Given an authenticated EDID refresh request may contain device_id, device, id, and display hints
      When Sunshine trims and resolves the first nonempty hint in that order
      Then a missing hint is a bad request
      And later aliases do not override an earlier nonempty hint

    Scenario: Frame-generation target parsing preserves defaults unless a valid custom value exists
      Given an authenticated EDID refresh request may contain a comma-separated targets value
      When Sunshine trims each item and its leading signed decimal conversion yields a positive in-range integer
      Then that converted value is accepted even if unconsumed suffix text remains, and at least one accepted value replaces the default ordered targets 120, 180, 240, and 288
      And empty, conversion-failing, overflowing, or nonpositive items are ignored while no accepted custom value leaves the defaults unchanged
      And compatibility examples remain:
        | token   | result       |
        | +120    | accept 120   |
        | 120.5   | accept 120   |
        | 120junk | accept 120   |
        | 0       | ignore       |
        | -120    | ignore       |
        | junk    | ignore       |

    Scenario: EDID lookup resolves identity and label predictably
      Given a nonempty EDID device hint is available
      When Sunshine matches it case-insensitively against device identity, logical display name, or friendly name
      Then an unknown display returns status false
      And a match reports its resolved identity with label preference friendly name, then logical name, then device identity

    Scenario Outline: Missing or structurally insufficient EDID remains unknown
      Given the resolved display has <EDID condition>
      When Sunshine evaluates the requested refresh targets
      Then every target has null support with method unknown
      And EDID presence is <presence>

      Examples:
        | EDID condition             | presence |
        | no readable EDID           | false    |
        | nonempty but too-short EDID | true    |

    Scenario: EDID refresh evidence is reduced to usable maxima
      Given the resolved display has readable EDID timing evidence
      When Sunshine evaluates it
      Then it derives the greatest valid vertical-range maximum and detailed-timing maximum, including valid extension detailed timings and doubled interlaced rates
      And nonfinite, nonpositive, or invalid-total evidence is ignored

    Scenario: Each requested EDID target is evaluated independently in order
      Given usable EDID maxima and an ordered target list are available
      When Sunshine compares each target with the default 0.5-hertz allowance
      Then vertical-range evidence has precedence, followed by detailed-timing evidence, and a reaching maximum reports true with that method
      And applicable evidence below the target reports false with that method while no usable maximum reports null with method unknown

    Scenario: EDID success reports optional maxima and ordered target results
      Given EDID evaluation resolves a display
      When Sunshine returns the authenticated response
      Then it reports status, resolved identity and label, EDID presence, and only the maxima that were derived
      And it preserves requested target order with each target's hertz, true, false, or null support, and method

    Scenario: EDID route exceptions are bad requests
      Given an authenticated EDID refresh request raises an unexpected parsing or evaluation exception
      When Sunshine forms the response
      Then it returns a bad request with the applicable error
      And it does not fabricate an unsupported result for the display

    Scenario Outline: Display maintenance POST routes require JSON and authentication
      Given a <operation> request lacks JSON content or authentication
      When it reaches the display API
      Then it is rejected before helper maintenance work
      And an accepted request reports a Boolean status for the operation

      Examples:
        | operation                    |
        | persistence reset            |
        | golden snapshot export       |

    Scenario: Golden export converts exceptions to false status
      Given an authenticated JSON golden-export request reaches display maintenance
      When export raises an exception
      Then the response status is false
      And no previous export result is reused

  Rule: Golden snapshot status remains diagnostic, ordered, and nonintrusive

    Scenario: Golden status uses the first existing candidate snapshot
      Given an authenticated status request can inspect ordered active-user, current-process, and common data locations
      When more than one candidate snapshot exists
      Then it reports the first existing candidate in that order
      And later candidates do not merge into that snapshot's status

    Scenario Outline: Golden schema and layout determine upgrade status
      Given the first existing golden snapshot has <snapshot condition>
      When Sunshine reports golden status against latest schema version 2
      Then snapshot version is <version result>
      And layout and upgrade status are <upgrade result>

      Examples:
        | snapshot condition                                                   | version result | upgrade result                                      |
        | version integer 2 or later and a nonempty layout entry that is an integer or has integer or string rotation | that version | layout present and no schema upgrade required |
        | missing, noninteger, or less-than-one version                         | null           | schema upgrade required                             |
        | version older than 2                                                  | that version   | schema upgrade required                             |
        | no qualifying layout entry                                            | parsed version | schema upgrade required                             |
        | unreadable object                                                     | null           | out of date for unreadable_snapshot                 |

    Scenario: Current comparison is explicit and skipped during streaming
      Given an authenticated golden-status request has compare_current
      When its value is not 1, true, or yes case-insensitively, or any stream lifecycle is active
      Then current comparison is not attempted
      And ordinary status still reports snapshot and restore-health fields

    Scenario: Current comparison requires a usable physical desktop
      Given compare_current is explicitly enabled and no stream is active
      When an active virtual display exists or current physical enumeration is unavailable or yields no usable physical displays
      Then comparison is unavailable without a mismatch reason
      And it does not compare stale or virtual topology with the golden snapshot

    Scenario: Current comparison normalizes only eligible physical identities
      Given compare_current can inspect the current physical desktop
      When Sunshine forms the current set
      Then it excludes virtual displays and configured excluded identities and compares trimmed case-insensitive physical identities
      And only active displays with logical names contribute current mode, HDR, origin, and primary evidence

    Scenario Outline: Golden comparison returns the first mismatch category
      Given all earlier comparison categories match
      When <difference> is the first applicable difference
      Then current mismatch reason is <reason>
      And no later mismatch category replaces it

      Examples:
        | difference                                              | reason                |
        | snapshot has no usable topology or mode identity set    | invalid_snapshot      |
        | physical identity sets differ                           | display_set_changed   |
        | width, height, or refresh differs                       | display_mode_changed  |
        | comparable HDR state differs                            | hdr_changed           |
        | comparable nonempty primary identity differs            | primary_changed       |
        | comparable display origin differs                       | layout_changed        |

    Scenario: Golden refresh comparison uses relative tolerance
      Given snapshot and current refresh values are finite
      When their absolute difference is no more than one ten-thousandth of the greatest of one hertz and the two absolute refresh values
      Then refresh is considered matching
      And a greater difference produces display_mode_changed when earlier categories match

    Scenario: Empty mismatch means an available matching comparison
      Given explicit current comparison has usable snapshot and physical evidence
      When every ordered category matches
      Then comparison is available with an empty mismatch reason
      And the result does not create an out-of-date reason by itself

    Scenario: Restore health can mark a snapshot out of date
      Given the selected snapshot has restore-health status
      When status explicitly marks it out of date or unresolved attempts reach the nonzero threshold and the nonzero failure window has elapsed since first failure
      Then golden status reports out of date
      And schema upgrade reason wins when already present, otherwise the reason is restore_failed_for_days

    Scenario: Missing restore health has stable empty fields
      Given no readable restore-health status accompanies the selected snapshot
      When Sunshine reports golden status
      Then counts and thresholds are zero, reasons are empty, and timestamps are null
      And missing health does not itself mark the snapshot out of date

    Scenario: Golden deletion is best effort across all candidates
      Given an authenticated golden-delete request can inspect all snapshot and status candidates
      When Sunshine attempts deletion
      Then it swallows individual deletion errors and continues through every candidate
      And deleted is true only if at least one snapshot was removed, not when only a status file was removed

  Rule: Final stream and application ownership determine restoration and cleanup

    Scenario: The final runtime owner clears transient target work first
      Given the last RTSP or WebRTC runtime owner ends
      When Sunshine finalizes shared display ownership
      Then it clears the runtime output, pending Apply work, and deferred stream-start actions before choosing restore or removal
      And an older deferred action cannot retarget capture after finalization

    Scenario: WebRTC capture teardown latches forced restoration until shared runtime is idle
      Given WebRTC capture teardown contributes finalization while another shared runtime owner may remain
      When it records forced display restoration for the next idle boundary
      Then that request remains latched until shared runtime finally has no owner
      And a later RTSP final owner may therefore perform the forced Revert even though WebRTC ended earlier
      And successful shared-runtime finalization clears the latched request for the next ownership cycle

    Scenario: The cancel route preserves its exact teardown order and deferred-launch exception
      Given an authenticated cancel request samples whether an application is running, launch is deferred, and no RTSP session is counted
      When all three conditions hold
      Then it synchronously terminates RTSP sessions while preserving the deferred launch
      And it skips application termination and skips idle virtual-display cleanup
      But otherwise it synchronously terminates RTSP sessions first, terminates a sampled running application next, and then requests lifecycle-serialized virtual cleanup only if runtime is idle

    Scenario: Preapplied application replacement skips the old revert
      Given a replacement session already applied its display configuration before the old application terminates
      When old application teardown reaches display restoration
      Then it clears any deferred application revert and skips the old application's Revert
      And the replacement display ownership remains intact

    Scenario: Application termination without another owner removes before revert
      Given an application used a virtual display and no other shared stream owner remains
      When the application terminates
      Then Sunshine removes virtual displays without restoration first and then requests Revert when applicable
      And a successful Revert with no RTSP session remaining permits ordinary helper-supervision stop

    Scenario: Application termination with another owner defers restoration
      Given an application ends while another shared stream owner remains
      When teardown reaches virtual removal and display restoration
      Then it neither removes the owned virtual display nor requests immediate Revert
      And it records deferred restoration for the final stream to consume only after the application is no longer paused

    Scenario: Application termination restores global runtime configuration safely
      Given an application with runtime overrides terminates
      When Sunshine clears those overrides
      Then it applies global configuration immediately if no other stream owner remains
      And otherwise it marks configuration for deferred reload

    Scenario Outline: A paused final stream follows configured virtual cleanup policy
      Given the final stream ends while its application remains paused
      And revert on disconnect is disabled
      When paused virtual-display timeout is <timeout>
      Then Sunshine <result>
      And a delayed removal does not restore physical display configuration

      Examples:
        | timeout             | result                                                        |
        | zero                | keeps the virtual display alive for resume                     |
        | a positive duration | schedules removal after that duration without restoration       |

    Scenario: A newer lifecycle owner cancels delayed paused cleanup
      Given paused virtual-display removal is waiting for its configured delay
      When a newer cleanup generation, shared runtime owner, or ended application is observed
      Then the delayed removal does nothing
      And it cannot remove a successor session's virtual display

    Scenario: Requested final restore keeps the virtual display alive
      Given the final stream requires restore because revert-on-disconnect, deferred application revert, or forced idle cleanup is active
      When Sunshine dispatches Revert
      Then it keeps the virtual display alive while restoration is requested
      And failed dispatch also leaves that display available rather than forcing remove-before-restore

    Scenario: Nonpaused final ownership without restore removes only virtual displays
      Given the final stream is not paused and no restore condition applies
      When Sunshine finalizes display ownership
      Then it removes the owned virtual display without restoring the physical database
      And it later completes ordinary shared platform shutdown

    Scenario: Stream platform start begins helper supervision
      Given shared streaming platform ownership begins
      When platform stream-start actions run
      Then helper supervision starts for the active display lifecycle
      And duplicate platform ownership does not require duplicate supervision

    Scenario: Stream platform stop preserves paused supervision
      Given shared streaming platform ownership ends
      When the application remains paused for resume
      Then helper supervision remains active to preserve helper heartbeat behavior
      And an unpaused application permits ordinary supervision stop

  Rule: Virtual cleanup preserves requested order and honest result semantics

    Scenario: Every virtual cleanup disables virtual watchdog feeding first
      Given a caller begins virtual-display cleanup
      When cleanup reserves its lifecycle scope
      Then virtual watchdog feeding is disabled before restoration or removal
      And cleanup remains observable as in progress for the whole reservation

    Scenario Outline: Cleanup follows the caller's restore order
      Given display restoration is enforced with <order>
      When virtual cleanup runs
      Then it <sequence>
      And failure in one stage does not silently reverse the requested order

      Examples:
        | order                 | sequence                                                |
        | restore before remove | dispatches helper Revert before virtual removal          |
        | remove before restore | removes virtual displays before dispatching helper Revert |

    Scenario: Restoration waits only for relevant virtual teardown settlement
      Given an active virtual display existed and restoration still needs removal settlement
      When removal has been requested
      Then Sunshine waits up to the default 5 seconds for virtual displays to disappear before restoration fallback
      And timeout is nonfatal and does not suppress the restoration attempt

    Scenario: Helper-unavailable cleanup falls back to direct database restoration
      Given cleanup enforces display restoration
      When helper Revert cannot be dispatched after the requested ordering stage
      Then Sunshine attempts direct Windows display-database restoration after removal
      And that fallback's Boolean result becomes the database-restoration result

    Scenario: Dispatched Revert is not verified restoration proof
      Given cleanup successfully dispatches helper Revert
      When it reports cleanup results
      Then helper-revert-dispatched and the broad database-restoration-applied flag are true
      And those flags mean accepted dispatch rather than confirmed final desktop restoration

    Scenario: Virtual removal success combines specific and tracked removal
      Given cleanup may name one virtual-display identity and always removes all tracked virtual displays
      When it reports virtual-displays-removed
      Then success requires both operations to report success
      And an absent or all-zero specific identity counts as a successful no-op for the specific removal

    Scenario: Removal-only cleanup never restores the display database
      Given cleanup is invoked with restoration enforcement disabled
      When virtual removal finishes
      Then it does not dispatch Revert or use direct database restoration
      And the combined virtual removal result remains independently visible

    Scenario: Restore hotkey uses restore-before-remove and forced supervision cleanup
      Given the configured global restore hotkey is triggered
      When Sunshine performs emergency display cleanup
      Then it prefers golden fallback, requests restore before virtual removal, and always force-stops helper supervision afterward
      And helper unavailability or virtual removal failure does not leave forced supervision running

    Scenario: Process shutdown joins recovery before global display teardown
      Given process shutdown begins while deferred virtual output or virtual recovery may still own work
      When Sunshine performs display shutdown
      Then it stops and finishes deferred-output work and virtual recovery before force-stopping helper supervision
      And only afterward does it close virtual-display support, preventing late work from using torn-down global state

  Rule: Platform wrappers expose helper availability without changing ordinary fallback policy

    Scenario Outline: Cross-platform helper wrapper results remain explicit
      Given a caller invokes a display-helper operation on <platform>
      When helper-specific work is evaluated
      Then it returns <helper outcome>
      And the caller <caller consequence>

      Examples:
        | platform    | helper outcome                                  | caller consequence                                              |
        | Windows     | the live helper or an explicitly eligible local fallback result | does not add another implicit in-process fallback        |
        | non-Windows | unavailable, false, or empty enumeration as appropriate         | may continue its ordinary platform display behavior      |
