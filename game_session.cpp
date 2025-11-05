// File: LocalWebSocketServer/game_session.cpp
//
// Additions:
// 1. ... (previous additions)
// 7. ADDED: A* Pathfinding and non-blocking game loop for server-side movement.
// 8. FIXED: Corrected the websocket timeout option to fix compile errors.
//
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
#include <map>
#include <mutex>
#include <memory>
#include <deque> // ADDED
#include <chrono> // ADDED
#include <cmath> // ADDED
#include <set> // ADDED
#include <queue> // ADDED

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- Server Data Definitions ---
static const std::vector<std::string> ALL_AREAS = {
    "FOREST", "CAVES", "RUINS", "SWAMP", "MOUNTAINS", "DESERT", "VOLCANO"
};

// --- MODIFIED: Grid definitions ---
static const int GRID_COLS = 40;
static const int GRID_ROWS = 22;
static const std::chrono::milliseconds MOVEMENT_DELAY{ 150 }; // Speed: ms per tile
static const int SERVER_TICK_RATE_MS = 50; // 20 ticks per second

// ADDED: Town Grid Map (0 = walkable, 1 = obstacle)
static const std::vector<std::vector<int>> TOWN_GRID = {
    //   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 0
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 1
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 2
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 3
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 4
        {0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0}, // 5 (Top of buildings)
        {0,0,0,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0}, // 6
        {0,0,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0}, // 7
        {0,0,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0}, // 8
        {0,0,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0}, // 9
        {0,0,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0}, // 10
        {0,0,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0}, // 11
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 12 (Path)
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 13 (Path)
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 14 (Path)
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 15 (Path)
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 16
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 17
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 18
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 19
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // 20
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}  // 21
};


// Monster Templates (Type, AssetKey)
static const std::map<std::string, std::string> MONSTER_ASSETS = {
    {"SLIME", "SLM"}, {"GOBLIN", "GB"}, {"WOLF", "WLF"}, {"BAT", "BAT"},
    {"SKELETON", "SKL"}, {"GIANT SPIDER", "SPDR"}, {"ORC BRUTE", "ORC"}
};

// ADDED: Monster Base Stats (Type -> Stats)
static const std::map<std::string, MonsterInstance> MONSTER_TEMPLATES = {
    {"SLIME", MonsterInstance(0, "", "", 30, 8, 5, 5, 10)},
    {"GOBLIN", MonsterInstance(0, "", "", 50, 12, 8, 8, 15)},
    {"WOLF", MonsterInstance(0, "", "", 40, 15, 6, 12, 12)},
    {"BAT", MonsterInstance(0, "", "", 20, 10, 4, 15, 8)},
    {"SKELETON", MonsterInstance(0, "", "", 60, 14, 10, 6, 20)},
    {"GIANT SPIDER", MonsterInstance(0, "", "", 70, 16, 8, 10, 25)},
    {"ORC BRUTE", MonsterInstance(0, "", "", 100, 20, 12, 5, 40)}
};
static const std::vector<std::string> MONSTER_KEYS = { "SLIME", "GOBLIN", "WOLF", "BAT", "SKELETON", "GIANT SPIDER", "ORC BRUTE" };


int global_monster_id_counter = 1;

// --- ADDED: Multiplayer Registry ---
static std::map<std::string, PlayerBroadcastData> g_player_registry;
static std::mutex g_player_registry_mutex;


// --- Class Starting Stats ---
PlayerStats getStartingStats(PlayerClass playerClass) {
    switch (playerClass) {
    case PlayerClass::FIGHTER: return PlayerStats(120, 20, 15, 12, 8);
    case PlayerClass::WIZARD: return PlayerStats(80, 100, 8, 6, 10);
    case PlayerClass::ROGUE: return PlayerStats(90, 40, 12, 8, 15);
    default: return PlayerStats(100, 50, 10, 10, 10);
    }
}

// ADDED: Create a monster instance from a template
MonsterInstance create_monster(int id, std::string type) {
    MonsterInstance monster = MONSTER_TEMPLATES.at(type);
    monster.id = id;
    monster.type = type;
    monster.assetKey = MONSTER_ASSETS.at(type);
    return monster;
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
        if (!area_list.empty()) area_list += ",";
        area_list += areas[i];
    }
    std::string response = "SERVER:AREAS:" + area_list;
    ws.write(net::buffer(response));
}

