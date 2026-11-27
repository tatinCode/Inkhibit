#include "inkhibit/server/server.hpp"
#include "album_png_fixture.hpp"

#include <httplib.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

fs::path server_executable;
fs::path frontend_root;

// Checks remain enabled in Release builds.
#define REQUIRE(condition)                                                \
    do {                                                                  \
        if (!(condition)) {                                               \
            throw std::runtime_error(std::string(__FILE__) + ":"           \
                + std::to_string(__LINE__) + ": " + #condition);           \
        }                                                                 \
    } while (false)

template<class Exception, class Function>
void expect_throw(Function&& function, const std::string& message)
{
    try {
        function();
    } catch (const Exception& error) {
        if (std::string(error.what()).find(message) == std::string::npos) {
            throw std::runtime_error("Expected exception containing '" + message
                + "', got: " + error.what());
        }
        return;
    }
    throw std::runtime_error("Expected exception containing: " + message);
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        // Like the existing tests, use isolated directories in the build tree.
        std::string pattern = (fs::current_path() / "server-tests-XXXXXX").string();
        char* result = ::mkdtemp(pattern.data());
        if (!result) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        path = result;
    }

    ~TemporaryDirectory() noexcept
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    fs::path path;
};

std::string read_file(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::string contents((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    REQUIRE(!input.bad());
    return contents;
}

void write_file(const fs::path& path, const std::string& contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    REQUIRE(!output.fail());
}

const std::string index_html = "<!doctype html><title>Inkhibit test</title>\n";
const std::string app_js = "console.log(\"inkhibit test\");\n";
const std::string styles_css = "body { color: black; }\n";
const std::string note_txt = "nested asset\n";
// Stored beside the web root; it must never appear in a response.
const std::string secret_txt = "inkhibit-secret-7f3a91\n";

// A web root containing known files, with a private file outside it.
struct Site {
    TemporaryDirectory directory;
    fs::path web = directory.path / "web";
    fs::path secret = directory.path / "secret.txt";

    Site()
    {
        fs::create_directory(web);
        fs::create_directory(web / "assets");
        write_file(web / "index.html", index_html);
        write_file(web / "app.js", app_js);
        write_file(web / "styles.css", styles_css);
        write_file(web / "assets" / "note.txt", note_txt);
        write_file(secret, secret_txt);
    }
};

// The production configuration, listening on a free loopback port.
class RunningServer {
public:
    explicit RunningServer(const fs::path& web_root)
    {
        inkhibit::configure_server(server, web_root);
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);

        thread_ = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
        if (!server.is_running()) {
            server.stop();
            thread_.join();
            throw std::runtime_error("Server did not start listening");
        }
    }

    ~RunningServer()
    {
        server.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    httplib::Server server;
    int port = -1;

private:
    std::thread thread_;
};

httplib::Result get(int port, const std::string& path)
{
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(5);
    client.set_read_timeout(5);
    client.set_write_timeout(5);
    return client.Get(path);
}

void require_file(int port, const std::string& path,
    const std::string& body, const std::string& content_type)
{
    const auto result = get(port, path);
    REQUIRE(result);
    REQUIRE(result->status == 200);
    REQUIRE(result->get_header_value("Content-Type") == content_type);
    REQUIRE(result->body == body);
}

void require_status(int port, const std::string& path, int status)
{
    const auto result = get(port, path);
    REQUIRE(result);
    REQUIRE(result->status == status);
}

class Socket {
public:
    Socket() : fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
    {
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "socket");
        }
    }

    ~Socket() noexcept { ::close(fd); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    const int fd;
};

sockaddr_in loopback(int port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

// Sends bytes without client-side path/header normalization.
std::string raw_request(int port, const std::string& request)
{
    Socket socket;
    const timeval timeout{5, 0};
    REQUIRE(::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO,
        &timeout, sizeof timeout) == 0);
    REQUIRE(::setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO,
        &timeout, sizeof timeout) == 0);

    const auto address = loopback(port);
    REQUIRE(::connect(socket.fd, reinterpret_cast<const sockaddr*>(&address),
        sizeof address) == 0);

    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto count = ::send(socket.fd, request.data() + sent,
            request.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::system_error(errno, std::generic_category(), "send");
        }
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        const auto count = ::recv(socket.fd, buffer, sizeof buffer, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            throw std::system_error(errno, std::generic_category(), "recv");
        }
        if (count == 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    return response;
}

std::string raw_get(int port, const std::string& target)
{
    return raw_request(port, "GET " + target + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\nConnection: close\r\n\r\n");
}

int status_of(const std::string& response)
{
    REQUIRE(response.size() >= 12);
    REQUIRE(response.compare(0, 9, "HTTP/1.1 ") == 0);
    for (std::size_t i = 9; i < 12; ++i) {
        REQUIRE(response[i] >= '0' && response[i] <= '9');
    }
    return std::stoi(response.substr(9, 3));
}

// Asks the kernel for an unused loopback port, then releases it.
int free_port()
{
    Socket socket;
    auto address = loopback(0);
    REQUIRE(::bind(socket.fd, reinterpret_cast<const sockaddr*>(&address),
        sizeof address) == 0);
    socklen_t size = sizeof address;
    REQUIRE(::getsockname(socket.fd, reinterpret_cast<sockaddr*>(&address),
        &size) == 0);
    return ntohs(address.sin_port);
}

// Runs inkhibit-server with stdout and stderr captured in one log file.
// A child that is still running when the test ends is killed and reaped.
class Child {
public:
    Child(const std::vector<std::string>& arguments, const fs::path& log)
    {
        std::vector<std::string> command{server_executable.string()};
        command.insert(command.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& argument : command) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        const int log_fd = ::open(log.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (log_fd < 0) {
            throw std::system_error(errno, std::generic_category(), "open log");
        }

        std::cout.flush();
        std::cerr.flush();
        pid_ = ::fork();
        if (pid_ < 0) {
            const int saved = errno;
            ::close(log_fd);
            throw std::system_error(saved, std::generic_category(), "fork");
        }
        if (pid_ == 0) {
            if (::dup2(log_fd, STDOUT_FILENO) < 0
                    || ::dup2(log_fd, STDERR_FILENO) < 0) {
                ::_exit(126);
            }
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        ::close(log_fd);
    }

    ~Child() noexcept
    {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    pid_t pid() const { return pid_; }

    // Returns the exit code once the child has exited, without blocking.
    std::optional<int> poll()
    {
        if (pid_ <= 0) {
            return exit_code_;
        }
        int status = 0;
        pid_t waited;
        do {
            waited = ::waitpid(pid_, &status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            throw std::system_error(errno, std::generic_category(), "waitpid");
        }
        if (waited == 0) {
            return std::nullopt;
        }
        pid_ = -1;
        if (!WIFEXITED(status)) {
            throw std::runtime_error("inkhibit-server ended without exiting");
        }
        exit_code_ = WEXITSTATUS(status);
        return exit_code_;
    }

    int wait_for_exit()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            if (const auto code = poll()) {
                return *code;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("inkhibit-server did not exit");
            }
            std::this_thread::sleep_for(10ms);
        }
    }

private:
    pid_t pid_ = -1;
    std::optional<int> exit_code_;
};

struct CliResult {
    int exit_code;
    std::string output;
};

// For invocations that must exit without serving.
CliResult run_cli(const TemporaryDirectory& directory,
    const std::vector<std::string>& arguments)
{
    const auto log = directory.path / "server.log";
    Child child(arguments, log);
    const int code = child.wait_for_exit();
    return {code, read_file(log)};
}

void require_exit(const CliResult& result, int expected)
{
    if (result.exit_code != expected) {
        throw std::runtime_error("Expected exit " + std::to_string(expected)
            + ", got " + std::to_string(result.exit_code) + ": " + result.output);
    }
}

struct ServerProcess {
    std::unique_ptr<Child> child;
    int port;
};

// Starts the executable on a free port and waits until it answers on loopback.
ServerProcess start_server_process(const fs::path& web_root, const fs::path& log,
    const std::vector<std::string>& extra_arguments)
{
    for (int attempt = 0; attempt < 5; ++attempt) {
        const int port = free_port();
        std::vector<std::string> arguments{
            "--port", std::to_string(port), "--web-root", web_root.string()};
        arguments.insert(arguments.end(),
            extra_arguments.begin(), extra_arguments.end());
        auto child = std::make_unique<Child>(arguments, log);

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            if (const auto code = child->poll()) {
                const auto output = read_file(log);
                // Another process took the port in between; pick another.
                if (*code == 1 && contains(output, "Could not listen")) {
                    break;
                }
                throw std::runtime_error("inkhibit-server exited early with "
                    + std::to_string(*code) + ": " + output);
            }
            const auto result = get(port, "/api/health");
            if (result && result->status == 200) {
                return {std::move(child), port};
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("inkhibit-server did not become ready");
            }
            std::this_thread::sleep_for(20ms);
        }
    }
    throw std::runtime_error("Could not find a free port");
}

void test_health()
{
    Site site;
    RunningServer server(site.web);
    require_file(server.port, "/api/health", "{\"status\":\"ok\"}\n",
        "application/json");
}

void test_static_files()
{
    Site site;
    RunningServer server(site.web);
    require_file(server.port, "/", index_html, "text/html");
    require_file(server.port, "/index.html", index_html, "text/html");
    require_file(server.port, "/app.js", app_js, "text/javascript");
    require_file(server.port, "/styles.css", styles_css, "text/css");
    require_file(server.port, "/assets/note.txt", note_txt, "text/plain");
}

void test_frontend()
{
    RunningServer server(frontend_root);
    require_file(server.port, "/", read_file(frontend_root / "index.html"),
        "text/html");
    require_file(server.port, "/app.js", read_file(frontend_root / "app.js"),
        "text/javascript");
    require_file(server.port, "/styles.css",
        read_file(frontend_root / "styles.css"), "text/css");
}

void test_not_found()
{
    Site site;
    RunningServer server(site.web);
    require_status(server.port, "/missing.html", 404);
    require_status(server.port, "/assets/missing.txt", 404);
    require_status(server.port, "/api", 404);
    require_status(server.port, "/api/missing", 404);
    require_status(server.port, "/api/health/extra", 404);
}

void test_traversal()
{
    Site site;
    RunningServer server(site.web);
    const char* targets[]{
        "/../secret.txt",
        "/assets/../../secret.txt",
        "/%2e%2e/secret.txt",
        "/%2E%2E/secret.txt",
        "/..%2fsecret.txt",
        "/assets/%2e%2e/%2e%2e/secret.txt",
    };
    for (const auto* target : targets) {
        const auto response = raw_get(server.port, target);
        if (status_of(response) < 400 || contains(response, secret_txt)) {
            throw std::runtime_error(std::string("Traversal not refused: ")
                + target + "\n" + response);
        }
    }
    // The server keeps serving normal requests afterwards.
    require_file(server.port, "/app.js", app_js, "text/javascript");
}

void test_symlink_escape()
{
    Site site;
    fs::create_symlink(site.secret, site.web / "leak.txt");
    fs::create_symlink(site.directory.path, site.web / "outside");
    RunningServer server(site.web);
    for (const auto* target : {"/leak.txt", "/outside/secret.txt"}) {
        const auto response = raw_get(server.port, target);
        if (status_of(response) != 403 || contains(response, secret_txt)) {
            throw std::runtime_error(std::string("Symlink escape not refused: ")
                + target + "\n" + response);
        }
    }
}

void test_invalid_root()
{
    Site site;
    {
        httplib::Server server;
        expect_throw<std::runtime_error>(
            [&] { inkhibit::configure_server(server, site.directory.path / "missing"); },
            "Web root is not a directory");
    }
    {
        httplib::Server server;
        expect_throw<std::runtime_error>(
            [&] { inkhibit::configure_server(server, site.web / "index.html"); },
            "Web root is not a directory");
    }
}

void test_duplicate_port()
{
    Site site;
    RunningServer first(site.web);

    httplib::Server second;
    inkhibit::configure_server(second, site.web);
    REQUIRE(!second.bind_to_port("127.0.0.1", first.port));

    // Every request still reaches the original server.
    for (int i = 0; i < 4; ++i) {
        require_status(first.port, "/api/health", 200);
    }
}

void test_rebind()
{
    Site site;
    int port;
    {
        RunningServer first(site.web);
        port = first.port;
        // The server closes this connection first, leaving TIME_WAIT on its port.
        require_status(port, "/api/health", 200);
    }

    httplib::Server second;
    inkhibit::configure_server(second, site.web);
    REQUIRE(second.bind_to_port("127.0.0.1", port));
    second.stop();
}

void test_cli_help()
{
    TemporaryDirectory directory;
    const auto result = run_cli(directory, {"--help"});
    require_exit(result, 0);
    REQUIRE(contains(result.output, "Usage: inkhibit-server"));
    REQUIRE(contains(result.output, "--web-root"));
    REQUIRE(contains(result.output,
        "Defaults: --bind 0.0.0.0 --port 8080 --web-root web"));
    REQUIRE(contains(result.output,
        "Use --bind 127.0.0.1 to accept connections from this machine only."));
    REQUIRE(contains(result.output, "[--album-root DIR]"));
    REQUIRE(contains(result.output,
        "--album-root DIR enables the unauthenticated album API."));
}

void test_cli_usage()
{
    TemporaryDirectory directory;
    const std::vector<std::vector<std::string>> invocations{
        {"extra"},
        {"--bind"},
        {"--port"},
        {"--port", ""},
        {"--port", "0"},
        {"--port", "65536"},
        {"--port", "100000"},
        {"--port", "-1"},
        {"--port", "+80"},
        {"--port", " 80"},
        {"--port", "80a"},
        {"--web-root"},
        {"--album-root"},
        {"--album-root", ""},
        {"--unknown", "value"},
        {"--help", "--help"},
        {"--port", "8080", "--bind"},
    };
    for (const auto& arguments : invocations) {
        const auto result = run_cli(directory, arguments);
        require_exit(result, 2);
        REQUIRE(contains(result.output, "Usage: inkhibit-server"));
    }
}

void test_cli_invalid_root()
{
    TemporaryDirectory directory;
    const auto missing = directory.path / "missing";
    const auto result = run_cli(directory,
        {"--port", std::to_string(free_port()), "--web-root", missing.string()});
    require_exit(result, 1);
    REQUIRE(contains(result.output,
        "inkhibit-server: Web root is not a directory: " + missing.string()));
}

void test_cli_port_in_use()
{
    Site site;
    RunningServer owner(site.web);
    const auto port = std::to_string(owner.port);

    // Both the loopback address and the all-interfaces default conflict with
    // a server already listening on 127.0.0.1.
    const auto loopback_result = run_cli(site.directory,
        {"--bind", "127.0.0.1", "--port", port, "--web-root", site.web.string()});
    require_exit(loopback_result, 1);
    REQUIRE(contains(loopback_result.output,
        "inkhibit-server: Could not listen on 127.0.0.1:" + port + "\n"));

    const auto default_result = run_cli(site.directory,
        {"--port", port, "--web-root", site.web.string()});
    require_exit(default_result, 1);
    REQUIRE(contains(default_result.output,
        "inkhibit-server: Could not listen on 0.0.0.0:" + port + "\n"));

    require_status(owner.port, "/api/health", 200);
}

// Sends signal to a running server and checks its clean exit and log.
std::string stop_server_process(ServerProcess& process, const fs::path& log,
    int signal)
{
    REQUIRE(::kill(process.child->pid(), signal) == 0);
    const int code = process.child->wait_for_exit();
    const auto output = read_file(log);
    if (code != 128 + signal) {
        throw std::runtime_error("Expected exit " + std::to_string(128 + signal)
            + ", got " + std::to_string(code) + ": " + output);
    }
    REQUIRE(contains(output, "Press Ctrl+C to stop.\n"));
    REQUIRE(contains(output, "Server stopped.\n"));
    return output;
}

void test_cli_signals()
{
    Site site;
    const auto log = site.directory.path / "server.log";
    for (const int signal : {SIGINT, SIGTERM}) {
        auto process = start_server_process(site.web, log, {"--bind", "127.0.0.1"});
        require_file(process.port, "/app.js", app_js, "text/javascript");

        const auto output = stop_server_process(process, log, signal);
        const auto port = std::to_string(process.port);
        REQUIRE(contains(output, "Serving " + site.web.string() + " on port "
            + port + " at http://127.0.0.1:" + port + "/\n"));
    }
}

// The mDNS name the server should print, built by the same rule.
std::string expected_host_url(int port)
{
    char host[256] = {};
    REQUIRE(::gethostname(host, sizeof host - 1) == 0);
    std::string name = host;
    REQUIRE(!name.empty());
    if (name.find('.') == std::string::npos) {
        name += ".local";
    }
    return "http://" + name + ":" + std::to_string(port) + "/";
}

void test_cli_default_bind()
{
    Site site;
    const auto log = site.directory.path / "server.log";
    // No --bind: the default listens on every interface, including loopback.
    auto process = start_server_process(site.web, log, {});
    require_file(process.port, "/app.js", app_js, "text/javascript");
    const auto output = stop_server_process(process, log, SIGTERM);

    const auto port = std::to_string(process.port);
    const std::string header = "Serving " + site.web.string() + " on port "
        + port + " (all network interfaces):\n";
    const auto start = output.find(header);
    REQUIRE(start != std::string::npos);
    const auto end = output.find("Press Ctrl+C to stop.\n", start);
    REQUIRE(end != std::string::npos);

    // Each line between the header and the prompt is one indented URL.
    std::vector<std::string> urls;
    auto position = start + header.size();
    while (position < end) {
        const auto newline = output.find('\n', position);
        REQUIRE(newline != std::string::npos && newline < end);
        const auto line = output.substr(position, newline - position);
        REQUIRE(line.compare(0, 9, "  http://") == 0);
        const std::string suffix = ":" + port + "/";
        REQUIRE(line.size() > 9 + suffix.size());
        REQUIRE(line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0);
        urls.push_back(line.substr(2));
        position = newline + 1;
    }

    // Hostname first, loopback last, with any interface addresses in between.
    REQUIRE(urls.size() >= 2);
    REQUIRE(urls.front() == expected_host_url(process.port));
    REQUIRE(urls.back() == "http://127.0.0.1:" + port + "/");
    for (std::size_t i = 1; i + 1 < urls.size(); ++i) {
        REQUIRE(urls[i] != urls.back());
        in_addr parsed{};
        const auto address = urls[i].substr(7, urls[i].size() - 7 - (port.size() + 2));
        REQUIRE(::inet_pton(AF_INET, address.c_str(), &parsed) == 1);
    }
    REQUIRE(!contains(output, "0.0.0.0"));
}

std::string png_body(std::uint32_t width = 600, std::uint32_t height = 400)
{
    const auto bytes = album_fixture::png(width, height);
    return {bytes.begin(), bytes.end()};
}

httplib::Client album_client(int port)
{
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(5);
    client.set_read_timeout(5);
    client.set_write_timeout(5);
    return client;
}

void check_response(const httplib::Result& result, int status,
    const std::string& body)
{
    REQUIRE(result);
    REQUIRE(result->status == status);
    REQUIRE(result->body == body);
}

void check_json(const httplib::Result& result, int status,
    const std::string& body)
{
    check_response(result, status, body);
    REQUIRE(result->get_header_value("Content-Type") == "application/json");
}

ServerProcess start_album(Site& site, const fs::path& root)
{
    return start_server_process(site.web, site.directory.path / "server.log",
        {"--bind", "127.0.0.1", "--album-root", root.string()});
}

void test_album_disabled()
{
    Site site;
    auto process = start_server_process(site.web, site.directory.path / "server.log",
        {"--bind", "127.0.0.1"});
    auto client = album_client(process.port);
    const auto listed = client.Get("/api/album");
    REQUIRE(listed && listed->status == 404);
    const auto post = client.Post("/api/album", png_body(), "image/png");
    REQUIRE(post && post->status == 404);
    const auto removed = client.Delete("/api/album/1");
    REQUIRE(removed && removed->status == 404);
}

void test_album_lifecycle()
{
    Site site;
    const auto root = site.directory.path / "album";
    const auto log = site.directory.path / "server.log";
    const auto body = png_body();
    {
        auto process = start_album(site, root);
        auto client = album_client(process.port);
        check_json(client.Get("/api/album"), 200, "{\"images\":[]}\n");
        check_json(client.Post("/api/album", body, "image/png"), 201, "{\"id\":\"1\"}\n");
        auto second = png_body();
        // A different valid PNG, so byte preservation isn't confused with IDs.
        const auto black = album_fixture::png(600, 400, 0);
        second.assign(black.begin(), black.end());
        check_json(client.Post("/api/album", second, "image/png"), 201, "{\"id\":\"2\"}\n");
        check_json(client.Get("/api/album"), 200, "{\"images\":[\"1\",\"2\"]}\n");
        REQUIRE(read_file(root / "1.png") == body);
        REQUIRE(read_file(root / "2.png") == second);
        stop_server_process(process, log, SIGTERM);
    }
    {
        auto process = start_album(site, root);
        auto client = album_client(process.port);
        check_json(client.Get("/api/album"), 200, "{\"images\":[\"1\",\"2\"]}\n");
        check_response(client.Delete("/api/album/2"), 204, "");
        REQUIRE(!fs::exists(root / "2.png"));
        const auto missing = client.Delete("/api/album/2");
        REQUIRE(missing && missing->status == 404);
        stop_server_process(process, log, SIGTERM);
    }
    {
        auto process = start_album(site, root);
        auto client = album_client(process.port);
        check_json(client.Get("/api/album"), 200, "{\"images\":[\"1\"]}\n");
        check_json(client.Post("/api/album", body, "image/png"), 201, "{\"id\":\"3\"}\n");
        REQUIRE(read_file(root / "1.png") == body);
    }
}

void test_album_invalid_uploads()
{
    Site site;
    const auto root = site.directory.path / "album";
    auto process = start_album(site, root);
    auto client = album_client(process.port);
    auto truncated = png_body();
    truncated.resize(33);
    for (const auto& body : {std::string{}, std::string("not a PNG"),
            png_body(599, 400), png_body(400, 600), truncated}) {
        const auto result = client.Post("/api/album", body, "image/png");
        REQUIRE(result && result->status == 400);
        check_json(client.Get("/api/album"), 200, "{\"images\":[]}\n");
        REQUIRE(fs::is_empty(root));
    }
    check_json(client.Post("/api/album", png_body(), "image/png"), 201, "{\"id\":\"1\"}\n");
}

void test_album_content_type()
{
    Site site;
    const auto root = site.directory.path / "album";
    auto process = start_album(site, root);
    const auto host = "127.0.0.1:" + std::to_string(process.port);
    const auto body = png_body();
    for (const auto& headers : {std::string{},
            std::string("Content-Type: image/jpeg\r\n"),
            std::string("Content-Type: application/octet-stream\r\n"),
            std::string("Content-Type: image/png\r\nContent-Type: image/png\r\n")}) {
        const auto result = raw_request(process.port, "POST /api/album HTTP/1.1\r\nHost: "
            + host + "\r\nConnection: close\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body);
        REQUIRE(status_of(result) == 415);
        REQUIRE(fs::is_empty(root));
    }
}

void test_album_payload_limit()
{
    Site site;
    const auto root = site.directory.path / "album";
    auto process = start_album(site, root);
    auto client = album_client(process.port);
    constexpr std::size_t limit = 2 * 1024 * 1024;
    auto body = png_body();
    body.resize(limit, '\0');
    check_json(client.Post("/api/album", body, "image/png"), 201, "{\"id\":\"1\"}\n");
    body.push_back('\0');
    const auto too_large = client.Post("/api/album", body, "image/png");
    REQUIRE(too_large && too_large->status == 413);

    // Chunked requests have no Content-Length; enforce the cumulative limit
    // as bytes arrive, not just the declared length of a fixed-size request.
    const auto host = "127.0.0.1:" + std::to_string(process.port);
    const auto response = raw_request(process.port,
        "POST /api/album HTTP/1.1\r\nHost: " + host
        + "\r\nContent-Type: image/png\r\nTransfer-Encoding: chunked\r\n"
          "Connection: close\r\n\r\n200000\r\n"
        + body.substr(0, limit) + "\r\n1\r\nX\r\n0\r\n\r\n");
    REQUIRE(status_of(response) == 413);
    check_json(client.Get("/api/album"), 200, "{\"images\":[\"1\"]}\n");
    REQUIRE(!fs::exists(root / "2.png"));
}

void test_album_host()
{
    Site site;
    const auto root = site.directory.path / "album";
    auto process = start_album(site, root);
    auto client = album_client(process.port);
    check_json(client.Post("/api/album", png_body(), "image/png"), 201, "{\"id\":\"1\"}\n");
    for (const auto& host : {std::string("untrusted.example:") + std::to_string(process.port),
            std::string("localhost:") + std::to_string(process.port)}) {
        const httplib::Headers headers{{"Host", host}};
        const auto listed = client.Get("/api/album", headers);
        const auto added = client.Post("/api/album", headers, png_body(), "image/png");
        const auto removed = client.Delete("/api/album/1", headers);
        REQUIRE(listed && listed->status == 403);
        REQUIRE(added && added->status == 403);
        REQUIRE(removed && removed->status == 403);
    }
    check_json(client.Get("/api/album"), 200, "{\"images\":[\"1\"]}\n");
}

void test_album_delete_ids()
{
    Site site;
    auto process = start_album(site, site.directory.path / "album");
    auto client = album_client(process.port);
    check_json(client.Post("/api/album", png_body(), "image/png"), 201, "{\"id\":\"1\"}\n");
    for (const auto* id : {"0", "01", "-1", "+1", "1x", "18446744073709551616"}) {
        const auto response = client.Delete(std::string("/api/album/") + id);
        REQUIRE(response && response->status == 400);
    }
    // A valid uint64_t ID isn't invalid merely because JavaScript can't represent it.
    for (const auto* id : {"2", "9007199254740993", "18446744073709551615"}) {
        const auto response = client.Delete(std::string("/api/album/") + id);
        REQUIRE(response && response->status == 404);
    }
    check_json(client.Get("/api/album"), 200, "{\"images\":[\"1\"]}\n");
    check_response(client.Delete("/api/album/1"), 204, "");
}

void test_album_private_root()
{
    Site site;
    fs::create_directory_symlink(site.web, site.directory.path / "web-link");
    for (const auto& root : {site.web, site.web / "new" / "album",
            site.directory.path / "web-link" / "album"}) {
        // Exercise relative paths and non-existent suffixes too.
        const auto relative = fs::relative(root, fs::current_path());
        const auto result = run_cli(site.directory,
            {"--bind", "127.0.0.1", "--port", std::to_string(free_port()),
             "--web-root", site.web.string(), "--album-root", relative.string()});
        require_exit(result, 1);
        REQUIRE(contains(result.output, "Album root must be outside the web root"));
    }
    REQUIRE(!fs::exists(site.web / "new"));
    REQUIRE(!fs::exists(site.web / "album"));
    // A name with the same prefix isn't actually inside the web root.
    auto process = start_album(site, site.directory.path / "web-album");
    require_file(process.port, "/api/album", "{\"images\":[]}\n", "application/json");
}

void test_album_storage_error()
{
    Site site;
    const auto root = site.directory.path / "album";
    auto process = start_album(site, root);
    auto client = album_client(process.port);
    fs::create_directory(root / "1.png.tmp"); // Deterministic write failure, even as root.
    check_response(client.Post("/api/album", png_body(), "image/png"), 500,
        "Could not save album image\n");
    check_json(client.Get("/api/album"), 200, "{\"images\":[]}\n");
    fs::remove(root / "1.png.tmp");
    check_json(client.Post("/api/album", png_body(), "image/png"), 201, "{\"id\":\"2\"}\n");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        REQUIRE(argc == 4);
        const std::string name = argv[1];
        server_executable = fs::absolute(argv[2]);
        frontend_root = fs::absolute(argv[3]);
        const std::pair<const char*, void (*)()> tests[]{
            {"health", test_health},
            {"static_files", test_static_files},
            {"frontend", test_frontend},
            {"not_found", test_not_found},
            {"traversal", test_traversal},
            {"symlink_escape", test_symlink_escape},
            {"invalid_root", test_invalid_root},
            {"duplicate_port", test_duplicate_port},
            {"rebind", test_rebind},
            {"cli_help", test_cli_help},
            {"cli_usage", test_cli_usage},
            {"cli_invalid_root", test_cli_invalid_root},
            {"cli_port_in_use", test_cli_port_in_use},
            {"cli_signals", test_cli_signals},
            {"cli_default_bind", test_cli_default_bind},
            {"album_disabled", test_album_disabled},
            {"album_lifecycle", test_album_lifecycle},
            {"album_invalid_uploads", test_album_invalid_uploads},
            {"album_content_type", test_album_content_type},
            {"album_payload_limit", test_album_payload_limit},
            {"album_host", test_album_host},
            {"album_delete_ids", test_album_delete_ids},
            {"album_private_root", test_album_private_root},
            {"album_storage_error", test_album_storage_error}
        };
        for (const auto& test : tests) {
            if (name == test.first) {
                test.second();
                std::cout << "PASS: " << name << '\n';
                return EXIT_SUCCESS;
            }
        }
        throw std::runtime_error("Unknown test: " + name);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << (argc > 1 ? argv[1] : "missing test name")
                  << ": " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
