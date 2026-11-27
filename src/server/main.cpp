#include "inkhibit/server/server.hpp"

#include "inkhibit/album/store.hpp"
#include "inkhibit/album/image.hpp"

#include <httplib.h>

#include <atomic>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <optional>
#include <filesystem>
#include <stdexcept>
#include <charconv>
#include <system_error>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

namespace{

    struct Options {
        std::string bind = "0.0.0.0";
        int port = 8080;
        std::string web_root = "web";
        std::optional<std::string> album_root;
    };

    void print_usage() {
        std::cout
            << "Usage: inkhibit-server "
            << "[--bind ADDRESS] [--port PORT] [--web-root DIR] [--album-root DIR]\n\n"
            << "Serves the Inkhibit gallery and API over HTTP.\n"
            << "Defaults: --bind 0.0.0.0 --port 8080 --web-root web\n"
            << "Use --bind 127.0.0.1 to accept connections from this machine only.\n"
            << "--album-root DIR enables the unauthenticated album API.\n"
            << "Press Ctrl+C to stop the server.\n";
    }


    // Accepts 1-65535 written as plain decimal digits.
    bool parse_port(const std::string& text, int& port) {
        if (text.empty() || text.size() > 5) {
            return false;
        }

        int value = 0;
        for (const char digit : text) {
            if (digit < '0' || digit > '9') {
                return false;
            }
            value = value * 10 + (digit - '0');
        }

        if (value < 1 || value > 65535) {
            return false;
        }

        port = value;
        return true;
    }

    //every option takes a value. Returns false for anything unrecognized.
    bool parse_options(int argc, char** argv, Options& options){
        //skipping arg[0]
        for (int i = 1; i < argc; i += 2){
            if(i + 1 >= argc){
                return false;
            }

            const std::string name = argv[i];
            const std::string value = argv[i + 1];


            if (name == "--bind"){
                options.bind = value;
            }
            else if (name == "--port"){
                if (!parse_port(value, options.port)){
                    return false;
                }
            }
            else if (name == "--web-root"){
                options.web_root = value;
            }
            else if (name == "--album-root") {
                if (value.empty()) {
                    return false;
                }
                options.album_root = value;
            }
            else {
                return false;
            }
        }   //end of for loop

        return true;

    }   //end of parse_options

    // Addresses a browser can use to reach a server bound to all interfaces.
    std::vector<std::string> reachable_urls(int port) {
        std::vector<std::string> urls;
        const std::string suffix = ":" + std::to_string(port) + "/";

        char host[256] = {};
        if (gethostname(host, sizeof host - 1) == 0 && host[0] != '\0') {
            std::string name = host;
            // A bare name like "bippy" is announced by mDNS as "bippy.local".
            if (name.find('.') == std::string::npos) {
                name += ".local";
            }
            urls.push_back("http://" + name + suffix);
        }

        ifaddrs* interfaces = nullptr;
        if (getifaddrs(&interfaces) == 0) {
            for (const ifaddrs* entry = interfaces; entry; entry = entry->ifa_next) {
                if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET
                        || !(entry->ifa_flags & IFF_UP)
                        || (entry->ifa_flags & IFF_LOOPBACK)) {
                    continue;
                }

                const auto* ipv4 =
                    reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
                char address[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof address)) {
                    urls.push_back("http://" + std::string(address) + suffix);
                }
            }
            freeifaddrs(interfaces);
        }

        urls.push_back("http://127.0.0.1" + suffix);
        return urls;

    }   //end of reachable_urls

}   //end of namespace inkhibit
    //