void send_current_monsters_list(websocket::stream<tcp::socket>& ws, PlayerState& player) {
    std::string json_monsters = "[";
    for (size_t i = 0; i < player.currentMonsters.size(); ++i) {
        const auto& monster = player.currentMonsters[i];
        if (i > 0) json_monsters += ",";
        json_monsters += "{\"id\":" + std::to_string(monster.id) +
            ",\"type\":\"" + monster.type +
            "\",\"asset\":\"" + monster.assetKey + "\"}";
    }
    json_monsters += "]";
    std::string response = "SERVER:MONSTERS:" + json_monsters;
    ws.write(net::buffer(response));
}

void generate_and_send_monsters(websocket::stream<tcp::socket>& ws, PlayerState& player) {
    player.currentMonsters.clear();
    int monster_count = (std::rand() % 3) + 2;
    for (int i = 0; i < monster_count; ++i) {
        int template_index = std::rand() % MONSTER_KEYS.size();
        std::string key = MONSTER_KEYS[template_index];
        MonsterState monster;
        monster.id = global_monster_id_counter++;
        monster.type = key;
        monster.assetKey = MONSTER_ASSETS.at(key);
        player.currentMonsters.push_back(monster);
    }
    send_current_monsters_list(ws, player);
}

// Send current player stats to client
void send_player_stats(websocket::stream<tcp::socket>& ws, const PlayerState& player) {
    std::ostringstream oss;
    oss << "SERVER:STATS:"
        << "{\"playerName\":\"" << player.playerName << "\""
        << ",\"health\":" << player.stats.health
        << ",\"maxHealth\":" << player.stats.maxHealth
        << ",\"mana\":" << player.stats.mana
        << ",\"maxMana\":" << player.stats.maxMana
        << ",\"attack\":" << player.stats.attack
        << ",\"defense\":" << player.stats.defense
        << ",\"speed\":" << player.stats.speed
        << ",\"level\":" << player.stats.level
        << ",\"experience\":" << player.stats.experience
        << ",\"experienceToNextLevel\":" << player.stats.experienceToNextLevel
        << ",\"availableSkillPoints\":" << player.availableSkillPoints
        << ",\"posX\":" << player.posX
        << ",\"posY\":" << player.posY;
    if (player.currentClass == PlayerClass::WIZARD && !player.spells.empty()) {
        oss << ",\"spells\":[";
        for (size_t i = 0; i < player.spells.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "\"" << player.spells[i] << "\"";
        }
        oss << "]";
    }
    else {
        oss << ",\"spells\":[]";
    }
    oss << "}";
    ws.write(net::buffer(oss.str()));
}

// --- Level Up Handler ---
void check_for_level_up(websocket::stream<tcp::socket>& ws, PlayerState& player) {
    while (player.stats.experience >= player.stats.experienceToNextLevel) {
        player.stats.level++;
        player.stats.experience -= player.stats.experienceToNextLevel;
        player.stats.experienceToNextLevel = static_cast<int>(player.stats.experienceToNextLevel * 1.5);

        player.availableSkillPoints += 3;
        player.stats.maxHealth += 10;
        player.stats.health = player.stats.maxHealth;
        player.stats.maxMana += 5;
        player.stats.mana = player.stats.maxMana;
        player.stats.attack += 2;
        player.stats.defense += 1;
        player.stats.speed += 1;
        std::cout << "[Level Up] Player " << player.playerName << " reached level " << player.stats.level << "\n";
        std::string level_msg = "SERVER:LEVEL_UP:You have reached level " + std::to_string(player.stats.level) + "! You feel stronger!";
        ws.write(net::buffer(level_msg));
        std::string prompt_msg = "SERVER:PROMPT:You have " + std::to_string(player.availableSkillPoints) + " new skill points to spend.";
        ws.write(net::buffer(prompt_msg));
    }
}


// --- ADDED: A* Pathfinding Implementation ---
struct Node {
    Point pos;
    int g, h, f; // g = cost, h = heuristic, f = g + h
    const Node* parent;

    Node(Point p, int g_val, int h_val, const Node* p_val)
        : pos(p), g(g_val), h(h_val), f(g_val + h_val), parent(p_val) {
    }

