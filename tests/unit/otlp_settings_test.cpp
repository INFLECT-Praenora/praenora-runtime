// The settings of the OTLP export (observability/otlp/otlp_settings.hpp): every row of the table
// of OTEL_* variables in docs/specs/observability.md#otlp-export, read from the process
// environment (each test sets the variables it reads and removes the others, OTEL_SDK_DISABLED,
// which CTest sets for every test, included), TelemetryOptions under them, values that are ignored
// with a reason that never repeats them, use_environment off, endpoints, key-value lists and the
// resource.
#include "observability/otlp/otlp_settings.hpp"

#include "scoped_environment.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace {

using praenora::TelemetryOptions;
using praenora::detail::append_signal_path;
using praenora::detail::HttpEndpoint;
using praenora::detail::HttpHeaders;
using praenora::detail::IgnoredSetting;
using praenora::detail::otlp_resource;
using praenora::detail::OtlpResolution;
using praenora::detail::OtlpSettings;
using praenora::detail::parse_http_endpoint;
using praenora::detail::parse_key_value_list;
using praenora::detail::random_uuid_v4;
using praenora::detail::resolve_otlp_settings;
using praenora::test::ScopedEnvironment;
using std::chrono::milliseconds;

// Every variable the settings read.
const char* const kVariables[] = {
    "OTEL_SDK_DISABLED",
    "OTEL_EXPORTER_OTLP_ENDPOINT",
    "OTEL_EXPORTER_OTLP_METRICS_ENDPOINT",
    "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT",
    "OTEL_EXPORTER_OTLP_PROTOCOL",
    "OTEL_EXPORTER_OTLP_METRICS_PROTOCOL",
    "OTEL_EXPORTER_OTLP_LOGS_PROTOCOL",
    "OTEL_EXPORTER_OTLP_HEADERS",
    "OTEL_EXPORTER_OTLP_METRICS_HEADERS",
    "OTEL_EXPORTER_OTLP_LOGS_HEADERS",
    "OTEL_EXPORTER_OTLP_TIMEOUT",
    "OTEL_EXPORTER_OTLP_METRICS_TIMEOUT",
    "OTEL_EXPORTER_OTLP_LOGS_TIMEOUT",
    "OTEL_METRICS_EXPORTER",
    "OTEL_LOGS_EXPORTER",
    "OTEL_METRIC_EXPORT_INTERVAL",
    "OTEL_SERVICE_NAME",
    "OTEL_RESOURCE_ATTRIBUTES",
};

using Environment = std::map<std::string, std::string>;

// The settings from `options` and the process environment, with exactly the variables of
// `environment` set for the call.
OtlpResolution resolve(const Environment& environment, const TelemetryOptions& options = {}) {
    for (const auto& entry : environment) {
        bool known = false;
        for (const char* name : kVariables) {
            known = known || entry.first == name;
        }
        EXPECT_TRUE(known) << entry.first << " is not one of the variables the test sets";
    }
    std::vector<std::unique_ptr<ScopedEnvironment>> scoped;
    for (const char* name : kVariables) {
        const auto found = environment.find(name);
        scoped.push_back(std::make_unique<ScopedEnvironment>(
            name,
            found == environment.end() ? std::nullopt : std::optional<std::string>(found->second)));
    }
    return resolve_otlp_settings(options, true);
}

std::string url_of(const std::optional<HttpEndpoint>& endpoint) {
    return endpoint ? endpoint->url : std::string("none");
}

std::vector<std::string> variables_of(const OtlpResolution& resolution) {
    std::vector<std::string> names;
    for (const IgnoredSetting& ignored : resolution.ignored) {
        names.push_back(ignored.variable);
    }
    return names;
}

const IgnoredSetting* ignored_variable(const OtlpResolution& resolution, const std::string& name) {
    for (const IgnoredSetting& ignored : resolution.ignored) {
        if (ignored.variable == name) {
            return &ignored;
        }
    }
    return nullptr;
}

constexpr const char* kBase = "http://127.0.0.1:4318";

} // namespace

