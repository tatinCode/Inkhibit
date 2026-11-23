#include "inkhibit/server/server.hpp"

#include <httplib.h>

#include <atomic>
#include <exception>
#include <iostream>
#include <string>
#include <thread>


#include <pthread.h>
#include <signal.h>
#include <time.h>

namespace{

    struct Options {
        std::string bind = "127.0.0.1";
        int port = 8080;
        std::string web_root = "web";
    };

    void print_usage() {
        std::cout
            << "Usage: inkhibit-server "
            << "[--bind ADDRESS] [--port PORT] [--web-root DIR]\n\n"
            << "Serves the Inkhibit gallery and API over HTTP.\n"
            << "Defaults: --bind 127.0.0.1 --port 8080 --web-root web\n"
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
            else {
                return false;
            }
        }   //end of for loop

        return true;

    }   //end of parse_options

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
        httplib::Server server;
        inkhibit::configure_server(server, options.web_root);

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

        std::cout
            << "Serving " << options.web_root << " at http://"
            << options.bind << ':' << options.port << "/\n"
            << "Press Ctrl+C to stop.\n"
            << std::flush;

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
