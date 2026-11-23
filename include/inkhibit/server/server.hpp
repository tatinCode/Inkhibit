#ifndef INKHIBIT_SERVER_SERVER_HPP
#define INKHIBIT_SERVER_SERVER_HPP

#include <filesystem>

namespace httplib{
    class Server;
}

namespace inkhibit{

    //serves the frontend from web_root and registers the API routes.
    //throws std::runtime_error if web_root is not a readable directory.
    void configure_server(
            httplib::Server& server,
            const std::filesystem::path& web_root
            );

}

#endif
