# NVIDIA preferences cleanup ownership

The host owns NVIDIA preference restoration in `main()`. A guard registered
immediately after logging initialization calls `nvprefs_instance.shutdown()` on
maintenance-command returns, early startup returns and initialization unwinding.
Normal shutdown calls it at the existing NVIDIA cleanup position after the
streaming workers/task pool have stopped, then disables the guard. The interface
retains its existing serialized-use contract; callers must finish streaming
operations before terminal cleanup.

`shutdown()` is terminal and `noexcept`. It attempts an owned profile restore,
closes even partially initialized NVAPI state, releases the exclusive undo-file
handle, and destroys its runtime implementation before returning. Its result is
cached. Repeated calls return that result without logging, restoration, driver
calls or file access. Calls which would reopen or modify preferences after this
boundary return failure. A reentrant shutdown request does not start another
cleanup attempt.

The global destructor never initiates restoration or logging. After explicit
shutdown its implementation pointer is empty, so it performs no deferred driver,
file or logging work during CRT teardown. Driver cleanup clears its session handle
before invoking native cleanup and contains C++ exceptions, preventing a second
destructor attempt through the same handle. This does not attempt to recover from
native process faults or hardware/driver failures outside C++ exception handling.

The undo journal records the pre-change settings before modifications are saved.
Successful cleanup restores/saves those settings and deletes the journal using
the existing restoration rules. Failed load, restore, save or deletion, including
C++ exceptions, does not delete an existing durable journal. Runtime ownership and
its exclusive lock are released; **a later host startup owns the recovery retry**.
Repeating shutdown in the same process does not retry through a closing runtime.
An incomplete-cleanup diagnostic is attempted while logging is alive; if logging
throws, a fixed diagnostic goes to stderr without rethrowing.

Startup recovery likewise keeps the journal on unreadable data, restore/save or
deletion failure, and on C++ exceptions. A failed attempt closes its local handle
so recovery can be retried. An unresolved existing journal prevents creation of a
replacement journal and therefore prevents saving new global preference changes
over its recovery data. Successful later recovery deletes the old journal and
allows normal modifications again. Unreadable journals remain visible for manual
recovery rather than being discarded as an incidental shutdown action.

The focused tests compile the production interface/lifetime code, real FailGuard
and main exit branches with deterministic Windows/NVAPI/logging boundaries. The
journal boundary uses real temporary files to verify byte preservation, lock
release and recovery by a later interface instance. Failure/exception injection
covers load, restore, save, deletion, native cleanup and log delivery, plus repeated
cleanup, reentrancy and destruction after the logger boundary. Negative controls
reject retained runtime state, escaping cleanup exceptions and deletion after a
failed startup recovery.

This fixes the unsafe CRT restoration lifetime proven by issue #570's dump. It
does not identify that incident's original C++ exception or claim a Windows runtime
reproduction/acceptance result.
