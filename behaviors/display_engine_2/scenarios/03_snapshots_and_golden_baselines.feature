@display-engine-v2
Feature: Display engine v2 snapshots and golden baselines
  The display engine captures, filters, and prioritizes eligible display
  baselines so later recovery can protect the physical desktop from managed
  virtual displays.

  Rule: Storage identity and startup discovery preserve cross-engine recovery compatibility

    Scenario Outline: Active baseline storage follows identity scope and deterministic fallback order
      Given the helper runs as <identity>
      When it selects the active persistence root
      Then it tries <location-order>
      And the first successfully resolved location becomes the active root

      Examples:
        | identity                    | location-order                                                                                                      |
        | an ordinary interactive user | per-user roaming application data, configured roaming application data, temporary storage, then current-directory Sunshine storage |
        | the machine SYSTEM identity | shared machine application data, then the ordinary per-user fallback order                                         |

    Scenario: Persistence-root creation is best effort rather than a fallback probe
      Given identity lookup selects a preferred baseline root
      When creating that root fails
      Then the helper continues with the selected root instead of advancing solely because creation failed
      And later reads or writes may fail without changing the location order

    Scenario: Compatible state filenames remain stable across helper engines
      Given either compatible display-helper engine uses a selected persistence root
      When it addresses persisted display state
      Then Golden uses "display_golden_restore.json" and golden health uses "display_golden_restore_status.json"
      And Current uses "display_session_current.json" and Previous uses "display_session_previous.json"
      And persisted helper-engine selection, exclusion, and managed-virtual identity state uses "vibeshine_state.json"

    Scenario: Startup search roots preserve order while removing duplicates
      Given the helper starts with persisted state possibly written by another identity or compatible installation layout
      When it builds its discovery order
      Then it searches per-user storage before shared machine storage
      And it then searches the compatible configuration location beside the executable, the configuration location one level above, and that parent location
      And roots equal as filesystem path values are removed without reordering the remaining roots
      But differently spelled aliases of the same location are not guaranteed to be deduplicated

    Scenario: Current and Previous independently adopt the first payload-valid startup source
      Given startup search roots contain missing, damaged, or compatible session baselines
      When the helper discovers Current and Previous
      Then it searches each tier independently in the deterministic root order
      And an earlier missing or payload-invalid tier is skipped and its invalid data is removed on a best-effort basis
      And the first tier payload containing both extractable topology and mode data is selected
      And Current and Previous may therefore originate from different roots

    Scenario: A failed transfer of the first compatible startup source does not continue discovery
      Given the first payload-valid Current or Previous source is outside the active persistence root
      When transferring that source into active storage fails
      Then startup does not report the transfer as a recovery guarantee
      And it does not continue to a later valid source for that tier during the same startup pass

    Scenario: Cross-root adoption preserves the source and may repeat on a later startup
      Given startup adopts a payload-valid Current or Previous tier from a compatible root outside active storage
      When it copies that tier into the active persistence root with replacement allowed
      Then it leaves the compatible source file in place
      And a later startup may adopt that still-present source again, including after the active copy was changed or removed
      And adoption directly overwrites the active tier rather than moving the source
      But it does not guarantee complete-old-or-complete-new observation by a concurrent reader

    Scenario: Cross-root startup adoption is limited to session tiers
      Given compatible roots contain Golden, golden-health, Current, and Previous data
      When startup performs cross-root baseline adoption
      Then it independently adopts only Current and Previous into the active root
      And Golden and golden-health data remain scoped to the selected active root

    Scenario: Startup exclusion discovery stops only when a state document satisfies the loader predicate
      Given ordered search roots may contain persisted snapshot exclusions and managed virtual-display identities
      When startup loads baseline exclusions
      Then it skips unreadable documents, documents without an object root, and documents whose recognized fields do not satisfy the stopping predicate
      And a root-level snapshot_exclude_devices array selects its document even when that array is empty
      And a parsed exclusion or managed-virtual list with at least one recognized identity entry also selects its document
      But an empty object-wrapped exclusion list or an empty managed-virtual list alone does not stop discovery
      And the selected document's two parsed lists are merged, empty identities are omitted, repeated identities coalesce for filtering, and later roots are not merged

  Rule: Shared persisted state retains helper-engine selection, exclusions, and recent managed virtual identities exactly

    Scenario: Persisting the selected display-helper engine writes only a changed non-empty value
      Given shared state is safe to update
      When a non-empty helper-engine value differs from root.display_helper_engine
      Then a successful replacement stores that value while retaining other readable compatible state
      But an empty value or unchanged value does not rewrite the document
      And an unsafe prior-state read or failed replacement provides no persistence success guarantee

    Scenario: Saving snapshot exclusions replaces the persisted list with non-empty supplied entries
      Given the supplied exclusion list contains non-empty identities, empty identities, or repeated identities
      When that list is persisted successfully
      Then empty identities are omitted
      And every non-empty entry, including repetitions, is retained in supplied order
      And an explicitly empty list replaces the earlier persisted values so application loading returns an empty list
      And other fields from a readable compatible prior document remain present in the updated document

    Scenario Outline: Unavailable exclusion state loads as an empty list
      Given persisted exclusion state has <state-condition>
      When the current exclusion list is loaded
      Then the result is an empty list

      Examples:
        | state-condition                                      |
        | no usable state path                                 |
        | a missing, unreadable, or malformed state document   |
        | no compatible root object                            |
        | no snapshot-exclusion field                          |

    Scenario: Loading managed virtual identities preserves stored order and repetitions
      Given the persisted managed-virtual list contains non-empty, empty, or repeated identities
      When those identities are loaded
      Then empty entries are omitted
      And non-empty entries preserve their stored spelling, order, and repetitions
      And an unavailable document, root, or managed-virtual field yields an empty list

    Scenario: Remembering an empty managed virtual identity changes nothing
      Given persisted display state already exists
      When the application is asked to remember an empty managed virtual identity
      Then it does not rewrite or reorder the persisted managed-virtual list

    Scenario: Remembering an existing managed virtual identity avoids a rewrite
      Given a managed virtual identity is already persisted with particular spelling and position
      When the same-length identity is remembered again using only different ASCII letter casing
      Then the persisted state is not rewritten
      And the original spelling and position remain unchanged
      But surrounding whitespace is identity data and can make an otherwise similar value distinct

    Scenario: Remembering a new managed virtual identity appends it and retains the newest 16 entries
      Given the remembered managed-virtual list does not already contain the new identity under ASCII case-insensitive comparison
      When that non-empty identity is persisted successfully
      Then it is appended after the retained existing entries
      And if the append makes more than 16 entries, the oldest entries are removed until the newest 16 remain
      And the retained entries preserve insertion order

    Scenario: The managed-virtual retention cap is enforced only when a new identity is appended
      Given older persisted state already contains more than 16 non-empty managed-virtual entries
      When an identity already present under ASCII case-insensitive comparison is remembered
      Then the no-rewrite branch leaves the oversized list unchanged
      But appending a genuinely new identity trims the result to its newest 16 entries

    Scenario Outline: State updates distinguish recreatable content from unsafe read failure
      Given a helper-engine, exclusion, or new managed-virtual update targets <existing-state>
      When the update is attempted
      Then <update-result>

      Examples:
        | existing-state                                  | update-result                                                                 |
        | a missing or blank state document               | a successful write creates a new compatible document                         |
        | a readable but malformed state document         | quarantine is attempted; a successful update recreates state, but failed quarantine may replace the corrupt path without retaining a forensic copy |
        | a path that cannot be inspected or read safely  | the update is refused rather than clobbering uncertain prior state            |

    Scenario: Failed state replacement does not report a persisted helper-engine, exclusion, or identity update
      Given prior compatible state exists
      And a helper-engine, exclusion, or new managed-virtual update has been prepared
      When replacement of the persisted document fails
      Then the update operation completes without a success guarantee
      And failure before replacement leaves the prior persisted state in place

  Rule: A persisted baseline has a compatible payload but earns recovery eligibility separately

    Scenario Outline: Snapshot fields have explicit save and load compatibility rules
      Given a baseline payload contains <field-set>
      When it is saved or later loaded for recovery
      Then <compatibility-result>

      Examples:
        | field-set                                                                                         | compatibility-result                                                                                                      |
        | valid non-empty topology, non-empty modes, and at least one eligible physical identity             | it is a saveable restore baseline; HDR, primary selection, origins, version, and layouts are recorded when available   |
        | topology and modes with HDR state, primary selection, or origins absent                             | it remains a legacy-compatible payload; missing optional fields are not invented                                          |
        | topology and modes but no layout version or layout records                                          | it remains compatible and does not require layout confirmation                                                            |
        | layout records in any compatible payload only for some eligible devices                             | only present, eligible layout rotation values participate in restoration and confirmation                                 |
        | neither extractable topology nor extractable modes                                                  | it is not adopted as a baseline payload                                                                                    |
        | damaged syntax with extractable topology or mode data and parseable numeric text                     | extracted fields remain a candidate only after filtering, application, and on-screen confirmation                         |
        | topology and display-settings records naming different device sets                                  | the payload is not rejected solely for that mismatch; later application and confirmation decide its usefulness            |

    Scenario: Newly saved baselines retain the version-2 field contract
      Given a valid filtered baseline is saved for compatibility with either helper engine
      When its persistence payload is produced
      Then "snapshot_version" is 2
      And "topology" records duplicate groups while "modes" records "w", "h", "num", and "den" for each retained identity
      And "hdr" records "on", "off", or null, "primary" records the selected identity, and "origins" records "x" and "y"
      And "layouts" records retained display rotations and remains present even when no rotation was captured

    Scenario Outline: Snapshot version metadata defaults without rejecting future integers
      Given a baseline carries <version-form>
      When compatibility metadata is loaded
      Then its effective snapshot version is <effective-version>
      And layout availability is decided by compatible layout records rather than by that version number alone

      Examples:
        | version-form                         | effective-version       |
        | no "snapshot_version" field        | 1                       |
        | a non-integer "snapshot_version"   | 1                       |
        | an integer less than 1               | 1                       |
        | integer 2                            | 2                       |
        | a future positive integer within the supported integer range | that future integer     |

    Scenario Outline: Compatible layout rotation forms normalize to cardinal degrees
      Given a layout identity carries <rotation-form>
      When recovery parses its layout metadata
      Then <rotation-result>

      Examples:
        | rotation-form                                                | rotation-result                                                   |
        | integer 0, 90, 180, or 270                                   | that cardinal rotation is retained                                |
        | an integer equivalent modulo 360, such as -90 or 450         | it is normalized to 270 or 90 respectively                        |
        | ASCII-case variants of landscape, portrait, landscape_flipped, or portrait_flipped text | they are retained as 0, 90, 180, or 270 respectively |
        | ASCII-case variants of landscape_inverted or portrait_inverted text                       | they are retained as 180 or 270 respectively          |
        | a hyphenated or whitespace-padded rotation name                                             | that identity has no retained layout requirement       |
        | an object containing one of the compatible rotation forms    | its nested rotation is retained                                   |
        | a non-cardinal or otherwise incompatible value               | that identity has no retained layout requirement                  |

    Scenario Outline: HDR compatibility values preserve enabled, disabled, and unknown state
      Given an HDR identity carries <stored-value>
      When compatibility data is loaded
      Then its retained HDR state is <loaded-state>

      Examples:
        | stored-value                 | loaded-state       |
        | lowercase on, or another stored token beginning with lowercase on   | enabled  |
        | lowercase off, or another stored token beginning with lowercase off | disabled |
        | uppercase ON or OFF, null, or a value with neither lowercase prefix | unknown  |

    Scenario: Missing numeric members default but malformed numeric text is not safely recoverable
      Given a compatibility payload contains a mode or origin record
      When a required numeric member is absent
      Then that missing member is extracted as zero and later eligibility, application, and confirmation decide the candidate
      But when present numeric text causes integer conversion to fail, direct active-tier recovery does not map it to a normal unavailable candidate
      And helper execution can terminate before recovery continues to another tier

    Scenario: Numeric compatibility does not require consuming the complete raw value
      Given raw mode or origin text after its member separator begins with a convertible decimal prefix followed by other characters
      When compatibility data is loaded
      Then the leading numeric value is retained without requiring the trailing text to be consumed
      But no convertible prefix or an out-of-range conversion follows the documented escaping failure path

    Scenario: Startup payload validation is stricter than direct active-tier loading
      Given a baseline contains only extractable topology or only extractable mode data
      When startup discovery evaluates it as a cross-root source
      Then it is rejected because discovery requires both topology and mode payload
      But direct loading from an already active tier may retain either form initially
      And recovery rejects mode-only data while an eligible non-empty topology can let topology-only data proceed

    Scenario: The latest baseline format retains legacy data without promoting it to a stricter schema
      Given a legacy or syntactically damaged payload is extractable
      When it is read
      Then absent origins remain compatible rather than unequal
      And malformed or absent layout rotation metadata is treated as unavailable rather than inferred
      And topology enumeration order does not change desktop equality
      And differing origins make otherwise origin-aware baselines unequal

    Scenario: Baseline replacement protects the prior chain only until overwrite begins
      Given Current and Previous are valid
      When a Current refresh is requested
      Then the engine captures and filters the candidate before changing either saved tier
      And it attempts Current-to-Previous history before saving the new Current
      And a history-save failure does not prevent a valid new Current from being saved
      And a failed capture before replacement begins leaves the known-good chain intact
      But once replacement of an existing tier begins, a failed overwrite is not guaranteed to preserve the earlier complete payload
      And a concurrent reader is not guaranteed to observe either a complete earlier payload or a complete replacement payload
      And overlapping saves to the same tier are not guaranteed to remain isolated or independently successful

    Scenario Outline: Golden export and pre-Apply fallback retry complete capture-and-save attempts
      Given <request> has a structurally valid physical candidate
      And an attempt to capture or save it fails transiently
      When baseline persistence retries the complete attempt
      Then it makes at most 3 attempts separated by 50 ms after failed attempts
      And it returns <failure-result> if all attempts fail
      And tuning the retry delay cannot overwrite a known-good baseline with an invalid or empty one

      Examples:
        | request            | failure-result                                      |
        | Golden export      | no new eligible golden baseline                     |
        | Apply pre-baseline | Apply continues with recovery availability unknown  |

    Scenario: Snapshot Current retries capture but reports a later persistence failure without replaying a new capture
      Given Snapshot Current has a structurally valid physical candidate
      And one of its first 2 capture attempts cannot produce a valid filtered baseline
      When the next capture succeeds after the default 50-ms retry interval
      Then it uses that captured baseline to refresh history and Current
      And it makes one Current persistence attempt for that captured baseline
      And a failure of that final persistence attempt reports a correlated Snapshot Current failure rather than treating an unsaved capture as current

    Scenario: Malformed numeric text in prior Current can abort Snapshot Current history refresh
      Given a valid replacement has been captured
      And the existing Current contains present mode or origin numeric text that cannot be converted
      When Snapshot Current loads that tier to preserve history
      Then the conversion failure is not mapped to an unavailable tier or an ordinary persistence failure
      And neither Previous nor Current has yet been written by that refresh
      And a correlated Snapshot Current failure result is not guaranteed before helper execution unwinds

    Scenario: Current, Previous, and Golden remain distinguishable persistence tiers
      Given a valid baseline is saved as Current, Previous, or Golden
      When recovery evaluates available tiers
      Then Current represents the latest session baseline
      And Previous contains at most the preceding Current baseline after best-effort promotion
      And Golden is an intentional baseline independent of normal session rotation
      And a candidate-locally confirmed golden restore attempts to remove Current and Previous before final recovery validation because they no longer describe the restored desktop

    Scenario: Tier presence is not proof of payload validity or recovery eligibility
      Given a Current, Previous, or Golden persistence entry exists but is unreadable or unusable
      When Apply checks whether a fallback Current capture is missing or Revert checks whether any tier exists
      Then the persistence entry counts as present for those preliminary gates
      And a present unusable Current can suppress the pre-Apply fallback capture
      And Revert can enter protected recovery before later loading rejects every unusable candidate
      And presence alone never permits a restored result

  Rule: Baseline capture is gated by tracked mutation state and snapshot validity, not by repeated stability observation

    Scenario: Snapshot Current captures the usable physical desktop
      Given no tracked Apply or recovery is active and the desktop contains one or more restore-capable physical displays
      When a client requests Snapshot Current
      Then the engine captures a current-session baseline for the usable physical desktop
      And it reports whether that baseline was saved for the requesting session
      And a saved capture does not guarantee that the desktop was unchanged across multiple observations or by an actor outside the engine

    Scenario: A current capture prepares a valid replacement before refreshing history
      Given a valid current-session baseline already exists
      When the engine captures a replacement current-session baseline
      Then it validates, filters, and captures applicable layout data for the replacement before refreshing history
      And it stores the valid replacement as current
      And when history refresh succeeds, previous retains the prior current desktop for one level of restore history

    Scenario: A valid current replacement is saved even when history promotion fails
      Given a valid current-session baseline already exists
      And a replacement current-session baseline has been captured and filtered successfully
      But saving the prior current baseline as previous fails
      When the engine refreshes the current-session baseline
      Then it still saves the valid replacement as current
      And it reports the refresh result from the new current save rather than failing solely because history promotion failed

    Scenario: An unchanged valid current capture remains a usable baseline
      Given the current desktop already matches the current-session baseline
      When the engine refreshes that current-session baseline
      Then it accepts the equivalent valid capture as a usable current baseline
      And it does not report failure merely because the desktop did not change

    Scenario: A failed capture leaves the known-good baseline chain intact
      Given valid current and previous session baselines exist
      When a new capture is malformed, has no restore payload, has invalid topology, or has no eligible devices
      Then the engine rejects the new capture
      And it does not rotate, replace, or delete either known-good session baseline
      And the Snapshot Current result reports failure when the request is correlated

    Scenario: Snapshot Current preserves the existing Current data until valid capture completes
      Given a valid Current baseline exists
      And Snapshot Current cannot obtain a valid replacement after its 3 default capture attempts
      When the request completes
      Then it reports failure without rotating, replacing, or deleting the existing Current baseline
      And it does not begin a Current persistence attempt for the invalid replacement

    Scenario: Snapshot capture rejects a structurally invalid topology
      Given a captured desktop has an empty or structurally invalid topology
      When the engine evaluates it for baseline saving
      Then it rejects the capture before saving a baseline
      And it preserves any earlier valid baseline for recovery

    Scenario: A transient operating-system topology probe does not invalidate a structural baseline
      Given a saved baseline has a structurally valid topology and eligible devices
      And an operating-system topology probe is temporarily unavailable while the desktop is changing
      When the engine evaluates that baseline for recovery
      Then it does not reject the baseline as structurally invalid solely because of that transient probe failure
      And it leaves the actual recovery attempt to determine whether the desktop can be restored

    Scenario: Golden export records an intentional restore baseline when tracked mutation is idle
      Given no tracked Apply or recovery is active and the desktop has a usable physical display baseline
      When a client exports a golden baseline
      Then the engine saves the filtered captured desktop as the golden baseline
      And that baseline is available for later golden recovery subject to current topology, device, cooldown, and confirmation checks
      And saving does not prove repeated stability or exclude changes made by an actor outside the engine

    Scenario: Baseline capture is not taken from a changing desktop
      Given apply or restore activity is changing the display configuration
      When a client requests Snapshot Current or golden export
      Then the engine does not save a baseline from that transitional desktop
      And an affected correlated Snapshot Current request reports failure

    Scenario: A blocked baseline command does not publish its exclusion update
      Given Snapshot Current or golden export carries a replacement exclusion list
      And Apply or recovery activity makes baseline capture ineligible
      When the engine rejects that baseline command before capture
      Then it leaves the existing exclusion policy unchanged
      And the rejected command cannot alter later baseline filtering solely through its unused payload

  Rule: Filtering uses current identity evidence without silently restoring a partial desktop

    Scenario Outline: Save and load eligibility use intentionally different identity evidence
      Given a baseline is being <phase>
      When a physical device is <device-condition>
      Then <result>

      Examples:
        | phase   | device-condition                                             | result                                                                                       |
        | saved   | active and has a restore-capable display identity           | it may remain in topology, modes, HDR, origins, layouts, and primary selection             |
        | saved   | virtual and active                                           | it is omitted while eligible physical devices remain                                        |
        | saved   | physical but lacks a restore-capable display identity       | it is omitted; an all-omitted baseline is rejected                                          |
        | loaded  | physical, connected, but currently inactive                 | matching device identity keeps it eligible; an active display name is not required          |
        | loaded  | non-excluded and no longer present                          | the entire candidate is rejected rather than partially restored                             |
        | loaded  | explicitly excluded and no longer present                   | it and every related field are removed; a non-empty remaining candidate may proceed         |
        | loaded  | an active virtual identity in the saved topology            | the candidate is rejected                                                                    |
        | loaded  | an active virtual identity only in ancillary settings       | only those ancillary values are removed; physical topology may continue eligibility checks  |

    Scenario Outline: Virtual-display classification preserves compatibility markers
      Given an enumerated display has <identity-evidence>
      When baseline filtering classifies that display
      Then <classification>

      Examples:
        | identity-evidence                                                                                         | classification                                      |
        | SunshineVirtualDisplay or Sunshine Virtual Display in its device, display, or friendly identity          | it is a virtual display, matched case-insensitively |
        | the friendly identity Sunshine Virtual Display Driver                                                    | it is a virtual display, matched case-insensitively |
        | EDID manufacturer SDD or SMK                                                                             | it is a virtual display, matched case-insensitively |
        | none of those text or EDID markers                                                                       | those rules do not classify it as virtual           |

    Scenario: Save-side filtering excludes a recognized virtual display even when it is inactive
      Given enumeration recognizes a virtual-display identity
      And that virtual display currently has no active display evidence
      When a captured baseline is filtered for saving
      Then the virtual identity and its related settings are omitted
      And only a non-empty restore-capable physical topology can make the capture saveable

    Scenario: An active virtual topology identity is rejected before exclusions can rescue it
      Given a loaded baseline topology contains a currently active recognized virtual display
      And the caller also explicitly excludes that virtual identity
      When recovery filters the candidate
      Then it rejects the entire candidate before applying the exclusion
      And an active virtual identity can be filtered from ancillary fields only when it is absent from the saved topology

    Scenario: Save and load use different device-identity matching rules
      Given enumeration supplies a stable device identity or falls back to an active display identity when the stable identity is empty
      When a captured baseline is filtered for saving
      Then retained topology and settings identities must exactly match the selected enumerated identity
      But load-side presence and explicit exclusions trim surrounding whitespace and compare identities case-insensitively

    Scenario: Save-side identity filtering can leave a topology without mode records
      Given a captured candidate initially has non-empty modes and a restore-capable physical topology
      But none of its mode identities exactly matches a retained physical identity
      When save-side filtering removes those mode records while leaving the topology non-empty
      Then the filtered candidate may still be persisted with an empty mode set
      And direct active-tier loading may retain it while cross-root startup adoption rejects it for lacking mode payload

    Scenario: Exclusion updates replace the policy instead of accumulating it
      Given the saved policy excludes one or more device identities
      When Apply, Snapshot Current, or Golden export supplies an exclusion-list field
      Then identifiers are matched case-insensitively
      And a non-empty list replaces the earlier list
      And a supplied empty list clears the earlier list
      And an omitted list leaves the current policy unchanged

    Scenario: Filtering removes every related value for an excluded or ineligible identity
      Given a captured or loaded baseline names an identity in topology, modes, HDR, origins, layouts, or primary selection
      When that identity is excluded or otherwise filtered out
      Then the retained candidate has no topology, mode, HDR, origin, layout, or primary reference to that identity
      And no excluded identity can become an implicit target during a later restore

  Rule: Filtering keeps only restore-capable physical display data

    Scenario: Saving omits active virtual displays without discarding usable physical displays
      Given a captured desktop contains a Sunshine-compatible virtual display and physical displays
      When the engine prepares a baseline for saving
      Then the virtual display is excluded from the saved restore baseline
      And the remaining usable physical displays are retained

    Scenario: Exclusions are case-insensitive and remove every related field
      Given a captured baseline contains a device named "DISPLAY-A" in its topology, modes, HDR states, origins, layouts, and primary selection
      And the caller excludes "display-a" using different casing
      When the engine prepares that baseline for saving or recovery
      Then "DISPLAY-A" is removed from topology, modes, HDR states, origins, layouts, and primary selection
      And no excluded device remains an implicit restore target

    Scenario: An explicit empty exclusion update clears earlier caller exclusions
      Given the active baseline policy excludes a physical display
      When Snapshot Current or golden export explicitly supplies an empty exclusion list
      Then the engine replaces the earlier caller exclusion list with the empty list
      And later baseline capture and recovery do not exclude that display solely because of the earlier list

    Scenario: An all-excluded or otherwise non-restorable capture is rejected
      Given every captured display is virtual, excluded, missing a safe display identity, or absent from the topology
      When the engine prepares a baseline for saving
      Then it rejects the capture instead of persisting an empty or partial restore baseline
      And it preserves any previously saved valid baseline

    Scenario: An inactive but connected physical display remains eligible on load
      Given a saved physical baseline contains a device that is currently connected but inactive
      When the engine evaluates the baseline for recovery
      Then the baseline is not rejected merely because that connected device has no active display name
      And the device may remain part of the complete restore candidate

    Scenario: A missing non-excluded baseline device invalidates the candidate
      Given a saved baseline contains a physical device that is no longer present
      And that device is not explicitly excluded
      When the engine evaluates the baseline for recovery
      Then it rejects the entire baseline rather than restoring only the remaining paths

    Scenario: A missing explicitly excluded device is removed instead of invalidating the candidate
      Given a saved baseline contains a device that is currently absent
      And that device is explicitly excluded by the caller
      When the engine evaluates the baseline for recovery
      Then it removes that device and all of its related baseline fields
      And it may use the remaining non-empty physical baseline

    Scenario: A loaded baseline whose topology contains an active virtual display is rejected
      Given a persisted baseline topology references a virtual display that is currently active
      When the engine evaluates it for recovery
      Then it rejects that baseline as a physical-desktop restore candidate
      And it continues only with another eligible restore tier

    Scenario: An ancillary active virtual-display reference is removed from an otherwise physical candidate
      Given a persisted baseline has a physical topology
      And an active virtual-display identifier appears only in its mode, HDR, origin, layout, or primary-display details
      When the engine evaluates the baseline for recovery
      Then it removes the virtual-display-only details from that candidate
      And the remaining physical baseline may continue through ordinary eligibility checks

  Rule: Compatible baseline content is compared by desktop meaning

    Scenario: Topology equality ignores Windows enumeration order
      Given two baselines contain the same duplicate groups and devices in different group or member orders
      And their modes, HDR states, primary selection, and required origins are the same
      When the engine compares either baseline with the current desktop
      Then it treats the topologies as equal
      And once a stable observation is available it does not reapply a restore solely because Windows reordered its enumeration

    Scenario: The stable-observation gate can still be disrupted by fluctuating enumeration order
      Given the current desktop is semantically unchanged
      But consecutive raw observations keep reordering otherwise equal topology groups or members
      When recovery waits for two identical non-empty observations before semantic comparison
      Then that stability gate may remain unconfirmed and exhaust its bounded read window
      And order-insensitive candidate equality does not guarantee that fluctuating raw observations will settle

    Scenario: Missing origins remain compatible with a legacy baseline
      Given a legacy baseline contains the same topology, modes, HDR states, and primary selection as the current desktop
      And either the legacy baseline or the current capture has no origin metadata
      When the engine compares the baseline with the current desktop
      Then it treats the origin metadata as compatible

    Scenario: Different origins make two origin-aware baselines unequal
      Given two baselines contain the same topology, modes, HDR states, and primary selection
      And both baselines provide origin metadata that differs
      When the engine compares the baselines
      Then the comparison reports that the desktops differ

    Scenario: Layout metadata remains optional for pre-layout baselines
      Given a baseline written before layout metadata was available has valid restore payload
      When the engine loads that baseline
      Then it remains compatible for recovery without invented layout requirements
      And when layout metadata is present it is retained only for eligible non-excluded devices

    Scenario: Invalid layout metadata does not turn an otherwise compatible legacy baseline into a new layout contract
      Given a compatible legacy baseline has no usable layout rotation metadata
      When the engine evaluates that baseline
      Then it treats layout information as unavailable rather than requiring an inferred rotation
      And it still requires the baseline's topology and display settings to be valid

  Rule: Persistence and recovery distinguish usable payload from candidate quality

    Scenario: Persisted data without a usable topology is not adopted
      Given a saved current, previous, or golden baseline is unreadable or has no usable topology restore payload
      When the helper starts or recovery evaluates that tier
      Then it does not adopt that data as a restore candidate
      And it leaves a different valid tier available for selection

    Scenario: Parseable restore fields may be retained from a syntactically damaged baseline
      Given a saved baseline has damaged encoding but still contains extractable topology and display-settings fields
      When recovery evaluates that tier
      Then it may retain the extracted candidate for the remaining eligibility checks
      And it requires later restore and confirmation before declaring the desktop restored

    Scenario: Replacing an existing baseline does not guarantee concurrent reader isolation
      Given a valid baseline is already persisted for a tier
      When the engine saves a replacement baseline
      Then it completes the replacement data before attempting to overwrite the saved baseline
      And a concurrent recovery reader is not guaranteed to observe only a complete earlier or replacement version

    Scenario: A topology and settings membership mismatch is not rejected solely for being incomplete
      Given a persisted baseline topology and its display-settings records name different sets of devices
      When recovery evaluates that tier
      Then it does not reject the candidate solely because those device sets differ
      And it requires later application and confirmation before declaring the desktop restored

    Scenario: Normal recovery tries eligible session baselines before golden
      Given current, previous, and golden baselines have been evaluated for eligibility
      And no current-missing preference applies
      When recovery begins with the normal session-first policy
      Then it tries current before previous and uses golden only after the eligible session choices fail

    Scenario: A missing current baseline can place golden before previous
      Given Current does not produce a filtered load candidate
      And previous and golden baselines exist
      And recovery is configured to prefer golden when current is missing
      When normal recovery begins
      Then it evaluates golden before attempting previous
      And it falls through to previous if golden is not confirmed

    Scenario: Current-missing preference is decided by candidate loadability
      Given Current loads as a filtered candidate but later fails topology validation, application, or confirmation
      And Previous and Golden baselines exist
      And recovery is configured to prefer Golden when Current is missing
      When normal recovery continues after trying Current
      Then it treats Current as attempted rather than unavailable
      And it attempts Previous before Golden

    Scenario: A disabled current-missing preference leaves previous ahead of golden
      Given current is unavailable
      And previous and golden baselines exist
      And recovery is not configured to prefer golden when current is missing
      When normal recovery begins
      Then it attempts previous before golden

    Scenario: Golden-first recovery tries an eligible golden baseline before session baselines
      Given current, previous, and golden baselines have been evaluated for eligibility
      When recovery begins with golden-first recovery selected
      Then it tries the eligible golden baseline before session baselines

    Scenario: Golden-first session fallback can revisit Golden before Previous
      Given the first Golden attempt is not confirmed
      And Current cannot be loaded as a filtered candidate
      And Previous and Golden persistence entries still exist
      And missing-Current preference is enabled
      When golden-first recovery enters its session-fallback ordering
      Then it attempts Golden again before Previous
      And that second Golden attempt is not deduplicated from the initial golden-first attempt

    Scenario: A repeated Golden success still enters outer pending accounting
      Given the initial golden-first attempt is not confirmed
      And missing-Current preference causes the session-fallback routine to try Golden again
      And that repeated Golden attempt is confirmed while the Golden entry remains loadable
      When control returns to the outer golden-first fallback decision
      Then it increments the pending-fallback count without retaining which tier actually confirmed
      And before the 3rd pending result it may withhold overall recovery success despite the repeated Golden confirmation

    Scenario: Confirmed current restoration advances its restore history
      Given a valid current-session baseline is selected and restoration is confirmed
      When the engine completes that recovery
      Then it attempts to advance the recovered current baseline to the previous tier
      And it retains the confirmed desktop outcome even if the optional history advancement cannot be completed

    Scenario: A confirmed previous restoration retires an attempted current baseline
      Given a current-session baseline was available but could not be restored
      And a previous-session baseline is restored and confirmed
      When the engine updates session baseline history
      Then it attempts to remove the attempted current baseline
      And it retains the confirmed restore outcome even if that best-effort removal fails

    Scenario Outline: Candidate-local recovery side effects precede final validation
      Given <tier-condition>
      And that candidate is confirmed by its tier-local application and observation checks
      When the engine performs the later 250-ms final recovery validation
      Then <side-effect> has already been attempted
      And a failed final validation does not roll that side effect back before recovery is scheduled again

      Examples:
        | tier-condition                                      | side-effect                                                                                           |
        | Current is the confirmed candidate                  | Current is promoted to Previous                                                                        |
        | Previous is confirmed after Current was attempted   | the attempted Current tier is removed                                                                  |
        | Golden is the confirmed candidate                   | staged-settings reset, session-tier removal, and golden-health clearing are attempted                   |
        | either Current or Previous is confirmed             | the in-memory latest-session-success time is updated for later Golden cooldown decisions              |

    Scenario: Successful Current promotion can leave both session tiers
      Given Current is confirmed by its tier-local recovery checks
      And saving that payload as Previous succeeds
      But removing Current fails
      When history promotion completes
      Then the promotion still reports success
      And both Current and Previous may remain persisted

    Scenario: Failed Current promotion preserves Current but not the final restore guarantee
      Given Current is confirmed by its tier-local recovery checks
      But saving that payload as Previous fails
      When history promotion is attempted
      Then Current is not removed by that promotion
      And the candidate-local restore result continues to final validation despite the failed optional history update

    Scenario: Each stable-read probe requires two consecutive equal observations
      Given recovery is sampling the current desktop
      When a stable-read probe runs for up to 2 seconds with 150-ms sample spacing
      Then it succeeds only after two consecutive captures are equal
      And it treats a capture as empty only when both topology and modes are empty
      And topology-only or modes-only captures can therefore satisfy this probe before later candidate checks decide recovery success

  Rule: Recovery selection and confirmation are deterministic after eligibility filtering

    Scenario Outline: Eligible candidate ordering follows the selected policy
      Given Current, Previous, and Golden have independently been evaluated for payload, identity, filtering, and topology eligibility
      When recovery uses <policy>
      Then it attempts candidates in <order>
      And each candidate must be confirmed before recovery reports success

      Examples:
        | policy                                                   | order                                      |
        | normal session-first                                     | Current, Previous, then Golden             |
        | normal with Current unavailable and preference enabled   | Golden, then Previous                      |
        | normal with Current unavailable and preference disabled  | Previous, then Golden                      |
        | golden-first                                             | Golden, then session fallback through Current, an optional repeated Golden-before-Previous when Current is unavailable and that preference applies, then Previous |

    Scenario: Candidate eligibility and candidate success remain different observations
      Given a tier has a parseable, filtered, structurally valid candidate
      When topology activation, settings application, layout application, or confirmation fails
      Then that tier is not reported as restored
      And recovery continues only to the next policy-eligible candidate or its scheduled retry
      And an OS topology probe that is transiently unavailable does not by itself convert a structurally valid payload into malformed data

    Scenario: Tier-local confirmation uses a candidate check followed by an independent quiet baseline
      Given a candidate has been applied or already appears to match the desktop
      When recovery performs tier-local confirmation
      Then a stable capture must match the candidate's canonical topology, exact modes, HDR, and primary selection, plus origins when both sides contain origins
      And an applicable layout requirement is checked once with that candidate comparison
      And the following 750-ms quiet period requires repeated stable captures to equal a newly obtained quiet-period base
      But that quiet-period base is not compared with the candidate and layout is not rechecked during the quiet period
      And after an unconfirmed application, it uses one 700-ms double-check and at most two apply-and-confirm attempts
      And tuning these default values still preserves those comparison boundaries and cancellation responsiveness

    Scenario: Layout confirmation is point-in-time and final recovery validation does not repeat it
      Given a layout-aware candidate passes tier-local snapshot and layout confirmation
      And its layout later changes during the quiet period or the following 250-ms final-validation delay without changing the snapshot fields used by those later comparisons
      When quiet confirmation and final recovery validation run
      Then it may report success without observing the changed layout
      And tier-local layout confirmation is not a guarantee that layout remained unchanged through final completion

    Scenario: Confirmed Current restoration advances Current to Previous on a best-effort basis
      Given Current was confirmed
      When its recovery completes
      Then Current is promoted to Previous on a best-effort basis without retracting the confirmed restore on promotion failure

    Scenario: Confirmed Previous restoration retires a Current tier that was tried and failed
      Given Current was tried but Previous is the confirmed tier
      When its recovery completes
      Then removal of the failed Current tier is attempted so a successful cleanup cannot retry it as the current desktop

  Rule: Golden ordering and health report unresolved golden candidates accurately

    Scenario: A recent session restoration applies the default Golden cooldown
      Given a session tier was confirmed less than 60 seconds ago
      When Golden would otherwise be eligible
      Then Golden is skipped during that session-success cooldown without being counted as a failed golden restore

    Scenario: Golden cooldown history is process-local
      Given a session tier was confirmed and started the 60-second Golden cooldown
      When the helper process restarts
      Then that prior session-success time is not recovered from persistence
      And the restarted helper does not suppress Golden solely because the earlier process restored a session tier

    Scenario: Final validation failure does not retract the Golden cooldown timestamp
      Given a session candidate passes its tier-local recovery checks
      And the helper records its latest-session-success time
      But the later 250-ms final recovery validation fails
      When recovery evaluates Golden again within 60 seconds in the same process
      Then the earlier candidate-local timestamp can still suppress Golden
      And final validation failure does not roll that timestamp back

    Scenario: Golden-first recovery accepts a session fallback after the default pending threshold
      Given golden-first recovery has an eligible but unconfirmed Golden and a confirmed session fallback
      When the same request repeats that fallback
      Then recovery leaves Golden pending for the first 2 consecutive fallbacks
      And it accepts the session result on the 3rd consecutive fallback and resets the pending count

    Scenario: A cooldown-skipped Golden can still make a session fallback pending
      Given golden-first recovery runs during the process-local session-success cooldown
      And Golden remains loadable but is skipped without a restore attempt
      And a session fallback is confirmed
      When recovery checks whether Golden still exists after that fallback
      Then it increments the same pending-fallback count used for an unconfirmed Golden
      And the first 2 such session results remain pending even though cooldown suppression did not record a golden issue

    Scenario: Tuning Golden cooldown duration cannot reorder ordinary eligibility or manufacture success
      Given an implementation changes the 60-second Golden cooldown duration
      When a golden and session candidate overlap
      Then missing devices, active virtual identities, exclusions, filtering, and confirmation still decide eligibility and success
      And a temporary cooldown skip is never recorded as an unhealthy golden restoration

    Scenario: A recent session restoration temporarily suppresses golden recovery
      Given a session baseline was restored successfully very recently
      And an otherwise eligible golden baseline is available
      When recovery evaluates golden candidates
      Then it skips golden during the 60-second session-success cooldown
      And it does not treat that temporary suppression as a golden restore failure

    Scenario: A golden baseline with a currently missing device is skipped safely
      Given an otherwise eligible golden baseline references a device that is not currently present
      When recovery evaluates golden candidates
      Then it skips that golden baseline without attempting a partial restore
      And it does not record a golden restore failure solely because the device is currently absent

    Scenario: Golden-first recovery allows only three pending session fallbacks
      Given golden-first recovery is selected
      And the golden baseline remains present but is not confirmed
      And an eligible session baseline is restored successfully
      When this golden-first fallback repeats
      Then it keeps golden pending for the first 2 consecutive confirmed session fallbacks
      And it accepts the confirmed session fallback on the 3rd and resets the pending fallback count

    Scenario: A new Revert does not itself reset an armed golden pending count
      Given golden-first recovery has accumulated fewer than 3 confirmed session fallbacks while Golden remains loadable
      And recovery remains armed
      When another explicit Revert starts a new recovery request
      Then request-local golden-health issue tracking is reset
      But the accumulated golden pending-fallback count remains until an Apply, recovery-state clear, non-golden-first branch, terminal fallback branch, or successful completion resets it

    Scenario: Threshold acceptance while Golden remains loadable does not persist its noted issue
      Given golden-first recovery notes that a loadable Golden was invalid or not confirmed
      And a session fallback is confirmed on the 3rd consecutive pending fallback
      When recovery accepts that session result while Golden still loads
      Then it resets the pending-fallback count without registering the noted issue in recurring health history
      And a later request reset may discard that uncommitted issue

    Scenario Outline: Golden candidate branches distinguish skipped state from a noted issue
      Given Golden is <candidate-condition>
      When recovery evaluates that candidate
      Then <health-effect>

      Examples:
        | candidate-condition                                                   | health-effect                                                       |
        | unavailable or rejected by payload, identity, or filtering checks    | no issue is noted solely for that skipped candidate                 |
        | suppressed by cooldown or by a currently missing device              | no issue is noted solely for that temporary skip                    |
        | loadable but rejected by topology validation                         | an invalid-topology issue is noted for the current request          |
        | applied but not confirmed                                             | a restore-not-confirmed issue is noted for the current request      |

    Scenario: Normal Golden-before-Previous fallback registers a noted issue on session success
      Given missing-Current preference tries Golden before Previous
      And that Golden attempt notes an issue
      When Previous is confirmed as the session fallback
      Then the noted Golden issue is registered in recurring health history before the session result completes

    Scenario: Golden-first fallback registers immediately when Golden no longer loads
      Given golden-first recovery notes a Golden issue
      And a session fallback is confirmed
      But Golden no longer produces a filtered load candidate at the pending check
      When recovery completes that fallback
      Then it registers the noted issue in recurring health history
      And it accepts the session result immediately and resets the pending-fallback count

    Scenario: Registering an unresolved Golden issue records recurring health information
      Given the current recovery flow has noted an unresolved Golden issue
      When registration successfully replaces the recurring Golden-health record
      Then it records the latest reason and increments the persisted unresolved-attempt count by 1
      And the persisted count carries no request identity or deduplication guarantee
      And one recovery request can therefore contribute another count when a later attempt notes and registers a new issue
      And a transient first failure does not by itself report the golden baseline as out of date

    Scenario: Repeated unresolved Golden registrations eventually report a stale baseline
      Given unresolved Golden issue registrations span a 72-hour observation window
      When at least 3 registrations have accumulated and the persisted first-failure time is at least 72 hours old
      Then the engine reports the golden baseline as possibly out of date
      And it retains the latest reason, first and latest failure times, accumulated registration count, threshold, and observation window for diagnosis

    Scenario: A future first-failure time cannot manufacture elapsed unhealthy age
      Given persisted recurring health has a first-failure time later than the current clock
      When another unresolved Golden issue is registered successfully
      Then the calculated unresolved age is clamped to zero rather than becoming negative
      And accumulated attempts alone do not report the Golden baseline as stale until the observation-window condition is also met

    Scenario: A golden health report does not by itself disqualify a currently eligible candidate
      Given prior unresolved golden restore history has been recorded
      And the golden baseline currently satisfies topology, device, cooldown, and filtering checks
      When recovery evaluates golden candidates
      Then it evaluates the candidate under the current recovery policy
      And it does not skip that candidate solely because the prior health report exists

    Scenario: A new recovery request does not reuse an uncommitted prior issue
      Given a prior recovery request noted a golden issue but did not record it as unresolved history
      When a new explicit recovery request begins
      Then only an issue from the new request can be added to unresolved health history
      And previously recorded health history remains unless a confirmed golden restoration or new golden export successfully clears it

    Scenario: Invalid prior golden-health content starts a new observable series
      Given the persisted golden-health document is unreadable or is not an object
      And replacement of that document succeeds
      When a new unresolved golden issue is registered
      Then the persisted attempt count starts again at 1
      And the first-failure time starts at the new issue because invalid prior history is not reconstructed

    Scenario: Partially compatible Golden-health fields retain asymmetric fallback behavior
      Given prior Golden health is an object and a new issue will be persisted successfully
      When a compatible field is absent
      Then first-failure time defaults to the new issue time and unresolved count defaults to zero independently
      But an invalid first-failure value prevents reuse of either prior value
      And a valid first-failure followed by an invalid unresolved count preserves that first time while restarting the persisted count at 1

    Scenario: An unwritable golden health record does not rewrite recovery eligibility or outcome
      Given an unresolved golden issue has been noted for the current recovery request
      And the health record cannot be replaced
      When the request reaches an unresolved terminal recovery outcome
      Then the engine retains the caller-visible recovery result from candidate selection and confirmation
      And it does not claim that a missing health record made the golden candidate healthy, restored, or ineligible

    Scenario: A failed golden-health write consumes the issue from that request
      Given the current request has noted an unresolved Golden issue
      And replacing the golden-health record fails
      When the engine attempts to register that issue
      Then the request-local issue is cleared before the failed persistence write returns
      And the same issue is not automatically retried unless recovery notes another issue

    Scenario: Golden-health replacement has the same overwrite limits as baseline replacement
      Given a golden-health record already exists
      When the engine writes an updated health record
      Then it completes replacement text before attempting to update the saved path
      But replacement failure is not guaranteed to preserve the complete earlier record
      And a concurrent reader is not guaranteed to observe only a complete earlier or replacement record
      And overlapping health updates are not guaranteed to remain isolated or independently successful

    Scenario: Confirmed golden restoration attempts health and stale-session cleanup
      Given an eligible golden baseline is restored and confirmed
      When recovery completes
      Then the engine resets request-local golden-health tracking and attempts to remove the recurring health report
      And it attempts to remove current and previous session baselines that no longer describe the restored desktop
      But cleanup failures do not retract the candidate-local Golden result and may leave stale files persisted

    Scenario: A successful new golden export attempts to clear the prior unhealthy status
      Given unresolved golden health history exists for an older golden baseline
      When a new Golden baseline is successfully exported
      Then the engine resets request-local golden-health tracking and attempts to remove the prior health record
      And the newly saved valid golden baseline becomes eligible for later recovery
      But a health-record deletion failure may leave the earlier persisted unhealthy status beside the new Golden baseline
