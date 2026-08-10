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
      Then it preflights the configuration against that topology without mutating or recovering the display stack
      And it applies the explicit topology once as staging immediately before one original SettingsManager request
      And it verifies the requested configuration against the intended target after the transaction
      And unrelated topology adjustments do not fail the request when the intended target remains usable

    Scenario: An already satisfied request remains a backend no-op and is still verified
      Given the requested topology is already active and its target is usable
      And the requested configuration is already active
      When the engine applies the request
      Then the helper makes at most one best-effort base-topology call and the backend may no-op the equal topology
      And it reports success only after the existing configuration is confirmed stable

    Scenario: Missing pre-Apply desktop evidence stops before mutation when no staging topology was supplied
      Given an Apply request contains a display configuration
      And the request supplies no usable staging topology
      When the current topology is structurally unavailable or the original session settings cannot be retained
      Then the request reports a retryable non-success before topology or settings mutation
      And failure to derive a valid topology for the requested configuration instead reports an invalid request
      And neither outcome may be reported as a verified Apply

    Scenario: A request without the required configuration is rejected before display mutation
      Given an Apply request has no display configuration
      When the engine validates the request
      Then it rejects the request as invalid
      And it does not activate a topology or change display settings

  Rule: Ordinary Apply uses a single non-mutating preflight and one transaction

    Scenario: A structurally invalid topology is rejected without activation
      Given an Apply request contains a topology with invalid structure
      When the engine validates the topology
      Then it rejects the request as invalid
      And it does not attempt to activate that topology

    Scenario: An OS-invalid computed topology fails without display-stack recovery
      Given an Apply request contains a structurally valid topology
      But the operating system initially rejects that topology during validation
      When the engine applies the request
      Then an operating-system general-failure result may receive one compatibility validation attempt using the alternate supplied-topology flag form
      And other validation errors do not receive that alternate flag-form attempt
      Then it reports invalid request if validation is still rejected
      And it does not invoke display-stack recovery, wait 500 ms, or retry the Apply mutation
      And the desktop remains untouched because validation precedes the durable mutation boundary

    Scenario: Ordinary Apply does not poll topology readiness
      Given an Apply request passed its non-mutating preflight
      When the engine reaches its mutation boundary
      Then it applies a supplied staging topology at most once on a best-effort basis
      And it calls SettingsManager exactly once with the original configuration
      And it does not poll full topology enumeration, invoke the restore transition engine, or globally recover the display stack

    Scenario: A topology mutation is protected before either activation or recovery can change the desktop
      Given a staging-topology set or SettingsManager request could alter the desktop
      When the engine reaches the first such mutation boundary
      Then it attempts to arm durable recovery and records whether that succeeded before starting that operation
      And only a successful attempt counts as durable protection
      And cancellation before that boundary starts no mutation and may clear provisional recovery state
      But cancellation after that boundary retains recovery because the desktop may have changed
      And failure to arm does not certify the desktop as safe, restored, or safely disarmed and may be retried after the mutation reports its outcome

  Rule: Target verification is distinct from unrelated display readiness

    Scenario: A configuration Apply waits for its intended target group, not every unrelated path
      Given SettingsManager has completed a configuration Apply
      And the intended target or duplicate group is active and queryable
      But an unrelated display path is still waking, sleeping, or otherwise not queryable
      When the engine performs target-scoped verification
      Then it may continue with the requested configuration for the intended target group
      And it does not reject or retarget the request solely because the unrelated path is not ready

    Scenario: Apply preflight performs one enumeration without readiness polling
      Given an Apply request needs current topology and device metadata to derive its target plan
      When the engine performs its non-mutating preflight
      Then it captures or uses the staging topology once and performs one minimal device enumeration
      And a supplied staging topology does not trigger a redundant live-topology capture
      And an unavailable current context reports retryable while a rejected supplied or computed topology reports invalid request
      And it does not wait in a 5-second topology-readiness loop

  Rule: Target identity remains stable when Windows reorders display paths

    Scenario: An explicit device identifier remains the configuration target
      Given an Apply request names a specific device identifier
      And Windows reorders unrelated display paths while applying the topology
      When the engine applies and verifies the configuration
      Then the named device remains the target for requested mode and HDR checks
      And a different display is never selected merely because it appears first after reordering

    Scenario: An empty device identifier selects the first planned group containing a primary candidate
      Given planning identifies one or more candidate identities from the original primary display group
      And an Apply request omits its device identifier
      When preflight derives the intended topology
      Then the engine resolves the target as the first planned topology group containing any candidate
      And planned group order determines the selected duplicate scope
      And it checks required mode, refresh, and HDR state against that planned group

  Rule: Topology staging preserves the original SettingsManager intent

    Scenario: The original configuration reaches one SettingsManager transaction
      Given a staging topology has been supplied or derived for a valid configuration request
      When the engine begins the requested settings transaction
      Then it calls SettingsManager once with the original device-preparation, mode, HDR, and primary intent
      And it does not rebase persistent state or rewrite EnsureActive or EnsureOnlyDisplay to VerifyOnly
      And only a successful requested-settings application permits position overrides followed by physical refresh overrides
      And sticky verification after that best-effort work is still required for final core Apply success

    Scenario Outline: Settings-stage failures retain their caller-visible status
      Given the ordinary Apply transaction has crossed its mutation boundary
      When SettingsManager encounters <condition>
      Then it reports <status>
      And it does not convert that condition into verified success

      Examples:
        | condition                                                         | status                    |
        | a temporarily unavailable display operation                       | retryable non-success     |
        | a device, primary, or mode preparation failure                     | verification failure      |
        | an HDR-state preparation failure                                   | HDR-state failure         |
        | an unexpected settings failure                                    | fatal non-success         |

  Rule: Failed or cancelled mutations preserve a path back to a safe desktop

    Scenario: Consecutive Applies retain the original session presentation baseline
      Given the first configuration Apply of a session starts from a valid desktop
      When later Applies run during that same session
      Then the engine retains the session's original mode, HDR, and primary-display restoration values
      And each preflight derives current configuration work from its supplied or current staging topology
      And a failed settings stage does not replace the original session baseline with partial display state

    Scenario: A failed settings transaction uses SettingsManager guards rather than a second rollback transaction
      Given the durable pre-Apply snapshot and recovery lease protect the session
      And the supplied staging topology was set before SettingsManager began
      But applying the requested mode, HDR, or primary-display settings fails
      When the engine handles the failed transaction
      Then SettingsManager owns its immediate component rollback guards
      And ordinary Apply does not launch a second topology transition and full snapshot replay
      And any possibly changed desktop remains covered by the durable recovery lease

    Scenario: Apply does not blindly retry a failed transaction outcome
      Given an Apply request reaches a retryable topology, settings, or verification outcome
      When the engine handles that outcome
      Then it reports the transaction outcome without a 500 ms or 1,000 ms full-Apply retry
      And only one initial-verification mismatch may cause one immediate repair transaction
      And only the specific virtual HDR-state failure may cause one effective-SDR fallback transaction

    Scenario: A later settings or verification failure retains durable recovery
      Given an Apply attempt has crossed a mutation boundary
      When SettingsManager or initial verification fails
      Then the desktop remains protected whenever the transaction may have mutated it
      And the original transaction status remains attributable to that request
      And no later cancellation may report a verified Apply for that failed attempt

    Scenario: An unconfirmed SettingsManager rollback retains desktop recovery protection
      Given a requested settings transaction failed after its mutation boundary
      And an internal component rollback cannot be proven complete
      When the failed Apply reaches its terminal outcome
      Then it retains recovery protection and reports a non-success outcome

    Scenario: Cancellation before a mutation leaves the desktop and recovery state unmodified
      Given an Apply request is waiting before any topology or settings mutation
      When the request is cancelled or superseded
      Then the engine does not start another display-changing stage for that request
      And it does not claim that the desktop changed

    Scenario: Cancellation after possible mutation retains recovery protection
      Given an Apply request has entered its staging-topology set or SettingsManager application
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
      And all named overrides share one active-device enumeration for that best-effort pass
      And it makes one best-effort position attempt without a retry sleep on the stream-start path
      And position failure may not turn into a multi-second core Apply gate

    Scenario: Empty or unrepositionable display identities do not redirect position work
      Given an Apply request contains a position override for an empty or nonempty display identity
      When the engine applies position overrides
      Then an empty identity is ignored without a position attempt
      And a nonempty identity that is missing, inactive, or temporarily unrepositionable remains bound to that same identity for its one best-effort attempt
      And it does not substitute another display identity for either case
      And cancellation stops remaining position attempts
      And any unresolved nonempty override remains a best-effort position outcome after that attempt

    Scenario: A position override with unknown dimensions retains the ordinary coordinate ceiling
      Given an Apply request contains a position override for a display whose dimensions cannot currently be read
      When the engine applies the position override
      Then it still constrains each origin to the supported coordinate range
      And it uses the ordinary upper coordinate ceiling of 32767 rather than inventing a display size
      And a failed or unavailable position operation remains best effort for that named display

    Scenario: Position overrides are best effort after the core configuration change
      Given the requested topology and target configuration are otherwise valid
      And one active monitor position override cannot be applied in its one best-effort attempt
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
      And valid active physical overrides share one batched mode-map operation instead of one operation per override
      And an unavailable physical identity fails only its own best-effort override rather than discarding active overrides
      And an already equivalent rational refresh rate succeeds without another display change
      And it ignores empty device identifiers or invalid zero-valued rates
      And cancellation is observed while collecting overrides and again before the batched mutation
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
      Then it performs the first target-scoped required-state observation immediately
      And every requested mode, refresh, and HDR condition must match at that observation
      And it waits the default 250 ms before repeating the same required-state observation
      And success requires both observations for the current uncancelled request
      And tuning that confirmation interval may not remove the second observation or accept a stale or cancelled request

    Scenario: Refresh and configuration verification require the requested targeted state
      Given an Apply request requires a resolution, refresh rate, or HDR state
      And every required property matches on the resolved active target
      When the engine verifies the completed display change
      Then its targeted mode and HDR queries must find the required observations in that scope
      And it verifies every requested resolution, refresh, and HDR property there
      But it does not add a separate primary-display query beyond SettingsManager, matching the v1 capture gate

    Scenario: A repeated configuration mismatch fails verification
      Given an Apply request requires a resolution, refresh rate, or HDR state
      And a required property does not match on repeated verification
      When the engine verifies the completed display change
      Then it reports verification failure rather than treating the accepted request as a successful display change

    Scenario: Normal Apply verifies only its planned target and requested settings
      Given a normal Apply has used topology as staging and Windows has adjusted unrelated paths
      When the engine verifies the transaction
      Then it checks only the requested resolution, refresh, and HDR settings in the preflight-resolved target scope
      And it does not require unrelated paths to reproduce the request's staging topology exactly
      And it does not apply or verify layout or rotation as part of Apply

  Rule: Resolved-target verification preserves duplicate-display semantics

    Scenario: An explicit target in a planned duplicate group has mixed verification scope
      Given an Apply names an explicit device that belongs to a preflight-planned duplicate group
      And it requests a resolution, refresh, or HDR state
      When the engine verifies the successful Apply
      Then the requested resolution must match across every member of that planned duplicate group
      And requested refresh and HDR state are checked against the explicit target representative
      And a matching duplicate-group member does not substitute for a failed representative refresh or HDR check

    Scenario: An omitted target verifies the first planned group containing an original-primary candidate
      Given an Apply omits its device identity and planning retained candidate identities from the original primary group
      And it requests a resolution, refresh, or HDR state
      When the engine verifies the successful Apply
      Then it selects the first planned topology group containing any retained candidate
      And requested resolution, refresh, and HDR state are checked across that selected group
      And planned group ordering selects the candidate-containing verification scope

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
