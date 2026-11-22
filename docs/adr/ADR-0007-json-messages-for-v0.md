# ADR-0007: JSON messages for v0; nlohmann/json; binary encodings deferred

- Status: Accepted
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

The module API starts as JSON in, JSON out. The runtime needs a JSON library, and large numeric arrays might eventually justify a binary encoding.

## Decision

- All ABI documents and workflow messages are UTF-8 JSON in v0.
- The runtime uses nlohmann/json internally (PRIVATE dependency; no nlohmann types in public headers, see ADR-0023). Canonical JSON for fingerprints and outputs is nlohmann's `dump()` with no indent (ADR-0016).
- The C++ SDK adapter has no JSON-library dependency; modules may use any parser. The examples use nlohmann/json.
- Binary payloads (Protocol Buffers or Arrow) are not part of v0; they would need a minor or major ABI evolution and a protocol addition (OQ-7).

## Alternatives considered

- **Protocol Buffers or FlatBuffers.** Compact and typed, but add a codegen dependency for module authors and obscure payloads during debugging.
- **Apache Arrow.** Good for large arrays, heavier dependency, and not needed by the MVP examples.
- **A different JSON library (RapidJSON, simdjson).** Faster in places, but nlohmann/json is the library chosen for the project and has the validator integration.

## Consequences

- Payloads are human-readable and easy to trace and diff.
- Large arrays pay a size and parsing cost; revisit under OQ-7.
- Determinism of number formatting depends on nlohmann's shortest round-trip formatter.

## References

- [docs/specs/protocol.md](../specs/protocol.md)
- [nlohmann/json](https://github.com/nlohmann/json)
- [docs/open-questions.md](../open-questions.md)
