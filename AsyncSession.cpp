// File: LocalWebSocketServer/AsyncSession.cpp
//
// FIXED: Merged all logic from game_session.cpp into this file.
// This resolves all "unresolved external symbol" linker errors.
// FIXED: C++17 structured binding errors.
// FIXED: Missing destructor definition.
//

#include "AsyncSession.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <mutex>
#include <vector>
#include <random>
#include <deque>
#include <cmath>
#include <set>
#include <queue>

// --- All Game Logic and Globals are now defined in this file ---

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

// Monster Base Stats (Type -> Stats)
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

// --- Multiplayer Registries ---
std::map<std::string, PlayerBroadcastData> g_player_registry;
std::mutex g_player_registry_mutex;

// --- ADDED: Session registry for broadcasting chat ---
std::map<std::string, std::weak_ptr<AsyncSession>> g_session_registry;
std::mutex g_session_registry_mutex;


// --- A* Pathfinding Implementation ---
namespace { // Use anonymous namespace to keep these helpers file-local
    struct Node {
        Point pos;
        int g, h, f; // g = cost, h = heuristic, f = g + h
        const Node* parent;

        Node(Point p, int g_val, int h_val, const Node* p_val)
            : pos(p), g(g_val), h(h_val), f(g_val + h_val), parent(p_val) {
        }

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
        return std::abs(a.x - b.x) + std::abs(a.y - b.y);
    }

    // Main A* function
    std::deque<Point> A_Star_Search(Point start, Point end) {
        std::deque<Point> path;
        std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open_list;
        std::set<std::pair<int, int>> closed_list;
        std::vector<std::unique_ptr<Node>> node_storage;

        if (!is_walkable(end.x, end.y)) {
            return path;
        }

        auto start_node = std::make_unique<Node>(start, 0, calculate_heuristic(start, end), nullptr);
        open_list.push(*start_node);
        node_storage.push_back(std::move(start_node));

        const int D = 1;
        int dx[] = { 0, 0, 1, -1 };
        int dy[] = { 1, -1, 0, 0 };
        int num_directions = 4;

        while (!open_list.empty()) {
            Node current = open_list.top();
            open_list.pop();

            if (current.pos == end) {
                const Node* trace = &current;
                while (trace != nullptr) {
                    path.push_front(trace->pos);
                    trace = trace->parent;
                }
                path.pop_front();
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
        return path;
    }
} // end anonymous namespace

// --- Class Starting Stats ---
PlayerStats getStartingStats(PlayerClass playerClass) {
    switch (playerClass) {
    case PlayerClass::FIGHTER: return PlayerStats(120, 20, 15, 12, 8);
    case PlayerClass::WIZARD: return PlayerStats(80, 100, 8, 6, 10);
    case PlayerClass::ROGUE: return PlayerStats(90, 40, 12, 8, 15);
    default: return PlayerStats(100, 50, 10, 10, 10);
    }
}

// Create a monster instance from a template
MonsterInstance create_monster(int id, std::string type) {
    MonsterInstance monster = MONSTER_TEMPLATES.at(type);
    monster.id = id;
    monster.type = type;
    monster.assetKey = MONSTER_ASSETS.at(type);
    return monster;
}


// --- AsyncSession Method Definitions ---

AsyncSession::AsyncSession(tcp::socket socket)
    : ws_(std::move(socket))
    , move_timer_(ws_.get_executor()) // --- FIXED: Use correct constructor ---
    , client_address_(ws_.next_layer().remote_endpoint().address().to_string())
{
    // Set timer to be "expired" so it doesn't run
    move_timer_.expires_at(std::chrono::steady_clock::time_point::max());
    std::cout << "--- New Client Connected from: " << client_address_ << " ---" << std::endl;
}

// --- ADDED: Destructor definition ---
AsyncSession::~AsyncSession()
{
    // This is a failsafe, on_session_end should already be called
    std::lock_guard<std::mutex> lock(g_session_registry_mutex);
    g_session_registry.erase(player_.userId);
}

void AsyncSession::run()
{
    net::dispatch(ws_.get_executor(),
        [self = shared_from_this()]()
        {
            self->on_run();
        });
}

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

                self->player_.userId = "Client_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
                self->player_.currentArea = "TOWN";
                self->player_.posX = 5;
                self->player_.posY = 5;
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

                static std::string welcome = "SERVER:WELCOME! Please enter your character name using SET_NAME:YourName";
                self->ws_.async_write(net::buffer(welcome),
                    net::bind_executor(self->ws_.get_executor(),
                        [self = self->shared_from_this()](beast::error_code ec, std::size_t bytes)
                        {
                            self->on_write(ec, bytes);
                        }));

                self->do_read();
                self->do_move_tick(beast::error_code{});
            }));
}

