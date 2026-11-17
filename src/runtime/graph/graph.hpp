// The graph of a workflow definition: static validation of its connections and the deterministic
// step schedule.
//
// Rules:
// - every endpoint names an existing step and port, or a declared workflow input or output
//   (PORT_UNKNOWN); a step's capability must be in the catalog (CATALOG_ENTRY_NOT_FOUND);
// - producer and consumer types are equal: the same id (PORT_TYPE_MISMATCH) and the same version
//   (MESSAGE_TYPE_INCOMPATIBLE);
// - each step input and each workflow output has at most one incoming connection (GRAPH_INVALID);
// - every required step input and every workflow output is connected (PORT_UNCONNECTED);
// - delay is 0 or 1; a delayed connection feeds only an optional step input (GRAPH_INVALID);
// - a required input is fed, with delay 0, only by a workflow input or a required output
//   (GRAPH_INVALID);
// - the delay-0 connections between steps form no cycle (GRAPH_CYCLE, naming the cycle as
//   "a -> b -> a").
// The schedule is the topological order of the steps over delay-0 connections, ties broken by
// lexicographic step id.
#ifndef PRAENORA_RUNTIME_GRAPH_GRAPH_HPP
#define PRAENORA_RUNTIME_GRAPH_GRAPH_HPP

#include "catalog/catalog.hpp"
#include "core/semver.hpp"

#include <praenora/runtime/error.hpp>

#include <map>
#include <string>
#include <vector>

namespace praenora::detail {

struct ResolvedConnection {
    Endpoint from, to;
    int delay = 0;
    std::string type; // the message type carried
    SemVer type_version;
};

struct WorkflowGraph {
    const WorkflowDef* definition = nullptr;
    std::vector<std::string> schedule; // step ids in execution order
    // step id -> input port -> the connection feeding it
    std::map<std::string, std::map<std::string, ResolvedConnection>> incoming;
    // workflow output name -> the connection feeding it
    std::map<std::string, ResolvedConnection> outputs;
};

// Every rule violation of `definition`, in a deterministic order. Diagnostics are located in the
// workflow definition's catalog entry (Catalog::locate(): its file, line, column and JSON pointer).
ValidationReport validate_graph(const WorkflowDef& definition, const Catalog& catalog);

// The graph of a valid definition. Throws praenora::Error with the first error validate_graph()
// reports.
WorkflowGraph build_graph(const WorkflowDef& definition, const Catalog& catalog);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_GRAPH_GRAPH_HPP