TEST(OtlpSettings, NothingIsExportedWithoutAnEndpoint) {
    const OtlpResolution resolution = resolve({});
    EXPECT_FALSE(resolution.settings.enabled());
    EXPECT_TRUE(resolution.ignored.empty());
    EXPECT_EQ(resolution.settings.metrics.timeout, milliseconds(10000));
    EXPECT_EQ(resolution.settings.logs.timeout, milliseconds(10000));
    EXPECT_EQ(resolution.settings.metric_export_interval, milliseconds(60000));
    EXPECT_EQ(resolution.settings.service_name, "praenora-runtime");
    EXPECT_TRUE(resolution.settings.resource_attributes.empty());
}

TEST(OtlpSettings, TheBaseEndpointGivesEachSignalItsPath) {
    const OtlpResolution plain = resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}});
    EXPECT_TRUE(plain.settings.enabled());
    EXPECT_EQ(url_of(plain.settings.metrics.endpoint), "http://127.0.0.1:4318/v1/metrics");
    EXPECT_EQ(url_of(plain.settings.logs.endpoint), "http://127.0.0.1:4318/v1/logs");
    EXPECT_TRUE(plain.ignored.empty());

    const OtlpResolution slash =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", "http://collector:4318/"}});
    EXPECT_EQ(url_of(slash.settings.metrics.endpoint), "http://collector:4318/v1/metrics");
    const OtlpResolution path = resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", "http://collector/otlp"}});
    EXPECT_EQ(url_of(path.settings.logs.endpoint), "http://collector:80/otlp/v1/logs");
    ASSERT_TRUE(path.settings.logs.endpoint);
    EXPECT_EQ(path.settings.logs.endpoint->host, "collector");
    EXPECT_EQ(path.settings.logs.endpoint->port, 80);
    EXPECT_EQ(path.settings.logs.endpoint->path, "/otlp/v1/logs");
}

