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
#include "ThreadPool.hpp" 
namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;
Point find_random_spawn_point(const AreaData& area);
void broadcast_monster_spawn(const std::string& areaName, const LiveMonster& monster);
// ==========================================
// Listener Class
// ==========================================f
class listener : public std::enable_shared_from_this<listener>
{
	net::io_context& ioc_;
	tcp::acceptor acceptor_;
	std::shared_ptr<DatabaseManager> db_manager_;
	std::shared_ptr<ThreadPool> db_pool_;
	std::shared_ptr<ThreadPool> save_pool_;

public:
	listener(net::io_context& ioc, tcp::endpoint endpoint, std::shared_ptr<DatabaseManager> db_manager, std::shared_ptr<ThreadPool> db_pool, std::shared_ptr<ThreadPool> save_pool)
		: ioc_(ioc), acceptor_(ioc), db_manager_(std::move(db_manager)), db_pool_(std::move(db_pool)), save_pool_(std::move(save_pool))
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
						self->db_manager_,
						self->db_pool_,
						self->save_pool_
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
void run_monster_tick_timer(net::steady_timer& timer, int interval_ms)
{
	timer.expires_after(std::chrono::milliseconds(interval_ms));

	timer.async_wait([&timer, interval_ms](const boost::system::error_code& ec)
		{
			if (ec)
			{
				if (ec != net::error::operation_aborted)
					std::cerr << "[MONSTER TICK TIMER ERROR] " << ec.message() << std::endl;
				return;
			}

			auto now = std::chrono::steady_clock::now();


			// We only need to know *which* areas had spawns, not the monsters themselves
			std::set<std::string> areas_to_update;


			// Iterate over all game areas
			for (auto& area_pair : g_areas)
			{
				AreaData& area = area_pair.second;
				if (area.live_monsters.empty()) continue; // Skip areas with no monsters

				// Lock this specific area's monster list
				std::lock_guard<std::mutex> lock(area.monster_mutex);
				bool monster_respawned_in_this_area = false; // Flag

				for (auto& monster_pair : area.live_monsters)
				{
					LiveMonster& lm = monster_pair.second;

					// Check if it's dead AND its respawn time has passed
					if (!lm.is_alive && now >= lm.respawn_time)
					{
						// --- Respawn it! ---
						lm.is_alive = true;
						lm.respawn_time = std::chrono::steady_clock::time_point::max();
						lm.position = lm.original_spawn_point;

						monster_respawned_in_this_area = true;
					}
				}

				if (monster_respawned_in_this_area) {
					areas_to_update.insert(area.name);
				}
			} // All mutexes are released here

			// --- MODIFICATION ---
			// Now, broadcast the full list for each area that had a change
			for (const std::string& areaName : areas_to_update)
			{
				broadcast_monster_list(areaName);
			}
			// --- END MODIFICATION ---

			// Re-arm the timer
			run_monster_tick_timer(timer, interval_ms);
		});
}
void run_combat_timer(net::steady_timer& timer) {
	// Check every 1 second (1000ms)
	timer.expires_after(std::chrono::milliseconds(1000));

	timer.async_wait([&timer](const boost::system::error_code& ec) {
		if (ec) return;

		// Run the check
		check_party_timeouts();

		// Re-arm
		run_combat_timer(timer);
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
	net::steady_timer monster_tick_timer(ioc);
	net::steady_timer combat_timer(ioc);
	auto db_pool = std::make_shared<ThreadPool>(4);
	auto save_pool = std::make_shared<ThreadPool>(1);

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
		initialize_monster_spell_definitions();
		initialize_item_prices();
		initialize_suffix_pools();
		initialize_random_effect_pool();
		initialize_item_database();
		/*  not using this anymore, went with postgres sequencing, atomic legit wasnt safe enough  on server crashes   initialize_item_id_counter(*db_manager);*/

		// --- Listener ---
		auto listener_ptr = std::make_shared<listener>(
			ioc, tcp::endpoint{ address, port }, db_manager, db_pool, save_pool);
		listener_ptr->run();

		// --- Batch Auto-Save ---
		const int SAVE_INTERVAL_SECONDS = 360; // every 6 minutes
		run_batch_save_timer(save_timer, SAVE_INTERVAL_SECONDS);
		run_monster_tick_timer(monster_tick_timer, 1000);
		run_combat_timer(combat_timer);
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
