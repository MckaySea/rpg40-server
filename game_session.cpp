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
#include <utility>
#include <sstream>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- Server Data Definitions ---
static const std::vector<std::string> ALL_AREAS = {
    "FOREST", "CAVES", "RUINS", "SWAMP", "MOUNTAINS", "DESERT", "VOLCANO"
};

static const std::vector<std::pair<std::string, std::string>> MONSTER_TEMPLATES = {
    {"SLIME", "SLM"},
    {"GOBLIN", "GB"},
    {"WOLF", "WLF"},
    {"BAT", "BAT"},
    {"SKELETON", "SKL"},
    {"GIANT SPIDER", "SPDR"},
    {"ORC BRUTE", "ORC"}
};

int global_monster_id_counter = 1;

// --- Class Starting Stats ---
PlayerStats getStartingStats(PlayerClass playerClass) {
    switch (playerClass) {
    case PlayerClass::FIGHTER:
        return PlayerStats(120, 20, 15, 12, 8);  // High HP, Attack, Defense
    case PlayerClass::WIZARD:
        return PlayerStats(80, 100, 8, 6, 10);   // High Mana, low physical stats
    case PlayerClass::ROGUE:
        return PlayerStats(90, 40, 12, 8, 15);   // Balanced, High Speed
    default:
        return PlayerStats(100, 50, 10, 10, 10); // Should never happen
    }
}

