// ==========================================
// File: server_main.cpp
// Description: Entry point for the C++ MMORPG server.
// Handles networking, DB connections, autosaving, and graceful shutdown.
// ==========================================

#include "AsyncSession.hpp"
#include "GameData.hpp"
#include "Items.hpp"
#include "DatabaseManager.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <sodium.h>

namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

// ==========================================
// Listener Class
// ==========================================f
class listener : public std::enable_shared_from_this<listener>
{
	net::io_context& ioc_;
	tcp::acceptor acceptor_;
	std::shared_ptr<DatabaseManager> db_manager_;

public:
	listener(net::io_context& ioc, tcp::endpoint endpoint, std::shared_ptr<DatabaseManager> db_manager)
		: ioc_(ioc), acceptor_(ioc), db_manager_(std::move(db_manager))
	{
		boost::system::error_code ec;

		acceptor_.open(endpoint.protocol(), ec);
		if (ec) throw std::runtime_error("Listener open: " + ec.message());

		acceptor_.set_option(net::socket_base::reuse_address(true), ec);
		if (ec) throw std::runtime_error("Listener set_option: " + ec.message());

		acceptor_.bind(endpoint, ec);
		if (ec) throw std::runtime_error("Listener bind: " + ec.message());

		acceptor_.listen(net::socket_base::max_listen_connections, ec);
		if (ec) throw std::runtime_error("Listener listen: " + ec.message());
	}

	void run() { do_accept(); }

	void stop()
	{
		net::dispatch(acceptor_.get_executor(), [this]() {
			boost::system::error_code ec;
			acceptor_.close(ec);
			});
	}

private:
	void do_accept()
	{
		acceptor_.async_accept(
			net::make_strand(ioc_),
			[self = shared_from_this()](boost::system::error_code ec, tcp::socket socket)
			{
				if (!ec)
				{
					std::make_shared<AsyncSession>(
						std::move(socket),
						self->db_manager_
					)->run();
				}
				else
				{
					std::cerr << "[ACCEPT ERROR] " << ec.message() << std::endl;
				}

				self->do_accept();
			});
	}
};

// ==========================================
// Batch Save Timer (Auto-Save System)
// ==========================================
void run_batch_save_timer(net::steady_timer& timer, int interval_seconds)
{
	timer.expires_after(std::chrono::seconds(interval_seconds));

	timer.async_wait([&timer, interval_seconds](const boost::system::error_code& ec)
		{
			if (ec)
			{
				if (ec != net::error::operation_aborted)
					std::cerr << "[BATCH SAVE TIMER ERROR] " << ec.message() << std::endl;
				return;
			}

			auto start_time = std::chrono::steady_clock::now();
			std::cout << "\n--- [BATCH SAVE STARTED] ---" << std::endl;

			// Snapshot sessions
			std::vector<std::shared_ptr<AsyncSession>> sessions;
			{
				std::lock_guard<std::mutex> lock(g_session_registry_mutex);
				for (auto const& [id, weak_session] : g_session_registry)
					if (auto s = weak_session.lock())
						sessions.push_back(s);
			}

			// ASYNCHRONOUS DISPATCH
			// We dispatch each save task to the thread pool instead of
			// running them one-by-one on this timer thread.

			std::cout << "[BATCH SAVE] Dispatching save tasks for " << sessions.size() << " players..." << std::endl;

			for (auto& s : sessions)
			{
				// Post the save_character function to that session's own strand
				// (which is tied to its websocket). This lets all saves
				// run in parallel on your io_context's thread pool.
				net::dispatch(s->getWebSocket().get_executor(), [s]() {
					try {
						// This code now runs on a worker thread, not the timer thread.
						s->save_character();
					}
					catch (const std::exception& e) {
						// This error is now caught on the background thread
						std::cerr << "[BATCH SAVE ERROR] Failed to save for a session: " << e.what() << std::endl;
					}
					});
			}

			// --- The blocking loop has been removed ---
			/*
			 int saved = 0;
			 for (auto& s : sessions)
			 {
				 try {
					 s->save_character(); // <-- THIS WAS BLOCKING
					 ++saved;
				 }
				 // ...
			 }
			*/

			auto end_time = std::chrono::steady_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

			// The timer handler now finishes almost instantly.
			// The saves will complete in the background.
			std::cout << "[BATCH SAVE DISPATCH COMPLETE] Dispatched " << sessions.size()
				<< " save tasks in " << duration_ms << " ms." << std::endl;
			std::cout << "--- [BATCH SAVE END] ---\n" << std::endl;

			// Re-arm timer
			run_batch_save_timer(timer, interval_seconds);
		});
}

