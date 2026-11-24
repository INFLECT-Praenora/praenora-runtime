// What a Workflow (praenora/runtime/workflow.hpp) holds: the run's engine and everything around it.
// Runtime::open() builds it; the Workflow's methods use it.
#ifndef PRAENORA_RUNTIME_API_WORKFLOW_IMPL_HPP
#define PRAENORA_RUNTIME_API_WORKFLOW_IMPL_HPP

#include "api/validation.hpp"
#include "engine/engine.hpp"
#include "observability/logging.hpp"
#include "observability/metrics.hpp"
#include "observability/otlp/exporter.hpp"
#include "observability/path_anchors.hpp"
#include "observability/status_file.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/workflow.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace praenora {

// The members are declared in the order of their lifetimes, so they are destroyed in reverse,
// whether or not close() ran: the context outlives the validated workflow inside the engine (which
// points into its catalog) and the instances (which use its services); the run's path anchors,
// the logs and the metrics outlive the engine and the status-file writer, whose producer and error
// handler use only the metrics and the anchors (status_document()); the writer, and
// `status_target` through which the engine's state changes notify it, outlive the engine, so a
// state change the engine's destructor makes (shutting down a run that is still ready or running)
// still reaches the status file, and the writer's last write, in its own destructor, shows the
// final state. The run's place in the OTLP export (`telemetry`), whose snapshots read the metrics,
// is removed before the metrics are gone.
struct Workflow::Impl {
    // Prepares the run of `workflow`, validated against `context`, so that nothing that can fail
    // runs after an earlier run in the output directory is removed: check_output_dir() first
    // (refusing an earlier run without `overwrite` before anything is written), then the log hub
    // with `logging` (which may create the log file), the engine (which registers the run and its
    // steps with the metrics), then prepare_output_dir() with `overwrite` (which removes an earlier
    // run), and, last, the status-file writer, whose first write is in its constructor, so a status
    // file inside the directory is never written before the earlier run is refused, nor removed
    // afterwards. Before all that, it fixes the run's path anchors (detail::PathAnchors): its
    // directories and the home directory as they are now, which the error records of the run
    // record and of the status file name by placeholders. With the context's OTLP exporter, the
    // log hub gets the exporter's sink, with the run's attributes (`telemetry_run`, whose number
    // Runtime::open() sets), and the run is added to the export (`telemetry`) once the engine
    // exists. Throws praenora::Error: IO_ERROR from the output directory or the log file, and the
    // engine's errors.
    Impl(std::shared_ptr<const detail::RuntimeContext> runtime_context,
         detail::ValidatedWorkflow workflow, const LogOptions& logging, bool overwrite);
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // Throws praenora::Error(INVALID_STATE) for control call `call` once the workflow is closed.
    void refuse_when_closed(const char* call) const;

    // Keeps `error`, which a control call is about to throw, when the run failed with it (the
    // first such error only), so that run() can throw it again later.
    void keep_failure(const Error& error);

    // A snapshot of the metrics taken now as a status-snapshot document (compact JSON, bytes that
    // are not UTF-8 replaced), its error records in `form`: as they are, with absolute paths, or
    // naming the run's directories by the placeholders of `anchors`. The status file holds the
    // placeholders form, and Workflow::status_json(PathForm) gives either. Thread-safe.
    std::string status_document(PathForm form) const;

    std::shared_ptr<const detail::RuntimeContext> context;
    std::string output_dir; // absolute UTF-8 path; empty when the run writes no files
    // The run's directories as they were when it was opened, each with its placeholder; the engine
    // holds a share of them for the run record.
    std::shared_ptr<const detail::PathAnchors> anchors;
    // Workflow::run_number(): set by Runtime::open() once the Workflow exists, before it is
    // returned; never changed afterwards.
    std::uint64_t run_number = 0;
    // The run as the OTLP export names it: its fingerprint, and the run number, which
    // Runtime::open() sets with run_number; none without an exporter.
    std::shared_ptr<detail::OtlpRun> telemetry_run;
    ValidationReport diagnostics; // the warnings and notes of the validation that opened the run
    std::unique_ptr<detail::LogHub> logs;
    detail::MetricsRegistry metrics;
    // The run's metrics in the OTLP export, from its snapshots: finished by close(), which has the
    // final state exported; empty without an exporter.
    detail::OtlpExporter::Run telemetry;
    std::unique_ptr<detail::StatusFileWriter> status_file;
    // The status-file writer that a change of the run's state notifies, or none.
    std::atomic<detail::StatusFileWriter*> status_target{nullptr};
    std::unique_ptr<detail::Engine> engine;
    std::atomic<bool> busy{false}; // a control call (start, tick, run, close) is in progress
    // The members below are read and written only by a control call, while busy.
    bool closed = false;          // close() has finished
    std::optional<Error> failure; // what start(), tick() or run() threw when the run failed
};

} // namespace praenora

#endif // PRAENORA_RUNTIME_API_WORKFLOW_IMPL_HPP
