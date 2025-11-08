// File: server_main.cpp
// Description: The main entry point for the WebSocket server.
// Sets up a thread pool and a listener to accept incoming client connections.

#include "game_session.hpp" // For basic types (not strictly needed here, but good practice)
#include "AsyncSession.hpp" 
#include "GameData.hpp"
#include <iostream>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // For make_strand
#include <vector>
#include <thread>
#include <memory>

namespace net = boost::asio;
using tcp = net::ip::tcp;

/**
 * @class listener
 * @brief Accepts incoming TCP connections and creates a new AsyncSession for each one.
 * This class lives as long as it's referenced (e.g., by its own async callbacks).
 */
class listener : public std::enable_shared_from_this<listener>
{
    net::io_context& ioc_; // Reference to the main I/O context
    tcp::acceptor acceptor_; // The Boost.Asio object that accepts connections

public:
    /**
     * @brief Constructs the listener.
     * @param ioc The I/O context to run on.
     * @param endpoint The IP address and port to bind to.
     */
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

        // Allow address reuse (e.g., for quick server restarts)
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


    /**
     * @brief Starts the asynchronous accept loop.
     */
    void run()
    {
        do_accept();
    }

private:
    /**
     * @brief The main accept loop. Waits for a connection.
     */
    void do_accept()
    {
        // Asynchronously wait for a new connection.
        acceptor_.async_accept(
            // Use net::make_strand to ensure that the on_accept handler
            // and the session's I/O operations are serialized,
            // even in a multi-threaded environment.
            net::make_strand(ioc_),
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket)
            {
                // When a connection arrives, call on_accept.
                self->on_accept(ec, std::move(socket));
            });
    }

    /**
     * @brief Callback for when a new connection is accepted.
     * @param ec An error code, if any.
     * @param socket The new client's socket.
     */
    void on_accept(beast::error_code ec, tcp::socket socket)
    {
        if (ec)
        {
            std::cerr << "accept: " << ec.message() << "\n";
        }
        else
        {
            // Create a new session for this client, give it the socket,
            // and start the session's run loop.
            std::make_shared<AsyncSession>(std::move(socket))->run();
        }

        // Continue the loop to accept the next connection.
        do_accept();
    }
};


/**
 * @brief Main server function.
 */
int main() {
    auto const address = net::ip::make_address("0.0.0.0"); // Listen on all interfaces
    auto const port = static_cast<unsigned short>(8080); // Port to listen on

    // The I/O context is the core of all Asio operations
    net::io_context ioc;

    // Create and launch the listener
    std::make_shared<listener>(ioc, tcp::endpoint{ address, port })->run();

    // --- Thread Pool ---
    // Use a number of threads equal to the hardware concurrency
    unsigned const threads = std::max<int>(1, std::thread::hardware_concurrency());
    std::vector<std::thread> v;
    v.reserve(threads - 1);

    std::cout << "Server is listening on port " << port << " with " << threads << " threads...\n";
    initialize_item_prices();
    // Run the I/O context in each thread
    for (auto i = threads - 1; i > 0; --i)
    {
        v.emplace_back(
            [&ioc]
            {
                // Each thread will block here, processing async operations
                ioc.run();
            });
    }
    // The main thread also participates in processing operations
    ioc.run();

    // This part is never reached in this example, but it's good practice
    // if the server had a graceful shutdown mechanism.
    for (auto& t : v)
    {
        t.join();
    }

    return 0;
}