#include "graph/graph.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

namespace praenora::detail {

namespace {

// What an endpoint carries: its message type, and whether the port is required (workflow inputs
// and outputs count as required).
struct PortInfo {
    std::string type;
    SemVer type_version;
    bool required = true;
};

class GraphAnalysis {
public:
    GraphAnalysis(const WorkflowDef& definition, const Catalog& catalog)
        : definition_(definition), catalog_(catalog),
          who_("workflow " + definition.name + "@" + to_string(definition.version) + ": ") {
        graph_.definition = &definition;
        resolve_capabilities();
        for (std::size_t index = 0; index < definition_.connections.size(); ++index) {
            add_connection(index);
        }
        check_connected();
        if (find_cycle()) {
            return;
        }
        compute_schedule();
    }

    ValidationReport& report() { return report_; }
    WorkflowGraph& graph() { return graph_; }

private:
    void add(ErrorCode code, std::string message, std::string pointer, std::string step = {}) {
        Diagnostic diagnostic;
        diagnostic.severity = Severity::error;
        diagnostic.code = code;
        diagnostic.message = who_ + std::move(message);
        // Located in the definition's catalog entry: its file, line and column.
        diagnostic.location =
            catalog_.locate("workflow-definition", definition_.name, definition_.version, pointer);
        diagnostic.step = std::move(step);
        report_.add(std::move(diagnostic));
    }

    void resolve_capabilities() {
        for (const auto& [id, step] : definition_.steps) {
            const CapabilityDef* capability =
                catalog_.find_capability(step.capability, step.capability_version);
            if (capability == nullptr) {
                add(ErrorCode::CATALOG_ENTRY_NOT_FOUND,
                    "step '" + id + "' uses unknown capability " + step.capability + "@" +
                        to_string(step.capability_version),
                    "/steps/" + id + "/capability", id);
            }
            capabilities_[id] = capability;
            edges_[id];
        }
    }

    // The port a connection starts (`source`) or ends at; reports PORT_UNKNOWN when there is none.
    std::optional<PortInfo> resolve(const Endpoint& endpoint, bool source,
                                    const std::string& pointer) {
        const std::string text = endpoint.to_string();
        const bool workflow_side = endpoint.kind != Endpoint::Kind::step;
        if (workflow_side) {
            const bool is_input = endpoint.kind == Endpoint::Kind::workflow_input;
            if (is_input != source) {
                add(ErrorCode::PORT_UNKNOWN,
                    source ? "connection source " + text + " is a workflow output"
                           : "connection target " + text + " is a workflow input",
                    pointer);
                return std::nullopt;
            }
            const auto& ports = is_input ? definition_.inputs : definition_.outputs;
            const auto it = ports.find(endpoint.port);
            if (it == ports.end()) {
                add(ErrorCode::PORT_UNKNOWN,
                    std::string("connection ") + (source ? "source " : "target ") + text +
                        " names no declared workflow " + (is_input ? "input" : "output"),
                    pointer);
                return std::nullopt;
            }
            return PortInfo{it->second.type, it->second.type_version, true};
        }
        const auto step = capabilities_.find(endpoint.node);
        if (step == capabilities_.end()) {
            add(ErrorCode::PORT_UNKNOWN,
                std::string("connection ") + (source ? "source " : "target ") + text +
                    " names unknown step '" + endpoint.node + "'",
                pointer);
            return std::nullopt;
        }
        if (step->second == nullptr) {
            return std::nullopt; // unknown capability, already reported
        }
        const CapabilityDef& capability = *step->second;
        const auto& ports = source ? capability.outputs : capability.inputs;
        const auto it = ports.find(endpoint.port);
        if (it == ports.end()) {
            add(ErrorCode::PORT_UNKNOWN,
                std::string("connection ") + (source ? "source " : "target ") + text +
                    ": capability " + capability.id + "@" + to_string(capability.version) +
                    " has no " + (source ? "output" : "input") + " port '" + endpoint.port + "'",
                pointer, endpoint.node);
            return std::nullopt;
        }
        return PortInfo{it->second.type, it->second.type_version, it->second.required};
    }

