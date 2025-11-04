// File: src/game_session.cpp
#include "game_session.hpp"
#include <iostream>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core.hpp>
#include <thread>
#include <vector>
#include <string>
#include <algorithm> 
#include <random>    
#include <utility>   // For std::pair

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- Server Data Definitions ---
static const std::vector<std::string> ALL_AREAS = {
    "FOREST", "CAVES", "RUINS", "SWAMP", "MOUNTAINS", "DESERT", "VOLCANO"
};

// Monster templates used to randomly populate an area
static const std::vector<std::pair<std::string, std::string>> MONSTER_TEMPLATES = {
    {"SLIME", "SLM"},
    {"GOBLIN", "GB"},
    {"WOLF", "WLF"},
    {"BAT", "BAT"},
    {"SKELETON", "SKL"},
    {"GIANT SPIDER", "SPDR"},
    {"ORC BRUTE", "ORC"}
};

// Global counter for unique monster IDs across the session
int global_monster_id_counter = 1;


// --- Helper Function to Send Areas ---
void send_available_areas(websocket::stream<tcp::socket>& ws) {
    // ... (unchanged area shuffling logic) ...
    std::vector<std::string> areas = ALL_AREAS;

    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::default_random_engine rng(seed);
    std::shuffle(areas.begin(), areas.end(), rng);

    int count = (std::rand() % 3) + 2;

    std::string area_list;
    for (int i = 0; i < std::min((size_t)count, areas.size()); ++i) {
        if (!area_list.empty()) {
            area_list += ",";
        }
        area_list += areas[i];
    }

    std::string response = "SERVER:AREAS:" + area_list;
    ws.write(net::buffer(response));
}

// Helper Function to Generate and Send Monsters
void generate_and_send_monsters(websocket::stream<tcp::socket>& ws, PlayerState& player) {
    player.currentMonsters.clear();

    // Generate 2 to 4 monsters
    int monster_count = (std::rand() % 3) + 2;
    std::string json_monsters = "[";

    for (int i = 0; i < monster_count; ++i) {
        // Pick a random template
        int template_index = std::rand() % MONSTER_TEMPLATES.size();
        const auto& template_pair = MONSTER_TEMPLATES[template_index];

        MonsterState monster;
        monster.id = global_monster_id_counter++; // Assign global ID
        monster.type = template_pair.first;
        monster.assetKey = template_pair.second;
        player.currentMonsters.push_back(monster);

        // Build JSON string segment (Manual JSON construction)
        if (i > 0) json_monsters += ",";
        json_monsters += "{\"id\":" + std::to_string(monster.id) +
            ",\"type\":\"" + monster.type +
            "\",\"asset\":\"" + monster.assetKey + "\"}";
    }
    json_monsters += "]";

    std::string response = "SERVER:MONSTERS:" + json_monsters;
    ws.write(net::buffer(response));
}


// dis handles a client session
void do_session(tcp::socket socket) {
    std::string client_address = socket.remote_endpoint().address().to_string();

    try {
        websocket::stream<tcp::socket> ws{ std::move(socket) };

        std::cout << "[" << client_address << "] Attempting handshake...\n";

        ws.accept();

        std::cout << "[" << client_address << "] Handshake successful. Session started.\n";

        PlayerState player;
        player.userId = "Client_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        player.currentArea = "TOWN";

        std::string welcome = "SERVER:WELCOME! Enter SELECT_CLASS:FIGHTER or SELECT_CLASS:WIZARD";
        ws.write(net::buffer(welcome));

        beast::flat_buffer buffer;
        while (true) {
            ws.read(buffer);
            std::string message = beast::buffers_to_string(buffer.data());

            std::cout << "[" << client_address << "] Received: " << message << "\n";

            if (message.rfind("SELECT_CLASS:", 0) == 0) {
                // ... (Class Selection Logic - calls send_available_areas) ...
                std::string class_str = message.substr(13);

                if (class_str == "FIGHTER" || class_str == "WIZARD") {
                    if (class_str == "FIGHTER") {
                        player.currentClass = PlayerClass::FIGHTER;
                    }
                    else {
                        player.currentClass = PlayerClass::WIZARD;
                    }

                    std::cout << "[" << client_address << "] --- CLASS SET: " << class_str << " ---\n";

                    std::string response = "SERVER:CLASS_SET:" + class_str;
                    ws.write(net::buffer(response));

                    send_available_areas(ws);
                }
                else {
                    ws.write(net::buffer("SERVER:ERROR:Invalid class."));
                }
            }
            else if (message.rfind("GO_TO:", 0) == 0) {
                // Area Travel Logic
                std::string target_area = message.substr(6);

                if (target_area == "TOWN") {
                    player.currentArea = "TOWN";
                    player.currentMonsters.clear(); // Clear monsters when returning to town
                    std::string response = "SERVER:AREA_CHANGED:TOWN";
                    ws.write(net::buffer(response));

                    send_available_areas(ws);
                }
                else if (std::find(ALL_AREAS.begin(), ALL_AREAS.end(), target_area) != ALL_AREAS.end()) {
                    player.currentArea = target_area;
                    std::string response = "SERVER:AREA_CHANGED:" + target_area;
                    ws.write(net::buffer(response));

                    // NEW: Generate and send monsters when entering a non-TOWN area
                    generate_and_send_monsters(ws, player);
                }
                else {
                    ws.write(net::buffer("SERVER:ERROR:Invalid or unknown travel destination."));
                }
                std::cout << "[" << client_address << "] --- AREA CHANGED TO: " << player.currentArea << " ---\n";
            }
            else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
                // NEW: Handle monster selection
                if (player.currentArea == "TOWN") {
                    ws.write(net::buffer("SERVER:STATUS:No monsters to fight in TOWN."));
                    buffer.consume(buffer.size());
                    continue;
                }

                try {
                    int selected_id = std::stoi(message.substr(17));

                    auto it = std::find_if(player.currentMonsters.begin(), player.currentMonsters.end(),
                        [selected_id](const MonsterState& m) { return m.id == selected_id; });

                    if (it != player.currentMonsters.end()) {
                        std::string response = "SERVER:MONSTER_ACTION:You targeted the " + it->type + " (ID: " + std::to_string(selected_id) + "). Moving to combat phase...";
                        ws.write(net::buffer(response));
                        // In a real game, this is where combat logic would begin.
                    }
                    else {
                        ws.write(net::buffer("SERVER:ERROR:Selected monster ID not found in this area."));
                    }
                }
                catch (const std::exception& e) {
                    ws.write(net::buffer("SERVER:ERROR:Invalid monster ID format."));
                }

            }
            else {
                // Echo logic with player status (Updated to include Area)
                std::string class_name = (player.currentClass == PlayerClass::FIGHTER) ? "FIGHTER" :
                    (player.currentClass == PlayerClass::WIZARD) ? "WIZARD" : "UNSELECTED";

                std::string echo = "SERVER:ECHO[" + player.userId + "][" + class_name + "][" + player.currentArea + "]: " + message;
                ws.write(net::buffer(echo));
            }

            buffer.consume(buffer.size());
        }
    }
    catch (beast::system_error const& se) {
        if (se.code() != websocket::error::closed) {
            std::cerr << "[" << client_address << "] Session Error: " << se.code().message() << "\n";
        }
    }
    catch (std::exception const& e) {
        std::cerr << "[" << client_address << "] Exception: " << e.what() << "\n";
    }

    std::cout << "[" << client_address << "] Client disconnected.\n";
}