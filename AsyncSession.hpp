// File: AsyncSession.hpp
// Description: Manages a single client's WebSocket session, handling
// all asynchronous I/O operations and holding the player's state.
#pragma once

#include "game_session.hpp" 
#include "DatabaseManager.hpp"// Includes PlayerState, Point, etc.
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <memory>
#include <string>
#include <functional>
#include <atomic>
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

//should manaage a single ws connection async
class AsyncSession : public std::enable_shared_from_this<AsyncSession>
{
	// --- Networking Members ---
	websocket::stream<tcp::socket> ws_;// The WebSocket stream
	net::steady_timer move_timer_; // Timer for player movement ticks
	beast::flat_buffer buffer_; // Buffer for reading messages
	std::string client_address_; // Client's IP for logging
	std::shared_ptr<DatabaseManager> db_manager_;
	std::atomic<bool> is_authenticated_ = false;
	std::atomic<int> account_id_ = 0;
	PlayerState player_; // This session's unique player state
	PlayerBroadcastData broadcast_data_; // Public data for other players

public:
	// Take ownership of the socket
	explicit AsyncSession(
		tcp::socket socket,
		std::shared_ptr<DatabaseManager> db_manager
	);

	// Destructor for proper cleanup
	~AsyncSession() noexcept;

	// Start the session's asynchronous operations
	void run();
	void save_character();
	void send_shutdown_warning(int seconds); // <-- ADD THIS
	void disconnect();
	// These allow the game logic functions to interact with the session
	PlayerState& getPlayerState() { return player_; }
	PlayerBroadcastData& getBroadcastData() { return broadcast_data_; }
	websocket::stream<tcp::socket>& getWebSocket() { return ws_; }
	bool grantSkillToPlayer(const std::string& skillName, std::string& outError);

	void handle_register(const std::string& credentials);
	void handle_login(const std::string& credentials);

private:
	void load_character(int accountId);
	void on_run();
	void do_read();
	void on_read(beast::error_code ec, std::size_t bytes_transferred);
	void on_write(beast::error_code ec, std::size_t bytes_transferred);
	void do_move_tick(beast::error_code ec);
	void on_session_end();
	void useItem(uint64_t itemInstanceId);
	void dropItem(uint64_t itemInstanceId, int quantity);
	void ensureAutoGrantedSkillsForClass();
	// --- ADD THESE FOUR LINES ---

	/**
	 * @brief Processes a single movement tick.
	 * Called by the move_timer_ (do_move_tick).
	 */
	void process_movement();

	/**
	 * @brief The main router for all incoming client messages.
	 * @param message The raw message from the client.
	 */
	void handle_message(const std::string& message);

	/**
	 * @brief Sends the player's complete stat block to the client.
	 */
	void send_player_stats();

	/**
	 * @brief Sends a dynamically generated list of available areas to travel to.
	 */
	void send_available_areas();

	/**
	 * @brief Generates a new list of monsters for the player's current area.
	 */
	void generate_and_send_monsters();

	/**
	 * @brief Sends the current list of monsters (player.currentMonsters) to the client.
	 */
	void send_current_monsters_list();

	//this just send the interactables for the current arera the player is in(needed for performance as well lol (12threadsbtw)
	void send_interactables(const std::string& areaName);

	/**
	 * @brief Checks if the player has enough XP to level up, and processes it.
	 */
	void check_for_level_up();

	/**
	 * @brief Sends the tile map data (e.g., TOWN_GRID) for the current area.
	 * @param areaName The name of the area to send map data for.
	 */
	void send_area_map_data(const std::string& areaName);




	//gets the plyrs final stats with equips and all
	PlayerStats getCalculatedStats();

	//this adds an item to the players inventory but always rolls for random stats from its base
	void addItemToInventory(const std::string& itemId, int quantity = 1);
	void sellItem(uint64_t itemInstanceId, int quantity);
	//equips an item from inventory if possible with the uniqueitem id
	std::string equipItem(uint64_t itemInstanceId);

	std::string unequipItem(EquipSlot slotToUnequip);

	//sends the players inventory n equipment to their client !
	void send_inventory_and_equipment();
};