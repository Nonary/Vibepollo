# Install-root validation policy and diagnostics

Drive-root ownership and ACL findings are diagnostic information. A drive root owned by an
account outside the guard's trusted list, a null/missing DACL, or a rejected
allow ACE must not by itself prevent installation. After logging the finding,
validation continues through Program Files, the destination, and protected
installer inputs. These notes do not appear as completion warnings or
make a successful installation appear partially successful. An early note
must not conceal a later blocking finding.

The protected file set comes from the incoming MSI's File/Component/Directory
tables and registered products' own cached MSI inventories during replacement
or removal. The active package adds only its scheduled custom-action inputs,
using the authored sequence conditions. Incoming optional actions are never
applied to an old product's root. ProductCode-less legacy registrations check
their legacy uninstaller entry point. Incoming directory-property overrides
use costed paths; a nonempty `SHORTFILENAMES` property selects short target
names and is passed into bootstrapper preflight before costing. Registered
file components use their actual registered key paths, preserving installed
short names and relocated directories. Every
directory leading to a protected file retains its security checks. The guard
does not enumerate unrelated files: a user-added `assets/windows.png` is not
an executable installer input simply because it lives below the install root.
There is no filename-extension exemption; an image managed by the package is
still protected if the installer will replace or remove it. The managed
bootstrapper checks the selected root/ancestors and empty-folder policy, then
delegates descendant classification to the native scoped guard; it does not
recursively reject unrelated links first.

Action targets include directories as well as files. Scheduled configuration
migrations explicitly protect their legacy and destination config files,
credentials directories, even when no MSI File entry exists beneath them.
Legacy directory migrations include their source and destination only when
the source directory exists; an unused covers destination is not inspected.
Directory leaves are validated as directories, rather
than incorrectly treated as packaged files. Missing targets may be created
by the selected operation. Unscheduled migrations do not inspect these data
targets. Rejections identify the actual failing ancestor or typed target.

Missing package files can be restored by repair/upgrade. If an inventory cannot
be read, the exact API failure is reported as a compatibility warning and
available inputs and root checks continue. Active custom-action inputs and
file inventory are collected independently so an unavailable File table does
not erase a known action's scope. Old inventories never fall back to the new
version's optional driver paths. A permission-inspection warning does not
stop checks of other independently accessible protected paths; the failed
object is never treated as successfully inspected or pinned. Reparse points on
protected paths and drive aliases that fail the existing volume-identity check
retain their existing classification. No unrelated files are opened, altered,
deleted, or granted different permissions by this validation.

Every path-security rejection should carry the evidence needed to explain the
decision in the normal installer report. `owner_or_dacl` was ambiguous; the
guard now distinguishes `untrusted_owner` from `null_dacl`. Permission findings
include the owner SID, trust result, DACL presence/null state, descriptor control
flags, and SDDL. Rejected ACEs include their index, type, flags, principal SID,
and full access mask in addition to the rejected access bits.

Evidence comes from the already opened object and security descriptor used by
the decision. Account-name lookup and a later reopening of the path are not
required. Large SDDL strings have an explicit truncation marker; the owner and
rejected ACE are retained separately. Failed diagnostic APIs report their
status without replacing the original reason. Path-policy failures include the
normalized destination and Windows' Program Files locations; reparse failures
include the observed attributes and tag when available.

The same details flow through the MSI properties into preflight logs and the
installer's collected logs. Drive-root notes use a separate diagnostics
property rather than the error/warning properties used for the completion UI.
Collection does not change ownership, permissions, or installed files.