void AsyncSession::do_read()
{
    ws_.async_read(buffer_,
        net::bind_executor(ws_.get_executor(),
            [self = shared_from_this()](beast::error_code ec, std::size_t bytes)
            {
                self->on_read(ec, bytes);
            }));
}

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
    std::cout << "[" << client_address_ << "] Received: " << message << "\n";

    handle_message(message);

    buffer_.consume(buffer_.size());
    do_read();
}

void AsyncSession::on_write(beast::error_code ec, std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);
    if (ec)
    {
        std::cerr << "[" << client_address_ << "] Write Error: " << ec.message() << "\n";
        return on_session_end();
    }
}

void AsyncSession::do_move_tick(beast::error_code ec)
{
    if (ec && ec != net::error::operation_aborted)
    {
        std::cerr << "[" << client_address_ << "] Move Timer Error: " << ec.message() << "\n";
        return on_session_end();
    }

    if (!player_.isInCombat && !player_.currentPath.empty()) {
        auto now = std::chrono::steady_clock::now();
        if (now - player_.lastMoveTime >= MOVEMENT_DELAY) {
            Point next_pos = player_.currentPath.front();
            player_.currentPath.pop_front();

            player_.posX = next_pos.x;
            player_.posY = next_pos.y;
            player_.lastMoveTime = now;

            broadcast_data_.posX = player_.posX;
            broadcast_data_.posY = player_.posY;
            {
                std::lock_guard<std::mutex> lock(g_player_registry_mutex);
                g_player_registry[player_.userId] = broadcast_data_;
            }
            send_player_stats();
        }
    }

    move_timer_.expires_after(std::chrono::milliseconds(SERVER_TICK_RATE_MS));
    move_timer_.async_wait(
        net::bind_executor(ws_.get_executor(),
            [self = shared_from_this()](beast::error_code ec)
            {
                self->do_move_tick(ec);
            }));
}

