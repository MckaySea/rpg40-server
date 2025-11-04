#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <string>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

void do_session(tcp::socket& socket) {
    try {
        websocket::stream<tcp::socket> ws{ std::move(socket) };

        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        std::cout << "Attempting handshake..." << std::endl;
        ws.accept();
        std::cout << "Handshake successful. Connection established." << std::endl;

        ws.write(net::buffer(std::string("Server: Hello from the C++ Dungeon Master!")));

        beast::flat_buffer buffer;

        for (;;) {
            ws.read(buffer);

            std::string received_message = beast::buffers_to_string(buffer.data());
            std::cout << "Received: " << received_message << std::endl;

            std::string echo_message = "Server Echo: " + received_message;
            ws.write(net::buffer(echo_message));

            buffer.consume(buffer.size());
        }
    }
    catch (beast::system_error const& se) {
        if (se.code() != websocket::error::closed) {
            std::cerr << "Boost.Beast error: " << se.code().message() << std::endl;
        }
    }
    catch (std::exception const& e) {
        std::cerr << "General error: " << e.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>\n"
            << "Example:\n"
            << "    " << argv[0] << " 8080\n";
        return EXIT_FAILURE;
    }

    auto const port = static_cast<unsigned short>(std::atoi(argv[1]));

    try {
        net::io_context ioc{ 1 };

        tcp::endpoint endpoint{ tcp::v4(), port };


        tcp::acceptor acceptor{ ioc, endpoint };

        std::cout << "Server is listening on port " << port << "..." << std::endl;

        for (;;) {
            tcp::socket socket{ ioc };

            acceptor.accept(socket);

            std::cout << "\nNew client connected." << std::endl;

            do_session(socket);

            std::cout << "Client disconnected." << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error in main: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