    // Comparison for priority queue
    bool operator>(const Node& other) const {
        return f > other.f;
    }
};

bool is_valid(int x, int y) {
    return x >= 0 && x < GRID_COLS && y >= 0 && y < GRID_ROWS;
}

bool is_walkable(int x, int y) {
    if (!is_valid(x, y)) return false;
    return TOWN_GRID[y][x] == 0;
}

int calculate_heuristic(Point a, Point b) {
    // Manhattan distance
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

// Main A* function
std::deque<Point> A_Star_Search(Point start, Point end) {
    std::deque<Point> path;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open_list;
    // Using std::set for fast "contains" check
    std::set<std::pair<int, int>> closed_list;
    std::vector<std::unique_ptr<Node>> node_storage; // To keep nodes alive

    if (!is_walkable(end.x, end.y)) {
        return path; // Invalid destination
    }

    auto start_node = std::make_unique<Node>(start, 0, calculate_heuristic(start, end), nullptr);
    open_list.push(*start_node);
    node_storage.push_back(std::move(start_node));

    const int D = 1; // Cost for cardinal directions

    // 4-directional movement (no diagonals)
    int dx[] = { 0, 0, 1, -1 };
    int dy[] = { 1, -1, 0, 0 };
    int num_directions = 4;

    while (!open_list.empty()) {
        Node current = open_list.top();
        open_list.pop();

        if (current.pos == end) {
            // Reconstruct path
            const Node* trace = &current;
            while (trace != nullptr) {
                path.push_front(trace->pos);
                trace = trace->parent;
            }
            path.pop_front(); // Remove starting position
            return path;
        }

        closed_list.insert({ current.pos.x, current.pos.y });

        for (int i = 0; i < num_directions; ++i) {
            Point next_pos = { current.pos.x + dx[i], current.pos.y + dy[i] };

            if (!is_walkable(next_pos.x, next_pos.y) ||
                closed_list.count({ next_pos.x, next_pos.y })) {
                continue;
            }

            int new_g = current.g + D;
            int new_h = calculate_heuristic(next_pos, end);

            // Find the node* that matches `current` in storage
            const Node* current_node_ptr = nullptr;
            for (auto it = node_storage.rbegin(); it != node_storage.rend(); ++it) {
                if ((*it)->pos == current.pos && (*it)->f == current.f) {
                    current_node_ptr = it->get();
                    break;
                }
            }

            // This check isn't strictly necessary for a grid but is good practice
            if (current_node_ptr == nullptr && current.pos == start) {
                for (auto it = node_storage.rbegin(); it != node_storage.rend(); ++it) {
                    if ((*it)->pos == start) {
                        current_node_ptr = it->get();
                        break;
                    }
                }
            }


            auto next_node = std::make_unique<Node>(next_pos, new_g, new_h, current_node_ptr);
            open_list.push(*next_node);
            node_storage.push_back(std::move(next_node));
        }
    }

    return path; // No path found
}
// --- END A* Implementation ---


// --- Main Session Handler (MODIFIED for Non-Blocking Tick Loop) ---
void do_session(tcp::socket socket) {
    std::string client_address = socket.remote_endpoint().address().to_string();
    std::string userId = "UNKNOWN";

    try {
        websocket::stream<tcp::socket> ws{ std::move(socket) };
        std::cout << "[" << client_address << "] Attempting handshake...\n";
        ws.accept();
        std::cout << "[" << client_address << "] Handshake successful. Session started.\n";

        // --- THE FIX: Set a timeout on blocking read operations ---
        // This allows us to have a "tick" loop without a fully non-blocking socket.
        websocket::stream_base::timeout options;
        options.idle_timeout = std::chrono::milliseconds(SERVER_TICK_RATE_MS);
        options.handshake_timeout = std::chrono::seconds(30); // Keep handshake timeout reasonable
        options.keep_alive_pings = false; // We aren't using pings

        ws.set_option(options);
        // --- END FIX ---

        PlayerState player;
        player.userId = "Client_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        userId = player.userId;
        player.currentArea = "TOWN";
        player.posX = 5;
        player.posY = 5;
        player.lastMoveTime = std::chrono::steady_clock::now(); // Init timer

        PlayerBroadcastData broadcastData;
        broadcastData.userId = player.userId;
        broadcastData.currentArea = "TOWN";
        broadcastData.posX = player.posX;
        broadcastData.posY = player.posY;

        {
            std::lock_guard<std::mutex> lock(g_player_registry_mutex);
            g_player_registry[player.userId] = broadcastData;
        }

        std::string welcome = "SERVER:WELCOME! Please enter your character name using SET_NAME:YourName";
        ws.write(net::buffer(welcome));

        beast::flat_buffer buffer;

        // --- MODIFIED: Non-blocking Game Loop ---
        while (true) {
            bool message_received = false;

            // --- 1. Read Network Messages (Blocking with Timeout) ---
            try {
                // This is now a *blocking* read, but it will time out
                ws.read(buffer);
                message_received = true;
            }
            catch (beast::system_error const& se) {
                // --- THE FIX: This is the error we expect on a timeout ---
                if (se.code() == beast::error::timeout) {
                    // No message received, just proceed to the tick logic
                    message_received = false;
                }
                // --- END FIX ---
                else {
                    throw; // Re-throw actual errors (like disconnect)
                }
            }

            // --- 2. Process Message (if one was received) ---
            if (message_received) {
                std::string message = beast::buffers_to_string(buffer.data());
                std::cout << "[" << client_address << "] Received: " << message << "\n";

                // --- Check for non-combat commands first ---
                if (message.rfind("SET_NAME:", 0) == 0 && player.playerName.empty()) {
                    std::string name = message.substr(9);
                    if (name.length() < 2 || name.length() > 20) {
                        ws.write(net::buffer("SERVER:ERROR:Name must be between 2 and 20 characters."));
                    }
                    else {
                        player.playerName = name;
                        broadcastData.playerName = name;
                        std::string response = "SERVER:NAME_SET:" + name;
                        ws.write(net::buffer(response));
                        std::string class_prompt = "SERVER:PROMPT:Welcome " + name + "! Choose your class: SELECT_CLASS:FIGHTER, SELECT_CLASS:WIZARD, or SELECT_CLASS:ROGUE";
                        ws.write(net::buffer(class_prompt));
                        std::cout << "[" << client_address << "] --- NAME SET: " << name << " ---\n";
                        { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcastData; }
                    }
                }
                else if (message.rfind("SELECT_CLASS:", 0) == 0 && player.currentClass == PlayerClass::UNSELECTED) {
                    std::string class_str = message.substr(13);
                    if (class_str == "FIGHTER") { player.currentClass = PlayerClass::FIGHTER; broadcastData.playerClass = player.currentClass; }
                    else if (class_str == "WIZARD") { player.currentClass = PlayerClass::WIZARD; broadcastData.playerClass = player.currentClass; player.spells = { "Fireball", "Lightning", "Freeze" }; }
                    else if (class_str == "ROGUE") { player.currentClass = PlayerClass::ROGUE; broadcastData.playerClass = player.currentClass; }
                    else { ws.write(net::buffer("SERVER:ERROR:Invalid class.")); buffer.consume(buffer.size()); continue; }
                    player.stats = getStartingStats(player.currentClass);
                    player.availableSkillPoints = 3;
                    player.hasSpentInitialPoints = false;
                    std::cout << "[" << client_address << "] --- CLASS SET: " << class_str << " ---\n";
                    std::string response = "SERVER:CLASS_SET:" + class_str;
                    ws.write(net::buffer(response));
                    send_player_stats(ws, player);
                    std::string prompt = "SERVER:PROMPT:You have 3 skill points to distribute. Use UPGRADE_STAT:stat_name to spend points.";
                    ws.write(net::buffer(prompt));
                    { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcastData; }
                }
                else if (message.rfind("UPGRADE_STAT:", 0) == 0) {
                    if (player.currentClass == PlayerClass::UNSELECTED) { ws.write(net::buffer("SERVER:ERROR:You must select a class first.")); }
                    else if (player.availableSkillPoints <= 0) { ws.write(net::buffer("SERVER:ERROR:You have no skill points available.")); }
                    else {
                        std::string stat_name = message.substr(13);
                        bool valid_stat = false;
                        if (stat_name == "health") { player.stats.maxHealth += 5; player.stats.health += 5; valid_stat = true; }
                        else if (stat_name == "mana") { player.stats.maxMana += 5; player.stats.mana += 5; valid_stat = true; }
                        else if (stat_name == "attack") { player.stats.attack += 1; valid_stat = true; }
                        else if (stat_name == "defense") { player.stats.defense += 1; valid_stat = true; }
                        else if (stat_name == "speed") { player.stats.speed += 1; valid_stat = true; }
                        if (valid_stat) {
                            player.availableSkillPoints--;
                            std::string response = "SERVER:STAT_UPGRADED:" + stat_name;
                            ws.write(net::buffer(response));
                            send_player_stats(ws, player);
                            if (player.availableSkillPoints == 0 && !player.hasSpentInitialPoints) {
                                player.hasSpentInitialPoints = true;
                                player.isFullyInitialized = true;
                                std::string complete = "SERVER:CHARACTER_COMPLETE:Character creation complete! You can now explore.";
                                ws.write(net::buffer(complete));
                                send_available_areas(ws);
                            }
                            else if (player.availableSkillPoints > 0) { ws.write(net::buffer("SERVER:PROMPT:You have " + std::to_string(player.availableSkillPoints) + " skill points remaining.")); }
                            else { ws.write(net::buffer("SERVER:STATUS:All skill points spent.")); }
                        }
                        else { ws.write(net::buffer("SERVER:ERROR:Invalid stat name.")); }
                    }
                }
                else if (message.rfind("GO_TO:", 0) == 0) {
                    if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
                    else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot travel while in combat!")); }
                    else {
                        std::string target_area = message.substr(6);
                        player.currentPath.clear(); // ADDED: Clear path on area change
                        broadcastData.currentArea = target_area;
                        { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcastData; }
                        if (target_area == "TOWN") {
                            player.currentArea = "TOWN";
                            player.currentMonsters.clear();
                            player.stats.health = player.stats.maxHealth; player.stats.mana = player.stats.maxMana;
                            std::string response = "SERVER:AREA_CHANGED:TOWN";
                            ws.write(net::buffer(response));
                            send_available_areas(ws);
                            send_player_stats(ws, player);
                        }
                        else if (std::find(ALL_AREAS.begin(), ALL_AREAS.end(), target_area) != ALL_AREAS.end()) {
                            player.currentArea = target_area;
                            std::string response = "SERVER:AREA_CHANGED:" + target_area;
                            ws.write(net::buffer(response));
                            generate_and_send_monsters(ws, player);
                        }
                        else {
                            ws.write(net::buffer("SERVER:ERROR:Invalid or unknown travel destination."));
                            broadcastData.currentArea = player.currentArea;
                            { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcastData; }
                        }
                        std::cout << "[" << client_address << "] --- AREA CHANGED TO: " << player.currentArea << " ---\n";
                    }
                }
                // --- MODIFIED: GRID MOVEMENT HANDLER ---
                else if (message.rfind("MOVE_TO:", 0) == 0) {
                    if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
                    else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot move while in combat!")); }
                    else if (player.currentArea != "TOWN") { ws.write(net::buffer("SERVER:ERROR:Grid movement is only available in TOWN.")); }
                    else {
                        try {
                            std::string coords_str = message.substr(8);
                            size_t comma_pos = coords_str.find(',');
                            if (comma_pos == std::string::npos) throw std::invalid_argument("Invalid coordinate format.");

                            int target_x = std::stoi(coords_str.substr(0, comma_pos));
                            int target_y = std::stoi(coords_str.substr(comma_pos + 1));

                            if (target_x < 0 || target_x >= GRID_COLS || target_y < 0 || target_y >= GRID_ROWS) {
                                ws.write(net::buffer("SERVER:ERROR:Target coordinates are out of bounds."));
                            }
                            else if (!is_walkable(target_x, target_y)) { // ADDED: Check walkability
                                ws.write(net::buffer("SERVER:ERROR:Cannot move to that location."));
                            }
                            else {
                                // --- THIS IS THE NEW LOGIC ---
                                // Find path from *current* position (or last path point)
                                Point start_pos;
                                if (player.currentPath.empty()) {
                                    start_pos = { player.posX, player.posY };
                                }
                                else {
                                    // Reroute from the *next* tile we were going to
                                    start_pos = player.currentPath.front();
                                }

                                Point end_pos = { target_x, target_y };

                                // Calculate the new path
                                player.currentPath = A_Star_Search(start_pos, end_pos);

                                // Set the move timer to now to allow immediate first step
                                player.lastMoveTime = std::chrono::steady_clock::now() - MOVEMENT_DELAY;
                            }
                        }
                        catch (const std::exception& e) {
                            std::cerr << "Error parsing MOVE_TO: " << e.what() << "\n";
                            ws.write(net::buffer("SERVER:ERROR:Invalid coordinate format."));
                        }
                    }
                }
                else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
                    if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
                    else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:You are already in combat!")); }
                    else if (player.currentArea == "TOWN") { ws.write(net::buffer("SERVER:STATUS:No monsters to fight in TOWN.")); }
                    else {
                        try {
                            int selected_id = std::stoi(message.substr(17));
                            auto it = std::find_if(player.currentMonsters.begin(), player.currentMonsters.end(), [selected_id](const MonsterState& m) { return m.id == selected_id; });
                            if (it != player.currentMonsters.end()) {
                                player.isInCombat = true;
                                player.currentOpponent = create_monster(it->id, it->type);
                                player.isDefending = false;
                                player.currentMonsters.erase(it);
                                std::cout << "[" << client_address << "] --- COMBAT STARTED vs " << player.currentOpponent->type << " ---\n";
                                std::ostringstream oss;
                                oss << "SERVER:COMBAT_START:"
                                    << "{\"id\":" << player.currentOpponent->id << ",\"type\":\"" << player.currentOpponent->type
                                    << "\",\"asset\":\"" << player.currentOpponent->assetKey << "\",\"health\":" << player.currentOpponent->health
                                    << ",\"maxHealth\":" << player.currentOpponent->maxHealth << "}";
                                ws.write(net::buffer(oss.str()));
                                ws.write(net::buffer("SERVER:COMBAT_LOG:You engaged the " + player.currentOpponent->type + "!"));
                                ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
                            }
                            else { ws.write(net::buffer("SERVER:ERROR:Selected monster ID not found.")); }
                        }
                        catch (const std::exception&) { ws.write(net::buffer("SERVER:ERROR:Invalid monster ID format.")); }
                    }
                }
                else if (message.rfind("COMBAT_ACTION:", 0) == 0) {
                    if (!player.isInCombat || !player.currentOpponent) { ws.write(net::buffer("SERVER:ERROR:You are not in combat.")); }
                    else {
                        std::string action_command = message.substr(14);
                        std::string action_type; std::string action_param;
                        size_t colon_pos = action_command.find(':');
                        if (colon_pos != std::string::npos) { action_type = action_command.substr(0, colon_pos); action_param = action_command.substr(colon_pos + 1); }
                        else { action_type = action_command; }

                        int player_damage = 0; int mana_cost = 0; bool fled = false;
                        if (action_type == "ATTACK") {
                            int base_damage = std::max(1, player.stats.attack - player.currentOpponent->defense);
                            float variance = 0.8f + ((float)(std::rand() % 41) / 100.0f);
                            player_damage = std::max(1, (int)(base_damage * variance));
                            ws.write(net::buffer("SERVER:COMBAT_LOG:You attack the " + player.currentOpponent->type + " for " + std::to_string(player_damage) + " damage!"));
                        }
                        else if (action_type == "SPELL") {
                            int base_damage = 0; mana_cost = 0; float variance = 1.0f;
                            if (action_param == "Fireball") { mana_cost = 20; if (player.stats.mana >= mana_cost) { base_damage = (player.stats.maxMana / 8) + player.stats.attack; variance = 0.8f + ((float)(std::rand() % 41) / 100.0f); } }
                            else if (action_param == "Lightning") { mana_cost = 15; if (player.stats.mana >= mana_cost) { base_damage = (player.stats.maxMana / 10) + player.stats.attack; variance = 0.7f + ((float)(std::rand() % 61) / 100.0f); } }
                            else if (action_param == "Freeze") { mana_cost = 10; if (player.stats.mana >= mana_cost) { base_damage = (player.stats.maxMana / 12) + (player.stats.attack / 2); variance = 0.9f + ((float)(std::rand() % 21) / 100.0f); } }
                            else { ws.write(net::buffer("SERVER:COMBAT_LOG:You don't know that spell!")); buffer.consume(buffer.size()); continue; }
                            if (mana_cost > 0 && player.stats.mana >= mana_cost) {
                                player.stats.mana -= mana_cost;
                                player_damage = std::max(1, (int)(base_damage * variance));
                                ws.write(net::buffer("SERVER:COMBAT_LOG:You cast " + action_param + " for " + std::to_string(player_damage) + " damage!"));
                            }
                            else if (mana_cost > 0) { ws.write(net::buffer("SERVER:COMBAT_LOG:Not enough mana to cast " + action_param + "! (Needs " + std::to_string(mana_cost) + ")")); }
                            else { ws.write(net::buffer("SERVER:COMBAT_LOG:Cannot cast " + action_param + ".")); }
                        }
                        else if (action_type == "DEFEND") { player.isDefending = true; ws.write(net::buffer("SERVER:COMBAT_LOG:You brace for the next attack.")); }
                        else if (action_type == "FLEE") {
                            float flee_chance = 0.5f + ((float)player.stats.speed - (float)player.currentOpponent->speed) * 0.05f;
                            if (flee_chance < 0.1f) flee_chance = 0.1f; if (flee_chance > 0.9f) flee_chance = 0.9f;
                            if (((float)std::rand() / RAND_MAX) < flee_chance) { fled = true; }
                            else { ws.write(net::buffer("SERVER:COMBAT_LOG:You failed to flee!")); }
                        }
                        if (fled) {
                            ws.write(net::buffer("SERVER:COMBAT_LOG:You successfully fled from the " + player.currentOpponent->type + "!"));
                            player.isInCombat = false; player.currentOpponent.reset();
                            ws.write(net::buffer("SERVER:COMBAT_VICTORY:Fled"));
                            send_current_monsters_list(ws, player); buffer.consume(buffer.size()); continue;
                        }
                        if (player_damage > 0) { player.currentOpponent->health -= player_damage; }
                        send_player_stats(ws, player);
                        ws.write(net::buffer("SERVER:COMBAT_UPDATE:" + std::to_string(player.currentOpponent->health)));
                        if (player.currentOpponent->health <= 0) {
                            ws.write(net::buffer("SERVER:COMBAT_LOG:You defeated the " + player.currentOpponent->type + "!"));
                            int xp_gain = player.currentOpponent->xpReward;
                            ws.write(net::buffer("SERVER:STATUS:Gained " + std::to_string(xp_gain) + " XP."));
                            player.stats.experience += xp_gain;
                            player.isInCombat = false; player.currentOpponent.reset();
                            ws.write(net::buffer("SERVER:COMBAT_VICTORY:Defeated"));
                            check_for_level_up(ws, player); send_player_stats(ws, player);
                            send_current_monsters_list(ws, player); buffer.consume(buffer.size()); continue;
                        }
                        int monster_damage = 0; int player_defense = player.stats.defense;
                        if (player.isDefending) { player_defense *= 2; player.isDefending = false; }
                        monster_damage = std::max(1, player.currentOpponent->attack - player_defense);
                        player.stats.health -= monster_damage;
                        ws.write(net::buffer("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " attacks you for " + std::to_string(monster_damage) + " damage!"));
                        send_player_stats(ws, player);
                        if (player.stats.health <= 0) {
                            player.stats.health = 0;
                            ws.write(net::buffer("SERVER:COMBAT_DEFEAT:You have been defeated!"));
                            player.isInCombat = false; player.currentOpponent.reset();
                            player.currentArea = "TOWN"; player.currentMonsters.clear();
                            player.stats.health = player.stats.maxHealth / 2; player.stats.mana = player.stats.maxMana;
                            player.posX = 5; player.posY = 5; player.currentPath.clear(); // ADDED: Clear path
                            broadcastData.currentArea = "TOWN"; broadcastData.posX = player.posX; broadcastData.posY = player.posY;
                            { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcastData; }
                            ws.write(net::buffer("SERVER:AREA_CHANGED:TOWN"));
                            send_available_areas(ws); send_player_stats(ws, player);
                            buffer.consume(buffer.size()); continue;
                        }
                        ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
                    }
                }
                else if (message.rfind("GIVE_XP:", 0) == 0) {
                    if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
                    else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot gain XP in combat.")); }
                    else {
                        try {
                            int xp_to_give = std::stoi(message.substr(8));
                            if (xp_to_give > 0) {
                                player.stats.experience += xp_to_give;
                                std::string response = "SERVER:STATUS:Gained " + std::to_string(xp_to_give) + " XP.";
                                ws.write(net::buffer(response));
                                check_for_level_up(ws, player);
                                send_player_stats(ws, player);
                            }
                            else { ws.write(net::buffer("SERVER:ERROR:Invalid XP amount.")); }
                        }
                        catch (const std::exception&) { ws.write(net::buffer("SERVER:ERROR:Invalid XP amount format.")); }
                    }
                }
                else if (message == "REQUEST_PLAYERS") {
                    if (player.currentArea == "TOWN") {
                        std::ostringstream oss;
                        oss << "SERVER:PLAYERS_IN_TOWN:[";
                        bool first_player = true;
                        {
                            std::lock_guard<std::mutex> lock(g_player_registry_mutex);
                            for (const auto& pair : g_player_registry) {
                                if (pair.first == player.userId) continue;
                                if (pair.second.currentArea == "TOWN" && pair.second.playerClass != PlayerClass::UNSELECTED) {
                                    if (!first_player) oss << ",";
                                    oss << "{\"id\":\"" << pair.second.userId
                                        << "\",\"name\":\"" << pair.second.playerName
                                        << "\",\"class\":" << static_cast<int>(pair.second.playerClass)
                                        << ",\"x\":" << pair.second.posX
                                        << ",\"y\":" << pair.second.posY
                                        << "}";
                                    first_player = false;
                                }
                            }
                        }
                        oss << "]";
                        ws.write(net::buffer(oss.str()));
                    }
                }
                else {
                    std::string echo = "SERVER:ECHO: " + message;
                    ws.write(net::buffer(echo));
                }

                // Clear the buffer after processing
                buffer.consume(buffer.size());
            }

            // --- 3. Process Movement Tick ---
            if (!player.isInCombat && !player.currentPath.empty()) {
                auto now = std::chrono::steady_clock::now();
                if (now - player.lastMoveTime >= MOVEMENT_DELAY) {
                    // Get next step and remove it from path
                    Point next_pos = player.currentPath.front();
                    player.currentPath.pop_front();

                    // Update player state
                    player.posX = next_pos.x;
                    player.posY = next_pos.y;
                    player.lastMoveTime = now;

                    // Update global registry for other players
                    broadcastData.posX = player.posX;
                    broadcastData.posY = player.posY;
                    {
                        std::lock_guard<std::mutex> lock(g_player_registry_mutex);
                        g_player_registry[player.userId] = broadcastData;
                    }

                    // Send update to the local player
                    send_player_stats(ws, player);
                }
            }

            // --- 4. Sleep (REMOVED) ---
            // The blocking read with timeout now serves as our "sleep" / tick timer
        }
    }
    catch (beast::system_error const& se) {
        // --- MODIFIED: Added specific check for operation_aborted ---
        // This is the error you were seeing. It's often a "clean" disconnect.
        if (se.code() == websocket::error::closed ||
            se.code() == net::error::eof ||
            se.code() == net::error::operation_aborted)
        {
            // Do nothing, just let the session end.
        }
        // This will catch the timeout error if it's not handled inside the loop
        else if (se.code() == beast::error::timeout) {
            // This is an expected error, just means no message was received.
            // We can ignore it and let the loop continue, but we'll log it
            // just in case it's happening unexpectedly.
            // std::cerr << "[" << client_address << "] Info: Read timed out (tick).\n";
        }
        else {
            std::cerr << "[" << client_address << "] Session Error: " << se.code().message() << "\n";
        }
    }
    catch (std::exception const& e) {
        std::cerr << "[" << client_address << "] Exception: " << e.what() << "\n";
    }

    // --- Unregister player on disconnect ---
    try {
        std::lock_guard<std::mutex> lock(g_player_registry_mutex);
        g_player_registry.erase(userId);
    }
    catch (std::exception const& e) {
        std::cerr << "[" << client_address << "] Exception during cleanup: " << e.what() << "\n";
    }

    std::cout << "[" << client_address << "] Client disconnected.\n";
}