int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage();
        return 0;
    }

    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    // Block termination signals before any thread starts, so every thread
    // inherits the mask. Only the waiter thread below receives them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "inkhibit-server: Could not block termination signals.\n";
        return 1;
    }

    try {
        std::optional<inkhibit::album::AlbumStore> album;
        httplib::Server server;
        inkhibit::configure_server(server, options.web_root);

        if (options.album_root) {
            namespace fs = std::filesystem;

            const auto web_root = fs::canonical(options.web_root);
            const auto album_root = 
                fs::weakly_canonical(fs::absolute(*options.album_root));

            // Reject the web root itself and any album directory beneath it.
            for (auto path = album_root;; path = path.parent_path()) {
                if (path == web_root) {
                    throw std::runtime_error("Album root must be outside the web root");
                }
                if (path == path.root_path()) {
                    break;
                }
            }

            album.emplace(album_root);
            const auto host = "127.0.0.1:" + std::to_string(options.port);

            server.Get("/api/album",
                    [&store = *album, host](const httplib::Request& request,
                        httplib::Response& response) {
                    // Loopback binding alone doesn't stop browser DNS rebinding.
                    if (request.get_header_value("Host") != host) {
                    response.status = 403;
                    return;
                    }

                    const auto ids = store.list();
                    std::string body = "{\"images\":[";
                    for (std::size_t i = 0; i < ids.size(); ++i) {
                        if (i != 0) {
                            body += ',';
                        }

                        body += '"' + std::to_string(ids[i]) + '"';

                    }
                    body += "]}\n";
                    response.set_content(body, "application/json");
                });


            server.set_payload_max_length(inkhibit::album::max_png_bytes);

            server.Post("/api/album",
                    [&store = *album, host](const httplib::Request& request,
                        httplib::Response& response) {
                    if (request.get_header_value("Host") != host) {
                    response.status = 403;
                    return;
                    }

                    if (request.get_header_value_count("Content-Type") != 1
                            || request.get_header_value("Content-Type") != "image/png") {
                    response.status = 415;
                    return;
                    }

                    const std::vector<std::uint8_t> png(
                            request.body.begin(), request.body.end());

                    inkhibit::album::ImageId id;
                    try {
                    id = store.add(png);
                    } catch (const std::invalid_argument& error) {
                        response.status = 400;
                        response.set_content(error.what(), "text/plain");
                        return;
                    } catch (const std::exception& error) {
                        std::cerr << "inkhibit-server: Album upload failed: "
                            << error.what() << '\n';
                        response.status = 500;
                        response.set_content("Could not save album image\n", "text/plain");
                        return;
                    }

                    response.status = 201;
                    response.set_content(
                            "{\"id\":\"" + std::to_string(id) + "\"}\n",
                            "application/json");
                    });

            server.Delete(R"(/api/album/([^/]+))",
                    [&store = *album, host](const httplib::Request& request,
                        httplib::Response& response) {
                    if (request.get_header_value("Host") != host) {
                    response.status = 403;
                    return;
                    }

                    const auto text = request.matches[1].str();
                    const char* end = text.data() + text.size();
                    inkhibit::album::ImageId id = 0;
                    const auto result = std::from_chars(text.data(), end, id);

                    if (result.ec != std::errc{} || result.ptr != end
                            || id == 0 || text.front() == '0') {
                    response.status = 400;
                    response.set_content("Invalid album image ID\n", "text/plain");
                    return;
                    }

                    try {
                        response.status = store.remove(id) ? 204 : 404;
                    } catch (const std::exception& error) {
                        std::cerr << "inkhibit-server: Album deletion failed: "
                            << error.what() << '\n';
                        response.status = 500;
                        response.set_content("Could not remove album image\n", "text/plain");
                    }
                    });
        }

        if (!server.bind_to_port(options.bind, options.port)) {
            std::cerr
                << "inkhibit-server: Could not listen on "
                << options.bind << ':' << options.port << '\n';
            return 1;
        }

        std::atomic<bool> finished{false};
        std::atomic<int> received{0};

        std::thread waiter([&] {
                const timespec interval{0, 100'000'000};

                while (!finished) {
                const int number = sigtimedwait(&signals, nullptr, &interval);
                if (number > 0) {
                received = number;
                server.stop();

                return;
                }
                }
                });

        std::cout << "Serving " << options.web_root << " on port " << options.port;
        if (options.bind == "0.0.0.0") {
            std::cout << " (all network interfaces):\n";
            for (const auto& url : reachable_urls(options.port)) {
                std::cout << "  " << url << '\n';
            }
        } else {
            std::cout << " at http://" << options.bind << ':' << options.port << "/\n";
        }
        std::cout << "Press Ctrl+C to stop.\n" << std::flush;

        bool listened = false;
        std::exception_ptr failure;

        try {
            listened = server.listen_after_bind();
        } catch (...) {
            failure = std::current_exception();
        }

        finished = true;
        waiter.join();

        if (failure) {
            std::rethrow_exception(failure);
        }

        if (received != 0) {
            std::cout << "Server stopped.\n";
            return 128 + received;
        }

        if (!listened) {
            std::cerr << "inkhibit-server: Server stopped unexpectedly.\n";
            return 1;
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "inkhibit-server: " << error.what() << '\n';
        return 1;
    }
}
