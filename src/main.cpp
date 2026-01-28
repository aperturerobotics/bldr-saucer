#include <saucer/smartview.hpp>
#include "pipe_client.h"
#include "pipe_connection.h"
#include "scheme_forwarder.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

// Simple JSON parser for SaucerInit.
struct SaucerInit {
    bool dev_tools = false;
    bool external_links = false;
};

SaucerInit parseSaucerInit(const std::string& json) {
    SaucerInit init;

    auto pos = json.find("\"devTools\"");
    if (pos == std::string::npos) pos = json.find("\"dev_tools\"");
    if (pos != std::string::npos) {
        auto colon = json.find(':', pos);
        if (colon != std::string::npos) {
            auto start = json.find_first_not_of(" \t\n\r", colon + 1);
            if (start != std::string::npos && json.substr(start, 4) == "true") {
                init.dev_tools = true;
            }
        }
    }

    pos = json.find("\"externalLinks\"");
    if (pos == std::string::npos) pos = json.find("\"external_links\"");
    if (pos != std::string::npos) {
        auto colon = json.find(':', pos);
        if (colon != std::string::npos) {
            auto start = json.find_first_not_of(" \t\n\r", colon + 1);
            if (start != std::string::npos && json.substr(start, 4) == "true") {
                init.external_links = true;
            }
        }
    }

    return init;
}

coco::stray start(saucer::application* app) {
    const char* runtime_id_env = std::getenv("BLDR_RUNTIME_ID");
    if (!runtime_id_env) {
        std::cerr << "BLDR_RUNTIME_ID not set" << std::endl;
        co_return;
    }
    std::string runtime_id = runtime_id_env;

    SaucerInit saucer_init;
    const char* init_json = std::getenv("BLDR_SAUCER_INIT");
    if (init_json) {
        saucer_init = parseSaucerInit(init_json);
    }

    // Connect to Go via pipesock.
    bldr::PipeClient pipe;
    std::string pipe_path = ".pipe-" + runtime_id;
    if (!pipe.connect(pipe_path)) {
        std::cerr << "Failed to connect to pipe: " << pipe_path << std::endl;
        co_return;
    }

    // Create yamux client session over the pipe.
    // C++ is the client (outbound=true), Go is the server (outbound=false).
    auto conn = std::make_unique<bldr::PipeConnection>(pipe);
    yamux::SessionConfig config;
    config.enable_keepalive = false;
    auto session = yamux::Session::Client(std::move(conn), config);
    if (!session) {
        std::cerr << "Failed to create yamux session" << std::endl;
        co_return;
    }

    // Create the scheme forwarder.
    bldr::SchemeForwarder forwarder(session.get());

    // Register bldr:// scheme BEFORE creating the webview.
    saucer::webview::register_scheme("bldr");

    auto window = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    window->set_title("Bldr");
    window->set_size({1024, 768});

    // Handle bldr:// scheme: forward all requests to Go over yamux.
    webview->handle_stream_scheme("bldr", [&forwarder](saucer::scheme::request req, saucer::scheme::stream_writer writer) {
        // Forward in background thread to not block the scheme handler.
        std::thread([&forwarder, req = std::move(req), writer = std::move(writer)]() mutable {
            forwarder.forward(req, writer);
        }).detach();
    });

    // Navigate to the bootstrap HTML served by Go.
    webview->set_url(saucer::url::make({.scheme = "bldr", .host = "localhost", .path = "/"}));

    if (saucer_init.dev_tools) {
        webview->set_dev_tools(true);
    }

    window->show();
    co_await app->finish();

    // Cleanup.
    session->Close();
    pipe.close();
}

int main() {
    auto app_result = saucer::application::create({.id = "bldr"});
    if (!app_result) {
        std::cerr << "Failed to create application" << std::endl;
        return 1;
    }
    return app_result->run(start);
}
