# ADR-0001: Record architecture decisions

- Status: Accepted
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Praenora spans a native ABI, a protocol, a build system and several language surfaces, and separate teams build modules against it. Choices made early are costly to reverse, and the reasons behind them are lost unless written down. Some decisions are fixed by project requirements; others are provisional calls that need maintainer review.

## Decision

We record each significant architecture decision as its own document in `docs/adr/`.
- One file per decision, named `ADR-NNNN-<kebab-title>.md`, with a unique, never-reused label.
- Each ADR has the sections Context, Decision, Alternatives considered, Consequences and References, plus a metadata list with Status, Date, Supersedes and Superseded by (see `template.md`).
- Statuses are Proposed, Accepted, Superseded and Deprecated. Decisions required by the project are Accepted. Provisional decisions made while designing the MVP are Proposed: the build proceeds on them and they stay open to review.
- An ADR is superseded by writing a new ADR and updating both headers; the old text is kept.
- Calls made while building that do not change what gets built are appended to the relevant ADR as a "Builder notes" subsection under Decision, with status Proposed.
- Open questions that remain are tracked in [docs/open-questions.md](../open-questions.md) and link to ADRs.

## Alternatives considered

- **Wiki or issue tracker only.** Easy to edit, but history is weak and the record is not versioned with the code.
- **Design documents without per-decision records.** Good for narrative, poor for knowing which choices are settled and which are provisional.
- **Code comments only.** Close to the code, but decisions cut across components and cannot be reviewed as a unit.

## Consequences

- Reviewers can see which decisions are firm and which are open.
- Changing a decision costs a new ADR, which keeps reversals deliberate.
- Authors must keep the index in `docs/adr/README.md` current.

## References

- [Documenting Architecture Decisions](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions)
- [docs/adr/README.md](README.md)
- [docs/open-questions.md](../open-questions.md)
