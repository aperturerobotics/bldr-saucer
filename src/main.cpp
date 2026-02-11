#include <saucer/smartview.hpp>
#include "fetch_proto.h"
#include "pipe_client.h"
#include "pipe_connection.h"
#include "scheme_forwarder.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    auto webview = saucer::smartview::create({.window = window});

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

    // Start accept loop for Go-initiated streams (debug eval).
    // webview is a std::expected; use &(*webview) to get a pointer to the contained value.
    auto* webview_ptr = &(*webview);
    std::thread accept_thread([session, webview_ptr, webview_mtx, webview_alive]() {
        while (true) {
            auto [stream, err] = session->Accept();
            if (err != yamux::Error::OK || !stream) {
                break;
            }

            // Handle each stream in a detached thread so accept loop continues.
            std::thread([stream, webview_ptr, webview_mtx, webview_alive]() {
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

                std::string code(data.begin(), data.end());

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

                // Write a simple "ok" response.
                std::string resp = "ok";
                uint8_t resp_len_buf[4];
                uint32_t resp_len = static_cast<uint32_t>(resp.size());
                std::memcpy(resp_len_buf, &resp_len, 4);
                stream->Write(resp_len_buf, 4);
                stream->Write(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
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