TEST(OtlpSettings, ASignalsEndpointIsUsedAsItIs) {
    const OtlpResolution resolution =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                 {"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "http://127.0.0.1:9000/custom"}});
    EXPECT_EQ(url_of(resolution.settings.metrics.endpoint), "http://127.0.0.1:9000/custom");
    EXPECT_EQ(url_of(resolution.settings.logs.endpoint), "http://127.0.0.1:4318/v1/logs");
    const OtlpResolution root =
        resolve({{"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "http://[::1]:4318"}});
    EXPECT_EQ(url_of(root.settings.logs.endpoint), "http://[::1]:4318/");
    EXPECT_EQ(url_of(root.settings.metrics.endpoint), "none");
}

TEST(OtlpSettings, TheEnvironmentWinsOverTheOptions) {
    TelemetryOptions options;
    options.metrics_endpoint = "http://127.0.0.1:1/options/metrics";
    options.logs_endpoint = "http://127.0.0.1:1/options/logs";
    options.headers = {{"x-from", "options"}};
    options.export_timeout_ms = 3000;
    options.metric_export_interval_ms = 30000;
    const OtlpResolution none = resolve({}, options);
    EXPECT_EQ(url_of(none.settings.metrics.endpoint), "http://127.0.0.1:1/options/metrics");
    EXPECT_EQ(url_of(none.settings.logs.endpoint), "http://127.0.0.1:1/options/logs");
    EXPECT_EQ(none.settings.metrics.headers, (HttpHeaders{{"x-from", "options"}}));
    EXPECT_EQ(none.settings.logs.timeout, milliseconds(3000));
    EXPECT_EQ(none.settings.metric_export_interval, milliseconds(30000));
    EXPECT_TRUE(none.ignored.empty());

    const OtlpResolution over = resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                                         {"OTEL_EXPORTER_OTLP_HEADERS", "x-from=environment"},
                                         {"OTEL_EXPORTER_OTLP_TIMEOUT", "2000"},
                                         {"OTEL_METRIC_EXPORT_INTERVAL", "5000"}},
                                        options);
    EXPECT_EQ(url_of(over.settings.metrics.endpoint), "http://127.0.0.1:4318/v1/metrics");
    EXPECT_EQ(url_of(over.settings.logs.endpoint), "http://127.0.0.1:4318/v1/logs");
    EXPECT_EQ(over.settings.logs.headers, (HttpHeaders{{"x-from", "environment"}}));
    EXPECT_EQ(over.settings.metrics.timeout, milliseconds(2000));
    EXPECT_EQ(over.settings.metric_export_interval, milliseconds(5000));
}

TEST(OtlpSettings, SdkDisabledExportsNothing) {
    for (const char* value : {"true", "TRUE", "True"}) {
        const OtlpResolution resolution = resolve({{"OTEL_SDK_DISABLED", value},
                                                   {"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                                                   {"OTEL_EXPORTER_OTLP_TIMEOUT", "not a number"}});
        EXPECT_FALSE(resolution.settings.enabled()) << value;
        EXPECT_TRUE(resolution.ignored.empty()) << value << ": nothing else is read";
    }
    const OtlpResolution enabled =
        resolve({{"OTEL_SDK_DISABLED", "false"}, {"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}});
    EXPECT_TRUE(enabled.settings.enabled());
    EXPECT_TRUE(enabled.ignored.empty());
    const OtlpResolution other =
        resolve({{"OTEL_SDK_DISABLED", "yes"}, {"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}});
    EXPECT_TRUE(other.settings.enabled());
    EXPECT_EQ(variables_of(other), (std::vector<std::string>{"OTEL_SDK_DISABLED"}));
}

TEST(OtlpSettings, OnlyHttpJsonIsSupported) {
    for (const char* value : {"http/json", "HTTP/JSON"}) {
        const OtlpResolution resolution = resolve(
            {{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}, {"OTEL_EXPORTER_OTLP_PROTOCOL", value}});
        EXPECT_TRUE(resolution.settings.metrics.endpoint) << value;
        EXPECT_TRUE(resolution.settings.logs.endpoint) << value;
        EXPECT_TRUE(resolution.ignored.empty()) << value;
    }
    const OtlpResolution grpc =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}, {"OTEL_EXPORTER_OTLP_PROTOCOL", "grpc"}});
    EXPECT_FALSE(grpc.settings.enabled());
    EXPECT_EQ(variables_of(grpc), (std::vector<std::string>{"OTEL_EXPORTER_OTLP_PROTOCOL"}));

    const OtlpResolution mixed = resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                                          {"OTEL_EXPORTER_OTLP_PROTOCOL", "http/protobuf"},
                                          {"OTEL_EXPORTER_OTLP_METRICS_PROTOCOL", "http/json"}});
    EXPECT_TRUE(mixed.settings.metrics.endpoint);
    EXPECT_FALSE(mixed.settings.logs.endpoint);
    const OtlpResolution logs = resolve(
        {{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}, {"OTEL_EXPORTER_OTLP_LOGS_PROTOCOL", "grpc"}});
    EXPECT_TRUE(logs.settings.metrics.endpoint);
    EXPECT_FALSE(logs.settings.logs.endpoint);
    EXPECT_EQ(variables_of(logs), (std::vector<std::string>{"OTEL_EXPORTER_OTLP_LOGS_PROTOCOL"}));
}

TEST(OtlpSettings, HeadersArePercentDecoded) {
    const OtlpResolution resolution =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                 {"OTEL_EXPORTER_OTLP_HEADERS", " api-key = a%20b%2Cc ,x-team=blue,, "},
                 {"OTEL_EXPORTER_OTLP_METRICS_HEADERS", "x-only=metrics"}});
    EXPECT_EQ(resolution.settings.logs.headers,
              (HttpHeaders{{"api-key", "a b,c"}, {"x-team", "blue"}}));
    EXPECT_EQ(resolution.settings.metrics.headers, (HttpHeaders{{"x-only", "metrics"}}));
    EXPECT_TRUE(resolution.ignored.empty());
}

TEST(OtlpSettings, InvalidHeadersAreIgnoredWithoutTheirValues) {
    TelemetryOptions options;
    options.headers = {{"x-from", "options"}};
    for (const char* value :
         {"novalue", "a=%zz", "a=b%0Asecret", "content-type=text/plain", "bad name=x", "=x"}) {
        const OtlpResolution resolution =
            resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}, {"OTEL_EXPORTER_OTLP_HEADERS", value}},
                    options);
        EXPECT_EQ(resolution.settings.metrics.headers, options.headers) << value;
        const IgnoredSetting* ignored = ignored_variable(resolution, "OTEL_EXPORTER_OTLP_HEADERS");
        ASSERT_NE(ignored, nullptr) << value;
        EXPECT_EQ(ignored->reason.find("secret"), std::string::npos) << ignored->reason;
        EXPECT_EQ(ignored->reason.find(value), std::string::npos) << ignored->reason;
    }
}

TEST(OtlpSettings, TimeoutsAreMilliseconds) {
    const OtlpResolution resolution = resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                                               {"OTEL_EXPORTER_OTLP_TIMEOUT", "2500"},
                                               {"OTEL_EXPORTER_OTLP_LOGS_TIMEOUT", "200"}});
    EXPECT_EQ(resolution.settings.metrics.timeout, milliseconds(2500));
    EXPECT_EQ(resolution.settings.logs.timeout, milliseconds(200));
    EXPECT_TRUE(resolution.ignored.empty());
    for (const char* value : {"0", "-1", "1.5", "abc", "2147483648"}) {
        const OtlpResolution bad = resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                                            {"OTEL_EXPORTER_OTLP_METRICS_TIMEOUT", value}});
        EXPECT_EQ(bad.settings.metrics.timeout, milliseconds(10000)) << value;
        EXPECT_EQ(variables_of(bad),
                  (std::vector<std::string>{"OTEL_EXPORTER_OTLP_METRICS_TIMEOUT"}))
            << value;
    }
}

