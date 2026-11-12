# Workflow definitions and capabilities

- **Version:** PMP 0.1.0; document formats `workflow-definition` 0.1.0 and
  `capability-definition` 0.1.0
- **Status:** Normative, MVP. Workflow definitions as versioned catalog data are Accepted
  ([ADR-0010](../adr/ADR-0010-workflow-definitions-as-catalog-data.md)); the graph rules and the
  execution model are Proposed ([ADR-0011](../adr/ADR-0011-tick-based-synchronous-execution.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

A workflow definition is a versioned graph template in the protocol catalog: its steps, each typed by
a capability, its workflow inputs and outputs, and the connections between them. A workflow
configuration pins one definition and binds its steps to modules; the topology always comes from the
definition, never from the configuration. This document specifies the two catalog kinds involved and
the rules a definition MUST satisfy. How catalog entries are stored, identified and loaded is in
[protocol.md](protocol.md#catalog).

## Capability definitions

A capability (`capability-definition`,
[`capability-definition.0.1.schema.json`](../../spec/schemas/capability-definition.0.1.schema.json))
says what a step needs: named input and output ports, each with a message type and a `required`
flag.

| Field | Required | Meaning |
|---|---|---|
| `kind`, `format_version` | yes | `capability-definition`, `0.1.0`. |
| `id`, `version` | yes | Catalog id and semantic version. |
| `status` | yes | `draft`, `stable` or `deprecated`. |
| `summary` | no | One sentence. |
| `inputs`, `outputs` | yes | `{<port>: {type, type_version, required, summary?}}`; either MAY be empty. |

- A port's `type` and `type_version` name a message type of the catalog.
- A required input MUST receive a message in every tick; an optional input MAY lack one.
- A module MUST write every required output in every result and MAY leave out an optional one; it
  MUST NOT write a port the capability does not declare (`OUTPUT_INVALID`).
- A module implements a capability when its manifest and its `describe()` document list the
  capability's id at exactly the definition's version.

## Workflow definitions

A workflow definition (`workflow-definition`,
[`workflow-definition.0.1.schema.json`](../../spec/schemas/workflow-definition.0.1.schema.json)):

| Field | Required | Meaning |
|---|---|---|
| `kind`, `format_version` | yes | `workflow-definition`, `0.1.0`. |
| `name`, `version` | yes | Catalog id and semantic version. |
| `status` | yes | `draft`, `stable` or `deprecated`. |
| `summary` | no | One sentence. |
| `inputs` | no | `{<name>: {type, type_version}}`: the workflow inputs, which the configuration binds to data sources. |
| `outputs` | yes | `{<name>: {type, type_version}}`: the workflow outputs, which a run can record. |
| `steps` | yes, at least one | `{<step>: {capability, capability_version}}`. |
| `connections` | yes | An array of `{from, to, delay?}`. |

A connection's `from` is a step output (`<step>.<port>`) or a workflow input (`inputs.<name>`); its
`to` is a step input (`<step>.<port>`) or a workflow output (`outputs.<name>`). `delay` is 0 (the
default) or 1: a connection with delay 1 is a feedback connection, which delivers the message of the
previous tick. Step ids, port names and input and output names follow the patterns in
[protocol.md](protocol.md#identifiers); `inputs` and `outputs` are reserved and cannot be step ids.

The example workflow, [`example.estimate-predict@0.1.0`](../../spec/catalog/workflows/example.estimate-predict/0.1.0.json),
feeds its predictions back into the next tick's estimate:

```json
{
  "kind": "workflow-definition", "format_version": "0.1.0",
  "name": "example.estimate-predict", "version": "0.1.0", "status": "stable",
  "inputs":  { "observations": { "type": "pmp.observation-batch", "type_version": "0.1.0" } },
  "steps":   { "estimate": { "capability": "example.estimate", "capability_version": "0.1.0" },
               "predict":  { "capability": "example.predict",  "capability_version": "0.1.0" } },
  "connections": [
    { "from": "inputs.observations", "to": "estimate.observations" },
    { "from": "estimate.state",      "to": "predict.state" },
    { "from": "predict.predictions", "to": "estimate.prior", "delay": 1 },
    { "from": "predict.predictions", "to": "outputs.predictions" },
    { "from": "estimate.state",      "to": "outputs.state" } ],
  "outputs": { "predictions": { "type": "pmp.prediction-set",     "type_version": "0.1.0" },
               "state":       { "type": "example.state-estimate", "type_version": "0.1.0" } }
}
```

The catalog's other definition is the draft starting workflow,
[`pmp.continuous-forecast@0.1.0`](../../spec/catalog/workflows/pmp.continuous-forecast/0.1.0.json)
([starting-workflow.md](starting-workflow.md)).

## Graph rules

A workflow definition MUST satisfy every rule below. Each violation is reported with its code,
located in the definition's catalog entry (file, line, column and JSON pointer):

| # | Rule | Error | Pointer |
|---:|---|---|---|
| 1 | Every step's capability is in the catalog. | `CATALOG_ENTRY_NOT_FOUND` | `/steps/<step>/capability` |
| 2 | A connection's `delay` is 0 or 1. | `GRAPH_INVALID` | `/connections/<i>/delay` |
| 3 | A connection starts at an output port of its step's capability or at a declared workflow input, and ends at an input port of its step's capability or at a declared workflow output. | `PORT_UNKNOWN` | `/connections/<i>/from`, `/connections/<i>/to` |
| 4 | A step input or a workflow output has at most one incoming connection. Fan-out is allowed; fan-in is not. | `GRAPH_INVALID` | `/connections/<i>/to` |
| 5 | Both ends of a connection carry the same message type: the same id and the same version. | `PORT_TYPE_MISMATCH` | `/connections/<i>` |
| 6 | A delayed connection feeds an optional step input; it never feeds a required input (in tick 0 there is no earlier message) or a workflow output. | `GRAPH_INVALID` | `/connections/<i>/delay` |
| 7 | A required step input is fed, with delay 0, by a workflow input or by a required output; an optional output cannot feed it. | `GRAPH_INVALID` | `/connections/<i>/from` |
| 8 | Every required step input is connected. | `PORT_UNCONNECTED` | `/steps/<step>` |
| 9 | Every workflow output is connected. | `PORT_UNCONNECTED` | `/outputs/<name>` |
| 10 | The delay-0 connections between steps form no cycle. | `GRAPH_CYCLE`, naming the cycle as `a -> b -> a` | `/connections` |

A workflow input MAY be left unconnected; the configuration still binds it. The message types the
definition's inputs and outputs and its capabilities' ports name MUST be in the catalog
(`CATALOG_ENTRY_NOT_FOUND`, checked when a configuration that pins the definition is validated).

### How the rules are checked

- Graph validation reports every violation it finds, in this order: unknown capabilities (rule 1),
  then each connection in definition order (rules 2 to 7), then unconnected ports (rules 8 and 9),
  then the first cycle (rule 10).
- Only the first cycle is reported. The search is a depth-first search over the delay-0 connections
  between steps that visits steps, and the successors of each step, in lexicographic order of step
  ids. When there is a cycle, no schedule is computed.
- An unknown capability skips that step's connection checks. For a connection that starts or ends
  at a port of the step, only its delay value (rule 2) and its other end (rule 3, and rule 4 when
  that end is the target) are checked; its message types (rule 5) and its delay placement (rules 6
  and 7) are not, and neither are the step's required inputs (rule 8).
- The schema admits any integer `delay` from 0 to 2147483647 (default 0); graph validation rejects
  every value other than 0 and 1 (rule 2). A connection with such a delay is not a delay-0
  connection, so it takes no part in the cycle search or the schedule, and rule 6 applies to it as
  to any delayed connection.
- When a configuration is validated, the graph is validated only if every capability and message
  type the definition names is in the catalog; otherwise the unknown entries are reported
  (`CATALOG_ENTRY_NOT_FOUND`) and the graph is not. `praenora graph` validates the graph of any
  definition and reports as described above.
- A run, and `praenora graph`, refuse a definition that breaks any rule, with its first error.

## Schedule

The steps run in a schedule computed once per run: Kahn's algorithm over the delay-0 connections
between steps, which always takes the smallest ready step id, so ties are broken by lexicographic
order. Connections from workflow inputs, to workflow outputs and with delay 1 do not constrain it.
The schedule of `example.estimate-predict` is `estimate, predict`; that of
`pmp.continuous-forecast` is `propose, curate, predict`. What the connections deliver in each tick is
specified in [protocol.md](protocol.md#execution-semantics).

## Binding a definition

A workflow configuration (`workflow-config`) uses a definition as follows
([ADR-0010](../adr/ADR-0010-workflow-definitions-as-catalog-data.md)):

- `workflow.name` and `workflow.version` pin the definition (`WORKFLOW_NOT_FOUND` when the catalog
  lacks it; the message lists the versions it has).
- `steps` binds every step of the definition, and no other, to a module name and exact version, with
  parameters (`BINDING_INVALID` otherwise). The module MUST implement the step's capability
  ([manifest.md](manifest.md#compatibility-and-library-checks)).
- `inputs` binds every workflow input, and no other, to a data source (`INPUT_BINDING_INVALID`
  otherwise).
- `outputs.record` lists the workflow outputs to record (`CONFIG_INVALID` for a name that is not a
  workflow output); without it, every workflow output is recorded.

## Versioning

A catalog entry is immutable once published: a change to a definition's steps, ports, connections or
types MUST be a new version of the entry, and a change to a capability or message type a new version
of that entry. In v0 every reference is exact: a step names one capability version, a port one
message type version, and a module must list the exact capability version
([compatibility-policy.md](compatibility-policy.md)). Changing the workflow a deployment runs is a
catalog change and a configuration change, never a code change.

## Inspecting a definition

`praenora graph (<config> | --workflow <name>@<version>) [--format text|json|dot|asyncapi]` prints a
definition's schedule and connections: as text, as JSON
(`{"workflow", "schedule", "connections": [{"from", "to", "delay", "type", "type_version"}]}`), as a
Graphviz DOT graph (delayed connections dashed) or as an AsyncAPI document
([protocol.md](protocol.md#asyncapi-documents)).

## References

- [ADR-0010](../adr/ADR-0010-workflow-definitions-as-catalog-data.md): definitions as catalog data.
- [ADR-0011](../adr/ADR-0011-tick-based-synchronous-execution.md): ticks, schedule and delays.
- [ADR-0014](../adr/ADR-0014-versioning-and-compatibility-policy.md): versions.
- [open-questions.md](../open-questions.md): OQ-6 (activation, failure policies, delays above 1),
  OQ-7 (evolution of message types), OQ-11 (parallel steps).
- [configuration.md](../configuration.md): binding a definition in a configuration.
