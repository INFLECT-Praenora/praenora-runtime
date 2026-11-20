# Time, cancellation and timeouts

- **Version:** runtime 0.2.0, ABI 0.0 and the `workflow-config` format 0.1.0
- **Status:** Normative, MVP. Proposed ([ADR-0012](../adr/ADR-0012-time-timeouts-and-cancellation.md),
  [ADR-0030](../adr/ADR-0030-library-gate-across-runs.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

A run mixes wall-clock facts, measured durations and model time. Keeping them apart keeps results
independent of the wall clock. Modules can be slow or hang, so the runtime gives every module call a
deadline, cancels cooperatively and can stop a run on request. This document specifies the time
representations, the tick windows, the durations of the configuration, deadlines, cancellation and
stop requests.

## Kinds of time

| Kind | Representation | Used in |
|---|---|---|
| Wall-clock instants | `int64` nanoseconds since the Unix epoch (UTC, leap seconds ignored), from `system_clock`. In JSON, RFC 3339 UTC with exactly nine fractional digits: `2026-11-10T16:04:05.123456789Z`. | Logs, status snapshots and the `execution` section of the run record only. |
| Durations and deadlines | `steady_clock`, `int64` nanoseconds. JSON fields that hold one end in `_ns`. | Deadlines, measured durations, uptime. |
| Logical time | A `double` in the workflow's time unit. | Tick windows, invocations, envelopes, observation batches. |
| Ticks | `uint64`, numbered from 0 (in JSON at most 2^53 − 1). | The passes of a run. |

Wall-clock time MUST NOT appear in envelopes, recorded outputs, the message trace or the
`reproducibility` section of the run record ([reproducibility.md](reproducibility.md)).

## Logical time and ticks

- The time base is the configuration's `run.time`: `unit`, a non-empty label (default `"1"`);
  `start`, a number (default 0.0); `period`, a number greater than 0 (default 1.0). It is passed to
  every module in the initialize document and is part of the run fingerprint.
- Tick `t` covers the window `[start + t * period, start + (t + 1) * period)`. Both bounds are
  computed by multiplication, never by repeated addition, so rounding errors do not accumulate.
- A tick's logical time is its window's end. It is the invocation's `logical_time`, the
  `logical_time` of every envelope produced in the tick, the tick result's logical time and the
  status snapshot's `logical_time`.
- An observation batch holds the rows whose time lies in the window: `start <= time < end`
  ([protocol.md](protocol.md#input-sources)).
- Logical time never depends on the wall clock. Ticks run as fast as the modules return; there is no
  real-time pacing, and calendars are not modeled ([open-questions.md](../open-questions.md), OQ-8
  and OQ-22). Units are labels and are never converted (OQ-18).

## Durations in the configuration

- A duration is a string `<number><unit>`, where the number matches `[0-9]+(\.[0-9]+)?` and the unit
  is one of `ns`, `us`, `ms`, `s`, `m` and `h`, or `"none"`, or zero (the integer `0` or the string
  `"0"`). Any zero value, such as `0s`, disables a timeout.
- A duration MUST come to a whole number of nanoseconds and fit in a signed 64-bit count of
  nanoseconds. A value outside the syntax (surrounding whitespace, an upper-case unit, a number
  without unit other than 0) is `SCHEMA_VIOLATION`; one that fits the syntax but not these limits,
  such as `1.5ns`, is `CONFIG_INVALID`. Both are located at the value.
- `run.timeouts` sets the run's `invoke`, `lifecycle` and `cancel_grace` (defaults 300 s, 60 s and
  5 s). `steps.<step>.timeouts` puts the step's own fields over the run's, field by field.
- `observability.status_interval` (default 1 s) sets how often the status file is rewritten; 0 or
  `none` rewrites it only on state changes.
- The run record writes durations canonically: the count in the largest of `s`, `ms`, `us` and `ns`
  that holds it exactly (`"300s"`, `"1500ms"`), and `"none"` when disabled.

## Deadlines

Every call into a module gets a deadline: the step's `invoke` timeout for `invoke`, and its
`lifecycle` timeout for `create`, `initialize`, `finalize`, `destroy` and `diagnostics`. (The
contract checks use their own timeouts: invoke 30 s, also for `self_test`, lifecycle 10 s and a
cancellation grace of 5 s.)

- The deadline counts on the steady clock from the moment the call is posted to the instance's
  worker thread. That is after the call's turn at its library's gate, which another run or
  contract-check session of the process may hold: the wait for it is not part of the deadline, and a
  stop requested during it ends a `create`, `initialize` or `invoke` without calling the module
  ([threading-and-ownership.md](threading-and-ownership.md#concurrency-across-instances)).
- Whether a call timed out depends on when the module returned, not on when the waiting thread woke.
- When the deadline passes, the runtime raises the call's cancellation request, so
  `cancel_requested()` returns nonzero, logs a warning (event `call.deadline`) and waits for up to
  `cancel_grace` more.
- A timeout of zero disables the call's deadline: the runtime waits without limit. A cancellation
  grace of zero waits without limit after the deadline, so the instance never becomes unresponsive.
  A duration too large for the clock counts as no deadline.

The outcome of a `create`, `initialize` or `invoke` call, and its effect on the run:

| The call | Outcome | Effect on the run |
|---|---|---|
| returns OK before its deadline | succeeded | none |
| returns `PRAENORA_STATUS_CANCELLED` before its deadline, after a stop was requested during it | cancelled | The run ends as stopped (`RUN_STOPPED`). |
| returns any other status before its deadline | failed | The run fails with `STEP_FAILED` (`OUTPUT_INVALID` for an unacceptable result). |
| returns after its deadline but within the cancellation grace, whatever its status | timed out | The run fails with `STEP_TIMEOUT`. |
| does not return within its deadline plus the cancellation grace | unresponsive | The run fails with `MODULE_UNRESPONSIVE`; the instance is abandoned ([threading-and-ownership.md](threading-and-ownership.md#unresponsive-instances)), or, in a module host, the module host is killed (`details.terminated: true`) and nothing is abandoned. |
| ends its module host (a signal, an exit) or breaks its channel, in process mode | crashed | The run fails with `MODULE_CRASHED` ([process-isolation.md](process-isolation.md#crashes)). |
| is not made: it would have to wait for an abandoned call that may still be running in its library | refused | The run fails with `MODULE_UNRESPONSIVE`, naming the abandoned call ([threading-and-ownership.md](threading-and-ownership.md#concurrency-across-instances)). |
| is not made: a stop was requested while it waited for its library | cancelled | The run ends as stopped (`RUN_STOPPED`). |

- A `finalize` or `destroy` call of the teardown, and a `diagnostics` call made while a failure is
  reported, never change how the run ends, whatever their outcome: a run that completed, stopped or
  failed keeps that ending and its error. A `finalize` that fails or times out is logged as a
  warning (event `teardown.finalize_failed`) and becomes its step's last error; so does a `destroy`
  whose module host crashes (event `teardown.destroy_failed`; a step in process mode,
  [process-isolation.md](process-isolation.md#runs-and-validation)), while a `destroy` that times
  out or never returns leaves only its instance's state; a `diagnostics` call that does not succeed
  only leaves the diagnostics out of the error's details; any of them that does not return within
  its deadline plus the cancellation grace still makes its instance unresponsive and abandoned, and
  a refused `finalize` or `destroy` leaves its instance as it is
  ([threading-and-ownership.md](threading-and-ownership.md#unresponsive-instances)).
- An invocation that fails, times out or is cancelled leaves its instance `ready`, so it is still
  finalized. A `create`, `initialize` or `finalize` that does not succeed leaves it `failed`.
- A `create` that stores an instance and returns after its deadline is timed out, and the instance
  is still destroyed later.
- Failures are fail-fast: the first one ends the run, and the teardown that follows gives each
  `finalize` and `destroy` call its own deadline.

## Cancellation

- Cancellation is cooperative: the runtime never terminates a thread. A module SHOULD poll
  `cancel_requested()` in long computations and, once it is nonzero, return
  `PRAENORA_STATUS_CANCELLED` as soon as it can. Only a step in process mode is ended against its
  will: when a call outlives its deadline and the grace, its module host is killed
  ([process-isolation.md](process-isolation.md#unresponsive-calls)).
- Cancellation is per call. `cancel_requested()` is nonzero only while the call in flight has passed
  its own deadline or had a stop requested while it ran. Every call starts with it zero, so the calls
  that follow a stop (`finalize` and `destroy` at the teardown, `diagnostics`) run normally, bounded
  by their own deadlines.
- A call that returns `PRAENORA_STATUS_CANCELLED` counts as cancelled only when a stop was requested
  during that call; otherwise it failed.
- The manifest's `features.cancellation` is informational: the runtime requests cancellation the
  same way from every module.

## Stop requests

`Workflow::request_stop()` asks a run to stop. It is thread-safe, lock-free and async-signal-safe,
so it MAY be called from any thread and from a signal handler.

- It raises the run's stop flag and requests cancellation of the module call in flight, if any. From
  then on the runtime starts no module call: a `create`, `initialize` or `invoke` that starts after
  the flag was raised sees `cancel_requested()` nonzero from its start. A call in a module host gets
  the cancellation over its channel within a few milliseconds.
- A load has no deadline, in the runtime's process as in a module host. A module host that is still
  loading its step's module when the stop comes (a static initializer that blocks, say) is killed,
  and the run ends as stopped, not as a crash.
- The step or tick in progress then ends the run as `stopped`. A tick cut short does not count and
  records no outputs; the trace lines it wrote stay. A stop requested between ticks takes effect at
  the next tick, which calls no module.
- The modules are finalized and destroyed normally, the final run record is written, and the run
  summary's error is `RUN_STOPPED`. A stopped run is not a failure: `start()`, `tick()` and `run()`
  return normally.
- `Workflow::close()`, and destroying a `Workflow`, end a run that is ready or running as stopped.
- `praenora run` turns SIGINT and SIGTERM (on Windows also Ctrl+C, Ctrl+Break and closing the
  console) into `request_stop()` and exits with 130 once the run has stopped. A second signal ends
  the process at once with exit code 130. On POSIX systems a signal that the process inherited as
  ignored stays ignored. A terminal's Ctrl+C never reaches a module host, which has a process group
  of its own: the runtime stops its call.
- In Python, `request_stop()` from another thread stops a blocking `run()`; interrupt handling is an
  open question (OQ-25).

## References

- [ADR-0011](../adr/ADR-0011-tick-based-synchronous-execution.md): ticks, failures and stops.
- [ADR-0012](../adr/ADR-0012-time-timeouts-and-cancellation.md): time, timeouts and cancellation.
- [ADR-0023](../adr/ADR-0023-public-cpp-api.md): the C++ API and the CLI's signals.
- [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339): date and time on the Internet.
- [configuration.md](../configuration.md#durations): durations in the configuration.
