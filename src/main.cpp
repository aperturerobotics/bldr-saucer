#include <saucer/smartview.hpp>
#include "fetch_proto.h"
#include "pipe_client.h"
#include "pipe_connection.h"
#include "scheme_forwarder.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// EvalRegistry tracks pending eval requests and their results.
// Worker threads register a request ID, execute JS that posts results via
// the saucer message channel, then wait on a condition variable for the
// message handler to deliver the result.
struct EvalRegistry {
    struct Pending {
        bool ready = false;
        std::string result;
        std::string error;
    };

    std::mutex mtx;
    std::condition_variable cv;
    std::unordered_map<std::string, Pending> pending;

    // Register registers a new eval request and returns the ID.
    void Register(const std::string& id) {
        std::lock_guard<std::mutex> lock(mtx);
        pending[id] = Pending{};
    }

    // Deliver delivers a result for a pending eval request.
    // Returns true if the ID was found.
    bool Deliver(const std::string& id, const std::string& result, const std::string& error) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = pending.find(id);
        if (it == pending.end()) {
            return false;
        }
        it->second.ready = true;
        it->second.result = result;
        it->second.error = error;
        cv.notify_all();
        return true;
    }

    // Wait waits for a result for the given eval ID (up to timeout_ms).
    // Returns the response, with empty fields on timeout.
    bldr::proto::EvalJSResponse Wait(const std::string& id, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, &id] {
            auto it = pending.find(id);
            return it != pending.end() && it->second.ready;
        });
        bldr::proto::EvalJSResponse resp;
        auto it = pending.find(id);
        if (it != pending.end()) {
            resp.result = std::move(it->second.result);
            resp.error = std::move(it->second.error);
            pending.erase(it);
        }
        return resp;
    }
};