    void add_connection(std::size_t index) {
        const ConnectionDef& connection = definition_.connections[index];
        const std::string base = "/connections/" + std::to_string(index);
        const std::string text = connection.from.to_string() + " -> " + connection.to.to_string();
        if (connection.delay < 0 || connection.delay > 1) {
            add(ErrorCode::GRAPH_INVALID,
                "connection " + text + " has delay " + std::to_string(connection.delay) +
                    "; the delay of a connection is 0 or 1",
                base + "/delay");
        }
        const auto source = resolve(connection.from, true, base + "/from");
        const auto target = resolve(connection.to, false, base + "/to");
        if (!target) {
            return;
        }
        const bool to_step = connection.to.kind == Endpoint::Kind::step;
        const bool taken = connected_.count(connection.to.to_string()) > 0;
        if (taken) {
            add(ErrorCode::GRAPH_INVALID,
                "connection " + text + ": " + connection.to.to_string() +
                    " already has an incoming connection (fan-in is not allowed)",
                base + "/to", to_step ? connection.to.node : std::string());
        }
        connected_.insert(connection.to.to_string());
        if (!source) {
            return;
        }
        if (source->type != target->type) {
            add(ErrorCode::PORT_TYPE_MISMATCH,
                "connection " + text + " carries " + source->type + "@" +
                    to_string(source->type_version) + " into a port of type " + target->type + "@" +
                    to_string(target->type_version),
                base);
        } else if (source->type_version != target->type_version) {
            add(ErrorCode::MESSAGE_TYPE_INCOMPATIBLE,
                "connection " + text + " carries " + source->type + "@" +
                    to_string(source->type_version) + " into a port that requires " + target->type +
                    "@" + to_string(target->type_version) +
                    "; both ends of a connection carry the same version of a message type",
                base);
        }
        if (connection.delay > 0) {
            if (!to_step) {
                add(ErrorCode::GRAPH_INVALID,
                    "connection " + text + ": a delayed connection cannot feed a workflow output",
                    base + "/delay");
            } else if (target->required) {
                add(ErrorCode::GRAPH_INVALID,
                    "connection " + text +
                        ": a delayed connection may feed only an optional "
                        "input, because at tick 0 there is no earlier message",
                    base + "/delay", connection.to.node);
            }
        } else if (to_step && target->required && connection.from.kind == Endpoint::Kind::step &&
                   !source->required) {
            add(ErrorCode::GRAPH_INVALID,
                "connection " + text + ": a required input is fed by an optional output",
                base + "/from", connection.to.node);
        }
        if (taken) {
            return;
        }
        ResolvedConnection resolved{connection.from, connection.to, connection.delay, source->type,
                                    source->type_version};
        if (to_step) {
            graph_.incoming[connection.to.node].emplace(connection.to.port, std::move(resolved));
        } else {
            graph_.outputs.emplace(connection.to.port, std::move(resolved));
        }
        if (connection.delay == 0 && to_step && connection.from.kind == Endpoint::Kind::step) {
            edges_[connection.from.node].insert(connection.to.node);
        }
    }

    void check_connected() {
        for (const auto& [id, capability] : capabilities_) {
            if (capability == nullptr) {
                continue;
            }
            for (const auto& [port, def] : capability->inputs) {
                if (def.required && connected_.count(id + "." + port) == 0) {
                    add(ErrorCode::PORT_UNCONNECTED,
                        "required input " + id + "." + port + " is not connected", "/steps/" + id,
                        id);
                }
            }
        }
        for (const auto& [name, port] : definition_.outputs) {
            (void)port;
            if (connected_.count("outputs." + name) == 0) {
                add(ErrorCode::PORT_UNCONNECTED,
                    "workflow output outputs." + name + " is not connected", "/outputs/" + name);
            }
        }
    }

    // Depth-first search over delay-0 connections, visiting steps and their successors in
    // lexicographic order; reports the first cycle found.
    bool find_cycle() {
        std::map<std::string, int> color; // 0 unvisited, 1 on the stack, 2 done
        std::vector<std::string> stack;
        for (const auto& entry : edges_) {
            if (color[entry.first] == 0 && visit(entry.first, color, stack)) {
                return true;
            }
        }
        return false;
    }

    bool visit(const std::string& step, std::map<std::string, int>& color,
               std::vector<std::string>& stack) {
        color[step] = 1;
        stack.push_back(step);
        for (const std::string& next : edges_[step]) {
            if (color[next] == 1) {
                const auto start = std::find(stack.begin(), stack.end(), next);
                std::string cycle;
                for (auto it = start; it != stack.end(); ++it) {
                    cycle += *it + " -> ";
                }
                cycle += next;
                add(ErrorCode::GRAPH_CYCLE,
                    "the connections with delay 0 form a cycle: " + cycle +
                        " (a feedback connection needs delay 1)",
                    "/connections", next);
                return true;
            }
            if (color[next] == 0 && visit(next, color, stack)) {
                return true;
            }
        }
        stack.pop_back();
        color[step] = 2;
        return false;
    }

    // Kahn's algorithm over delay-0 connections, always taking the smallest ready step id.
    void compute_schedule() {
        std::map<std::string, int> indegree;
        for (const auto& [step, successors] : edges_) {
            indegree[step];
            for (const auto& next : successors) {
                ++indegree[next];
            }
        }
        std::set<std::string> ready;
        for (const auto& [step, count] : indegree) {
            if (count == 0) {
                ready.insert(step);
            }
        }
        while (!ready.empty()) {
            const std::string step = *ready.begin();
            ready.erase(ready.begin());
            graph_.schedule.push_back(step);
            for (const auto& next : edges_[step]) {
                if (--indegree[next] == 0) {
                    ready.insert(next);
                }
            }
        }
    }

    const WorkflowDef& definition_;
    const Catalog& catalog_;
    const std::string who_;
    ValidationReport report_;
    WorkflowGraph graph_;
    std::map<std::string, const CapabilityDef*> capabilities_;
    std::map<std::string, std::set<std::string>> edges_; // delay-0 step -> step
    std::set<std::string> connected_; // endpoints that have an incoming connection
};

} // namespace

ValidationReport validate_graph(const WorkflowDef& definition, const Catalog& catalog) {
    GraphAnalysis analysis(definition, catalog);
    return std::move(analysis.report());
}

WorkflowGraph build_graph(const WorkflowDef& definition, const Catalog& catalog) {
    GraphAnalysis analysis(definition, catalog);
    if (const Diagnostic* first = analysis.report().first_error()) {
        throw Error(first->code, first->message, first->location);
    }
    return std::move(analysis.graph());
}

} // namespace praenora::detail
