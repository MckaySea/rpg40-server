// File: LocalWebSocketServer/AsyncSession.hpp
#pragma once

#include "game_session.hpp" // Includes PlayerState, Point, etc.
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <memory>
#include <string>
#include <functional>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Manages a single WebSocket session asynchronously
class AsyncSession : public std::enable_shared_from_this<AsyncSession>
{
    websocket::stream<tcp::socket> ws_;
    net::steady_timer move_timer_;
    beast::flat_buffer buffer_;

    PlayerState player_;
    PlayerBroadcastData broadcast_data_;
    std::string client_address_;

public:
    // Take ownership of the socket
    explicit AsyncSession(tcp::socket socket);

    // Destructor for proper cleanup
    ~AsyncSession();

    // Start the session
    void run();

private:
    void on_run(); // This is the first function called
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);

    // Game logic handlers
    void do_move_tick(beast::error_code ec);
    void handle_message(const std::string& message);

    // Session cleanup
    void on_session_end();

    // --- HELPER FUNCTIONS ---
    // These are now private members of the session
    void send_player_stats();
    void send_available_areas();
    void generate_and_send_monsters();
    void send_current_monsters_list();
    void check_for_level_up();
};