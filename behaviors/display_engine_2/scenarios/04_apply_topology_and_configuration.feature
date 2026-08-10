@display-engine-v2
Feature: Display engine v2 applies and verifies display topology and configuration
  A display change is successful only when its intended usable display state has
  been confirmed; accepting a request alone never proves that Windows applied it.
  Durations in this feature are current suggested defaults and may be tuned, but
  the stated ordering, attempt ceilings, target ownership, and terminal outcomes are fixed.

  Rule: A configuration-bearing Apply may stage topology or leave it unchanged

    Scenario: A configuration-only request derives and applies the required topology
      Given the current desktop has a valid topology
      And an Apply request specifies a display configuration without an explicit topology
      When the engine applies the request
      Then it derives the topology needed for that configuration from the current desktop
      And it applies and verifies the requested configuration against its intended target

    Scenario: A standalone topology-only request is rejected before display mutation
      Given an Apply request supplies an explicit topology but no display configuration
      When the engine validates the request
      Then it rejects the request as invalid
      And it does not activate the supplied topology or change display settings

    Scenario: An explicit topology is optional staging for a configuration request
      Given an Apply request specifies both a valid topology and a target display configuration
      When the engine applies the request
      Then it makes the topology usable before applying mode, HDR, primary-display, or position settings
      And it uses the explicit topology as staging for that configuration rather than as a standalone success result
      And it verifies the requested configuration against the resolved target that Windows accepted
      And unrelated topology adjustments do not fail the request when the intended target remains usable

    Scenario: An already satisfied request is still verified without needless topology activation
      Given the requested topology is already active and its target is usable
      And the requested configuration is already active
      When the engine applies the request
      Then it does not activate the same topology again solely because it was requested
      And it reports success only after the existing configuration is confirmed stable

    Scenario: Missing pre-Apply desktop evidence stops before mutation
      Given an Apply request contains a display configuration
      When the current topology is structurally unavailable or the original session settings cannot be retained
      Then the request reports a retryable non-success before topology or settings mutation
      And failure to derive a valid topology for the requested configuration instead reports an invalid request
      And neither outcome may be reported as a verified Apply

    Scenario: A request without the required configuration is rejected before display mutation
      Given an Apply request has no display configuration
      When the engine validates the request
      Then it rejects the request as invalid
      And it does not activate a topology or change display settings

  Rule: Topology activation accepts only a usable result for the intended target

    Scenario: A structurally invalid topology is rejected without activation
      Given an Apply request contains a topology with invalid structure
      When the engine validates the topology
      Then it rejects the request as invalid
      And it does not attempt to activate that topology

    Scenario: An OS-invalid topology is retried only after display-stack recovery
      Given an Apply request contains a structurally valid topology
      But the operating system initially rejects that topology during validation
      When the engine applies the request
      Then an operating-system general-failure result receives one compatibility validation attempt using the alternate supplied-topology flag form
      And other validation errors do not receive that alternate flag-form attempt
      Then it protects the desktop before recovery can affect it
      And it performs one display-stack recovery and the default 500 ms settle before retrying validation
      And failure of the recovery action itself does not skip the settled revalidation
      And it reports a retryable non-success result if validation is still rejected
      And a changed settle duration may not add another validation retry, start a later mutation after cancellation, or weaken the recovery guard

    Scenario: A topology activation is retried only within its two-attempt recovery policy
      Given an Apply request contains a topology that passed validation
      And its first activation does not become usable
      When the engine applies the request
      Then it makes at most 2 activation attempts
      And after each attempt it allows the default 5 seconds for the accepted target or duplicate group to become usable
      And it performs display-stack recovery and the default 500 ms settle only between those attempts
      And it reports a retryable non-success result when the second attempt has not produced a usable accepted target
      And the cadence of intermediate readiness observations is not a contract
      And a tuned readiness duration may not add an activation attempt, accept a missing target, or delay cancellation past the next safe boundary

    Scenario: Exact readiness can confirm a transient activation result
      Given a structurally and operating-system-valid topology is being activated
      And the topology application returns false rather than succeeding or throwing an exception
      But the exact requested topology and intended target group become usable within the current attempt
      When the engine evaluates readiness
      Then it accepts that exact ready topology for the settings stage
      But an exception produces fatal failure and ends the attempt without adopting later readiness

    Scenario: A successful Windows-adjusted topology is accepted when the intended target survives
      Given an Apply request has a target display or duplicate group
      And Windows reports successful activation of a usable topology that differs from the requested topology
      And the intended target remains active and enumerable in that adjusted topology
      When the engine evaluates the activation result
      Then it accepts the adjusted topology
      And it applies settings only to the intended target

    Scenario: A Windows-adjusted topology is rejected when the intended target is lost
      Given an Apply request has a target display or duplicate group
      And Windows reports successful activation of a usable topology that differs from the requested topology
      And the intended target is no longer active or enumerable in that adjusted topology
      When the engine evaluates the activation result
      Then it treats the activation as unconfirmed and does not apply settings to an arbitrary replacement display

    Scenario: A non-successful activation does not adopt an adjusted topology
      Given an Apply request has a target display or duplicate group
      And Windows reports a non-successful activation result
      But the visible topology differs from the requested topology and still contains the intended target
      When the engine evaluates the activation result
      Then it does not accept that adjusted topology as the basis for configuration
      And it follows the applicable retry or recovery result instead

    Scenario: A topology mutation is protected before either activation or recovery can change the desktop
      Given a topology validation retry, activation, display-stack recovery, or settings change could alter the desktop
      When the engine reaches the first such mutation boundary
      Then it attempts to arm durable recovery and records whether that succeeded before starting that operation
      And only a successful attempt counts as durable protection
      And cancellation before that boundary starts no mutation and may clear provisional recovery state
      But cancellation after that boundary retains recovery because the desktop may have changed
      And failure to arm does not certify the desktop as safe, restored, or safely disarmed and may be retried after the mutation reports its outcome

  Rule: Target readiness is distinct from unrelated display readiness

    Scenario: A configuration Apply waits for its intended target group, not every unrelated path
      Given Windows has accepted a topology for a configuration Apply
      And the intended target or duplicate group is active and queryable
      But an unrelated display path is still waking, sleeping, or otherwise not queryable
      When the engine evaluates topology readiness
      Then it may continue with the requested configuration for the intended target group
      And it does not reject or retarget the request solely because the unrelated path is not ready

    Scenario: An active target that cannot be enumerated is not ready for configuration
      Given Windows reports the requested target in the active topology
      But the target or its required duplicate-group member cannot yet be enumerated with usable display information
      When the engine evaluates topology readiness
      Then it waits only within the default 5-second readiness allowance for that activation attempt
      And it does not apply configuration settings until the target is usable

  Rule: Target identity remains stable when Windows reorders display paths

    Scenario: An explicit device identifier remains the configuration target
      Given an Apply request names a specific device identifier
      And Windows reorders unrelated display paths while applying the topology
      When the engine applies and verifies the configuration
      Then the named device remains the target for mode, HDR, and primary-display checks
      And a different display is never selected merely because it appears first after reordering

    Scenario: An empty device identifier selects the first accepted group containing an original-primary candidate
      Given planning identifies one or more candidate identities from the original primary display group
      And an Apply request omits its device identifier
      When Windows accepts the requested or adjusted topology
      Then the engine resolves the target as the first accepted topology group containing any planned candidate
      And accepted group order may determine the result if Windows splits those candidates across adjusted groups
      And it checks required mode, refresh, HDR, and primary status against the selected accepted group

  Rule: Topology staging preserves settings intent without repeating device activation

    Scenario: Accepted topology is rebased before requested settings are applied
      Given topology activation has produced a usable accepted topology
      And the session retained its original mode, HDR, and primary-display restoration values
      When the engine begins the requested settings stage
      Then it bases that stage on the topology Windows actually accepted while preserving those original restoration values
      And an EnsurePrimary preparation request remains EnsurePrimary
      But VerifyOnly, EnsureActive, and EnsureOnlyDisplay preparation requests all become VerifyOnly for this stage because topology activation is complete
      And only a successful requested-settings application permits position overrides followed by physical refresh overrides
      And sticky verification after that best-effort work is still required for final core Apply success

    Scenario Outline: Settings-stage failures retain their caller-visible status
      Given topology activation has succeeded for an Apply
      When the requested settings stage encounters <condition>
      Then it reports <status>
      And it does not convert that condition into verified success

      Examples:
        | condition                                                         | status                    |
        | an unusable accepted topology or settings baseline                | verification failure      |
        | failure to retain the rebased restoration values                  | retryable non-success     |
        | a temporarily unavailable display operation                       | retryable non-success     |
        | a device, primary, mode, or HDR preparation failure               | verification failure      |
        | an unexpected settings failure                                    | fatal non-success         |

  Rule: Failed or cancelled mutations preserve a path back to a safe desktop

    Scenario: Consecutive Applies retain the original session presentation baseline
      Given the first configuration Apply of a session starts from a valid desktop
      When later Applies use topologies accepted during that same session
      Then the engine retains the session's original mode, HDR, and primary-display restoration values
      And it derives later configuration work from the topology Windows has most recently accepted
      And a failed settings stage does not replace the original session baseline with partial display state

    Scenario: A failed settings stage rolls back the captured baseline when possible
      Given the engine captured a valid pre-Apply baseline before display mutation
      And topology activation succeeded
      But applying the requested mode, HDR, or primary-display settings fails
      When the engine handles the failed settings stage
      Then it first attempts to restore the captured baseline topology
      And only after that topology is usable does it restore the baseline modes, HDR, primary display, and origins
      And it confirms the captured snapshot after those rollback stages
      And it treats the desktop as no longer changed only after that baseline is confirmed restored

    Scenario: Apply retries only retryable transaction outcomes
      Given an Apply request reaches a retryable topology, settings, or verification outcome
      When the engine schedules another Apply
      Then it permits at most 3 total attempts
      And it waits 500 ms before attempt 2 and 1,000 ms before attempt 3
      And it reports the last topology, settings, or verification non-success after the third attempt
      And invalid-request, fatal, or cancellation outcomes do not receive another Apply retry
      And a confirmed rollback does not turn the failed attempt into success and does not remove retry eligibility from a retryable status
      And changing a retry wait may not alter retry eligibility, the three-attempt cap, rollback/recovery ownership, or the terminal status

    Scenario: A later topology or settings failure retains the correct recovery result
      Given an Apply attempt has crossed a mutation boundary
      When a later topology stage, settings stage, or verification fails
      Then a confirmed baseline rollback may report that the desktop no longer needs recovery
      But an absent, failed, cancelled, or unconfirmed rollback retains recovery protection
      And a retryable transaction failure remains retryable only while its retry budget remains
      And no later retry, rollback, or cancellation may report a verified Apply for that failed attempt

    Scenario: An unconfirmed rollback retains desktop recovery protection
      Given a requested settings stage failed after topology activation
      And the engine attempted to restore the captured baseline
      And that rollback cannot be confirmed
      When the failed Apply reaches its terminal outcome
      Then it retains recovery protection and reports a non-success outcome

    Scenario: Cancellation before a mutation leaves the desktop and recovery state unmodified
      Given an Apply request is waiting before any topology or settings mutation
      When the request is cancelled or superseded
      Then the engine does not start another display-changing stage for that request
      And it does not claim that the desktop changed

    Scenario: Cancellation after possible mutation retains recovery protection
      Given an Apply request has entered topology activation, display-stack recovery, or settings application
      When the request is cancelled or superseded before its result is confirmed
      Then the engine stops later cancellable stages for that request
      And it treats the desktop as possibly changed
      And recovery remains armed until a safe terminal outcome is confirmed
      And any successfully created or already existing durable safeguard remains available for that recovery

  Rule: Apply restores requested presentation details within safe limits

    Scenario: HDR blanking is a post-verification workaround, not Apply success
      Given an Apply request specifies an HDR state for its resolved target
      And that request explicitly asks for HDR blanking
      When the Apply is verified successful for the current, uncancelled session
      Then it requests the serialized post-verification workaround with a default 1-second blank duration over the active displays observed HDR-enabled
      And discovery or failures inside a successfully launched workaround do not alter the verified Apply result
      But failure to launch the workaround execution is not caught at this boundary and has no unchanged-result guarantee
      And it does not blank HDR when the request omitted the option, Apply failed, verification is absent, or the completion is stale or cancelled
      And a tuned blank duration may not run before verification or change target, cancellation, recovery, or Apply-result ownership

    Scenario: Position overrides are constrained to a usable desktop coordinate range
      Given an Apply request contains a position override for an active display with known dimensions
      And the requested origin would place part of the display outside the usable coordinate range
      When the engine applies the position override
      Then it constrains the origin so the whole display fits within the supported range
      And it clamps each x and y origin to [-32768, 32767 - dimension + 1] for that display dimension
      And it makes at most 15 position attempts at the default 200 ms retry interval, for an approximately 3-second settling allowance
      And changing that interval may not turn positioning into core Apply verification or postpone cancellation

    Scenario: Empty or unrepositionable display identities do not redirect position work
      Given an Apply request contains a position override for an empty or nonempty display identity
      When the engine applies position overrides
      Then an empty identity is ignored without a position attempt
      And a nonempty identity that is missing, inactive, or temporarily unrepositionable remains bound to that same identity for the 15-attempt allowance
      And it does not substitute another display identity for either case
      And cancellation stops remaining position attempts
      And any unresolved nonempty override remains a best-effort position outcome after its 15-attempt allowance

    Scenario: A position override with unknown dimensions retains the ordinary coordinate ceiling
      Given an Apply request contains a position override for a display whose dimensions cannot currently be read
      When the engine applies the position override
      Then it still constrains each origin to the supported coordinate range
      And it uses the ordinary upper coordinate ceiling of 32767 rather than inventing a display size
      And a failed or unavailable position operation remains best effort for that named display

    Scenario: Position overrides are best effort after the core configuration change
      Given the requested topology and target configuration are otherwise valid
      And one active monitor position override cannot be applied in its 15-attempt settling allowance
      When the engine completes the Apply
      Then it preserves the unresolved position override as a best-effort failure
      And it does not silently retarget another display to satisfy that override
      And the core Apply outcome remains governed by topology and requested-configuration verification

    Scenario: Refresh-rate overrides restore physical displays without changing the virtual target
      Given an Apply request includes valid refresh-rate overrides and its requested mode, HDR, and primary-display changes have succeeded
      And one override identifies the configured virtual display and another identifies a physical display
      When the requested settings application has succeeded and the engine performs post-configuration work before initial sticky verification
      Then it skips the virtual display override
      And it recognizes the configured virtual display only by exact case-sensitive device-identity equality
      And a differently cased identity is attempted as an ordinary best-effort physical refresh override
      And it attempts to restore the requested physical display refresh rate when that display is available
      And an already equivalent rational refresh rate succeeds without another display change
      And it ignores empty device identifiers or invalid zero-valued rates
      And cancellation is observed before each override and stops that and all remaining refresh work
      And it performs this best-effort restoration after the successful core configuration even when no virtual display was created
      And virtual-display creation is a specific reason for the restoration because it can reset physical refresh rates

    Scenario: Physical refresh-rate overrides remain best effort after a successful core configuration
      Given an Apply request includes valid physical-display refresh-rate overrides
      And one physical display cannot accept its requested override
      When the engine applies the overrides after successful requested settings and before initial sticky verification
      Then it leaves that physical display's override unapplied without changing the virtual target
      And it does not convert an otherwise verified core configuration into a different Apply result solely because that separate override failed

    Scenario: Core verification uses two sticky observations
      Given requested settings have applied and all best-effort position and physical-refresh work has reached its boundary
      When the engine performs initial verification
      Then it waits the default 250 ms settle before the first required-state observation
      And the resolved target must remain active and every requested mode, refresh, HDR, and primary condition must match at that observation
      And it waits another default 250 ms before repeating the same required-state observation
      And success requires both observations for the current uncancelled request
      And tuning either settle may not remove the second confirmation or accept a stale or cancelled request

    Scenario: Refresh and configuration verification require the requested active state
      Given an Apply request requires a resolution, refresh rate, HDR state, or primary status
      And every required property matches on the resolved active target
      When the engine verifies the completed display change
      Then it verifies the resolved target remains active
      And it verifies every requested resolution, refresh, HDR, and primary-display property in that target scope

    Scenario: A repeated configuration mismatch fails verification
      Given an Apply request requires a resolution, refresh rate, HDR state, or primary status
      And a required property does not match on repeated verification
      When the engine verifies the completed display change
      Then it reports verification failure rather than treating the accepted request as a successful display change

    Scenario: Normal Apply verifies only its accepted target and requested settings
      Given a normal Apply has used topology as staging and Windows has adjusted unrelated paths
      When the engine verifies the transaction
      Then it checks that the resolved target remains active
      And it checks only the requested resolution, refresh, HDR, and primary-display settings in that target scope
      And it does not require unrelated paths to reproduce the request's staging topology exactly
      And it does not apply or verify layout or rotation as part of Apply

  Rule: Resolved-target verification preserves duplicate-display semantics

    Scenario: An explicit target in an accepted duplicate group has mixed verification scope
      Given an Apply names an explicit device that belongs to an accepted duplicate group
      And it requests a resolution, refresh, HDR state, or primary-display status
      When the engine verifies the successful Apply
      Then the requested resolution must match across every member of that accepted duplicate group
      And requested refresh and HDR state are checked against the explicit target representative
      And requested primary-display status is checked against that explicit representative
      And a matching duplicate-group member does not substitute for a failed representative refresh, HDR, or primary check

    Scenario: An omitted target verifies the first accepted group containing an original-primary candidate
      Given an Apply omits its device identity and planning retained candidate identities from the original primary group
      And it requests a resolution, refresh, HDR state, or primary-display status
      When the engine verifies the successful Apply
      Then it selects the first accepted topology group containing any planned candidate
      And requested resolution, refresh, and HDR state are checked across that selected group
      And requested primary-display status succeeds when any member of that selected group is primary
      And if Windows split planned candidates across groups, accepted group ordering may select a different candidate-containing group

    Scenario: HDR-disabled verification accepts an unavailable HDR observation
      Given a verified Apply requests HDR disabled for its resolved verification scope
      When HDR state is absent or unavailable for a checked display
      Then the engine accepts that observation as HDR disabled
      But an Apply that requests HDR enabled requires an observed enabled HDR state for every checked display

    Scenario: Refresh verification applies its fixed tolerance to rational rates
      Given a verified Apply requests a refresh rate for its resolved verification scope
      When requested and observed rates are rational values with positive denominators
      Then the engine accepts exact equivalents and any values whose numerical difference is no greater than the fixed 0.9 Hz compatibility tolerance
      And a zero denominator cannot satisfy refresh verification

    Scenario: Refresh verification preserves legacy decimal tolerance
      Given a verified Apply requests a refresh rate for its resolved verification scope
      When either requested or observed rate is a legacy decimal value
      Then the engine accepts values whose difference is no greater than the fixed 0.9 Hz legacy compatibility tolerance
      And it does not require byte-for-byte refresh-rate identity
