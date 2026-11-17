# ADR-0020: Message envelope modeled on CloudEvents; AsyncAPI generated from workflow definitions

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Messages between steps need identity, type, origin and timing, and workflow topology needs a machine-readable description that other tools can read. Established standards beat ad hoc formats.

## Decision

- The runtime builds envelopes; a module returns only `data` per output port. Envelope fields: `pmp_version`, `id`, `type`, `type_version`, `source`, `tick`, `logical_time`, `data`.
- `id` is `<first 16 hex digits of the run fingerprint>:<tick>:<source>`, deterministic and unique within a run. `source` is an endpoint (`<step>.<port>` or `inputs.<name>`).
- Envelopes contain no wall-clock time.
- A documented, not implemented, CloudEvents mapping: `id` to `id`; `source` to `urn:praenora:run:<fp16>:<endpoint>`; `type` plus `type_version` to `<type>.v<version>`; the schema URN to `dataschema`; `tick` and `logical_time` to extensions `pmptick` and `pmplogicaltime` (string-encoded, since CloudEvents has no float type); `data` to `data`.
- `praenora graph --format asyncapi` generates an AsyncAPI 3.0.0 document per workflow definition: channels per connection source, messages per type, payloads built from the envelope and data schemas. Generated copies are committed under `spec/asyncapi/` and checked by tests.
- Future binary payloads and a remote control API (Protocol Buffers or Arrow, OpenAPI 3.1) are design notes only (OQ-7, OQ-9).

### Builder notes (Proposed)

- The envelope schema (`pmp-envelope`) requires every field of the envelope and allows no others at the envelope level except `x-` fields; `id` must match `<16 lowercase hex digits>:<tick>:<endpoint>`, and `source` must be an endpoint (`<step>.<port>` or `inputs.<name>`). The invocation document embeds the same envelope definition for each input port.
- The engine builds envelopes with `make_envelope()` (`src/runtime/engine/envelope.hpp`); the id's prefix is `fingerprint16()`, the first 16 hex digits of the run fingerprint. With strict validation, every envelope it builds, for workflow inputs and step outputs alike, is checked against `pmp-envelope`. Only a runtime defect can break one, so a violation is `INTERNAL` (exit 70) and blames neither the module nor the data: its details name what the envelope wraps (`step`, `module` and `port` for a step's output, `input` for a workflow input), the `tick` and the schema `issues`, and a step's invocation still counts as succeeded (ADR-0011).
- The AsyncAPI document comes from `export_graph()` (`src/runtime/api/graph_export.hpp`), behind `Runtime::graph()` and `praenora graph --format asyncapi`: two-space indented JSON with sorted keys and a final newline. `info` holds the workflow's name as `title`, its version and, when the definition has one, its summary as `description` (`example.estimate-predict` has none). There is one channel per connection source, keyed by the endpoint, with address `<workflow name>/<endpoint>` and its one message keyed by the message type's id; one operation `<endpoint>.publish` (action `send`) per channel; and one components message per message type, `{name: <id>, title: "<id>@<version>", contentType: "application/json", payload}`, the title being the only place that names the version.
- Each payload is self-contained: the `pmp-envelope` schema without `$schema`, `$id` and `definitions`, its local references (`"$ref": "#/..."`) replaced by the definitions they name, and with `properties.data` replaced by the message type's schema, likewise without `$schema`, its local references inlined and its `definitions` dropped. Embedded as they are, every payload would repeat the envelope's `$id` and its references would resolve against the AsyncAPI document's root. Since messages are keyed by type, a workflow that carries two versions of one message type cannot be exported (`UNSUPPORTED`), nor can a schema with a recursive local reference.
- The generated documents are committed as `spec/asyncapi/<name>-<version>.asyncapi.json` for `example.estimate-predict` and `pmp.continuous-forecast`. `GraphExport.CommittedAsyncApiMatchesGenerator` compares them with the generator's output byte for byte (with `PRAENORA_UPDATE_GOLDEN=1` it rewrites them instead), and `cli.graph.asyncapi_matches_committed` compares the CLI's output with the committed file.
- Recorded outputs (`outputs/<output>.jsonl`) and the message trace (`trace/messages.jsonl`) are JSON lines: `canonical_json()` of each envelope or trace record, then `\n`, written in binary mode. A trace record is `{tick, step, direction, port, envelope[, diagnostics]}`. An `in` record is written, before the call, for each envelope a step receives; workflow inputs appear there, with source `inputs.<name>`. An `out` record is written for each envelope a step produces, with the result's `diagnostics` object when it has one. A result with a `diagnostics` object but no outputs gets an `out` record of its own, with `port` and `envelope` null.
- Later: [ADR-0032](ADR-0032-service-lookups-in-the-message-trace.md) adds lookup records to the trace, `{tick, step, phase, service, major, provided, count}`: one per runtime service a call into a step's instance looked up, written right after the call (for an invocation, between its `in` and its `out` records), with a null `tick` outside invocations.
- The documented CloudEvents mapping is completed as follows, still without an implementation: `specversion` is `"1.0"` and `datacontenttype` `"application/json"`; `pmp_version` maps to the extension `pmpversion` (a String); `tick` maps to `pmptick` as a String holding the tick in decimal digits, since a CloudEvents Integer is 32-bit while ticks go up to 2^53 - 1 (a first draft of this note mapped it to an Integer, which could not hold ticks above 2147483647; nothing implements the mapping and it was never released, so the correction leaves the PMP version at 0.1.0); `logical_time` maps to `pmplogicaltime` as a String holding the number as canonical JSON writes it; and `dataschema` is left out, because PMP 0.1 gives message-type data schemas no URI (they have no `$id`), while `type` already names the catalog entry that holds the schema.

## Alternatives considered

- **Custom envelope with no mapping.** Simpler to write, but no path to interoperate.
- **Native CloudEvents envelopes.** Fully conformant CloudEvents would force `logical_time` (a float, which CloudEvents lacks) into a string-encoded extension attribute and `tick` into a 32-bit integer, and PMP envelopes deliberately never carry wall-clock time (the optional CloudEvents `time` attribute is never populated).
- **No machine-readable topology description.** Harder to document and to validate against.
- **AsyncAPI 2.x.** Superseded by 3.0 for the channel and operation model used here.

## Consequences

- Envelopes stay deterministic while remaining mappable to CloudEvents.
- Generated AsyncAPI documents need regeneration whenever a workflow definition changes; tests detect drift.

## References

- [docs/specs/protocol.md](../specs/protocol.md)
- [CloudEvents 1.0](https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md)
- [AsyncAPI 3.0.0](https://www.asyncapi.com/docs/reference/specification/v3.0.0)
