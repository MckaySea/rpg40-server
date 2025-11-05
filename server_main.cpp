// File: LocalWebSocketServer/server_main.cpp
//
// MODIFIED: Converted to an asynchronous, thread-pooled server model.
// FIXED: Replaced bind_front_handler with a standard lambda.
//
#include "game_session.hpp" 
#include "AsyncSession.hpp" // --- NEW: Include the session class ---
#include <iostream>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // --- ADDED: Include for make_strand ---
#include <vector>
#include <thread>
#include <memory>

namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- NEW: Listener class to handle incoming connections ---
class listener : public std::enable_shared_from_this<listener>
{
    net::io_context& ioc_;
    tcp::acceptor acceptor_;

public:
    listener(net::io_context& ioc, tcp::endpoint endpoint)
        : ioc_(ioc)
        , acceptor_(ioc)
    {
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
        {
            std::cerr << "listener open: " << ec.message() << "\n";
            return;
        }

        // Allow address reuse
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec)
        {
            std::cerr << "listener set_option: " << ec.message() << "\n";
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec)
        {
            std::cerr << "listener bind: " << ec.message() << "\n";
            return;
        }

        // Start listening for connections
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec)
        {
            std::cerr << "listener listen: " << ec.message() << "\n";
            return;
        }
    }

    // Start accepting connections
    void run()
    {
        do_accept();
    }

private:
    void do_accept()
    {
        // The new socket for the next connection
        // We use net::make_strand to ensure all handlers for a session
        // are sequential, even across threads.
        acceptor_.async_accept(
            net::make_strand(ioc_), // --- This is correct ---
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket)
            {
                self->on_accept(ec, std::move(socket));
            });
    }

    void on_accept(beast::error_code ec, tcp::socket socket)
    {
        if (ec)
        {
            std::cerr << "accept: " << ec.message() << "\n";
        }
        else
        {
            // Create the AsyncSession and run it
            std::make_shared<AsyncSession>(std::move(socket))->run();
        }

        // Accept the next connection
        do_accept();
    }
};


int main() {
    auto const address = net::ip::make_address("0.0.0.0");
    auto const port = static_cast<unsigned short>(8080);

    net::io_context ioc;

    // Create and launch the listener
    std::make_shared<listener>(ioc, tcp::endpoint{ address, port })->run();

    // --- NEW: Thread Pool ---
    // Run the I/O service on a pool of threads
    // Use a reasonable number of threads, e.g., hardware concurrency
    unsigned const threads = std::max<int>(1, std::thread::hardware_concurrency());
    std::vector<std::thread> v;
    v.reserve(threads - 1);

    std::cout << "Server is listening on port " << port << " with " << threads << " threads...\n";

    // Run one I/O context in each thread
    for (auto i = threads - 1; i > 0; --i)
    {
        v.emplace_back(
            [&ioc]
            {
                ioc.run();
            });
    }
    // The main thread also participates
    ioc.run();

    // (This part is never reached, but good practice)
    for (auto& t : v)
    {
        t.join();
    }

    return 0;
}