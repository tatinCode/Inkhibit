#include "inkhibit/server/server.hpp"

#include <httplib.h>    //the star of teh show

#include <stdexcept>
#include <string>
#include <system_error>

namespace inkhibit{
    void configure_server(
            httplib::Server& server,
            const std::filesystem::path& web_root
            ){

        // cpp-httplib enables SO_REUSEPORT by default, which lets a second
        // server bind the same port and share its connections. SO_REUSEADDR
        // still allows a quick restart, but reports a port already in use.
        server.set_socket_options([](socket_t sock){
                httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
                });

        std::error_code error;

        if(!std::filesystem::is_directory(web_root, error)){
            throw std::runtime_error(
                    "Web root is not a directory: " + web_root.string()
                    );
        }

        if(!server.set_mount_point("/", web_root.string())){
            throw std::runtime_error(
                    "Cannot serve web root: " + web_root.string()
                    );
        }

        server.Get("/api/health",
                [](const httplib::Request&, httplib::Response& response){
                    response.set_content("{\"status\":\"ok\"}\n", "application/json");
                });

    }
}   //end of namespace inkhibit
