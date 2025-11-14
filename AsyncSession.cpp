#include "AsyncSession.hpp"
#include "GameData.hpp"
#include <iostream>



// Description: Implements the networking and session management logic for AsyncSession.
// Handles connections, disconnections, async reads, writes, and timers.

/**
 * @brief Constructs the session, moving the socket into the WebSocket stream.
 */
AsyncSession::AsyncSession(
	tcp::socket socket,
	std::shared_ptr<DatabaseManager> db_manager // <-- ADD THIS
)
	: ws_(std::move(socket))
	, move_timer_(ws_.get_executor())
	, client_address_(ws_.next_layer().remote_endpoint().address().to_string())
	, db_manager_(db_manager) // <-- STORE THE MANAGER
{
	move_timer_.expires_at(std::chrono::steady_clock::time_point::max());
	std::cout << "--- New Client Connected from: " << client_address_ << " ---" << std::endl;
}

/**
 * @brief Destructor. Ensures cleanup logic is run.
 */
AsyncSession::~AsyncSession()
{
	// This is a failsafe, on_session_end should already be called
	std::lock_guard<std::mutex> lock(g_session_registry_mutex);
	g_session_registry.erase(player_.userId);
}

/**
 * @brief Starts the session by posting the on_run handler to the strand.
 */
void AsyncSession::run()
{
	// Use net::dispatch to run the handler on the session's strand
	net::dispatch(ws_.get_executor(),
		[self = shared_from_this()]()
		{
			self->on_run();
		});
}

/**
 * @brief Performs the WebSocket handshake.
 */
void AsyncSession::on_run()
{
	// Start the asynchronous WebSocket handshake
	ws_.async_accept(
		net::bind_executor(ws_.get_executor(),
			[self = shared_from_this()](beast::error_code ec)
			{
				if (ec)
				{
					std::cerr << "[" << self->client_address_ << "] Handshake Error: " << ec.message() << "\n";
					return self->on_session_end();
				}

				std::cout << "[" << self->client_address_ << "] Handshake successful. Session started.\n";

				// --- Initial Player Setup ---
				// This is game logic, but it's tied to the session start.
				self->player_.userId = "Client_" + std::to_string(g_session_id_counter++);
				self->player_.currentArea = "TOWN";
				self->player_.posX = 5;
				self->player_.posY = 5;
				self->player_.lastMoveTime = std::chrono::steady_clock::now();

				// Setup broadcast data
				self->broadcast_data_.userId = self->player_.userId;
				self->broadcast_data_.currentArea = "TOWN";
				self->broadcast_data_.posX = self->player_.posX;
				self->broadcast_data_.posY = self->player_.posY;

				// Add to global registries
				{
					std::lock_guard<std::mutex> lock(g_player_registry_mutex);
					g_player_registry[self->player_.userId] = self->broadcast_data_;
				}
				{
					std::lock_guard<std::mutex> lock(g_session_registry_mutex);
					g_session_registry[self->player_.userId] = self->shared_from_this();
				}

				// Send the welcome message
				static std::string welcome = "SERVER:WELCOME! Please enter your character name using SET_NAME:YourName";
				self->ws_.async_write(net::buffer(welcome),
					net::bind_executor(self->ws_.get_executor(),
						[self = self->shared_from_this()](beast::error_code ec, std::size_t bytes)
						{
							self->on_write(ec, bytes);
						}));

				// Start the read loop
				self->do_read();
				// Start the movement timer
				self->do_move_tick(beast::error_code{});
			}));
}

/**
 * @brief Posts an asynchronous read operation.
 */
void AsyncSession::do_read()
{
	// Wait for a message from the client
	ws_.async_read(buffer_,
		net::bind_executor(ws_.get_executor(),
			[self = shared_from_this()](beast::error_code ec, std::size_t bytes)
			{
				self->on_read(ec, bytes);
			}));
}

/**
 * @brief Callback for when a read completes.
 */
void AsyncSession::on_read(beast::error_code ec, std::size_t bytes_transferred)
{
	boost::ignore_unused(bytes_transferred);

	// Handle standard closure or errors
	if (ec == websocket::error::closed || ec == net::error::eof || ec == net::error::operation_aborted)
		return on_session_end();

	if (ec)
	{
		std::cerr << "[" << client_address_ << "] Read Error: " << ec.message() << "\n";
		return on_session_end();
	}

	// Convert the message to a string
	std::string message = beast::buffers_to_string(buffer_.data());

	if (message != "REQUEST_PLAYERS") {
		std::cout << "[" << client_address_ << "] Received: " << message << "\n";
	}
	// --- Pass the message to the game logic handler ---
	handle_message(message);

	// Clear the buffer and post the next read
	buffer_.consume(buffer_.size());
	do_read();
}

