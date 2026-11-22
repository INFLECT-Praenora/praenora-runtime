// A program that the isolation tests put where the runtime looks for its module host, to see how
// the runtime refuses one that does not answer as a module host of its own build does. It reads
// the runtime's hello on descriptor 3 and then does what the environment variable
// PRAENORA_TEST_FAKE_MODULE_HOST says: "build" answers as a module host of another runtime
// version, "protocol" as one of another channel protocol, "garbage" with bytes that are no frame,
// "hello" with a frame of the wrong kind, "exit" ends at once with code 7, and "silent" never
// answers. Two modes answer the handshake as a module host of this build does and then answer the
// load as none does: "provider-log" with a provider's log record of provider 5, and
// "provider-error" with a load error located in the manifest of provider 5 (the load names fewer).
// Five more break the rules of the shared compiled models instead: "store-unshared" asks the store
// (as a module host of a step that does not share would not), "store-provider" asks it for
// provider 5, "store-key" saves under a key of 300 bytes, "store-huge" announces a save of nearly
// 4 GiB and sends none of it, "store-save-unshared" announces a save of 128 MiB (within the
// frame's limit), as a module host of a step that does not share would not, sends its first 8
// bytes and then nothing more for 3 s before it ends, "store-entry-over" saves an entry one byte
// over 128 MiB under an empty key (within the frame's limit), and "store-refused-within" refuses an
// entry within the limits; and
// "store-deaf" asks the store for the entry "deaf" of provider 0 and then reads nothing more for
// 60 s, as a module host that stopped would. Three more answer the load as a module host does (they
// load the module themselves, with the loader) and then stop reading for 60 s: "deaf-after-load",
// "deaf-store-after-load", which first asks the store for "deaf" as "store-deaf" does, and
// "deaf-at-invoke", which first answers a create (an instance made) and an initialize as a module
// host whose module succeeds would, without calling the module, and stops reading once it has
// answered the initialize, before the first invoke. It ends when the runtime closes the channel.
#include "core/paths.hpp"
#include "discovery/manifest.hpp"
#include "isolation/channel.hpp"
#include "loader/loaded_module.hpp"

#include <praenora/runtime/version.hpp>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

