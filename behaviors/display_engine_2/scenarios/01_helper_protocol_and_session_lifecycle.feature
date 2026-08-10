@display-engine-v2
Feature: Display engine v2 helper protocol and session lifecycle
  The display engine v2 helper accepts one current control session and reports
  command outcomes only to the request and session that own them.

  Rule: Helper entry preserves engine choice and best-effort single ownership

    Scenario: An explicit startup engine overrides compatible saved state
      Given compatible saved state names one display-helper engine
      And startup supplies a nonempty explicit engine selection
      When the common helper entry chooses an engine
      Then it uses the explicit selection instead of the saved selection
      And the exact value "legacy" selects the legacy engine while every other value selects v2

    Scenario: Restore startup uses the first compatible saved engine
      Given startup does not supply a nonempty explicit engine selection
      And ordered compatible state locations contain empty, unusable, and nonempty engine values
      When the common helper entry chooses an engine
      Then it uses the first nonempty engine value from the ordered compatible locations
      And the exact value "legacy" selects the legacy engine while every other value selects v2

    Scenario: Helper entry defaults to v2 without a usable engine choice
      Given startup has no nonempty explicit engine selection
      And no compatible state location provides a nonempty engine value
      When the common helper entry chooses an engine
      Then it selects v2
      And corrupt or unreadable saved state does not prevent startup

    Scenario Outline: Saving the selected helper engine avoids unnecessary state changes
      Given compatible state currently retains <saved state>
      When Sunshine asks to save <requested value> as the selected helper engine
      Then <persistence result>
      And the save operation publishes no success result to its caller

      Examples:
        | saved state     | requested value                    | persistence result                                      |
        | any prior value | an empty value                      | the save is ignored and the prior state remains         |
        | legacy          | the exact value legacy              | the state file is not rewritten                         |
        | legacy          | a value differing only by casing    | the new exact spelling is written                       |
        | legacy          | v2                                  | the persisted selection is changed to v2                |

    Scenario Outline: An unusable helper-engine state source has its defined save outcome
      Given a compatible state location is selected for the helper-engine value
      When saving a different nonempty selection encounters <state condition>
      Then <save outcome>
      And the save operation does not claim success or failure to its caller

      Examples:
        | state condition                               | save outcome                                                   |
        | no usable state path                          | the save stops without replacing prior state                   |
        | an unsafe read or inspection failure          | the save stops without replacing prior state                   |
        | missing, blank, or readable but corrupt state | compatible state is recreated and the requested value is attempted |

    Scenario Outline: Helper-engine replacement failures preserve their real guarantee boundary
      Given saving a changed helper-engine selection has reached durable replacement
      When <failure point>
      Then <persistence guarantee>
      And later startup chooses whatever usable persisted value actually remains

      Examples:
        | failure point                                      | persistence guarantee                                      |
        | a replacement attempt fails before the target is replaced | the prior target remains available                  |
        | post-write verification fails after replacement      | no rollback to the prior target is guaranteed            |

    Scenario: Machine-wide singleton ownership is preferred
      Given no helper currently owns the control service
      When a helper can claim machine-wide singleton ownership
      Then that ownership governs the helper instance
      And it does not also claim per-user singleton ownership

    Scenario: Access denial alone permits per-user singleton fallback
      Given the helper is denied access to machine-wide singleton ownership
      When it attempts startup ownership
      Then it attempts the per-user singleton scope
      And any other machine-wide ownership failure does not cause that scope fallback

    Scenario: Confirmed singleton ownership prevents a newcomer takeover
      Given either supported singleton scope reports an existing owner
      When another helper instance starts
      Then the existing helper remains the control service
      And the newcomer exits without taking over display control

    Scenario: Unavailable singleton primitives do not suppress recovery startup
      Given neither supported singleton scope can provide a usable ownership result
      When the helper starts
      Then it continues on a best-effort basis
      And later control-service ownership still determines whether it can serve requests

    Scenario: Unsupported platforms do not perform helper display control
      Given the common helper entry runs on a platform without Windows display support
      When it starts
      Then it exits successfully without selecting a display engine
      And it performs no display-control action

  Rule: One helper owns the active control service

    Scenario: A second helper does not take over an active control service
      Given a display engine v2 helper is already active
      When another helper instance starts
      Then the existing helper remains the control service
      And the new instance exits without taking over display control

    Scenario: The normal helper remains available while it waits for control
      Given no display engine v2 helper is active
      When the helper starts normally before a control client connects
      Then it remains available to accept a later control client
      And it continues any already-required display safety behavior while waiting

    Scenario: A normal helper renews its listener wait without abandoning safety work
      Given the normally started helper has no connected control client
      When its default 15-second control-listener wait ends without a client
      Then it renews availability for a later control client
      And it continues already-required display safety behavior throughout the renewed wait

    Scenario: Control availability retains its compatibility fallback order
      Given the helper is ready to offer its control service
      When the preferred anonymous endpoint form is unavailable
      Then it tries the named compatibility form
      And failure of both forms leaves recovery and safety timers active while control availability is retried

    Scenario: Temporary control-endpoint unavailability does not abandon safety behavior
      Given the helper has required display safety behavior in progress
      And it cannot currently offer a control endpoint
      When it continues running
      Then it keeps the safety behavior active
      And it retries making control available instead of exiting solely for that reason

    Scenario: Restore-mode startup begins recovery without a control client
      Given a valid restore context is available
      When the helper is started in restore mode
      Then it begins the requested restoration without a control client
      And it remains available for later recovery opportunities until a confirmed restore or shutdown ends restore mode

    Scenario: A confirmed restore ends restore mode
      Given the helper is running in restore mode with a usable recovery candidate
      When restoration and final validation confirm the desktop
      Then it reports the safe completion
      And it exits restore mode

    Scenario: An unconfirmed restore does not end restore mode
      Given the helper is running in restore mode with an existing recovery candidate
      When that candidate's restoration or final validation remains unconfirmed
      Then it retains the recovery opportunity for later eligible events
      And it does not exit merely because its initial restoration did not succeed

    Scenario: Startup adopts a valid current-session restore baseline
      Given a compatible prior context contains a valid current-session restore baseline
      When the helper starts
      Then it makes that current-session baseline available to its restore session

    Scenario: Startup adopts a valid previous-session restore baseline
      Given a compatible prior context contains a valid previous-session restore baseline
      When the helper starts
      Then it makes that previous-session baseline available to its restore session

    Scenario: Startup rejects an unusable prior session baseline
      Given a discovered session baseline has no usable restoration data
      When the helper starts
      Then it is not adopted as a restore candidate
      And it is not retained as a usable session baseline for a later restore

    Scenario: Startup combines saved user exclusions with managed virtual displays
      Given the first usable saved display state names excluded devices
      And that saved state names managed virtual displays
      When the helper starts
      Then it begins with the combined unique exclusion set
      And those displays are not eligible for helper-managed baseline capture or restore

    Scenario: Startup does not invent exclusions when no saved exclusion state is usable
      Given no compatible saved display state provides exclusions
      When the helper starts
      Then it starts without an additional saved exclusion policy

    Scenario Outline: Unusable saved exclusion state does not block helper startup
      Given the saved display state is <state condition>
      When the helper starts
      Then it does not stop solely because the saved state is unusable
      And it does not use the unusable state as an exclusion policy

      Examples:
        | state condition |
        | missing         |
        | corrupt         |

    Scenario Outline: Startup log-level arguments accept case-insensitive names or one digit
      Given no other display engine v2 helper is active
      When the helper starts with <log-level argument>
      Then the effective log level is <result>

      Examples:
        | log-level argument  | result                         |
        | "--log-level debug" | the requested debug level      |
        | "--log-level WARNING" | the requested warning level    |
        | "--log-level=6"     | the requested none level       |
        | "--log-level loud"  | the default informational level |
        | "--log-level=9"     | the default informational level |

  Rule: The control boundary accepts defined frames and rejects unsafe input

    Scenario Outline: A valid command frame retains its caller identity
      Given an active control client with control-session identity A
      When it sends a valid <command> command frame
      Then the helper accepts that command as belonging to control session A
      And any defined reply is sent only to control session A

      Examples:
        | command          |
        | Apply            |
        | Revert           |
        | Reset            |
        | Export Golden    |
        | Disarm           |
        | Snapshot Current |
        | Refresh Rate     |
        | Ping             |
        | Log Level        |
        | Stop             |

    Scenario Outline: Supported frame formats carry the same control intent
      Given an active control client sends a valid Apply request
      When it uses the <frame format> form of the control protocol
      Then the helper accepts the request with the same command and payload meaning

      Examples:
        | frame format             |
        | legacy raw command-and-data |
        | length-declared command-and-data |

    Scenario: Every transport record has a stable length-prefixed compatibility boundary
      Given Sunshine and the helper exchange one control or result body
      When that body is written to the live transport
      Then four little-endian bytes declare the unsigned body length before the body
      And the declared body consists of one command or result byte followed by its payload, so the length counts both
      And declared lengths from 1 byte through 2 MiB are plausible transport records while zero or larger declarations are not

    Scenario: Transport decoding preserves partial, sequential, and malformed records exactly
      Given transport bytes may contain a partial record, multiple records, or an implausible length declaration
      When the receiver searches for its next complete record
      Then a plausible partial record remains buffered across timeout returns for a later receive call to complete
      And after any successful partial read, that receive call consumes only bytes available without another wait and returns timeout if the record is still incomplete
      And it returns one complete record at a time while preserving later complete records for later receives
      And a zero or greater-than-2-MiB declaration drops one leading byte at a time until a plausible boundary is found

    Scenario Outline: An otherwise plausible record larger than its receiver capacity blocks later records
      Given a complete positive length-prefixed record is no larger than 2 MiB but exceeds the <endpoint> receive capacity of <capacity>
      And another complete record follows it
      When the endpoint attempts to receive the first record
      Then the oversized record is neither consumed nor skipped
      And the later record remains blocked and receives may repeatedly time out until the connection is retired

      Examples:
        | endpoint                     | capacity     |
        | helper request delivery      | 65,536 bytes |
        | Sunshine result waiting      | 2,048 bytes  |

    Scenario Outline: An Apply rejected by wire decoding never reaches session mutation
      Given an active control client sends an Apply request with a valid request identifier
      And the requested display configuration is <wire-invalid configuration>
      When the helper validates the wire payload before dispatch
      Then it leaves the display session unchanged
      And it does not dispatch an Apply command to session-state handling
      And it returns a failed Apply result carrying that same request identifier

      Examples:
        | wire-invalid configuration |
        | malformed                  |
        | incomplete                 |

    Scenario Outline: An empty or unrecognized complete command has no control effect
      Given an active control client
      When it sends <frame condition>
      Then the helper does not apply, restore, reset, disarm, or stop the current session
      And it does not invent a success reply for that frame

      Examples:
        | frame condition                                  |
        | an empty frame                                    |
        | a complete frame whose command type is unsupported |

    Scenario Outline: An incomplete refresh-rate request is rejected immediately
      Given an active control client sends Refresh Rate with <missing requirement>
      When the helper validates the refresh-rate request
      Then it returns a failed refresh-rate result immediately
      And a supplied v2 request identifier is echoed in that failed result

      Examples:
        | missing requirement               |
        | no device identifier               |
        | a zero refresh numerator           |
        | a zero refresh denominator         |

    Scenario: A runtime log-level update clamps its first numeric byte to the supported range
      Given an active control client
      When it sends Log Level with a nonempty first numeric byte above the supported range
      Then the helper uses the highest supported log level
      And the update does not act as a display-control command
      And it does not send a completion result

    Scenario: An empty runtime log-level frame preserves the current level
      Given an active control client with an established log level
      When it sends Log Level without a level value
      Then the helper preserves the established log level
      And it does not change display-control intent

    Scenario Outline: Dispatch-only commands do not fabricate an immediate result message
      Given an active control client sends <command>
      When the helper accepts that command
      Then it does not emit an immediate Apply, verification, snapshot, or refresh-rate result solely as an acknowledgement

      Examples:
        | command       |
        | Revert        |
        | Disarm        |
        | Export Golden |
        | Reset         |
        | Log Level     |
        | Stop          |

  Rule: Apply options retain valid intent and default safely

    Scenario: Apply retains a valid requested configuration and topology together
      Given an Apply request contains a valid display configuration and a valid requested topology
      When the helper accepts the request
      Then it retains both as the request's display-change intent

    Scenario Outline: Core Apply JSON member names and shapes remain wire-compatible
      Given an Apply request carries the exact JSON member <member>
      When the helper parses its core single-display configuration
      Then that member carries <meaning>
      And missing, null, malformed, or unknown data retains the separately documented parse result rather than being renamed by a new implementation

      Examples:
        | member       | meaning |
        | device_id    | the target identity as a string |
        | device_prep  | exactly VerifyOnly, EnsureActive, EnsurePrimary, or EnsureOnlyDisplay |
        | resolution   | null or an object whose width and height JSON numbers are converted to unsigned platform integers |
        | refresh_rate | null, or an object with type equal to lowercase double and a numeric value, or type equal to lowercase rational and a value object whose numerator and denominator JSON numbers are converted to unsigned platform integers |
        | hdr_state    | null or exactly Disabled or Enabled |

    Scenario: Core unsigned configuration fields accept every JSON number category before conversion
      Given core resolution width or height, or core rational numerator or denominator, is a signed-integer, unsigned-integer, or floating JSON number
      When the dependency converts the core Apply configuration
      Then it does not first require the unsigned-integer JSON category
      And representable fractional values truncate toward zero while signed integers use unsigned platform conversion
      But a particular result for an out-of-range floating value is not a portable clean-room guarantee

    Scenario Outline: Every core Apply member is required even when its value may be null
      Given an Apply JSON object omits <required member>
      When the helper parses its core single-display configuration
      Then core configuration parsing fails and the Apply is not accepted
      And resolution, refresh_rate, and hdr_state must be present as explicit null when that optional change is absent

      Examples:
        | required member |
        | device_id       |
        | device_prep     |
        | resolution      |
        | refresh_rate    |
        | hdr_state       |

    Scenario: Unknown core JSON members do not create display intent
      Given an Apply object contains all five valid core members and additional unrecognized members
      When extension extraction has completed and core configuration is parsed
      Then unrecognized members are ignored by the core configuration parser
      And only recognized core and separately documented extension members affect the request

    Scenario Outline: Apply extension names use one fixed extraction sequence independent of JSON text order
      Given an Apply JSON object contains the exact extension member <member>
      When the helper evaluates optional request policy in its fixed extraction sequence
      Then the member represents <meaning>
      And its recognized, ignored, defaulted, erased, or exceptional type behavior follows the dedicated scenarios below regardless of where that member appeared in the JSON text

      Examples:
        | member                                      | meaning |
        | sunshine_apply_id                           | unsigned JSON-integer Apply correlation identity |
        | sunshine_omit_final_initial_hdr_reapply     | Boolean request to omit the final initial HDR correction |
        | wa_hdr_toggle                               | Boolean post-verification HDR blanking request |
        | sunshine_virtual_layout                     | named virtual-display arrangement string |
        | sunshine_monitor_positions                  | object keyed by device identity whose values contain integer x and y |
        | sunshine_snapshot_exclude_devices           | baseline-exclusion payload |
        | sunshine_topology                           | array of duplicate-display groups |
        | sunshine_always_restore_from_golden         | Boolean golden-first recovery policy |
        | sunshine_restore_on_disconnect              | Boolean disconnect recovery policy |
        | sunshine_device_refresh_rate_overrides      | object keyed by device identity whose values contain unsigned-integer num and den |

    Scenario: Apply topology retains every array group containing at least one string
      Given an Apply request contains a valid display configuration and a topology array
      And its topology contains array and non-array groups with string and non-string members
      When the helper accepts the request
      Then it drops non-array groups and non-string group members
      And it retains every array group that still contains at least one string, including an empty-string identity
      And it attaches topology intent only when at least one such group remains

    Scenario: A non-list topology does not create topology intent
      Given an Apply request contains a non-list topology value
      When the helper accepts the remaining valid request data
      Then it does not attach topology intent to that request

    Scenario: Apply retains complete monitor placement instructions
      Given an Apply request supplies an object-key device identifier and JSON-integer horizontal and vertical placement values that fit a signed platform integer
      When the helper accepts the request
      Then it retains that monitor placement as part of the display-change intent
      And the object-key device identifier may be empty

    Scenario Outline: Incomplete monitor placement is not treated as a placement instruction
      Given an Apply request supplies a monitor placement with <incomplete part>
      When the helper accepts the remaining valid request data
      Then it does not retain that incomplete placement as a display-change instruction

      Examples:
        | incomplete part                         |
        | no device placement record               |
        | no horizontal coordinate                 |
        | no vertical coordinate                   |
        | a non-integer JSON coordinate             |

    Scenario: Apply retains complete per-device refresh overrides
      Given an Apply request supplies an object-key device identifier with unsigned JSON-integer refresh numerator and denominator values that fit an unsigned platform integer
      When the helper accepts the request
      Then it retains that per-device refresh override as part of the display-change intent
      And an empty device key or zero numerator or denominator is retained here even though the standalone Refresh Rate command rejects those values

    Scenario Outline: Incomplete per-device refresh data is not treated as an override
      Given an Apply request supplies a refresh override with <incomplete part>
      When the helper accepts the remaining valid request data
      Then it does not retain that incomplete override as a display-change instruction

      Examples:
        | incomplete part                |
        | no override record             |
        | no numerator                   |
        | no denominator                 |
        | a non-unsigned-integer JSON rate component |

    Scenario: Apply retains an explicit HDR blanking request
      Given an Apply request contains a valid explicit HDR blanking choice
      When the helper accepts the request
      Then it retains that HDR transition choice as display-change intent

    Scenario: A non-Boolean stream-start HDR option is erased and becomes false
      Given an Apply request contains a non-Boolean stream-start HDR option
      When the helper extracts optional Apply policy
      Then it records the option as false, removes that extension before core configuration parsing, and continues extracting later extensions

    Scenario: A non-Boolean golden preference is ignored while later extension parsing continues
      Given an Apply request contains a non-Boolean golden-baseline preference
      When the helper extracts optional Apply policy
      Then it does not replace the default preference or remove that unrecognized extension
      And it continues extracting later extensions before core configuration parsing ignores the unknown key

    Scenario: A non-Boolean HDR blanking option aborts later extension extraction
      Given an Apply request contains a non-Boolean HDR blanking choice
      And layout, placement, exclusions, topology, recovery policy, or refresh overrides occupy later positions in the helper's fixed extraction sequence regardless of JSON text order
      When Boolean conversion fails
      Then extension extraction stops before every later extraction position and retains only extension values extracted before the failure
      And the original JSON still reaches core display-configuration parsing, which may accept its core configuration while ignoring extension keys

    Scenario Outline: Over-range extension integers narrow instead of causing a range failure
      Given an Apply request contains <over-range value> that still has the required JSON number category
      And other extension members occupy later positions in the helper's fixed extraction sequence
      When the helper converts that extension value to <destination>
      Then it performs the platform narrowing conversion without an explicit range rejection
      And extension extraction continues through later positions rather than aborting on that value
      And <portable boundary>

      Examples:
        | over-range value                                           | destination                  | portable boundary |
        | a signed or unsigned JSON-integer monitor coordinate outside signed 32-bit range | a signed 32-bit coordinate   | no particular out-of-range signed result is a portable guarantee |
        | an unsigned JSON-integer refresh component above 4,294,967,295        | an unsigned 32-bit component | the retained value is reduced modulo 4,294,967,296 |

    Scenario: Apply retains a named virtual display arrangement
      Given an Apply request contains a valid named virtual display arrangement
      When the helper accepts the request
      Then it retains that arrangement as display-change intent

    Scenario: A non-text virtual display arrangement does not create arrangement intent
      Given an Apply request contains a non-text virtual display arrangement
      When the helper evaluates the optional arrangement
      Then it does not attach a virtual display arrangement to the request

    Scenario: Apply replaces its baseline exclusion policy with a supplied list
      Given an Apply request supplies recognizable device identities to exclude from its baseline
      When the helper accepts the request
      Then those identities replace the request's baseline exclusion policy

    Scenario: An explicit empty Apply exclusion list clears the request's baseline exclusion policy
      Given an Apply request supplies an explicit empty baseline exclusion list
      When the helper accepts the request
      Then the request's baseline exclusion policy is cleared

    Scenario: Apply retains a valid golden-baseline preference
      Given an Apply request contains a valid preference for golden-baseline restoration
      When the helper accepts the request
      Then it retains that preference as the request's recovery policy

    Scenario: An Apply request without a disconnect policy defaults to safety restoration
      Given an Apply request omits its disconnect restoration policy
      When the helper accepts the request
      Then the request remains eligible for restoration after a broken control connection

    Scenario: An invalid Apply disconnect policy defaults to safety restoration
      Given an Apply request contains an invalid disconnect restoration policy
      When the helper accepts the remaining valid request data
      Then the request remains eligible for restoration after a broken control connection

    Scenario: An Apply request can deliberately retain a paused session after disconnect
      Given an Apply request explicitly disables restoration after disconnect
      When the helper accepts the request
      Then the request retains that paused-session policy

    Scenario: The stream-start option can omit the final initial HDR correction
      Given an Apply request explicitly asks to omit the final initial HDR correction for a capture-gated stream start
      When the helper accepts the request
      Then it retains that stream-start option

    Scenario: An Apply request without the stream-start HDR option uses normal handling
      Given an Apply request omits the stream-start HDR option
      When the helper accepts the request
      Then it retains normal initial HDR handling for that request

    Scenario Outline: A valid Apply policy option preserves unrelated display intent
      Given an Apply request contains a valid display configuration and topology
      And it contains <policy option>
      When the helper accepts the request
      Then it retains <policy option> only for its named policy
      And it preserves the request's unrelated display configuration and topology intent

      Examples:
        | policy option                                      |
        | a golden-baseline recovery preference              |
        | a disconnect restoration preference                |
        | a stream-start HDR handling preference             |
        | a baseline exclusion policy                        |

    Scenario: An unsigned JSON-integer Apply identifier establishes reply correlation
      Given an Apply request contains unsigned JSON-integer sunshine_apply_id 1001
      When the helper accepts the request
      Then it retains identifier 1001 for its Apply and verification outcomes

    Scenario Outline: An unusable Apply identifier cannot impersonate a caller identifier
      Given an Apply request has <identifier condition>
      When the helper accepts the remaining valid request data
      Then it does not assign an unrelated caller identifier to the request

      Examples:
        | identifier condition                    |
        | no sunshine_apply_id                    |
        | a signed negative sunshine_apply_id     |
        | a floating-point sunshine_apply_id      |
        | a nonnumeric sunshine_apply_id          |
        | sunshine_apply_id equal to zero         |

  Rule: Revert and baseline commands honor only valid optional policy

    Scenario Outline: Revert JSON policy names remain wire-compatible
      Given Revert carries the exact JSON member <member>
      When the helper parses its optional Boolean policy
      Then that member controls <meaning>
      And a missing or non-Boolean member preserves the separately documented default

      Examples:
        | member                                      | meaning |
        | sunshine_prefer_golden_if_current_missing   | whether golden may replace a missing Current baseline |
        | sunshine_always_restore_from_golden         | the optional golden-first override |

    Scenario: Revert defaults to golden fallback when the current baseline is unavailable
      Given an active control client sends Revert without an optional preference
      When the helper accepts the request
      Then it retains the default policy that permits golden fallback if the current baseline is unavailable

    Scenario: Revert can enable golden fallback when the current baseline is unavailable
      Given an active control client sends Revert with a valid enabled golden-fallback preference
      When the helper accepts the request
      Then it retains the enabled golden-fallback preference

    Scenario: Revert can disable golden fallback when the current baseline is unavailable
      Given an active control client sends Revert with a valid disabled golden-fallback preference
      When the helper accepts the request
      Then it retains the disabled golden-fallback preference

    Scenario: Revert retains an explicit golden-first override
      Given an active control client sends Revert with a valid golden-first override
      When the helper accepts the request
      Then it retains that golden-first override

    Scenario Outline: Invalid Revert preferences preserve the safe defaults
      Given an active control client sends Revert with <invalid preference>
      When the helper accepts the request
      Then it preserves the default fallback policy
      And it does not invent a golden-first override

      Examples:
        | invalid preference                |
        | malformed optional preference data |
        | a non-Boolean preference           |

    Scenario Outline: Baseline commands accept current and legacy-compatible exclusion lists
      Given an active control client sends <command> with <list form> of device identities to exclude
      When the helper accepts the command
      Then it retains those identities as that command's exclusion policy

      Examples:
        | command          | list form                                      |
        | Export Golden    | a direct list                                  |
        | Snapshot Current | a list of records that each identify a device |

    Scenario Outline: Baseline JSON metadata names remain wire-compatible
      Given a baseline command carries the exact JSON member <member>
      When the helper parses its metadata
      Then that member represents <meaning>
      And no renamed alias is inferred beyond the explicitly accepted forms

      Examples:
        | member                 | meaning |
        | sunshine_snapshot_id   | unsigned JSON-integer Snapshot Current correlation identity |
        | exclude_devices        | primary object member containing the exclusion list |
        | devices                | legacy object-member alias used only when exclude_devices is absent |

    Scenario: Correlated Snapshot Current preserves a bare-array compatibility payload
      Given a v2 Snapshot Current caller supplies a bare exclusion array
      When Sunshine adds its nonzero correlation identity
      Then it wraps that array under the exact exclude_devices member and adds sunshine_snapshot_id
      And an object payload keeps its existing members while receiving that identifier

    Scenario Outline: An explicit empty baseline exclusion list clears command policy
      Given an active control client sends <command> with an explicit empty exclusion list
      When the helper accepts the command
      Then that command's exclusion policy is cleared

      Examples:
        | command          |
        | Export Golden    |
        | Snapshot Current |

    Scenario: Correlation-only Snapshot Current metadata preserves exclusion policy
      Given an active control client has an existing snapshot exclusion policy
      When it sends Snapshot Current with a correlation identifier but no exclusion instruction
      Then the request may be correlated without changing the existing exclusion policy

    Scenario: Malformed Snapshot Current exclusion data preserves exclusion policy
      Given an active control client has an existing snapshot exclusion policy
      When it sends Snapshot Current with malformed exclusion data
      Then it preserves the existing exclusion policy

    Scenario: Malformed Export Golden exclusion data preserves exclusion policy
      Given an active control client has an existing snapshot exclusion policy
      When it sends Export Golden with malformed exclusion data
      Then it preserves the existing exclusion policy

  Rule: Replies are correlated to their request and live control session

    Scenario: A successful Apply status reports a successful Apply result
      Given an active control client owns Apply request identifier 1001
      When that request reaches a successful Apply outcome
      Then it receives a successful Apply result carrying identifier 1001

    Scenario Outline: Any non-success Apply status is reported as a failed Apply result
      Given an active control client owns Apply request identifier 1001
      When that request reaches the <status> outcome
      Then it receives a failed Apply result carrying identifier 1001

      Examples:
        | status                     |
        | invalid-request             |
        | verification-failed         |
        | retryable                   |
        | fatal                       |

    Scenario Outline: Apply acknowledgement and initial verification remain correlated
      Given an active control client sends Apply with request identifier 1001
      When the helper reaches an initial verification outcome of <verification outcome>
      Then the Apply result identifies request 1001
      And the verification result identifies request 1001 with <verification outcome>
      And neither result can satisfy a different Apply request

      Examples:
        | verification outcome |
        | success              |
        | failure              |

    Scenario: Uncorrelated Snapshot Current remains dispatch-only
      Given an active control client sends Snapshot Current without a correlation identifier
      When the snapshot operation completes
      Then no Snapshot Current completion reply is sent

    Scenario: A nonnumeric Snapshot Current identifier remains dispatch-only
      Given an active control client sends Snapshot Current with a nonnumeric correlation identifier
      When the snapshot operation completes
      Then no Snapshot Current completion reply is sent

    Scenario Outline: Correlated Snapshot Current reports its matching outcome
      Given an active control client sends Snapshot Current with nonzero correlation identifier 3003
      When the snapshot operation completes with <snapshot outcome>
      Then its Snapshot Current result identifies 3003 with <snapshot outcome>

      Examples:
        | snapshot outcome |
        | success          |
        | failure          |

    Scenario Outline: Legacy Refresh Rate receives an uncorrelated result
      Given an active control client sends a valid legacy Refresh Rate request
      When the refresh-rate operation completes with <refresh outcome>
      Then it receives a <refresh outcome> refresh-rate result without a request identifier

      Examples:
        | refresh outcome |
        | success         |
        | failure         |

    Scenario Outline: Correlated v2 Refresh Rate receives its matching result
      Given an active control client sends a correlated v2 Refresh Rate request with identifier 2002
      When the refresh-rate operation completes with <refresh outcome>
      Then it receives a <refresh outcome> refresh-rate result carrying identifier 2002

      Examples:
        | refresh outcome |
        | success         |
        | failure         |

    Scenario: A zero v2 Refresh Rate identifier remains uncorrelated
      Given an active control client sends a valid v2 Refresh Rate request with identifier zero
      When the refresh-rate operation completes
      Then it receives a refresh-rate result without a request identifier

    Scenario: Ping acknowledges liveness without waiting for display work
      Given an active control client
      When it sends Ping while display work is pending
      Then the helper records the client as live
      And it returns a Ping acknowledgement without waiting for that display work

    Scenario Outline: A timed-out correlated response cannot be reused
      Given a client is waiting for a correlated <command> response
      When the response deadline expires without that request's matching reply
      Then the client treats the request as unsuccessful
      And it retires the affected connection before a late reply can answer a later request

      Examples:
        | command          |
        | Apply            |
        | Snapshot Current |
        | Refresh Rate     |

  Rule: A replacement client cannot inherit stale control or completion traffic

    Scenario Outline: Commands from a retired connection are ignored
      Given control client A owns control-session identity A
      And client A is retired because of <retirement cause>
      When a pending command from control session A reaches its control boundary
      Then it does not alter the current display session
      And it cannot disarm recovery or replace a restore baseline

      Examples:
        | retirement cause       |
        | a connection error     |
        | a disconnect           |
        | a liveness timeout     |

    Scenario: Late completions cannot reply through a replacement client
      Given client A started a correlated request in control session A
      And client B is now the active control client in replacement control session B
      When the result for client A's request arrives
      Then client B receives no result for client A's request
      And the late result cannot be treated as an outcome for client B's request

    Scenario Outline: Superseded work has no current-session effect
      Given work belongs to <obsolete ownership>
      When that work reaches a command or completion boundary
      Then it produces no current-session transition
      And it produces no reply through the current control connection

      Examples:
        | obsolete ownership                       |
        | a retired control session                |
        | work cancelled by newer control intent   |

    Scenario Outline: Replacement control intent takes precedence over obsolete completion traffic
      Given a completion from an earlier control intent is pending
      And a replacement client supplies <replacement intent>
      When the helper processes the replacement intent
      Then it treats the replacement intent as the current control decision
      And the earlier completion cannot regain control of the session

      Examples:
        | replacement intent |
        | Apply              |
        | Revert             |
        | Disarm             |
        | Reset              |

    Scenario: A reconnect cancels the earlier disconnect grace
      Given a disconnected client has started a pending disconnect grace period
      When a replacement control client reconnects and supplies current control intent
      Then the earlier disconnect does not trigger restoration for the replacement client
      And the replacement client's intent is evaluated as the live session intent

    Scenario: A completed old restore does not terminate a newer live session
      Given a restore originated from a retired control session
      And a newer control client is active
      When that older restore completes successfully
      Then the helper remains available for the newer control client

    Scenario: A broken connection leaves only autonomous safety behavior eligible
      Given an active control connection breaks while the helper has display safety work to perform
      When the helper retires that connection
      Then it removes that connection's pending control intent and reply authority
      And only helper-owned safety behavior may continue for the retired session

  Rule: Stop ends the helper without abandoning required cleanup

    Scenario: Stop requests a clean helper shutdown
      Given an active control client
      When it sends Stop
      Then the helper stops accepting further control work
      And it completes required shutdown cleanup before it exits

  Rule: Public command forms retain their compatibility boundary

    Scenario Outline: Only Apply and Refresh Rate reject invalid required request data
      Given an active control session
      When it sends <command> with <wire-invalid required data>
      Then the helper rejects that wire request before state dispatch or returns its immediate failed result without display mutation
      And that wire-invalid payload has no display-control side effect
      And any result remains attributable only to the sending session

      Examples:
        | command      | wire-invalid required data                           |
        | Apply        | no parseable display configuration                   |
        | Refresh Rate | missing, zero, or incomplete rate or device identity |

    Scenario Outline: Compatibility controls still dispatch with unexpected or malformed optional data
      Given an active control session sends <command> with <payload condition>
      When the helper receives the command
      Then it preserves the command's compatible control effect
      And it uses current or default optional policy where the metadata is unusable
      And it does not invent a completion result unless that command normally has one

      Examples:
        | command          | payload condition                                      |
        | Revert           | malformed, nonobject, or extra optional metadata       |
        | Reset            | arbitrary extra data                                   |
        | Disarm           | arbitrary extra data                                   |
        | Export Golden    | malformed exclusions or arbitrary extra data           |
        | Snapshot Current | malformed exclusions, unknown metadata, or extra data  |
        | Ping             | arbitrary extra data                                   |
        | Stop             | arbitrary extra data                                   |

    Scenario Outline: A parseable Apply uses the documented optional-field defaults
      Given a parseable Apply configuration omits or has a non-activating <field>
      When the helper accepts the request
      Then the effective <field> is <effective behavior>
      And the request retains no unintended alternate display policy

      Examples:
        | field                         | effective behavior                                     |
        | restore-on-disconnect          | enabled                                                |
        | omit-final-initial-HDR-reapply | disabled                                               |
        | HDR-blanking Boolean           | disabled when absent                                   |
        | golden-first Boolean           | absent when not Boolean                                |
        | virtual-layout text            | absent when not text                                   |
        | topology                       | absent unless it contains at least one nonempty group  |
        | monitor position               | absent unless both coordinates are whole numbers       |
        | per-device refresh override    | absent unless both values are unsigned whole numbers   |
        | request identifier             | uncorrelated when absent, nonnumeric, or zero          |

    Scenario: A malformed Apply optional field retains parser-compatible failure or fallback behavior
      Given an Apply includes malformed optional metadata that is not a documented request form
      When the helper evaluates the complete Apply payload
      Then it may return a failed Apply result if the remaining payload is not accepted
      And if the remaining configuration is accepted, the malformed metadata does not opt into its alternate policy

    Scenario Outline: The result form is compatible with the command's correlation form
      Given a valid <command> was accepted in <compatibility form>
      When it reaches its terminal observable outcome
      Then the helper emits <reply behavior>
      And that reply cannot satisfy a different request or a later control session

      Examples:
        | command          | compatibility form                    | reply behavior                                            |
        | Apply            | nonzero unsigned JSON-integer identifier | Apply result and verification result with that identifier |
        | Apply            | absent, signed, floating, nonnumeric, or zero identifier | Apply result using identifier zero; verification uses zero |
        | Snapshot Current | nonzero unsigned snapshot identifier   | Snapshot result with that identifier                       |
        | Snapshot Current | absent or unusable snapshot identifier | no completion result                                      |
        | Refresh Rate     | v2 marker and nonzero identifier       | Refresh Rate result with that identifier                   |
        | Refresh Rate     | legacy form or zero identifier         | Refresh Rate result without an identifier                  |
        | Revert           | any accepted form                      | dispatch-only; no invented completion result               |
        | Disarm           | any accepted form                      | dispatch-only; no invented completion result               |
        | Reset            | any accepted form                      | dispatch-only; no invented completion result               |
        | Export Golden    | any accepted form                      | dispatch-only; no invented completion result               |
        | Log Level        | any accepted form                      | dispatch-only; no invented completion result               |
        | Stop             | any accepted form                      | dispatch-only; no invented completion result               |

    Scenario: A complete length-declared message never borrows trailing data
      Given a length-declared control message contains one complete command and later bytes
      When the helper reads the command
      Then its first four little-endian bytes declare a positive length counting the following command byte and payload
      And only the declared command and declared payload determine its control intent
      And later bytes cannot change that command's request identity or outcome

    Scenario: A short or malformed length declaration is compatible with the legacy raw form
      Given a control message does not contain a complete positive length declaration
      When its first byte identifies a supported command
      Then the helper interprets the remaining bytes as that command's legacy raw payload
      And it retains the legacy reply behavior for that payload

    Scenario: A foreign or late reply cannot become a current session result
      Given a control session has ended or has been replaced
      When an earlier request reaches a result boundary
      Then no reply is delivered through the current session
      And the new session retains sole ownership of its own replies

    Scenario Outline: Public command codes retain their established meanings
      Given a peer uses the established control-code vocabulary
      When it sends code <code>
      Then the helper interprets it as <command>
      And no other code is accepted as an alias for that command

      Examples:
        | code | command          |
        | 1    | Apply            |
        | 2    | Revert           |
        | 3    | Reset            |
        | 4    | Export Golden    |
        | 5    | Log Level        |
        | 7    | Disarm           |
        | 8    | Snapshot Current |
        | 10   | Refresh Rate     |
        | 254  | Ping             |
        | 255  | Stop             |

    Scenario Outline: Public result codes retain their established reply meanings
      Given a peer receives a result from the helper
      When the result uses code <code>
      Then it interprets the result as <result>
      And it applies the established correlation rule for that result type

      Examples:
        | code | result              |
        | 6    | Apply Result        |
        | 9    | Verification Result |
        | 11   | Refresh Rate Result |
        | 12   | Snapshot Result     |

    Scenario Outline: Startup log-level parsing maps accepted text and digit values to seven levels
      Given helper startup receives <startup value> through either supported startup argument form
      When the value is parsed case-insensitively
      Then the effective startup level is <level>
      And absent or invalid startup input uses informational level

      Examples:
        | startup value | level         |
        | verbose       | verbose       |
        | DEBUG         | debug         |
        | info          | informational |
        | warning       | warning       |
        | error         | error         |
        | fatal         | fatal         |
        | none          | none          |
        | 0             | verbose       |
        | 6             | none          |

    Scenario Outline: Live Log Level maps its first numeric byte and ignores the rest
      Given an active control session has an established log level
      When it sends a nonempty Log Level payload whose first numeric byte is <first byte>
      Then the live effective level is <level>
      And later bytes do not change that level
      And no completion result is sent

      Examples:
        | first byte | level   |
        | 0          | verbose |
        | 1          | debug   |
        | 6          | none    |
        | 7          | none    |
        | 255        | none    |

    Scenario: Refresh Rate v2 correlation requires its established marker and complete fields
      Given a peer sends a Refresh Rate request using the v2 marker
      When it includes positive numerator and denominator, a nonzero identifier, and a nonempty device identity
      Then the result carries that identifier

    Scenario: Refresh Rate request bytes retain their v2 and legacy schemas
      Given a peer encodes a Refresh Rate request after its command byte
      When it uses the v2 form
      Then the payload begins with hexadecimal bytes 53 46 52 32, the ASCII marker SFR2, followed by a little-endian unsigned 32-bit numerator, a little-endian unsigned 32-bit denominator, a little-endian unsigned 64-bit request identifier, and all remaining bytes as the device identity
      But the legacy form omits the four marker bytes and request identifier while retaining numerator, denominator, and remaining device bytes

    Scenario Outline: Result payload bytes retain their public binary schema
      Given the helper emits <result>
      When a compatible peer decodes its payload after the result code
      Then the payload contains <schema>

      Examples:
        | result                         | schema                                                                 |
        | Apply Result                   | one success byte followed by the little-endian unsigned 64-bit Apply identifier, with raw parse-error bytes optionally following a failed parse |
        | Verification Result            | one success byte followed by the little-endian unsigned 64-bit Apply identifier |
        | Snapshot Result                | one success byte followed by the little-endian unsigned 64-bit Snapshot identifier |
        | correlated Refresh Rate Result | one success byte followed by its little-endian unsigned 64-bit identifier |
        | legacy Refresh Rate Result     | one success byte and no identifier                              |
        | Ping acknowledgement           | no payload after public code 254                                |

    Scenario: Refresh Rate legacy compatibility keeps its untagged result form
      Given a peer sends a Refresh Rate request without the v2 marker
      When it includes positive numerator and denominator and a nonempty device identity
      Then the result has no request identifier
      And it cannot satisfy a later correlated request

    Scenario: Invalid Refresh Rate data produces only an applicable uncorrelated failure
      Given a peer sends Refresh Rate with a missing or zero rate component or no device identity
      When the helper evaluates the request
      Then it returns a failed Refresh Rate result
      And that result carries an identifier only when a valid nonzero v2 identifier was supplied
