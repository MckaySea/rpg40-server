// File: src/server_main.cpp
#include "game_session.hpp"
#include <iostream>
#include <boost/asio/ip/tcp.hpp>
#include <thread> 

namespace net = boost::asio;
using tcp = net::ip::tcp;

int main() {
    net::io_context ioc;

    // Bind the acceptor to all interfaces (0.0.0.0) on port 8080
    tcp::acceptor acceptor{ ioc, {net::ip::make_address("0.0.0.0"), 8080} };

    std::cout << "Server is listening on port 8080...\n";

    while (true) {
        try {
            tcp::socket socket{ ioc };

            // Block until a new client connects
            acceptor.accept(socket);

            // Log the new connection before handing it off to a session thread
            std::cout << "--- New Client Connected from: " << socket.remote_endpoint() << " ---\n";

            // Start a new thread for the session.
            // std::move(socket) correctly calls the do_session(tcp::socket socket) overload.
            std::thread{ do_session, std::move(socket) }.detach();
        }
        catch (std::exception const& e) {
            std::cerr << "Listener Error: " << e.what() << "\n";
        }
    }

    return 0;
}