TEST(OtlpSettings, AnExporterOfNoneTurnsASignalOff) {
    const OtlpResolution metrics_off =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}, {"OTEL_METRICS_EXPORTER", "none"}});
    EXPECT_FALSE(metrics_off.settings.metrics.endpoint);
    EXPECT_TRUE(metrics_off.settings.logs.endpoint);
    const OtlpResolution logs_off =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}, {"OTEL_LOGS_EXPORTER", "None"}});
    EXPECT_TRUE(logs_off.settings.metrics.endpoint);
    EXPECT_FALSE(logs_off.settings.logs.endpoint);
    const OtlpResolution otlp =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}, {"OTEL_LOGS_EXPORTER", "otlp"}});
    EXPECT_TRUE(otlp.settings.logs.endpoint);
    EXPECT_TRUE(otlp.ignored.empty());
    const OtlpResolution console =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase}, {"OTEL_METRICS_EXPORTER", "console"}});
    EXPECT_TRUE(console.settings.metrics.endpoint);
    EXPECT_EQ(variables_of(console), (std::vector<std::string>{"OTEL_METRICS_EXPORTER"}));
}

TEST(OtlpSettings, TheMetricExportIntervalIsAtLeast100Milliseconds) {
    TelemetryOptions options;
    options.metric_export_interval_ms = 30000;
    EXPECT_EQ(
        resolve({{"OTEL_METRIC_EXPORT_INTERVAL", "5000"}}, options).settings.metric_export_interval,
        milliseconds(5000));
    EXPECT_EQ(resolve({{"OTEL_METRIC_EXPORT_INTERVAL", "100"}}).settings.metric_export_interval,
              milliseconds(100));
    const OtlpResolution low = resolve({{"OTEL_METRIC_EXPORT_INTERVAL", "99"}}, options);
    EXPECT_EQ(low.settings.metric_export_interval, milliseconds(30000));
    EXPECT_EQ(variables_of(low), (std::vector<std::string>{"OTEL_METRIC_EXPORT_INTERVAL"}));
}

TEST(OtlpSettings, ServiceNameAndResourceAttributes) {
    TelemetryOptions options;
    options.resource_attributes = {{"team", "a"}, {"deployment.environment", "options"}};
    const OtlpResolution attributes =
        resolve({{"OTEL_RESOURCE_ATTRIBUTES",
                  "deployment.environment=test,service.name=from%20attributes"}},
                options);
    EXPECT_EQ(attributes.settings.service_name, "from attributes");
    EXPECT_EQ(attributes.settings.resource_attributes,
              (HttpHeaders{{"team", "a"}, {"deployment.environment", "test"}}));
    EXPECT_TRUE(attributes.ignored.empty());

    const OtlpResolution named =
        resolve({{"OTEL_RESOURCE_ATTRIBUTES", "service.name=from-attributes"},
                 {"OTEL_SERVICE_NAME", "from-variable"}});
    EXPECT_EQ(named.settings.service_name, "from-variable");
    EXPECT_TRUE(named.settings.resource_attributes.empty());

    TelemetryOptions service;
    service.service_name = "from-options";
    EXPECT_EQ(resolve({}, service).settings.service_name, "from-options");

    const OtlpResolution bad = resolve({{"OTEL_RESOURCE_ATTRIBUTES", "a=1,b"}}, options);
    EXPECT_EQ(bad.settings.resource_attributes, options.resource_attributes);
    EXPECT_EQ(variables_of(bad), (std::vector<std::string>{"OTEL_RESOURCE_ATTRIBUTES"}));
}

