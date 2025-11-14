#include "AsyncSession.hpp"
#include "GameData.hpp"
#include <iostream>

/**
 * @brief Constructs the session, moving the socket into the WebSocket stream.
 */
AsyncSession::AsyncSession(
	tcp::socket socket,
	std::shared_ptr<DatabaseManager> db_manager,
	std::shared_ptr<ThreadPool> db_pool,
	std::shared_ptr<ThreadPool> save_pool
)
	: ws_(std::move(socket))
	, move_timer_(ws_.get_executor())
	, client_address_(ws_.next_layer().remote_endpoint().address().to_string())
	, db_manager_(db_manager)
	, db_pool_(db_pool)
	, save_pool_(save_pool)
{
	move_timer_.expires_at(std::chrono::steady_clock::time_point::max());
	std::cout << "--- New Client Connected from: " << client_address_ << " ---" << std::endl;
}

/**
 * @brief Destructor. Ensures cleanup logic is run.
 */
AsyncSession::~AsyncSession()
{
	std::lock_guard<std::mutex> lock(g_session_registry_mutex);
	g_session_registry.erase(player_.userId);
}

/**
 * @brief Starts the session by posting the on_run handler to the strand.
 */
void AsyncSession::run()
{
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
				self->player_.userId = "Client_" + std::to_string(g_session_id_counter++);
				self->player_.currentArea = "TOWN";
				self->player_.posX = 26; // Town spawn X
				self->player_.posY = 12; // Town spawn Y
				self->player_.lastMoveTime = std::chrono::steady_clock::now();

				self->broadcast_data_.userId = self->player_.userId;
				self->broadcast_data_.currentArea = "TOWN";
				self->broadcast_data_.posX = self->player_.posX;
				self->broadcast_data_.posY = self->player_.posY;

				{
					std::lock_guard<std::mutex> lock(g_player_registry_mutex);
					g_player_registry[self->player_.userId] = self->broadcast_data_;
				}
				{
					std::lock_guard<std::mutex> lock(g_session_registry_mutex);
					g_session_registry[self->player_.userId] = self->shared_from_this();
				}

				// Use our new async send function
				self->send("SERVER:WELCOME! Please log in or register.");

				self->do_read();
				self->do_move_tick(beast::error_code{});
			}));
}

/**
 * @brief Posts an asynchronous read operation.
 */
void AsyncSession::do_read()
{
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

	if (ec == websocket::error::closed || ec == net::error::eof || ec == net::error::operation_aborted)
		return on_session_end();

	if (ec)
	{
		std::cerr << "[" << client_address_ << "] Read Error: " << ec.message() << "\n";
		return on_session_end();
	}

	std::string message = beast::buffers_to_string(buffer_.data());

	if (message != "REQUEST_PLAYERS") {
		std::cout << "[" << client_address_ << "] Received: " << message << "\n";
	}

	handle_message(message);

	buffer_.consume(buffer_.size());
	do_read();
}

// --- NEW ASYNC WRITE QUEUE ---

/**
 * @brief Public function to send a message.
 * Adds the message to the queue and starts the write loop if not running.
 */
void AsyncSession::send(std::string message)
{
	auto shared_msg = std::make_shared<std::string>(std::move(message));

	net::dispatch(ws_.get_executor(),
		[self = shared_from_this(), shared_msg]()
		{
			self->write_queue_.push(shared_msg);
			if (!self->is_writing_)
			{
				self->do_async_write();
			}
		});
}

/**
 * @brief The actual async write operation.
 * This is always called from within the session's strand.
 */
void AsyncSession::do_async_write()
{
	is_writing_ = true;
	auto msg = write_queue_.front();

	ws_.async_write(net::buffer(*msg),
		net::bind_executor(ws_.get_executor(),
			[self = shared_from_this()](beast::error_code ec, std::size_t bytes)
			{
				self->on_write(ec, bytes);
			}));
}

/**
 * @brief Callback for when a write completes.
 */
void AsyncSession::on_write(beast::error_code ec, std::size_t bytes_transferred)
{
	boost::ignore_unused(bytes_transferred);

	// Handle disconnect errors silently. This is what prevents the crash.
	if (ec == websocket::error::closed || ec == net::error::eof || ec == net::error::operation_aborted)
		return on_session_end();

	if (ec)
	{
		std::cerr << "[" << client_address_ << "] Write Error: " << ec.message() << "\n";
		return on_session_end();
	}

	// The write is complete, clear the message from the queue
	write_queue_.pop();

	if (!write_queue_.empty())
	{
		do_async_write(); // Keep writing if more messages are queued
	}
	else
	{
		is_writing_ = false; // Queue is empty
	}
}
// --- END ASYNC WRITE QUEUE ---


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

	if (ec == net::error::operation_aborted)
		return; // Timer was cancelled

	try
	{
		process_movement();
	}
	catch (const std::exception& e)
	{
		std::cerr << "[" << client_address_ << "] CRITICAL ERROR in process_movement: " << e.what() << "\n";
		return on_session_end();
	}
	catch (...)
	{
		std::cerr << "[" << client_address_ << "] CRITICAL UNKNOWN ERROR in process_movement.\n";
		return on_session_end();
	}

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
	move_timer_.cancel();

	if (is_authenticated_ && account_id_ != 0)
	{
		// This is now safe. It calls the fast, non-blocking
		// save_character(), which queues the real save on the save_pool_
		save_character();
	}

	try {
		std::lock_guard<std::mutex> lock(g_player_registry_mutex);
		g_player_registry.erase(player_.userId);
	}
	catch (std::exception const& e) {
		std::cerr << "[" << client_address_ << "] Exception during data cleanup: " << e.what() << "\n";
	}

	try {
		std::lock_guard<std::mutex> lock(g_session_registry_mutex);
		g_session_registry.erase(player_.userId);
	}
	catch (std::exception const& e) {
		std::cerr << "[" << client_address_ << "] Exception during session cleanup: " << e.what() << "\n";
	}

	std::cout << "[" << client_address_ << "] Client disconnected.\n";
}

/**
 * @brief Sends a non-blocking shutdown warning to the client.
 */
void AsyncSession::send_shutdown_warning(int seconds)
{
	// Now uses our safe, non-blocking send() function
	send("SERVER:SHUTDOWN:" + std::to_string(seconds));
}

/**
 * @brief Posts a disconnect operation to the session's strand.
 */
void AsyncSession::disconnect()
{
	net::dispatch(ws_.get_executor(),
		[self = shared_from_this()]()
		{
			beast::error_code ec;
			self->ws_.close(websocket::close_code::service_restart, ec);
			// This will trigger on_session_end()
		});
}