coco::stray start(saucer::application* app) {
    const char* runtime_id_env = std::getenv("BLDR_RUNTIME_ID");
    if (!runtime_id_env) {
        std::cerr << "[bldr-saucer] BLDR_RUNTIME_ID not set" << std::endl;
        co_return;
    }
    std::string runtime_id = runtime_id_env;

    bldr::proto::SaucerInit saucer_init;
    const char* init_b64 = std::getenv("BLDR_SAUCER_INIT");
    if (init_b64) {
        auto data = bldr::proto::Base64Decode(init_b64);
        if (!data.empty() && !bldr::proto::DecodeSaucerInit(data.data(), data.size(), saucer_init)) {
            std::cerr << "[bldr-saucer] failed to decode BLDR_SAUCER_INIT" << std::endl;
        }
    }

    // Connect to Go via pipesock.
    bldr::PipeClient pipe;
    std::string pipe_path = ".pipe-" + runtime_id;
    if (!pipe.connect(pipe_path)) {
        std::cerr << "[bldr-saucer] failed to connect to pipe: " << pipe_path << std::endl;
        co_return;
    }

    // Create yamux client session over the pipe.
    // C++ is the client (outbound=true), Go is the server (outbound=false).
    auto conn = std::make_unique<bldr::PipeConnection>(pipe);
    yamux::SessionConfig config;
    config.enable_keepalive = false;
    auto session = yamux::Session::Client(std::move(conn), config);
    if (!session) {
        std::cerr << "[bldr-saucer] failed to create yamux session" << std::endl;
        co_return;
    }

    // Create the scheme forwarder (shared_ptr to avoid use-after-free in detached threads).
    auto forwarder = std::make_shared<bldr::SchemeForwarder>(session.get());

    // Register bldr:// scheme BEFORE creating the webview.
    saucer::webview::register_scheme("bldr");

    auto window = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window, .non_persistent_data_store = true});

    window->set_title("Bldr");
    window->set_size({1024, 768});

    // Handle bldr:// scheme: forward all requests to Go over yamux.
    webview->handle_stream_scheme("bldr", [forwarder](saucer::scheme::request req, saucer::scheme::stream_writer writer) {
        std::thread([forwarder, req = std::move(req), writer = std::move(writer)]() mutable {
            forwarder->forward(req, writer);
        }).detach();
    });

    // Navigate via HTML redirect (works around WebKit's loadFileURL issue with custom schemes).
    std::string nav_url = "bldr:///index.html";
    const char* doc_id_env = std::getenv("BLDR_WEB_DOCUMENT_ID");
    if (doc_id_env && doc_id_env[0] != '\0') {
        nav_url += "?webDocumentId=" + std::string(doc_id_env);
    }
    std::string redirect_html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<script>window.location.replace('" + nav_url + "');</script>"
        "</head><body></body></html>";
    webview->set_html(redirect_html);

    if (saucer_init.dev_tools) {
        webview->set_dev_tools(true);
    }

    // Shutdown guard: prevents webview->execute() calls after webview destruction.
    // The mutex ensures no thread is inside execute() when we set the flag.
    auto webview_mtx = std::make_shared<std::mutex>();
    auto webview_alive = std::make_shared<std::atomic<bool>>(true);

    // Eval result registry: worker threads register pending evals, the message
    // handler delivers results from JavaScript back to the waiting thread.
    auto eval_registry = std::make_shared<EvalRegistry>();

    // Register a message handler to intercept eval results from JavaScript.
    // The Go side wraps JS code so it posts the result via postMessage with a
    // prefix format: __bldr_eval:<eval_id>:r:<result> or __bldr_eval:<eval_id>:e:<error>.
    // The smartview's own handler returns unhandled for unrecognized messages,
    // so this handler sees them next.
    constexpr std::string_view eval_prefix = "__bldr_eval:";
    webview->on<saucer::webview::event::message>({{.func = [eval_registry, eval_prefix](std::string_view message) -> saucer::status {
        if (!message.starts_with(eval_prefix)) {
            return saucer::status::unhandled;
        }

        // Parse prefix format: __bldr_eval:<eval_id>:<type>:<data>
        auto rest = message.substr(eval_prefix.size());
        auto sep1 = rest.find(':');
        if (sep1 == std::string_view::npos || sep1 + 2 >= rest.size()) {
            return saucer::status::unhandled;
        }
        auto sep2 = rest.find(':', sep1 + 1);
        if (sep2 == std::string_view::npos) {
            return saucer::status::unhandled;
        }

        std::string eval_id(rest.substr(0, sep1));
        char type = rest[sep1 + 1];
        std::string data(rest.substr(sep2 + 1));

        if (type == 'r') {
            eval_registry->Deliver(eval_id, data, "");
        } else {
            eval_registry->Deliver(eval_id, "", data);
        }
        return saucer::status::handled;
    }}});

    // Start accept loop for Go-initiated streams (debug eval).
    // webview is a std::expected; use &(*webview) to get a pointer to the contained value.
    auto* webview_ptr = &(*webview);
    auto eval_counter = std::make_shared<std::atomic<uint64_t>>(0);
    std::thread accept_thread([session, webview_ptr, webview_mtx, webview_alive, eval_registry, eval_counter]() {
        while (true) {
            auto [stream, err] = session->Accept();
            if (err != yamux::Error::OK || !stream) {
                break;
            }

            // Handle each stream in a detached thread so accept loop continues.
            std::thread([stream, webview_ptr, webview_mtx, webview_alive, eval_registry, eval_counter]() {
                // Read length-prefixed command frame.
                uint8_t len_buf[4];
                size_t total = 0;
                while (total < 4) {
                    auto [n, rerr] = stream->Read(len_buf + total, 4 - total);
                    if (rerr != yamux::Error::OK || n == 0) {
                        stream->Close();
                        return;
                    }
                    total += n;
                }
                uint32_t msg_len;
                std::memcpy(&msg_len, len_buf, 4);
                if (msg_len > 10 * 1024 * 1024) {
                    stream->Close();
                    return;
                }

                std::vector<uint8_t> data(msg_len);
                total = 0;
                while (total < msg_len) {
                    auto [n, rerr] = stream->Read(data.data() + total, msg_len - total);
                    if (rerr != yamux::Error::OK || n == 0) break;
                    total += n;
                }
                if (total < msg_len) {
                    stream->Close();
                    return;
                }

                // Decode the EvalJSRequest protobuf from Go.
                bldr::proto::EvalJSRequest req;
                if (!bldr::proto::DecodeEvalJSRequest(data.data(), data.size(), req)) {
                    stream->Close();
                    return;
                }

                // The code from Go is already wrapped in an async IIFE that posts
                // the result via postMessage. It contains a placeholder __EVAL_ID__
                // that we replace with a unique ID for result correlation.
                std::string code = std::move(req.code);
                std::string eval_id = "e" + std::to_string(eval_counter->fetch_add(1));

                // Replace the __EVAL_ID__ placeholder with the actual eval ID.
                const std::string placeholder = "__EVAL_ID__";
                auto pos = code.find(placeholder);
                if (pos != std::string::npos) {
                    code.replace(pos, placeholder.size(), eval_id);
                }

                // Register the eval request before executing the code.
                eval_registry->Register(eval_id);

                // Execute the JavaScript code in the webview (guarded against shutdown).
                // Cast to webview* to call webview::execute(cstring_view) instead of
                // smartview::execute(format_string) which has a consteval constructor
                // that breaks std::thread lambdas in C++23.
                {
                    std::lock_guard<std::mutex> lock(*webview_mtx);
                    if (webview_alive->load()) {
                        static_cast<saucer::webview*>(webview_ptr)->execute(code);
                    }
                }

                // Wait for the JavaScript result (30 second timeout).
                auto resp = eval_registry->Wait(eval_id, 30000);
                if (resp.result.empty() && resp.error.empty()) {
                    resp.error = "eval timeout";
                }

                // Encode the EvalJSResponse protobuf and send it back.
                auto resp_buf = bldr::proto::EncodeEvalJSResponse(resp);
                uint8_t resp_len_buf[4];
                uint32_t resp_len = static_cast<uint32_t>(resp_buf.size());
                std::memcpy(resp_len_buf, &resp_len, 4);
                stream->Write(resp_len_buf, 4);
                stream->Write(resp_buf.data(), resp_buf.size());
                stream->Close();
            }).detach();
        }
    });
    accept_thread.detach();

    window->show();
    co_await app->finish();

    // Shutdown: close session first (causes Accept/Read/Write to return errors,
    // winding down detached threads), then mark webview as dead.
    session->Close();
    {
        std::lock_guard<std::mutex> lock(*webview_mtx);
        webview_alive->store(false);
    }
    pipe.close();
}

int main() {
    auto app_result = saucer::application::create({.id = "bldr"});
    if (!app_result) {
        std::cerr << "[bldr-saucer] failed to create application" << std::endl;
        return 1;
    }
    return app_result->run(start);
}