int main() {
    using praenora::detail::Channel;
    using praenora::detail::Frame;
    using praenora::detail::FrameKind;
    using praenora::detail::PayloadWriter;
    const char* value = std::getenv("PRAENORA_TEST_FAKE_MODULE_HOST");
    const std::string mode = value != nullptr ? value : "";
    if (mode == "exit") {
        return 7;
    }
    Channel channel(3);
    const Channel::ReadResult hello = channel.read();
    if (hello.status != Channel::ReadStatus::frame) {
        return 1;
    }
    Frame answer;
    answer.kind = FrameKind::ready;
    if (mode == "build") {
        answer.payload = PayloadWriter()
                             .u32(praenora::detail::kChannelProtocol)
                             .bytes(praenora::version_info().runtime + "-other")
                             .take();
    } else if (mode == "protocol") {
        answer.payload = PayloadWriter().u32(999).bytes(praenora::version_info().runtime).take();
    } else if (mode == "hello") {
        answer.kind = FrameKind::hello;
        answer.payload = PayloadWriter()
                             .u32(praenora::detail::kChannelProtocol)
                             .bytes(praenora::version_info().runtime)
                             .take();
    } else if (mode == "garbage") {
        const std::string garbage(64, 'g');
        static_cast<void>(::send(3, garbage.data(), garbage.size(), 0));
    }
    const bool deaf_after_load =
        mode == "deaf-after-load" || mode == "deaf-store-after-load" || mode == "deaf-at-invoke";
    if (deaf_after_load) {
        answer.payload = PayloadWriter()
                             .u32(praenora::detail::kChannelProtocol)
                             .bytes(praenora::version_info().runtime)
                             .take();
        static_cast<void>(channel.write(answer));
        const Channel::ReadResult load = channel.read();
        if (load.status != Channel::ReadStatus::frame || load.frame.kind != FrameKind::load) {
            return 1;
        }
        // The answer of a module host of this build: the module loaded with the loader.
        const nlohmann::json request = nlohmann::json::parse(load.frame.payload);
        const auto module = praenora::detail::LoadedModule::load(praenora::detail::load_manifest(
            praenora::detail::path_from_utf8(request.at("manifest").get<std::string>())));
        Frame loaded;
        loaded.kind = FrameKind::loaded;
        loaded.id = load.frame.id;
        loaded.payload = nlohmann::json{{"describe", module->describe()},
                                        {"library_sha256", module->library_sha256()},
                                        {"diagnostics", module->api().diagnostics != nullptr},
                                        {"self_test", module->api().self_test != nullptr}}
                             .dump();
        static_cast<void>(channel.write(loaded));
        if (mode == "deaf-store-after-load") {
            Frame ask;
            ask.kind = FrameKind::store_load;
            ask.id = 1;
            ask.payload = PayloadWriter().u32(0).bytes("deaf").take();
            static_cast<void>(channel.write(ask));
        }
        if (mode == "deaf-at-invoke") {
            for (const FrameKind kind : {FrameKind::create, FrameKind::initialize}) {
                const Channel::ReadResult call = channel.read();
                if (call.status != Channel::ReadStatus::frame || call.frame.kind != kind) {
                    return 1;
                }
                Frame result;
                result.kind = FrameKind::result;
                result.id = call.frame.id;
                result.payload =
                    PayloadWriter().i32(0).u8(kind == FrameKind::create ? 1 : 0).take();
                static_cast<void>(channel.write(result));
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    const bool store = mode.rfind("store-", 0) == 0;
    if (mode == "provider-log" || mode == "provider-error" || store) {
        answer.payload = PayloadWriter()
                             .u32(praenora::detail::kChannelProtocol)
                             .bytes(praenora::version_info().runtime)
                             .take();
        static_cast<void>(channel.write(answer));
        const Channel::ReadResult load = channel.read();
        if (load.status != Channel::ReadStatus::frame || load.frame.kind != FrameKind::load) {
            return 1;
        }
        Frame reply;
        reply.id = load.frame.id;
        if (mode == "provider-log") {
            reply.kind = FrameKind::provider_log;
            reply.payload = PayloadWriter().u32(5).i32(2).u32(2).bytes("[]").bytes("hi").take();
        } else if (mode == "store-deaf") {
            reply.kind = FrameKind::store_load;
            reply.payload = PayloadWriter().u32(0).bytes("deaf").take();
        } else if (mode == "store-unshared") {
            reply.kind = FrameKind::store_load;
            reply.payload = PayloadWriter().u32(0).bytes("key").take();
        } else if (mode == "store-provider") {
            reply.kind = FrameKind::store_load;
            reply.payload = PayloadWriter().u32(5).bytes("key").take();
        } else if (mode == "store-key") {
            reply.kind = FrameKind::store_save;
            reply.payload =
                PayloadWriter().u32(0).u32(300).bytes(std::string(300, 'k')).bytes("data").take();
        } else if (mode == "store-entry-over") {
            // Within the frame's limit (its key is empty), and one byte over the entry's.
            reply.kind = FrameKind::store_save;
            reply.payload = PayloadWriter()
                                .u32(0)
                                .u32(0)
                                .bytes(std::string((std::size_t{128} << 20) + 1, 'e'))
                                .take();
        } else if (mode == "store-refused-within") {
            reply.kind = FrameKind::store_refused;
            reply.payload = PayloadWriter().u32(0).u64(3).u64(10).bytes("key").take();
        } else if (mode == "store-huge") {
            const auto header =
                praenora::detail::encode_header(FrameKind::store_save, 1, 0xFFFFFFF0u);
            static_cast<void>(::send(3, header.data(), header.size(), 0));
        } else if (mode == "store-save-unshared") {
            // The provider's index and an empty key; the entry never follows.
            const std::string start = PayloadWriter().u32(0).u32(0).take();
            const auto header = praenora::detail::encode_header(
                FrameKind::store_save, 1, start.size() + (std::size_t{128} << 20));
            static_cast<void>(::send(3, header.data(), header.size(), 0));
            static_cast<void>(::send(3, start.data(), start.size(), 0));
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return 0;
        } else {
            reply.kind = FrameKind::load_failed;
            reply.payload = R"({"code":303,"message":"m","pointer":"","line":0,"column":0,)"
                            R"("details":"{}","provider":5})";
        }
        if (mode != "store-huge") {
            static_cast<void>(channel.write(reply));
        }
        if (mode == "store-deaf") {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 0;
        }
    }
    if (mode == "build" || mode == "protocol" || mode == "hello") {
        static_cast<void>(channel.write(answer));
    }
    // Until the runtime lets go.
    while (channel.read().status == Channel::ReadStatus::frame) {
    }
    return 0;
}