/**
 * @brief Callback for when a write completes.
 */
void AsyncSession::on_write(beast::error_code ec, std::size_t bytes_transferred)
{
	boost::ignore_unused(bytes_transferred);
	if (ec)
	{
		std::cerr << "[" << client_address_ << "] Write Error: " << ec.message() << "\n";
		return on_session_end();
	}
	// Writes are "fire and forget," so no further action is needed
}

/**
 * @brief The timer tick loop for movement.
 */
void AsyncSession::do_move_tick(beast::error_code ec)
{
	if (ec && ec != net::error::operation_aborted)
	{
		std::cerr << "[" << client_address_ << "] Move Timer Error: " << ec.message() << "\n";
		return on_session_end();
	}

	// Timer was not cancelled, so process the game logic for movement
	process_movement();

	// Re-queue the timer
	move_timer_.expires_after(std::chrono::milliseconds(SERVER_TICK_RATE_MS));
	move_timer_.async_wait(
		net::bind_executor(ws_.get_executor(),
			[self = shared_from_this()](beast::error_code ec)
			{
				self->do_move_tick(ec);
			}));
}

/**
 * @brief Cleans up the session on disconnect or error.
 */
void AsyncSession::on_session_end()
{
	// Stop the movement timer
	move_timer_.cancel();

	// --- ASYNCHRONOUS SAVE ON DISCONNECT ---
	// Check if the player was actually logged in before trying to save
	if (is_authenticated_ && account_id_ != 0)
	{
		// --- FIX ---
		// Post the save_character work to the session's own strand
		// (which runs on the main thread pool).
		// Capturing 'self' (a shared_ptr) keeps the object alive
		// until this posted task completes.
		net::post(ws_.get_executor(), [self = shared_from_this()]() {
			try {
				// This is now running on a background thread
				self->save_character();
			}
			catch (std::exception const& e) {
				// This error is now logged from the background thread
				std::cerr << "[" << self->client_address_ << "] CRITICAL: FAILED TO SAVE on async disconnect: " << e.what() << "\n";
			}
			});
		// --- END FIX ---
	}

	// The rest of your cleanup logic runs *immediately*
	// without waiting for the save to finish.
	try {
		std::lock_guard<std::mutex> lock(g_player_registry_mutex);
		g_player_registry.erase(player_.userId);
	}
	catch (std::exception const& e) {
		std::cerr << "[" << client_address_ << "] Exception during data cleanup: " << e.what() << "\n";
	}

	// Remove session from the chat registry
	try {
		std::lock_guard<std::mutex> lock(g_session_registry_mutex);
		g_session_registry.erase(player_.userId);
	}
	catch (std::exception const& e) {
		std::cerr << "[" << client_address_ << "] Exception during session cleanup: " << e.what() << "\n";
	}

	std::cout << "[" << client_address_ << "] Client disconnected.\n";
	// The session (shared_ptr) will be destroyed when this handler finishes
	// (or when the async save task above finally completes, whichever is later)
}
/**
 * @brief Sends a non-blocking shutdown warning to the client.
 */
void AsyncSession::send_shutdown_warning(int seconds)
{
	auto shared_msg = std::make_shared<std::string>(
		"SERVER:SHUTDOWN:" + std::to_string(seconds)
	);
	// Post the write operation to the session's strand to ensure thread safety
	net::dispatch(ws_.get_executor(),
		[self = shared_from_this(), shared_msg]()
		{
			// We use async_write but don't need a handler
			self->ws_.async_write(net::buffer(*shared_msg),
				[](beast::error_code, std::size_t) {
					// Log error if you want, but don't stop the shutdown
				});
		});
}

/**
 * @brief Posts a disconnect operation to the session's strand.
 */
void AsyncSession::disconnect()
{
	// Post the close operation to this session's strand
	net::dispatch(ws_.get_executor(),
		[self = shared_from_this()]()
		{
			beast::error_code ec;
			// This will trigger on_session_end(), which runs save_character()
			self->ws_.close(websocket::close_code::service_restart, ec);
		});
}