TEST(OtlpSettings, AnInvalidEndpointExportsNothing) {
    for (const char* value :
         {"https://127.0.0.1:4318", "ftp://127.0.0.1", "127.0.0.1:4318", "http://", "http://:80",
          "http://h:0", "http://h:65536", "http://h:x", "http://user:secret@h:4318",
          "http://h/v1?secret=1", "http://h/v1#secret", "http://h/a secret", "http://[::1",
          "http://[zz]:1", "http://h_x:1", "http://[::1]x"}) {
        const OtlpResolution base = resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", value}});
        EXPECT_FALSE(base.settings.enabled()) << value;
        const IgnoredSetting* ignored = ignored_variable(base, "OTEL_EXPORTER_OTLP_ENDPOINT");
        ASSERT_NE(ignored, nullptr) << value;
        EXPECT_EQ(ignored->reason.find("secret"), std::string::npos) << ignored->reason;
    }
    // A refused signal endpoint does not fall back to the base endpoint.
    const OtlpResolution signal =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                 {"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "https://127.0.0.1:4318/v1/metrics"}});
    EXPECT_FALSE(signal.settings.metrics.endpoint);
    EXPECT_EQ(url_of(signal.settings.logs.endpoint), "http://127.0.0.1:4318/v1/logs");
    EXPECT_EQ(variables_of(signal),
              (std::vector<std::string>{"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT"}));
    // An invalid value is reported also where a higher source shadows it.
    const OtlpResolution shadowed =
        resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", "https://shadowed"},
                 {"OTEL_EXPORTER_OTLP_METRICS_ENDPOINT", "http://127.0.0.1:1/m"},
                 {"OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "http://127.0.0.1:1/l"}});
    EXPECT_TRUE(shadowed.settings.metrics.endpoint);
    EXPECT_TRUE(shadowed.settings.logs.endpoint);
    EXPECT_EQ(variables_of(shadowed), (std::vector<std::string>{"OTEL_EXPORTER_OTLP_ENDPOINT"}));
}

TEST(OtlpSettings, AnEmptyVariableCountsAsUnset) {
    TelemetryOptions options;
    options.logs_endpoint = "http://127.0.0.1:1/v1/logs";
    const OtlpResolution resolution = resolve({{"OTEL_EXPORTER_OTLP_ENDPOINT", ""},
                                               {"OTEL_SDK_DISABLED", ""},
                                               {"OTEL_EXPORTER_OTLP_TIMEOUT", ""}},
                                              options);
    EXPECT_EQ(url_of(resolution.settings.logs.endpoint), "http://127.0.0.1:1/v1/logs");
    EXPECT_FALSE(resolution.settings.metrics.endpoint);
    EXPECT_TRUE(resolution.ignored.empty());
}

TEST(OtlpSettings, WithoutTheEnvironmentOnlyTheOptionsCount) {
    std::vector<std::unique_ptr<ScopedEnvironment>> scoped;
    scoped.push_back(std::make_unique<ScopedEnvironment>("OTEL_SDK_DISABLED", std::nullopt));
    scoped.push_back(std::make_unique<ScopedEnvironment>("OTEL_EXPORTER_OTLP_ENDPOINT", kBase));
    scoped.push_back(std::make_unique<ScopedEnvironment>("OTEL_EXPORTER_OTLP_TIMEOUT", "x"));
    TelemetryOptions options;
    options.metrics_endpoint = "http://127.0.0.1:1/v1/metrics";
    const OtlpResolution resolution = resolve_otlp_settings(options, false);
    EXPECT_EQ(url_of(resolution.settings.metrics.endpoint), "http://127.0.0.1:1/v1/metrics");
    EXPECT_FALSE(resolution.settings.logs.endpoint);
    EXPECT_TRUE(resolution.ignored.empty());
    // The lookup overload without a lookup reads no environment either.
    EXPECT_FALSE(resolve_otlp_settings(TelemetryOptions{}, praenora::detail::EnvironmentLookup())
                     .settings.enabled());
}

TEST(OtlpSettings, InvalidOptionsAreIgnored) {
    TelemetryOptions options;
    options.metrics_endpoint = "https://127.0.0.1:4318/v1/metrics";
    options.logs_endpoint = "http://127.0.0.1:4318/v1/logs";
    options.export_timeout_ms = 0;
    options.metric_export_interval_ms = 50;
    options.service_name = "";
    options.headers = {{"bad name", "x"}};
    options.resource_attributes = {{"", "x"}};
    const OtlpResolution resolution = resolve({}, options);
    EXPECT_FALSE(resolution.settings.metrics.endpoint);
    EXPECT_EQ(url_of(resolution.settings.logs.endpoint), "http://127.0.0.1:4318/v1/logs");
    EXPECT_EQ(resolution.settings.logs.timeout, milliseconds(10000));
    EXPECT_EQ(resolution.settings.metric_export_interval, milliseconds(60000));
    EXPECT_EQ(resolution.settings.service_name, "praenora-runtime");
    EXPECT_TRUE(resolution.settings.logs.headers.empty());
    EXPECT_TRUE(resolution.settings.resource_attributes.empty());
    EXPECT_EQ(
        variables_of(resolution),
        (std::vector<std::string>{
            "TelemetryOptions::metrics_endpoint", "TelemetryOptions::headers",
            "TelemetryOptions::service_name", "TelemetryOptions::resource_attributes",
            "TelemetryOptions::metric_export_interval_ms", "TelemetryOptions::export_timeout_ms"}));
}

TEST(OtlpEndpoint, ParsesHostsPortsAndPaths) {
    std::string reason;
    const std::optional<HttpEndpoint> plain =
        parse_http_endpoint("http://127.0.0.1:4318/v1/metrics", reason);
    ASSERT_TRUE(plain) << reason;
    EXPECT_EQ(plain->host, "127.0.0.1");
    EXPECT_EQ(plain->port, 4318);
    EXPECT_EQ(plain->path, "/v1/metrics");
    EXPECT_EQ(plain->url, "http://127.0.0.1:4318/v1/metrics");

    const std::optional<HttpEndpoint> named = parse_http_endpoint("HTTP://Collector", reason);
    ASSERT_TRUE(named) << reason;
    EXPECT_EQ(named->host, "Collector");
    EXPECT_EQ(named->port, 80);
    EXPECT_EQ(named->path, "/");
    EXPECT_EQ(named->url, "http://Collector:80/");

    const std::optional<HttpEndpoint> ipv6 = parse_http_endpoint("http://[::1]:65535/x/", reason);
    ASSERT_TRUE(ipv6) << reason;
    EXPECT_EQ(ipv6->host, "::1");
    EXPECT_EQ(ipv6->port, 65535);
    EXPECT_EQ(ipv6->url, "http://[::1]:65535/x/");

    EXPECT_EQ(append_signal_path(*plain, "v1/logs").path, "/v1/metrics/v1/logs");
    EXPECT_EQ(append_signal_path(*named, "v1/logs").url, "http://Collector:80/v1/logs");
    EXPECT_EQ(append_signal_path(*ipv6, "v1/logs").url, "http://[::1]:65535/x/v1/logs");

    EXPECT_FALSE(parse_http_endpoint("https://127.0.0.1", reason));
    EXPECT_NE(reason.find("http"), std::string::npos) << reason;
}

TEST(OtlpEndpoint, KeyValueLists) {
    std::string reason;
    const std::optional<HttpHeaders> list =
        parse_key_value_list("a=1, b = x%3Dy ,c=%E2%9C%93", false, reason);
    ASSERT_TRUE(list) << reason;
    EXPECT_EQ(*list, (HttpHeaders{{"a", "1"}, {"b", "x=y"}, {"c", "\xE2\x9C\x93"}}));
    EXPECT_TRUE(parse_key_value_list("", true, reason));
    EXPECT_TRUE(parse_key_value_list(" , ", true, reason));
    EXPECT_FALSE(parse_key_value_list("a=%4", false, reason));
    EXPECT_FALSE(parse_key_value_list("a", false, reason));
    EXPECT_FALSE(parse_key_value_list("Host=x", true, reason));
    EXPECT_TRUE(parse_key_value_list("Host=x", false, reason));
    EXPECT_FALSE(parse_key_value_list("x y=1", true, reason));
}

TEST(OtlpResource, HasTheSpecifiedOrder) {
    OtlpSettings settings;
    settings.service_name = "svc";
    settings.resource_attributes = {{"deployment.environment", "test"}};
    int draws = 0;
    const auto resource = otlp_resource(settings, "0.2.0", 42, [&] {
        ++draws;
        return std::string("id-1");
    });
    ASSERT_EQ(resource.size(), 5u);
    EXPECT_EQ(resource[0].key, "service.name");
    EXPECT_EQ(resource[0].value, "svc");
    EXPECT_EQ(resource[1].key, "service.version");
    EXPECT_EQ(resource[1].value, "0.2.0");
    EXPECT_EQ(resource[2].key, "service.instance.id");
    EXPECT_EQ(resource[2].value, "id-1");
    EXPECT_EQ(resource[3].key, "process.pid");
    EXPECT_EQ(resource[3].value, 42);
    EXPECT_TRUE(resource[3].value.is_number_integer());
    EXPECT_EQ(resource[4].key, "deployment.environment");
    EXPECT_EQ(resource[4].value, "test");
    EXPECT_EQ(draws, 1);
}

TEST(OtlpResource, AConfiguredInstanceIdIsNotDrawn) {
    OtlpSettings settings;
    settings.resource_attributes = {{"a", "1"}, {"service.instance.id", "fixed"}};
    int draws = 0;
    const auto resource = otlp_resource(settings, "0.2.0", 7, [&] {
        ++draws;
        return std::string("drawn");
    });
    ASSERT_EQ(resource.size(), 5u);
    EXPECT_EQ(resource[0].value, "praenora-runtime");
    EXPECT_EQ(resource[2].value, "fixed");
    EXPECT_EQ(resource[4].key, "a");
    EXPECT_EQ(draws, 0);
}

TEST(OtlpResource, InstanceIdsAreRandomUuidsOfVersion4) {
    const std::regex uuid("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    const std::string first = random_uuid_v4();
    const std::string second = random_uuid_v4();
    EXPECT_TRUE(std::regex_match(first, uuid)) << first;
    EXPECT_TRUE(std::regex_match(second, uuid)) << second;
    EXPECT_NE(first, second);
}

TEST(OtlpSettings, AReasonNeverRepeatsTheValue) {
    TelemetryOptions options;
    options.export_timeout_ms = 987654321987;
    options.metric_export_interval_ms = 12;
    const OtlpResolution resolution = resolve({{"OTEL_SDK_DISABLED", "secret-1"},
                                               {"OTEL_EXPORTER_OTLP_ENDPOINT", kBase},
                                               {"OTEL_EXPORTER_OTLP_PROTOCOL", "secret-2"},
                                               {"OTEL_METRICS_EXPORTER", "secret-3"},
                                               {"OTEL_EXPORTER_OTLP_TIMEOUT", "secret-4"},
                                               {"OTEL_METRIC_EXPORT_INTERVAL", "secret-5"},
                                               {"OTEL_EXPORTER_OTLP_HEADERS", "a=secret-6,b"},
                                               {"OTEL_RESOURCE_ATTRIBUTES", "k=secret-7,x"}},
                                              options);
    EXPECT_EQ(resolution.ignored.size(), 9u);
    for (const IgnoredSetting& ignored : resolution.ignored) {
        EXPECT_EQ(ignored.reason.find("secret"), std::string::npos)
            << ignored.variable << ": " << ignored.reason;
        EXPECT_EQ(ignored.reason.find("987654321987"), std::string::npos) << ignored.variable;
        EXPECT_EQ(ignored.reason.find(" 12 "), std::string::npos) << ignored.variable;
    }
}
