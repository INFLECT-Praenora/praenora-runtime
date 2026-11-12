# ADR-0012: Time representation, timeouts and cooperative cancellation

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Runs mix wall-clock facts (logs, status), measured durations, and model time. Mixing them leaks nondeterminism into results. Modules can hang, so the host needs deadlines and a stop mechanism.

## Decision

- **Wall-clock instants:** `int64` nanoseconds since the Unix epoch from `system_clock`; in JSON, RFC 3339 UTC with exactly 9 fractional digits. Used only in logs, status and the run record's `execution` section.
- **Durations and deadlines:** `steady_clock`, `int64` nanoseconds; JSON fields end in `_ns`. Configuration durations are `<number><unit>` with unit `ns|us|ms|s|m|h`; `0` or `none` disables a timeout.
- **Logical time:** a `double` in the workflow's `time.unit`. Tick `k` covers `[start + k*period, start + (k+1)*period)`, with boundaries computed by multiplication. The invocation's `logical_time` is the window end. Defaults: start 0.0, unit "1", period 1.0.
- **Timeouts:** each call gets a deadline (`invoke` for `invoke`, `lifecycle` for other calls). Defaults: invoke 300 s, lifecycle 60 s, cancel grace 5 s. At the deadline the host raises the instance's cancel flag and waits up to `cancel_grace`. A call that returns within the cancel grace period after its deadline is recorded as `STEP_TIMEOUT` and fails the run; one that does not return within the grace period makes the instance `unresponsive` (`MODULE_UNRESPONSIVE`).
- **Cancellation is cooperative:** modules poll `cancel_requested()` and return `CANCELLED`.
- **Stop requests:** `Workflow::request_stop()` is thread-safe and callable from a signal handler through the CLI. It raises the cancel flag, stops before the next tick, finalizes and destroys instances normally, and ends the run as `stopped` (`RUN_STOPPED`). The CLI maps SIGINT, SIGTERM or Windows Ctrl+C to it; a second interrupt exits with code 130.
- Envelopes carry no wall-clock time.

### Builder notes (Proposed)

- RFC 3339 formatting goes through `gmtime_r` (POSIX) or `gmtime_s` (Windows). An instant before 1970 is split by floor division, so its fraction stays in [0, 1 s).
- Configuration durations use the number syntax `[0-9]+(\.[0-9]+)?` and must come to a whole number of nanoseconds (`1.5ns` is rejected). Any zero value (`0`, `none`, `0s`) disables a timeout. Values that do not fit in 64-bit nanoseconds, surrounding whitespace and upper-case units are rejected with `CONFIG_INVALID`.
- A call's deadline counts from the moment the call is posted to the instance's worker, on `steady_clock`. Whether a call timed out depends on when the module returned, not on when the waiting thread woke up: a call that returns after its deadline is `timed_out` even with status OK, and one that returns before it is not. The cancellation grace period counts from the deadline.
- A timeout or cancellation grace of 0 or less is disabled: a call without a deadline is waited for without limit, and without a grace limit the host waits for a cancelled call without limit, so the instance never becomes unresponsive. Durations too large for the clock count as no deadline.
- Cancellation is per call. `cancel_requested()` is nonzero only while the call in flight has passed its own deadline or had a stop requested (`request_cancel()`) while it ran. Every call starts with it clear, so the calls that follow a stop (finalize and destroy at teardown, diagnostics) run normally, bounded only by their own deadlines. A stop requested while no call is in flight reaches no call: stopping before the next call is the engine's job. A call returning `CANCELLED` counts as `cancelled` only when the stop arrived during that call; otherwise it is `failed`.
- Implementation: calls are numbered, and the host API context holds, as atomics, the number of the call in flight (set just before the call is posted, cleared once its result is collected; 0 between calls), the number of the call whose deadline passed, and the number of the call a stop was requested for. `request_cancel()` raises the last of these to the number in flight, if any, with a lock-free compare-exchange loop that never lowers it, so a canceller that read an earlier call's number and then stalled cannot take a stop back from a later call. It allocates nothing and takes no lock, so it stays async-signal-safe.
- The caller's stop flag (`InstanceOptions::stop_flag`; the engine passes the workflow's flag) closes the gap between the caller's last check of its flag and the start of the next call: right after publishing a create, initialize or invoke as in flight, the instance reads the flag and, when it is set, marks that call stopped. The order (in flight first, then the flag; the stopper sets the flag first, then calls `request_cancel()`) means a stop is never lost between the two. finalize, destroy and diagnostics ignore the flag and always start clear, so teardown after a stop runs normally.
- An invocation that fails, times out or is cancelled leaves the instance `ready`, so shutdown still finalizes every initialized instance; a create, initialize or finalize that does not succeed leaves it `failed`. A create that returns OK with an instance after its deadline is `timed_out`, and the instance is still destroyed later.
- An unresponsive call is reported with status `TIMEOUT`, empty outputs and the time waited. The instance is abandoned before the call returns to the caller: state `unresponsive`, library pinned, worker thread detached, host API kept valid (ADR-0006). The host logs the deadline at warn level (event `call.deadline`) and the abandonment at error level (event `instance.unresponsive`).
- In a workflow configuration the schema's duration pattern refuses malformed text first: surrounding whitespace, an upper-case unit or a bare number other than 0 is `SCHEMA_VIOLATION`. `CONFIG_INVALID` is left for a duration that matches the pattern but not the limits of `parse_duration()`, such as `1.5ns` or a value beyond 64-bit nanoseconds; the note on configuration durations above describes `parse_duration()` itself.

## Alternatives considered

- **Wall-clock for everything.** Simple, but makes outputs and fingerprints time-dependent.
- **Forced thread termination at the deadline.** Unsafe for native code and unsupported on some platforms.
- **Cancellation by signals.** Not portable and not safe with in-process modules.
- **Logical time with calendars.** Deferred (OQ-22).

## Consequences

- Outputs never depend on the wall clock.
- A module that ignores cancellation costs a pinned library and a detached thread, but cannot stall the host forever.
- Timeout behavior is deterministic and testable through a test-only gate service (ADR-0021).

## References

- [docs/specs/time-cancellation-timeouts.md](../specs/time-cancellation-timeouts.md)
- [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339)
- [docs/open-questions.md](../open-questions.md)
