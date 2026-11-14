# Security policy

## Public security improvements

Security enhancements and defense-in-depth improvements are welcome as public
pull requests when they do not disclose an exploitable vulnerability. Examples
include clearer documentation, safer defaults, and additional validation around
an already enforced security boundary.

## Private vulnerability reports

If you discover a way to escalate privileges, bypass authentication or
authorization, or otherwise gain unauthorized access, submit a
[private security report](https://github.com/Nonary/Vibepollo/security/advisories/new).
Do not disclose the exploit in a public issue or pull request before it has been
reviewed and coordinated with the maintainers. If you are unsure whether a
finding is a hardening suggestion or an exploitable vulnerability, report it
privately first.

Include the affected version, operating system, installation mode, required
access, reproduction steps, and the privileges obtained. For privilege
escalation reports, identify the starting account's permissions and the security
boundary crossed.

## Command execution and Windows elevation

Vibepollo's Web UI intentionally allows an authenticated operator to configure
applications and preparation commands. The presence of command execution or an
elevation option alone does not establish a vulnerability.

When the Windows service launches these commands, it uses the active desktop
user's token. It does not grant administrator privileges to a standard user:
requesting elevation for a non-administrator retains that user's existing access
level. For an administrator, the service can use that user's existing linked
administrator token. If the required user token cannot be obtained, the launch
fails instead of falling back to the service's SYSTEM privileges. When Vibepollo
is run directly rather than as a service, child processes inherit the privileges
with which Vibepollo was launched.

Here, "administrator" refers to the Windows account in the active desktop
session, not simply someone signed in to the Web UI. These Windows token rules
should not be assumed to describe other platforms.

Automated and AI-assisted scanners can overlook this distinction and assume that
a privileged service necessarily runs every configured command with its own
privileges. A report based only on that assumption, or on an administrator using
the intended elevation feature, does not demonstrate privilege escalation.
Validate the actual launch path and resulting process token before making that
claim.

Reports showing that an unauthorized caller can reach command execution, that a
standard user can obtain administrator or SYSTEM privileges, or that another
security boundary can be bypassed are welcome and should be submitted privately.