void AsyncSession::on_session_end()
{
    move_timer_.cancel();

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


// --- All Helper Functions are now Methods of AsyncSession ---

void AsyncSession::send_player_stats() {
    std::ostringstream oss;
    oss << "SERVER:STATS:"
        << "{\"playerName\":\"" << player_.playerName << "\""
        << ",\"health\":" << player_.stats.health
        << ",\"maxHealth\":" << player_.stats.maxHealth
        << ",\"mana\":" << player_.stats.mana
        << ",\"maxMana\":" << player_.stats.maxMana
        << ",\"attack\":" << player_.stats.attack
        << ",\"defense\":" << player_.stats.defense
        << ",\"speed\":" << player_.stats.speed
        << ",\"level\":" << player_.stats.level
        << ",\"experience\":" << player_.stats.experience
        << ",\"experienceToNextLevel\":" << player_.stats.experienceToNextLevel
        << ",\"availableSkillPoints\":" << player_.availableSkillPoints
        << ",\"posX\":" << player_.posX
        << ",\"posY\":" << player_.posY;
    if (player_.currentClass == PlayerClass::WIZARD && !player_.spells.empty()) {
        oss << ",\"spells\":[";
        for (size_t i = 0; i < player_.spells.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "\"" << player_.spells[i] << "\"";
        }
        oss << "]";
    }
    else {
        oss << ",\"spells\":[]";
    }
    oss << "}";
    ws_.write(net::buffer(oss.str()));
}

void AsyncSession::send_available_areas() {
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
    ws_.write(net::buffer(response));
}

void AsyncSession::send_current_monsters_list() {
    std::string json_monsters = "[";
    for (size_t i = 0; i < player_.currentMonsters.size(); ++i) {
        const auto& monster = player_.currentMonsters[i];
        if (i > 0) json_monsters += ",";
        json_monsters += "{\"id\":" + std::to_string(monster.id) +
            ",\"type\":\"" + monster.type +
            "\",\"asset\":\"" + monster.assetKey + "\"}";
    }
    json_monsters += "]";
    std::string response = "SERVER:MONSTERS:" + json_monsters;
    ws_.write(net::buffer(response));
}

void AsyncSession::generate_and_send_monsters() {
    player_.currentMonsters.clear();
    int monster_count = (std::rand() % 3) + 2;
    for (int i = 0; i < monster_count; ++i) {
        int template_index = std::rand() % MONSTER_KEYS.size();
        std::string key = MONSTER_KEYS[template_index];
        MonsterState monster;
        monster.id = global_monster_id_counter++;
        monster.type = key;
        monster.assetKey = MONSTER_ASSETS.at(key);
        player_.currentMonsters.push_back(monster);
    }
    send_current_monsters_list();
}

void AsyncSession::check_for_level_up() {
    while (player_.stats.experience >= player_.stats.experienceToNextLevel) {
        player_.stats.level++;
        player_.stats.experience -= player_.stats.experienceToNextLevel;
        player_.stats.experienceToNextLevel = static_cast<int>(player_.stats.experienceToNextLevel * 1.5);

        player_.availableSkillPoints += 3;
        player_.stats.maxHealth += 10;
        player_.stats.health = player_.stats.maxHealth;
        player_.stats.maxMana += 5;
        player_.stats.mana = player_.stats.maxMana;
        player_.stats.attack += 2;
        player_.stats.defense += 1;
        player_.stats.speed += 1;
        std::cout << "[Level Up] Player " << player_.playerName << " reached level " << player_.stats.level << "\n";
        std::string level_msg = "SERVER:LEVEL_UP:You have reached level " + std::to_string(player_.stats.level) + "! You feel stronger!";
        ws_.write(net::buffer(level_msg));
        std::string prompt_msg = "SERVER:PROMPT:You have " + std::to_string(player_.availableSkillPoints) + " new skill points to spend.";
        ws_.write(net::buffer(prompt_msg));
    }
}


// --- All Game Logic is handled here ---
void AsyncSession::handle_message(const std::string& message)
{
    // --- Replaced all function calls with member function calls ---

    if (message.rfind("SET_NAME:", 0) == 0 && player_.playerName.empty()) {
        std::string name = message.substr(9);
        if (name.length() < 2 || name.length() > 20) {
            ws_.write(net::buffer("SERVER:ERROR:Name must be between 2 and 20 characters."));
        }
        else {
            player_.playerName = name;
            broadcast_data_.playerName = name;
            std::string response = "SERVER:NAME_SET:" + name;
            ws_.write(net::buffer(response));
            std::string class_prompt = "SERVER:PROMPT:Welcome " + name + "! Choose your class: SELECT_CLASS:FIGHTER, SELECT_CLASS:WIZARD, or SELECT_CLASS:ROGUE";
            ws_.write(net::buffer(class_prompt));
            std::cout << "[" << client_address_ << "] --- NAME SET: " << name << " ---\n";
            { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player_.userId] = broadcast_data_; }
        }
    }
    else if (message.rfind("SELECT_CLASS:", 0) == 0 && player_.currentClass == PlayerClass::UNSELECTED) {
        std::string class_str = message.substr(13);
        if (class_str == "FIGHTER") { player_.currentClass = PlayerClass::FIGHTER; broadcast_data_.playerClass = player_.currentClass; }
        else if (class_str == "WIZARD") { player_.currentClass = PlayerClass::WIZARD; broadcast_data_.playerClass = player_.currentClass; player_.spells = { "Fireball", "Lightning", "Freeze" }; }
        else if (class_str == "ROGUE") { player_.currentClass = PlayerClass::ROGUE; broadcast_data_.playerClass = player_.currentClass; }
        else { ws_.write(net::buffer("SERVER:ERROR:Invalid class.")); return; }
        player_.stats = getStartingStats(player_.currentClass);
        player_.availableSkillPoints = 3;
        player_.hasSpentInitialPoints = false;
        std::cout << "[" << client_address_ << "] --- CLASS SET: " << class_str << " ---\n";
        std::string response = "SERVER:CLASS_SET:" + class_str;
        ws_.write(net::buffer(response));
        send_player_stats();
        std::string prompt = "SERVER:PROMPT:You have 3 skill points to distribute. Use UPGRADE_STAT:stat_name to spend points.";
        ws_.write(net::buffer(prompt));
        { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player_.userId] = broadcast_data_; }
    }
    else if (message.rfind("UPGRADE_STAT:", 0) == 0) {
        if (player_.currentClass == PlayerClass::UNSELECTED) { ws_.write(net::buffer("SERVER:ERROR:You must select a class first.")); }
        else if (player_.availableSkillPoints <= 0) { ws_.write(net::buffer("SERVER:ERROR:You have no skill points available.")); }
        else {
            std::string stat_name = message.substr(13);
            bool valid_stat = false;
            if (stat_name == "health") { player_.stats.maxHealth += 5; player_.stats.health += 5; valid_stat = true; }
            else if (stat_name == "mana") { player_.stats.maxMana += 5; player_.stats.mana += 5; valid_stat = true; }
            else if (stat_name == "attack") { player_.stats.attack += 1; valid_stat = true; }
            else if (stat_name == "defense") { player_.stats.defense += 1; valid_stat = true; }
            else if (stat_name == "speed") { player_.stats.speed += 1; valid_stat = true; }
            if (valid_stat) {
                player_.availableSkillPoints--;
                std::string response = "SERVER:STAT_UPGRADED:" + stat_name;
                ws_.write(net::buffer(response));
                send_player_stats();
                if (player_.availableSkillPoints == 0 && !player_.hasSpentInitialPoints) {
                    player_.hasSpentInitialPoints = true;
                    player_.isFullyInitialized = true;
                    std::string complete = "SERVER:CHARACTER_COMPLETE:Character creation complete! You can now explore.";
                    ws_.write(net::buffer(complete));
                    send_available_areas();
                }
                else if (player_.availableSkillPoints > 0) { ws_.write(net::buffer("SERVER:PROMPT:You have " + std::to_string(player_.availableSkillPoints) + " skill points remaining.")); }
                else { ws_.write(net::buffer("SERVER:STATUS:All skill points spent.")); }
            }
            else { ws_.write(net::buffer("SERVER:ERROR:Invalid stat name.")); }
        }
    }
    else if (message.rfind("GO_TO:", 0) == 0) {
        if (!player_.isFullyInitialized) { ws_.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player_.isInCombat) { ws_.write(net::buffer("SERVER:ERROR:Cannot travel while in combat!")); }
        else {
            std::string target_area = message.substr(6);
            player_.currentPath.clear();
            broadcast_data_.currentArea = target_area;
            { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player_.userId] = broadcast_data_; }
            if (target_area == "TOWN") {
                player_.currentArea = "TOWN";
                player_.currentMonsters.clear();
                player_.stats.health = player_.stats.maxHealth; player_.stats.mana = player_.stats.maxMana;
                std::string response = "SERVER:AREA_CHANGED:TOWN";
                ws_.write(net::buffer(response));
                send_available_areas();
                send_player_stats();
            }
            else if (std::find(ALL_AREAS.begin(), ALL_AREAS.end(), target_area) != ALL_AREAS.end()) {
                player_.currentArea = target_area;
                std::string response = "SERVER:AREA_CHANGED:" + target_area;
                ws_.write(net::buffer(response));
                generate_and_send_monsters();
            }
            else {
                ws_.write(net::buffer("SERVER:ERROR:Invalid or unknown travel destination."));
                broadcast_data_.currentArea = player_.currentArea;
                { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player_.userId] = broadcast_data_; }
            }
            std::cout << "[" << client_address_ << "] --- AREA CHANGED TO: " << player_.currentArea << " ---\n";
        }
    }
    else if (message.rfind("MOVE_TO:", 0) == 0) {
        if (!player_.isFullyInitialized) { ws_.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player_.isInCombat) { ws_.write(net::buffer("SERVER:ERROR:Cannot move while in combat!")); }
        else if (player_.currentArea != "TOWN") { ws_.write(net::buffer("SERVER:ERROR:Grid movement is only available in TOWN.")); }
        else {
            try {
                std::string coords_str = message.substr(8);
                size_t comma_pos = coords_str.find(',');
                if (comma_pos == std::string::npos) throw std::invalid_argument("Invalid coordinate format.");

                int target_x = std::stoi(coords_str.substr(0, comma_pos));
                int target_y = std::stoi(coords_str.substr(comma_pos + 1));

                if (target_x < 0 || target_x >= GRID_COLS || target_y < 0 || target_y >= GRID_ROWS) {
                    ws_.write(net::buffer("SERVER:ERROR:Target coordinates are out of bounds."));
                }
                else if (TOWN_GRID[target_y][target_x] != 0) { // Check walkability
                    ws_.write(net::buffer("SERVER:ERROR:Cannot move to that location."));
                }
                else {
                    Point start_pos;
                    if (player_.currentPath.empty()) {
                        start_pos = { player_.posX, player_.posY };
                    }
                    else {
                        start_pos = player_.currentPath.front();
                    }
                    Point end_pos = { target_x, target_y };
                    player_.currentPath = A_Star_Search(start_pos, end_pos);
                    player_.lastMoveTime = std::chrono::steady_clock::now() - MOVEMENT_DELAY;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "Error parsing MOVE_TO: " << e.what() << "\n";
                ws_.write(net::buffer("SERVER:ERROR:Invalid coordinate format."));
            }
        }
    }
    // --- ADDED: Chat Message Handler ---
    else if (message.rfind("SEND_CHAT:", 0) == 0) {
        if (!player_.isFullyInitialized) {
            ws_.write(net::buffer("SERVER:ERROR:Must complete character creation to chat."));
            return;
        }
        std::string chat_text = message.substr(10);
        if (chat_text.empty() || chat_text.length() > 100) {
            ws_.write(net::buffer("SERVER:ERROR:Chat message must be 1-100 characters."));
            return;
        }

        // Format the message
        // Create a shared_ptr to the string to ensure it lives long enough
        auto shared_chat_msg = std::make_shared<std::string>(
            "SERVER:CHAT_MSG:{\"sender\":\"" + player_.playerName + "\",\"text\":\"" + chat_text + "\"}");

        // Get a list of all active sessions
        std::vector<std::shared_ptr<AsyncSession>> all_sessions;
        {
            std::lock_guard<std::mutex> lock(g_session_registry_mutex);
            // --- FIXED: C++14 compatible loop ---
            for (auto const& pair : g_session_registry) {
                // auto id = pair.first; // not needed
                auto weak_session = pair.second;
                if (auto session = weak_session.lock()) {
                    all_sessions.push_back(session);
                }
            }
        }

        // Dispatch a write task to each session's strand
        for (auto& session : all_sessions) {
            // --- FIXED: Use session variable correctly ---
            net::dispatch(session->ws_.get_executor(), [session, shared_chat_msg]() {
                // This is a blocking write, but it's on the *target's* strand,
                // so it's safe and won't block the sender's read loop.
                try {
                    session->ws_.write(net::buffer(*shared_chat_msg));
                }
                catch (std::exception const& e) {
                    // Handle write error (e.g., client disconnected)
                    std::cerr << "Chat broadcast write error: " << e.what() << "\n";
                }
                });
        }
    }
    // --- END: Chat Message Handler ---
    else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
        if (!player_.isFullyInitialized) { ws_.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player_.isInCombat) { ws_.write(net::buffer("SERVER:ERROR:You are already in combat!")); }
        else if (player_.currentArea == "TOWN") { ws_.write(net::buffer("SERVER:STATUS:No monsters to fight in TOWN.")); }
        else {
            try {
                int selected_id = std::stoi(message.substr(17));
                auto it = std::find_if(player_.currentMonsters.begin(), player_.currentMonsters.end(), [selected_id](const MonsterState& m) { return m.id == selected_id; });
                if (it != player_.currentMonsters.end()) {
                    player_.isInCombat = true;
                    player_.currentOpponent = create_monster(it->id, it->type);
                    player_.isDefending = false;
                    player_.currentMonsters.erase(it);
                    std::cout << "[" << client_address_ << "] --- COMBAT STARTED vs " << player_.currentOpponent->type << " ---\n";
                    std::ostringstream oss;
                    oss << "SERVER:COMBAT_START:"
                        << "{\"id\":" << player_.currentOpponent->id << ",\"type\":\"" << player_.currentOpponent->type
                        << "\",\"asset\":\"" << player_.currentOpponent->assetKey << "\",\"health\":" << player_.currentOpponent->health
                        << ",\"maxHealth\":" << player_.currentOpponent->maxHealth << "}";
                    ws_.write(net::buffer(oss.str()));
                    ws_.write(net::buffer("SERVER:COMBAT_LOG:You engaged the " + player_.currentOpponent->type + "!"));
                    ws_.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
                }
                else { ws_.write(net::buffer("SERVER:ERROR:Selected monster ID not found.")); }
            }
            catch (const std::exception&) { ws_.write(net::buffer("SERVER:ERROR:Invalid monster ID format.")); }
        }
    }
    else if (message.rfind("COMBAT_ACTION:", 0) == 0) {
        if (!player_.isInCombat || !player_.currentOpponent) { ws_.write(net::buffer("SERVER:ERROR:You are not in combat.")); }
        else {
            std::string action_command = message.substr(14);
            std::string action_type; std::string action_param;
            size_t colon_pos = action_command.find(':');
            if (colon_pos != std::string::npos) { action_type = action_command.substr(0, colon_pos); action_param = action_command.substr(colon_pos + 1); }
            else { action_type = action_command; }

            int player_damage = 0; int mana_cost = 0; bool fled = false;
            if (action_type == "ATTACK") {
                int base_damage = std::max(1, player_.stats.attack - player_.currentOpponent->defense);
                float variance = 0.8f + ((float)(std::rand() % 41) / 100.0f);
                player_damage = std::max(1, (int)(base_damage * variance));
                ws_.write(net::buffer("SERVER:COMBAT_LOG:You attack the " + player_.currentOpponent->type + " for " + std::to_string(player_damage) + " damage!"));
            }
            else if (action_type == "SPELL") {
                int base_damage = 0; mana_cost = 0; float variance = 1.0f;
                if (action_param == "Fireball") { mana_cost = 20; if (player_.stats.mana >= mana_cost) { base_damage = (player_.stats.maxMana / 8) + player_.stats.attack; variance = 0.8f + ((float)(std::rand() % 41) / 100.0f); } }
                else if (action_param == "Lightning") { mana_cost = 15; if (player_.stats.mana >= mana_cost) { base_damage = (player_.stats.maxMana / 10) + player_.stats.attack; variance = 0.7f + ((float)(std::rand() % 61) / 100.0f); } }
                else if (action_param == "Freeze") { mana_cost = 10; if (player_.stats.mana >= mana_cost) { base_damage = (player_.stats.maxMana / 12) + (player_.stats.attack / 2); variance = 0.9f + ((float)(std::rand() % 21) / 100.0f); } }
                else { ws_.write(net::buffer("SERVER:COMBAT_LOG:You don't know that spell!")); return; }
                if (mana_cost > 0 && player_.stats.mana >= mana_cost) {
                    player_.stats.mana -= mana_cost;
                    player_damage = std::max(1, (int)(base_damage * variance));
                    ws_.write(net::buffer("SERVER:COMBAT_LOG:You cast " + action_param + " for " + std::to_string(player_damage) + " damage!"));
                }
                else if (mana_cost > 0) { ws_.write(net::buffer("SERVER:COMBAT_LOG:Not enough mana to cast " + action_param + "! (Needs " + std::to_string(mana_cost) + ")")); }
                else { ws_.write(net::buffer("SERVER:COMBAT_LOG:Cannot cast " + action_param + ".")); }
            }
            else if (action_type == "DEFEND") { player_.isDefending = true; ws_.write(net::buffer("SERVER:COMBAT_LOG:You brace for the next attack.")); }
            else if (action_type == "FLEE") {
                float flee_chance = 0.5f + ((float)player_.stats.speed - (float)player_.currentOpponent->speed) * 0.05f;
                if (flee_chance < 0.1f) flee_chance = 0.1f; if (flee_chance > 0.9f) flee_chance = 0.9f;
                if (((float)std::rand() / RAND_MAX) < flee_chance) { fled = true; }
                else { ws_.write(net::buffer("SERVER:COMBAT_LOG:You failed to flee!")); }
            }
            if (fled) {
                ws_.write(net::buffer("SERVER:COMBAT_LOG:You successfully fled from the " + player_.currentOpponent->type + "!"));
                player_.isInCombat = false; player_.currentOpponent.reset();
                ws_.write(net::buffer("SERVER:COMBAT_VICTORY:Fled"));
                send_current_monsters_list(); return;
            }
            if (player_damage > 0) { player_.currentOpponent->health -= player_damage; }
            send_player_stats();
            ws_.write(net::buffer("SERVER:COMBAT_UPDATE:" + std::to_string(player_.currentOpponent->health)));
            if (player_.currentOpponent->health <= 0) {
                ws_.write(net::buffer("SERVER:COMBAT_LOG:You defeated the " + player_.currentOpponent->type + "!"));
                int xp_gain = player_.currentOpponent->xpReward;
                ws_.write(net::buffer("SERVER:STATUS:Gained " + std::to_string(xp_gain) + " XP."));
                player_.stats.experience += xp_gain;
                player_.isInCombat = false; player_.currentOpponent.reset();
                ws_.write(net::buffer("SERVER:COMBAT_VICTORY:Defeated"));
                check_for_level_up(); send_player_stats();
                send_current_monsters_list(); return;
            }
            int monster_damage = 0; int player_defense = player_.stats.defense;
            if (player_.isDefending) { player_defense *= 2; player_.isDefending = false; }
            monster_damage = std::max(1, player_.currentOpponent->attack - player_defense);
            player_.stats.health -= monster_damage;
            ws_.write(net::buffer("SERVER:COMBAT_LOG:The " + player_.currentOpponent->type + " attacks you for " + std::to_string(monster_damage) + " damage!"));
            send_player_stats();
            if (player_.stats.health <= 0) {
                player_.stats.health = 0;
                ws_.write(net::buffer("SERVER:COMBAT_DEFEAT:You have been defeated!"));
                player_.isInCombat = false; player_.currentOpponent.reset();
                player_.currentArea = "TOWN"; player_.currentMonsters.clear();
                player_.stats.health = player_.stats.maxHealth / 2; player_.stats.mana = player_.stats.maxMana;
                player_.posX = 5; player_.posY = 5; player_.currentPath.clear();
                broadcast_data_.currentArea = "TOWN"; broadcast_data_.posX = player_.posX; broadcast_data_.posY = player_.posY;
                { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player_.userId] = broadcast_data_; }
                ws_.write(net::buffer("SERVER:AREA_CHANGED:TOWN"));
                send_available_areas(); send_player_stats();
                return;
            }
            ws_.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
        }
    }
    else if (message.rfind("GIVE_XP:", 0) == 0) {
        if (!player_.isFullyInitialized) { ws_.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player_.isInCombat) { ws_.write(net::buffer("SERVER:ERROR:Cannot gain XP in combat.")); }
        else {
            try {
                int xp_to_give = std::stoi(message.substr(8));
                if (xp_to_give > 0) {
                    player_.stats.experience += xp_to_give;
                    std::string response = "SERVER:STATUS:Gained " + std::to_string(xp_to_give) + " XP.";
                    ws_.write(net::buffer(response));
                    check_for_level_up();
                    send_player_stats();
                }
                else { ws_.write(net::buffer("SERVER:ERROR:Invalid XP amount.")); }
            }
            catch (const std::exception&) { ws_.write(net::buffer("SERVER:ERROR:Invalid XP amount format.")); }
        }
    }
    else if (message == "REQUEST_PLAYERS") {
        if (player_.currentArea == "TOWN") {
            std::ostringstream oss;
            oss << "SERVER:PLAYERS_IN_TOWN:[";
            bool first_player = true;
            {
                std::lock_guard<std::mutex> lock(g_player_registry_mutex);
                // --- FIXED: C++14 compatible loop ---
                for (auto const& pair : g_player_registry) {
                    if (pair.first == player_.userId) continue;
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
            ws_.write(net::buffer(oss.str()));
        }
    }
    else {
        std::string echo = "SERVER:ECHO: " + message;
        ws_.write(net::buffer(echo));
    }
}