@display-engine-v2
Feature: Display engine v2 configuration and mode policy
  Persisted, per-session, and runtime choices become one bounded display intent
  without inventing values when an input is disabled, invalid, or uncertain.

  Rule: Stored display vocabulary has stable values and safe fallbacks

    Scenario Outline: Public option text maps to its established display policy
      Given a saved <setting> has the text <value>
      When Sunshine parses the display setting
      Then its effective policy is <policy>
      And only the exact listed spelling is accepted while other casing follows the unrecognized fallback

      Examples:
        | setting                 | value                       | policy                       |
        | device preparation      | disabled                    | disabled                     |
        | device preparation      | verify_only                 | verify only                  |
        | device preparation      | ensure_active               | ensure active                |
        | device preparation      | ensure_primary              | ensure primary               |
        | device preparation      | ensure_only_display         | ensure only that display     |
        | device preparation      | an unrecognized value       | disabled                     |
        | resolution selection    | disabled                    | disabled                     |
        | resolution selection    | auto                        | automatic                    |
        | resolution selection    | manual                      | manual                       |
        | resolution selection    | an unrecognized value       | disabled                     |
        | refresh selection       | disabled                    | disabled                     |
        | refresh selection       | auto                        | automatic                    |
        | refresh selection       | manual                      | manual                       |
        | refresh selection       | prefer_highest              | prefer highest               |
        | refresh selection       | an unrecognized value       | disabled                     |
        | HDR selection           | disabled                    | disabled                     |
        | HDR selection           | auto                        | automatic                    |
        | HDR selection           | an unrecognized value       | disabled                     |
        | HDR request override    | auto                        | automatic                    |
        | HDR request override    | force_on                    | force on                     |
        | HDR request override    | force_off                   | force off                    |
        | HDR request override    | an unrecognized value       | automatic                    |
        | helper engine           | v2                          | v2                           |
        | helper engine           | legacy                      | legacy                       |
        | helper engine           | auto                        | automatic                    |
        | helper engine           | an unrecognized value       | automatic                    |
        | virtual-display mode    | disabled                    | disabled                     |
        | virtual-display mode    | per_client                  | per client                   |
        | virtual-display mode    | shared                      | shared                       |
        | virtual-display mode    | an unrecognized value       | per client                   |
        | virtual-display layout  | exclusive                   | exclusive                    |
        | virtual-display layout  | extended                    | extended                     |
        | virtual-display layout  | extended_primary            | extended primary             |
        | virtual-display layout  | extended_isolated           | extended isolated            |
        | virtual-display layout  | extended_primary_isolated   | extended primary isolated    |
        | virtual-display layout  | an unrecognized value       | exclusive                    |

    Scenario Outline: An absent setting retains its product default
      Given a new installation has no saved value for <setting>
      When Sunshine forms its display policy
      Then <setting> has the default <default>
      And that product default remains distinct from any helper wire default

      Examples:
        | setting                                  | default                    |
        | virtual-display mode                     | per client                 |
        | virtual-display layout                   | exclusive                  |
        | device preparation                       | verify only                |
        | resolution selection                     | automatic                  |
        | refresh selection                        | automatic                  |
        | HDR selection                            | automatic                  |
        | HDR request override                     | automatic                  |
        | revert delay                             | 3 seconds                  |
        | revert on disconnect                     | disabled                   |
        | always restore from golden               | enabled                    |
        | helper engine                            | automatic                  |
        | restore hotkey                           | disabled                   |
        | restore hotkey modifiers on Windows      | Control plus Alt plus Shift |
        | selected virtual-display backend         | Sunshine driver            |
        | activate selected virtual display        | disabled                   |
        | virtual-display scale                    | resolution based           |
        | permanent virtual-display count          | 0 and not explicitly set   |
        | snapshot exclusions                      | empty                      |
        | mode remapping                           | empty                      |
        | dummy-plug HDR workaround                | disabled                   |
        | Vulkan HDR exposure                      | enabled                    |

    Scenario: An unconfigured older Windows host keeps virtual display disabled
      Given the host is older than Windows 11
      And virtual-display mode is absent or empty
      When Sunshine applies platform defaults
      Then virtual-display mode is disabled
      And any explicit nonempty virtual-display mode is parsed instead of being suppressed by this compatibility default

    Scenario Outline: Virtual-display scale accepts only the published values
      Given whole-configuration application begins from the resolution-based scale default
      When a saved scale produces <parsed input>
      Then the effective scale is <result>
      And an unsupported value does not preserve a prior hot-loaded scale

      Examples:
        | parsed input                                      | result                                  |
        | integer -1                                        | resolution-based automatic scaling      |
        | integer 0                                         | preserve the operating-system choice    |
        | integer 100, 125, 150, 175, 200, 225, or 250     | that explicit percentage                |
        | integer 300, 350, 400, 450, or 500                | that explicit percentage                |
        | any other resulting integer                       | resolution-based automatic scaling      |

    Scenario: Shared integer text conversion is permissive before range policy
      Given a ranged display integer setting has a present saved value
      When Sunshine derives the integer that the setting will range-check
      Then an empty value produces zero; text beginning with a quote has its first and last characters removed; an ensuing lowercase 0x prefix selects the permissive hexadecimal conversion; and all other text uses positional base-ten arithmetic with only a leading minus treated specially
      And ordinary characters are not first validated as decimal digits, so arbitrary text may produce an in-range accepted integer rather than a distinct invalid-text outcome

    Scenario Outline: Revert and paused-cleanup delays have different absent-value behavior
      Given Sunshine is applying a complete configuration
      When <setting> has <parsed input>
      Then its effective value is <result>
      And accepted revert-delay integers are milliseconds while accepted paused-cleanup integers are seconds

      Examples:
        | setting                                | parsed input                                        | result                       |
        | revert delay                           | no value or a resulting integer outside 0 through 2,147,483,647 | the default 3 seconds remains |
        | revert delay                           | a resulting integer from 0 through 2,147,483,647    | that many milliseconds       |
        | paused virtual-display cleanup delay  | no value or a resulting integer outside 0 through 2,147,483,647 | 0 seconds                    |
        | paused virtual-display cleanup delay  | a resulting integer from 0 through 2,147,483,647    | that many seconds            |

    Scenario Outline: Display booleans share the legacy truth-text rules
      Given <setting> begins complete configuration application at <reset default>
      When Sunshine parses a saved Boolean value for that setting
      Then exact lowercase true, yes, enable, enabled, or on is enabled, as is any nonempty text containing the literal character 1
      And every other nonempty value, including differently cased truth words, is disabled, while absent or empty text retains <reset default>
      And the removed HDR-toggle switch is parsed by the same rules only for compatibility consumption and never changes current HDR behavior

      Examples:
        | setting                                      | reset default |
        | revert on disconnect                         | disabled      |
        | always restore from golden                   | enabled       |
        | Sunshine virtual-display backend selection   | enabled       |
        | automatic virtual-display activation         | disabled      |
        | Vulkan HDR exposure                          | enabled       |
        | dummy-plug HDR workaround                    | disabled      |

    Scenario: Permanent virtual-display count preserves compatibility and explicitness
      Given a configuration may use the current permanent-count key or its legacy static-monitor alias
      When Sunshine parses the count
      Then the current key wins when both are present and only a permissively converted resulting integer within zero through four is accepted, otherwise the count is zero
      And the policy records whether either key was explicitly present so an absent zero remains distinguishable from an explicit zero

    Scenario: Removed HDR-toggle workarounds are consumed without effect
      Given saved configuration contains the legacy HDR-toggle switch or delay
      When Sunshine parses display configuration
      Then it accepts the obsolete keys without treating them as unknown
      And neither key changes current HDR behavior

  Rule: Runtime override layers remain narrow and deterministic

    Scenario: Display overrides are limited to the published session keys
      Given an application or paired client supplies runtime configuration overrides
      When Sunshine filters display-related keys
      Then among the wider runtime vocabulary it accepts the following display-automation keys:
        | key                                      |
        | adapter_name                             |
        | adapter_pnp_id                           |
        | dd_configuration_option                  |
        | dd_resolution_option                     |
        | dd_manual_resolution                     |
        | dd_refresh_rate_option                   |
        | dd_manual_refresh_rate                   |
        | dd_hdr_option                            |
        | dd_hdr_request_override                  |
        | dd_config_revert_delay                   |
        | dd_config_revert_on_disconnect           |
        | dd_paused_virtual_display_timeout_secs   |
        | dd_always_restore_from_golden            |
        | dd_display_helper_engine                 |
        | dd_snapshot_exclude_devices              |
        | dd_snapshot_restore_hotkey               |
        | dd_snapshot_restore_hotkey_modifiers     |
        | dd_use_sunshine_virtual_display_driver   |
        | dd_activate_virtual_display              |
        | dd_virtual_display_scale                 |
        | dd_virtual_display_permanent_count       |
        | dd_mode_remapping                        |
        | dd_wa_dummy_plug_hdr10                   |
      And a candidate key must be 1 through 128 alphanumeric or underscore characters

    Scenario: Runtime layers override the file without mutating it
      Given file configuration supplies one display value
      And the application and paired client supply accepted runtime layers for the same session
      When Sunshine forms effective session configuration
      Then the runtime value overlays the file value and the later paired-client layer overlays the application layer
      And clearing runtime overrides exposes the file value again without persisting the temporary values

    Scenario Outline: Adapter identity is a paired override
      Given inherited adapter selection is already available
      When a runtime layer supplies <adapter input>
      Then the effective adapter selection is <result>
      And a device identity never retargets display ownership by itself

      Examples:
        | adapter input                                      | result                                      |
        | a device identity without an adapter name          | the inherited adapter selection             |
        | an empty adapter name                              | no adapter name and no inherited identity    |
        | a nonempty name without a nonempty device identity | historical name-only matching                |
        | a nonempty name and nonempty device identity       | the paired exact preference                  |

    Scenario: First-session display preparation snapshots its adapter pair
      Given the first stream has resolved its effective adapter name and optional device identity
      When display preparation begins
      Then that effective pair becomes the active capture ownership choice
      And a later hot configuration change cannot retarget the already-owned session

    Scenario: Display-changing hot configuration reverts cached idle state
      Given one of the following effective settings changes:
        | setting                                      |
        | device preparation                           |
        | resolution selection or manual resolution    |
        | refresh selection or manual refresh          |
        | HDR selection or HDR request override        |
        | revert delay or revert-on-disconnect policy  |
        | paused virtual-display cleanup delay         |
        | selected virtual-display backend             |
        | automatic virtual-display activation         |
        | virtual-display scale                        |
        | permanent count or its explicitness          |
        | snapshot exclusions                          |
        | dummy-plug HDR workaround                    |
      And no stream is active, pending, stopping, or retaining capture ownership
      And no runtime configuration override remains
      When hot configuration is applied
      Then Sunshine requests display restoration to clear cached state
      And disabling previously enabled display configuration also stops idle helper supervision after the restore request

    Scenario: Live ownership suppresses the display hot-revert
      Given a display-changing hot setting changes
      And a stream lifecycle owner or any runtime configuration override remains
      When hot configuration is applied
      Then Sunshine does not revert the owned display state from that hot-change path
      And the live or deferred owner keeps responsibility for applying the effective policy

    Scenario: Non-cache settings do not trigger the display hot-revert by themselves
      Given only helper engine, restore hotkey, hotkey modifiers, golden preference, mode-remapping data, virtual mode, virtual layout, or Vulkan HDR exposure changes
      And no separately watched display setting changes
      When hot configuration is applied
      Then the cached-state hot-revert is not requested solely for that change
      And each setting remains available to its own next applicable action

  Rule: Snapshot exclusions and restore hotkeys accept compatible input forms

    Scenario Outline: Snapshot exclusions accept JSON compatibility forms
      Given the snapshot-exclusion value uses <form>
      When Sunshine parses the exclusions
      Then it retains each recognized nonempty display identity in encounter order
      And unsupported members do not create an exclusion

      Examples:
        | form                                                           |
        | a direct array of strings                                      |
        | a direct array of objects with device_id strings               |
        | a direct array of objects with id strings                      |
        | an object whose exclude_devices member is one of those arrays  |
        | an object whose devices member is one of those arrays          |

    Scenario: Snapshot exclusion aliases have fixed precedence without fallback
      Given an exclusion object may contain both exclude_devices and devices
      When Sunshine resolves the array source
      Then presence of exclude_devices selects it even when it is not an array, and devices is considered only when exclude_devices is absent
      And invalid or non-array data in the selected higher-priority member does not fall through to the lower alias
      And within an array object element a string device_id wins over a string id, while a missing or non-string device_id permits a string id

    Scenario: Snapshot exclusion fallback is used only for invalid JSON
      Given a snapshot-exclusion value is not valid JSON
      When Sunshine parses the exclusions
      Then it splits the raw value on commas
      And it trims outer whitespace, removes one surrounding quote pair, and skips empty identities

    Scenario: Valid nonarray exclusion JSON does not become comma text
      Given a snapshot-exclusion value is valid JSON but does not resolve to an array
      When Sunshine parses the exclusions
      Then the result is empty
      And it does not reinterpret that valid JSON as a comma-separated fallback

    Scenario Outline: Restore hotkey text maps to its established virtual key
      Given the restore-hotkey value is <input>
      When Sunshine parses the hotkey case-insensitively after trimming it
      Then the effective key is <result>
      And an out-of-range arithmetic result disables the hotkey while malformed text may still compute an accepted in-range result

      Examples:
        | input                                  | result                                  |
        | empty, disabled, none, or off          | disabled                                |
        | 0x01 through 0xFF with optional vk_    | that hexadecimal virtual key            |
        | F1 through F24 with optional vk_       | that function key                       |
        | one alphabetic character               | its uppercase virtual key               |
        | one decimal digit                      | that digit key                          |
        | decimal 1 through 255                  | that virtual key                        |

    Scenario: Malformed decimal and function-key text keeps permissive positional parsing
      Given restore-hotkey text reaches decimal parsing directly or as the suffix after a leading F
      When one or more characters are not decimal digits
      Then each character still contributes its raw character offset from digit zero in positional base-10 arithmetic, with only a leading minus sign treated specially
      And an arithmetic result from 1 through 255 is accepted as a virtual key even when the original text was not a valid decimal number
      And an F-prefixed result from 1 through 24 is translated to its corresponding function key before the general decimal fallback

    Scenario: Hexadecimal hotkey text with a suffix treats every alphabetic character as a convertible digit
      Given trimmed restore-hotkey text begins with 0x and has at least one following character after any optional vk_ prefix
      When its remaining characters are converted in right-to-left pairs
      Then decimal digits keep values 0 through 9 and every ASCII letter is case-folded and mapped from a as 10 upward without stopping at f
      And non-alphanumeric characters are skipped by that conversion
      And the computed value is accepted only from 1 through 255, so malformed text such as 0x0g is accepted as virtual key 16

    Scenario: A bare hexadecimal prefix falls through to permissive decimal parsing
      Given trimmed restore-hotkey text is exactly 0x after any optional vk_ prefix
      When Sunshine parses the hotkey
      Then it does not enter hexadecimal conversion because no suffix exists
      And permissive positional decimal conversion produces virtual key 72 from the characters 0 and x

    Scenario: Restore modifier text combines only recognized modifiers
      Given a Windows restore-modifier value contains tokens separated by plus, bar, comma, semicolon, or whitespace
      When Sunshine parses the value case-insensitively
      Then ctrl or control, alt, shift, and win, windows, or meta are combined without duplication
      And unknown tokens are ignored, while default means Control plus Alt plus Shift and disabled, none, off, or empty means no modifiers

    Scenario: Unsupported platforms expose no restore-hotkey modifiers
      Given restore modifiers are parsed on a platform without Windows global-hotkey support
      When Sunshine forms the restore-hotkey policy
      Then the effective modifier set is empty
      And no Windows hotkey registration behavior is implied

  Rule: A session configuration preserves parsing and precedence exactly

    Scenario: Disabled device preparation short-circuits configuration parsing
      Given device preparation is disabled
      And resolution, refresh, or HDR input may otherwise be invalid
      When Sunshine parses the session display configuration
      Then it returns configuration-disabled without parsing those later fields
      And it does not fabricate a partial Apply configuration

    Scenario Outline: Device preparation maps to the corresponding request intent
      Given device preparation is <configured choice>
      When Sunshine parses a session display configuration
      Then the request preparation is <request intent>
      And the target identity is the current effective output name

      Examples:
        | configured choice   | request intent            |
        | verify only         | verify only               |
        | ensure active       | ensure active             |
        | ensure primary      | ensure primary            |
        | ensure only display | ensure only that display  |

    Scenario: A client display-mode override has resolution priority
      Given a session carries a client display-mode override
      And saved resolution policy differs from the client's width and height
      When Sunshine parses resolution
      Then nonnegative client dimensions are used even when either dimension is zero
      And either negative dimension fails the whole configuration instead of falling back to saved policy

    Scenario Outline: Resolution syntax has one exact accepted shape
      Given manual or remapping resolution input is <input>
      When Sunshine parses the resolution after trimming outer whitespace
      Then the parse result is <result>
      And an accepted numeric component must fit from zero through 4,294,967,295

      Examples:
        | input                                      | result                                  |
        | digits, lowercase x, then digits           | the two unsigned dimensions             |
        | digits, multiplication sign, then digits   | the two unsigned dimensions             |
        | 0x0                                        | accepted zero dimensions                |
        | empty                                      | an absent optional resolution           |
        | uppercase X or spaces around the separator | failure                                 |
        | signs, decimals, or trailing text          | failure                                 |
        | a component beyond the supported range     | failure                                 |

    Scenario Outline: Resolution policy chooses only its defined source
      Given the client has no display-mode override
      And resolution selection is <selection>
      When Sunshine parses the session resolution
      Then the resolution result is <result>
      And no lower-priority source fills a missing or invalid required value

      Examples:
        | selection | result                                                               |
        | automatic | nonnegative session width and height, including zero; otherwise failure |
        | manual    | the required valid nonempty manual resolution; otherwise failure      |
        | disabled  | no resolution change                                                   |

    Scenario: Effective automatic refresh has a fixed source order
      Given refresh selection is automatic or a client display-mode override is active
      When Sunshine resolves the session refresh
      Then it prefers positive exact frame-generation millihertz, then positive client-display millihertz, then positive integer frame-generation hertz, then positive session FPS
      And it stores the selected millihertz over 1000 while no positive source fails the whole configuration

    Scenario Outline: Manual refresh syntax preserves its exact rational value
      Given manual or remapping refresh input is <input>
      When Sunshine parses the refresh after trimming outer whitespace
      Then the parse result is <result>
      And the retained whole-and-fraction digits forming the numerator must fit from zero through 4,294,967,295

      Examples:
        | input                                  | result                                      |
        | whole digits                           | that value over 1                           |
        | digits followed by dot and digits      | the decimal digits over their power of ten  |
        | leading integer zeroes                 | the same value without leading zeroes        |
        | fractional trailing zeroes             | the same value without those zeroes           |
        | zero or zero-point-zero                 | accepted zero over 1                          |
        | empty                                  | an absent optional refresh                    |
        | a sign, exponent, leading dot, or trailing dot | failure                               |
        | embedded whitespace or trailing text   | failure                                      |

    Scenario: Extreme decimal precision has no guaranteed denominator result
      Given decimal refresh text has a fitting numerator but enough retained fractional digits that ten to that digit count exceeds 4,294,967,295
      When Sunshine parses the otherwise accepted decimal syntax
      Then no separate denominator-range rejection is guaranteed before the decimal scale is converted
      And neither successful parsing nor a particular resulting denominator is a portable contract for that over-range edge

    Scenario Outline: Refresh policy chooses only its defined behavior
      Given the client has no display-mode override
      And refresh selection is <selection>
      When Sunshine parses the session refresh
      Then the refresh result is <result>
      And no lower-priority source repairs an invalid required value

      Examples:
        | selection      | result                                                       |
        | automatic      | the required positive effective session refresh over 1000    |
        | manual         | the required valid nonempty manual rational                   |
        | prefer highest | the exact 10000 over 1 operating-system maximum hint          |
        | disabled       | no refresh change                                              |

    Scenario Outline: HDR policy follows its strict precedence
      Given the session has <condition>
      When Sunshine parses source-display HDR intent
      Then the requested source-display HDR state is <result>
      And no lower-priority HDR choice overrides that result

      Examples:
        | condition                                                        | result                          |
        | app-enabled RTX HDR conversion                                    | disabled                        |
        | no RTX HDR and the dummy-plug HDR workaround                      | enabled                         |
        | neither higher choice and automatic HDR with effective HDR request | enabled                         |
        | neither higher choice and automatic HDR without effective HDR request | disabled                    |
        | neither higher choice and disabled HDR selection                  | no HDR change                   |

    Scenario: Effective HDR requires all session permission checks
      Given automatic HDR evaluates a client HDR request
      When Sunshine resolves effective HDR intent
      Then HDR is requested only when the client requested HDR, 10-bit SDR preference is off, and forced SDR is off
      And either SDR policy suppresses display HDR without rewriting the client's original request

    Scenario Outline: Host HDR request override rewrites effective session intent
      Given HDR request override is <override>
      When Sunshine forms an RTSP or WebRTC launch session
      Then effective session HDR is <HDR result>
      And SDR policy is <SDR result>

      Examples:
        | override  | HDR result                              | SDR result                                      |
        | automatic | the client's requested HDR state        | the client's 10-bit SDR preference remains      |
        | force on  | enabled                                 | 10-bit SDR preference and forced SDR are cleared |
        | force off | disabled                                | forced SDR is enabled                           |

    Scenario: WebRTC force-off rejects a client HDR request
      Given host HDR request override is force off
      When a WebRTC client explicitly requests HDR or validates HDR capability
      Then the request is rejected as disabled by host display policy
      And advertised WebRTC HDR policy reports that HDR is not allowed

    Scenario: WebRTC force-on requires an HDR-capable encoded format
      Given host HDR request override is force on
      When WebRTC validates or builds the requested video configuration
      Then the request requires encoded HEVC or AV1 with applicable Main10 capability before it may carry HDR
      And unavailable Main10 falls back to no HDR encode only where validation has not already rejected the request

    Scenario: Any required parse failure rejects the whole configuration
      Given device preparation is enabled
      And a required resolution, refresh, or selected remapping field is invalid
      When Sunshine parses the session display configuration
      Then it returns a parse failure
      And it does not dispatch or retain a partially parsed configuration

  Rule: Mode remapping uses one ordered list without normalization

    Scenario: A nonempty remapping value requires three named child paths but not collection types
      Given a saved mode-remapping value is nonempty
      When Sunshine parses that setting
      Then the value must be valid JSON with mixed, resolution_only, and refresh_rate_only child paths
      And malformed JSON or any missing required child raises a parsing exception without installing a partially parsed remapping
      But a scalar child is accepted as an empty list and an object child is iterated without requiring array shape

    Scenario: An absent or empty remapping value restores the reachable default policy
      Given the current live configuration may have a nonempty remapping policy
      And the next complete configuration has an absent or empty mode-remapping value
      When Sunshine applies that complete configuration from product defaults
      Then the effective policy becomes three empty remapping lists
      And skipping empty parser input does not preserve the prior hot-loaded mapping across the complete application

    Scenario: Malformed remapping during hot reload is not transactional
      Given a live configuration exists and a hot reload has malformed nonempty remapping JSON or omits a required child
      When remapping parsing raises its exception after complete configuration application has begun from product defaults
      Then the previous live configuration is not restored and defaults or fields parsed before the failure may remain effective
      And later state persistence and display hot-revert processing for that reload are not reached

    Scenario: Malformed remapping during startup is not normalized to a configuration result
      Given startup configuration has malformed nonempty remapping JSON or omits a required child
      When remapping parsing raises its non-filesystem exception
      Then the startup configuration boundary does not convert it to an ordinary unsuccessful-load result
      And the exception may escape and terminate startup

    Scenario: Remapping entries preserve input order and default missing fields
      Given each remapping child contains array elements or object-valued entries with any of requested_resolution, requested_fps, final_resolution, and final_refresh_rate
      When Sunshine parses the complete remapping value
      Then it retains child nodes in encounter order while ignoring their property names
      And each missing field becomes empty while unrecognized fields do not alter the four public fields
      And present scalar field values use textual scalar conversion rather than requiring a JSON string type

    Scenario Outline: Automatic choices select exactly one remapping list
      Given resolution selection is <resolution choice>
      And refresh selection is <refresh choice>
      When Sunshine chooses mode remapping
      Then it consults <list>
      And all other remapping lists are ignored for that configuration

      Examples:
        | resolution choice | refresh choice | list              |
        | automatic         | automatic      | mixed             |
        | automatic         | not automatic  | resolution only   |
        | not automatic     | automatic      | refresh only      |
        | not automatic     | not automatic  | no remapping      |

    Scenario: Client display-mode override bypasses every remapping list
      Given a session carries a client display-mode override
      And a remapping entry would otherwise match
      When Sunshine finishes the session configuration
      Then it keeps the client-derived resolution and refresh
      And it does not inspect or apply mode remapping

    Scenario: Only fields relevant to the selected remapping list are parsed
      Given a remapping entry contains both resolution and refresh fields
      And the selected list maps only one of those dimensions
      When Sunshine parses that entry
      Then it requires valid requested and final syntax only for the selected dimension
      And invalid ignored fields do not fail or affect the entry

    Scenario: Remapping requested fields are optional wildcards
      Given a selected remapping entry has an empty requested resolution or requested FPS
      When Sunshine compares the entry with the already parsed session mode
      Then each empty requested field matches any value for its selected dimension
      And a nonempty requested field must equal that dimension exactly

    Scenario: Every reached remapping entry requires a final value
      Given an entry in the selected list has no applicable final resolution and no applicable final refresh
      When ordered remapping reaches that entry
      Then the whole configuration fails
      And no later entry is considered

    Scenario: The first valid matching remapping entry wins
      Given the selected remapping list contains multiple valid entries
      When Sunshine scans them in configured order
      Then it applies the applicable final values from the first matching entry and stops
      And final fields absent from that entry preserve the already parsed dimension

    Scenario: An invalid reached entry fails before any later match
      Given an invalid entry appears before a later valid matching entry in the selected list
      When Sunshine scans the list in configured order
      Then the whole configuration fails at the invalid entry
      And the later match is not used

    Scenario: A match prevents later invalid data from being reached
      Given a valid matching entry appears before an invalid entry in the selected list
      When Sunshine scans the list in configured order
      Then it applies the first match successfully
      And the later invalid entry does not affect that configuration

    Scenario: No remapping match is a successful no-op
      Given every reached selected entry is valid but none matches
      When Sunshine finishes ordered remapping
      Then it keeps the already parsed resolution and refresh
      And absence of a match does not turn the configuration into a failure

    Scenario: Remapping refresh equality does not normalize equivalent fractions
      Given an automatic session refresh is represented as 60000 over 1000
      And a requested remapping FPS is represented as 60 over 1
      When Sunshine compares the requested refresh values
      Then the entry does not match because numerator and denominator are not both identical
      And numerical equivalence alone does not normalize them for remapping

    Scenario: Requested remapping FPS accepts whole digits only
      Given a selected remapping entry has a requested FPS
      When Sunshine parses that match field
      Then empty or whole digits are accepted using refresh rational rules
      And a decimal requested FPS fails even though a decimal final refresh is accepted

  Rule: Refresh-override detection preserves its narrower compatibility test

    Scenario: Manual refresh always reports an active override
      Given refresh selection is manual
      When Sunshine asks whether refresh override behavior is active
      Then it reports active before validating the manual text or client mode override
      And remapping data is not consulted for that result

    Scenario: Client display-mode override disables remapping-based detection
      Given refresh selection is not manual
      And a client display-mode override is active
      When Sunshine asks whether refresh override behavior is active
      Then it reports inactive
      And a matching refresh-remapping entry does not change that result

    Scenario: Remapping-based detection uses integer frame-generation FPS before session FPS
      Given the selected automatic remapping list includes refresh fields
      When Sunshine derives the detection target
      Then it uses positive integer frame-generation FPS when present and otherwise session FPS
      And a negative target reports inactive while zero remains a valid zero-over-one target

    Scenario: Detection requires the first eligible final refresh match
      Given refresh-override detection scans the selected list in order
      When it reaches entries
      Then an entry without final refresh is skipped and the first wildcard or exact requested-FPS match with final refresh reports active
      And an invalid reached entry reports inactive without considering later entries

    Scenario: Detection deliberately ignores requested resolution
      Given mixed remapping has an FPS match with final refresh but its requested resolution differs from the session
      When Sunshine asks whether refresh override behavior is active
      Then detection may report active even though full mode remapping will not apply that entry
      And callers retain that observable distinction instead of treating detection as proof of a remap

  Rule: Output identity distinguishes absence, inactivity, and uncertainty

    Scenario: Empty output identity preserves primary-display semantics
      Given the effective output name is empty
      When Sunshine maps or checks that output
      Then mapping preserves the empty name and existence and activity checks return possible and active
      And no enumeration is required to invent a concrete target

    Scenario: A Windows logical display name passes through unchanged
      Given an output begins case-insensitively with the Windows logical-display prefix
      When Sunshine maps that output for capture
      Then it returns the supplied output unchanged
      And it does not require a device or friendly-name lookup

    Scenario: A known identity maps to its logical display name
      Given a nonempty output exactly matches a device identity, logical display name, or friendly name case-insensitively
      When Sunshine maps the output using minimal display evidence
      Then it returns the first matching device's nonempty logical display name
      And existence reports true while activity reports whether that logical name is present

    Scenario: A match without a logical display name preserves the original input
      Given the first matching device has no logical display name
      When Sunshine maps the output
      Then it returns the original input
      And the activity check remains false even though existence is true

    Scenario: An unknown output remains distinct from enumeration uncertainty
      Given minimal enumeration succeeds but no device identity matches the output
      When Sunshine maps and checks the output
      Then mapping preserves the original input while existence and activity report false
      And the result may drive configured-output fallback policy

    Scenario: Enumeration failure avoids false-negative output policy
      Given minimal display enumeration fails unexpectedly
      When Sunshine maps and checks a nonempty output
      Then mapping preserves the original input while existence and activity report possible and active
      And callers do not manufacture a missing or inactive display fallback from that uncertainty

    Scenario: Platforms without Windows identity mapping pass through safely
      Given output identity is evaluated on a platform without Windows display enumeration
      When Sunshine maps or checks the output
      Then mapping preserves the supplied value and existence and activity report possible and active
      And no Windows logical-name guarantee is implied