// --- Helper Functions ---
void send_available_areas(websocket::stream<tcp::socket>& ws) {
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

void generate_and_send_monsters(websocket::stream<tcp::socket>& ws, PlayerState& player) {
    player.currentMonsters.clear();
    int monster_count = (std::rand() % 3) + 2;
    std::string json_monsters = "[";

    for (int i = 0; i < monster_count; ++i) {
        int template_index = std::rand() % MONSTER_TEMPLATES.size();
        const auto& template_pair = MONSTER_TEMPLATES[template_index];

        MonsterState monster;
        monster.id = global_monster_id_counter++;
        monster.type = template_pair.first;
        monster.assetKey = template_pair.second;
        player.currentMonsters.push_back(monster);

        if (i > 0) json_monsters += ",";
        json_monsters += "{\"id\":" + std::to_string(monster.id) +
            ",\"type\":\"" + monster.type +
            "\",\"asset\":\"" + monster.assetKey + "\"}";
    }
    json_monsters += "]";

    std::string response = "SERVER:MONSTERS:" + json_monsters;
    ws.write(net::buffer(response));
}

// Send current player stats to client
void send_player_stats(websocket::stream<tcp::socket>& ws, const PlayerState& player) {
    std::ostringstream oss;
    oss << "SERVER:STATS:"
        << "{\"health\":" << player.stats.health
        << ",\"mana\":" << player.stats.mana
        << ",\"attack\":" << player.stats.attack
        << ",\"defense\":" << player.stats.defense
        << ",\"speed\":" << player.stats.speed
        << ",\"availableSkillPoints\":" << player.availableSkillPoints
        << "}";

    ws.write(net::buffer(oss.str()));
}

// --- Main Session Handler ---
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

        std::string welcome = "SERVER:WELCOME! Please enter your character name using SET_NAME:YourName";
        ws.write(net::buffer(welcome));

        beast::flat_buffer buffer;
        while (true) {
            ws.read(buffer);
            std::string message = beast::buffers_to_string(buffer.data());
            std::cout << "[" << client_address << "] Received: " << message << "\n";

            // Step 1: Set Player Name
            if (message.rfind("SET_NAME:", 0) == 0 && player.playerName.empty()) {
                std::string name = message.substr(9);

                // Validate name (basic validation)
                if (name.length() < 2 || name.length() > 20) {
                    ws.write(net::buffer("SERVER:ERROR:Name must be between 2 and 20 characters."));
                }
                else {
                    player.playerName = name;
                    std::string response = "SERVER:NAME_SET:" + name;
                    ws.write(net::buffer(response));

                    std::string class_prompt = "SERVER:PROMPT:Welcome " + name + "! Choose your class: SELECT_CLASS:FIGHTER, SELECT_CLASS:WIZARD, or SELECT_CLASS:ROGUE";
                    ws.write(net::buffer(class_prompt));

                    std::cout << "[" << client_address << "] --- NAME SET: " << name << " ---\n";
                }
            }
            // Step 2: Select Class
            else if (message.rfind("SELECT_CLASS:", 0) == 0 && player.currentClass == PlayerClass::UNSELECTED) {
                if (player.playerName.empty()) {
                    ws.write(net::buffer("SERVER:ERROR:You must set your name first using SET_NAME:YourName"));
                    buffer.consume(buffer.size());
                    continue;
                }

                std::string class_str = message.substr(13);

                if (class_str == "FIGHTER") {
                    player.currentClass = PlayerClass::FIGHTER;
                }
                else if (class_str == "WIZARD") {
                    player.currentClass = PlayerClass::WIZARD;
                }
                else if (class_str == "ROGUE") {
                    player.currentClass = PlayerClass::ROGUE;
                }
                else {
                    ws.write(net::buffer("SERVER:ERROR:Invalid class. Choose FIGHTER, WIZARD, or ROGUE."));
                    buffer.consume(buffer.size());
                    continue;
                }

                // Apply starting stats
                player.stats = getStartingStats(player.currentClass);
                player.availableSkillPoints = 3;
                player.hasSpentInitialPoints = false;

                std::cout << "[" << client_address << "] --- CLASS SET: " << class_str << " ---\n";

                std::string response = "SERVER:CLASS_SET:" + class_str;
                ws.write(net::buffer(response));

                // Send initial stats
                send_player_stats(ws, player);

                // Prompt for skill point allocation
                std::string prompt = "SERVER:PROMPT:You have 3 skill points to distribute. Use UPGRADE_STAT:stat_name to spend points. Available stats: health, mana, attack, defense, speed";
                ws.write(net::buffer(prompt));
            }
            // Step 3: Upgrade Stats
            else if (message.rfind("UPGRADE_STAT:", 0) == 0) {
                if (player.currentClass == PlayerClass::UNSELECTED) {
                    ws.write(net::buffer("SERVER:ERROR:You must select a class first."));
                    buffer.consume(buffer.size());
                    continue;
                }

                if (player.availableSkillPoints <= 0) {
                    ws.write(net::buffer("SERVER:ERROR:You have no skill points available."));
                    buffer.consume(buffer.size());
                    continue;
                }

                std::string stat_name = message.substr(13);
                bool valid_stat = false;

                if (stat_name == "health") {
                    player.stats.health += 5;
                    valid_stat = true;
                }
                else if (stat_name == "mana") {
                    player.stats.mana += 5;
                    valid_stat = true;
                }
                else if (stat_name == "attack") {
                    player.stats.attack += 1;
                    valid_stat = true;
                }
                else if (stat_name == "defense") {
                    player.stats.defense += 1;
                    valid_stat = true;
                }
                else if (stat_name == "speed") {
                    player.stats.speed += 1;
                    valid_stat = true;
                }

                if (valid_stat) {
                    player.availableSkillPoints--;

                    std::string response = "SERVER:STAT_UPGRADED:" + stat_name;
                    ws.write(net::buffer(response));

                    // Send updated stats
                    send_player_stats(ws, player);

                    // Check if points are all spent
                    if (player.availableSkillPoints == 0 && !player.hasSpentInitialPoints) {
                        player.hasSpentInitialPoints = true;
                        player.isFullyInitialized = true;

                        std::string complete = "SERVER:CHARACTER_COMPLETE:Character creation complete! You can now explore.";
                        ws.write(net::buffer(complete));

                        send_available_areas(ws);
                    }
                    else {
                        std::string remaining = "SERVER:PROMPT:You have " + std::to_string(player.availableSkillPoints) + " skill points remaining.";
                        ws.write(net::buffer(remaining));
                    }
                }
                else {
                    ws.write(net::buffer("SERVER:ERROR:Invalid stat name. Choose: health, mana, attack, defense, or speed"));
                }
            }
            // Area Travel (only if fully initialized)
            else if (message.rfind("GO_TO:", 0) == 0) {
                if (!player.isFullyInitialized) {
                    ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
                    buffer.consume(buffer.size());
                    continue;
                }

                std::string target_area = message.substr(6);

                if (target_area == "TOWN") {
                    player.currentArea = "TOWN";
                    player.currentMonsters.clear();
                    std::string response = "SERVER:AREA_CHANGED:TOWN";
                    ws.write(net::buffer(response));
                    send_available_areas(ws);
                }
                else if (std::find(ALL_AREAS.begin(), ALL_AREAS.end(), target_area) != ALL_AREAS.end()) {
                    player.currentArea = target_area;
                    std::string response = "SERVER:AREA_CHANGED:" + target_area;
                    ws.write(net::buffer(response));
                    generate_and_send_monsters(ws, player);
                }
                else {
                    ws.write(net::buffer("SERVER:ERROR:Invalid or unknown travel destination."));
                }

                std::cout << "[" << client_address << "] --- AREA CHANGED TO: " << player.currentArea << " ---\n";
            }
            // Monster Selection
            else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
                if (!player.isFullyInitialized) {
                    ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
                    buffer.consume(buffer.size());
                    continue;
                }

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
                        std::string response = "SERVER:MONSTER_ACTION:You targeted the " + it->type +
                            " (ID: " + std::to_string(selected_id) + "). Moving to combat phase...";
                        ws.write(net::buffer(response));
                    }
                    else {
                        ws.write(net::buffer("SERVER:ERROR:Selected monster ID not found in this area."));
                    }
                }
                catch (const std::exception&) {
                    ws.write(net::buffer("SERVER:ERROR:Invalid monster ID format."));
                }
            }
            // Echo/Debug
            else {
                std::string class_name = (player.currentClass == PlayerClass::FIGHTER) ? "FIGHTER" :
                    (player.currentClass == PlayerClass::WIZARD) ? "WIZARD" :
                    (player.currentClass == PlayerClass::ROGUE) ? "ROGUE" : "UNSELECTED";

                std::string echo = "SERVER:ECHO[" + player.playerName + "][" + class_name + "][" + player.currentArea + "]: " + message;
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