// ==========================================
// Main Entry Point
// ==========================================
int main()
{
	const std::string connection_string =
		"postgres://postgres.iowmzljtduxacxlqyfdn:4rTBejGo9Ax2jAoi@aws-1-us-east-1.pooler.supabase.com:6543/postgres?sslmode=require";

	const auto address = net::ip::make_address("0.0.0.0");
	const unsigned short port = 8080;
	net::io_context ioc;
	net::steady_timer save_timer(ioc);

	try {
		// --- Database Initialization ---
		auto db_manager = std::make_shared<DatabaseManager>(connection_string);
		std::cout << "Database connected successfully.\n";

		// --- Crypto ---
		if (sodium_init() < 0)
			throw std::runtime_error("Libsodium failed to initialize!");
		std::cout << "Libsodium initialized successfully.\n";

		// --- Game Systems Initialization ---
		initializeAreas();
		initialize_skill_definitions();
		initialize_item_prices();
		initialize_suffix_pools();
		initialize_random_effect_pool();
		initialize_item_database();
		/*  not using this anymore, went with postgres sequencing, atomic legit wasnt safe enough  on server crashes   initialize_item_id_counter(*db_manager);*/

		// --- Listener ---
		auto listener_ptr = std::make_shared<listener>(
			ioc, tcp::endpoint{ address, port }, db_manager);
		listener_ptr->run();

		// --- Batch Auto-Save ---
		const int SAVE_INTERVAL_SECONDS = 360; // every 6 minutes
		run_batch_save_timer(save_timer, SAVE_INTERVAL_SECONDS);

		std::cout << "Server is listening on port " << port << "...\n";
		std::cout << "Type 'exit' or 'shutdown' to stop the server.\n";

		// --- Console Command Thread ---
		std::thread console_thread([&ioc, listener_ptr]() {
			std::string command;
			while (std::getline(std::cin, command))
			{
				if (command == "exit" || command == "shutdown")
				{
					std::cout << "\n--- SHUTDOWN INITIATED ---" << std::endl;
					listener_ptr->stop();

					const int grace_period = 30;
					{
						std::lock_guard<std::mutex> lock(g_session_registry_mutex);
						std::cout << "Broadcasting shutdown warning to "
							<< g_session_registry.size() << " clients.\n";

						for (auto const& [id, weak_session] : g_session_registry)
							if (auto session = weak_session.lock())
								session->send_shutdown_warning(grace_period);
					}

					// Graceful shutdown timer
					auto shutdown_timer = std::make_shared<net::steady_timer>(ioc);
					shutdown_timer->expires_after(std::chrono::seconds(grace_period));

					shutdown_timer->async_wait([&ioc, shutdown_timer](const boost::system::error_code& ec)
						{
							if (ec && ec != net::error::operation_aborted)
							{
								std::cerr << "[SHUTDOWN TIMER ERROR] " << ec.message() << std::endl;
								return;
							}

							std::cout << "--- Final save/disconnect phase ---" << std::endl;
							std::vector<std::shared_ptr<AsyncSession>> sessions;

							{
								std::lock_guard<std::mutex> lock(g_session_registry_mutex);
								for (auto const& [id, weak_session] : g_session_registry)
									if (auto s = weak_session.lock())
										sessions.push_back(s);
							}

							int count = 0;
							for (auto& s : sessions)
							{
								try {
									s->disconnect();
									++count;
								}
								catch (const std::exception& e) {
									std::cerr << "[DISCONNECT ERROR] " << e.what() << std::endl;
								}
							}

							std::cout << "Finalized disconnects for " << count << " players.\n";
							ioc.stop();
						});

					break;
				}
			}
			});

		// --- Thread Pool ---
		unsigned const threads = std::max<int>(1, std::thread::hardware_concurrency());
		std::vector<std::thread> thread_pool;
		thread_pool.reserve(threads - 1);
		for (unsigned i = 1; i < threads; ++i)
			thread_pool.emplace_back([&ioc] { ioc.run(); });

		ioc.run(); // main thread

		for (auto& t : thread_pool) t.join();
		console_thread.join();

	}
	catch (const std::exception& e) {
		std::cerr << "FATAL ERROR: " << e.what() << std::endl;
		return 1;
	}

	std::cout << "Server shut down cleanly.\n";
	return 0;
}
