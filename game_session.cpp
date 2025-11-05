// File: LocalWebSocketServer/game_session.cpp
//
// MODIFIED: Removed the do_session function.
// This file now only contains shared helper functions and globals.
//
#include "game_session.hpp"
#include <iostream>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <utility>
#include <sstream>
#include <map> 
#include <mutex> 
#include <memory>
#include <deque> 
#include <chrono> 
#include <cmath> 
#include <set> 
#include <queue> 

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- Server Data Definitions ---
const std::vector<std::string> ALL_AREAS = {
    "FOREST", "CAVES", "RUINS", "SWAMP", "MOUNTAINS", "DESERT", "VOLCANO"
};

// ADDED: Town Grid Map (0 = walkable, 1 = obstacle)
const std::vector<std::vector<int>> TOWN_GRID = {
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
std::map<std::string, PlayerBroadcastData> g_player_registry;
std::mutex g_player_registry_mutex;


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
    unsigned seed = (unsigned int)std::chrono::system_clock::now().time_since_epoch().count();
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