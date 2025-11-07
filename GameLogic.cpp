// File: GameLogic.cpp
// Description: Implements all game logic handlers for the AsyncSession class.
// This includes message parsing, combat, movement, and state updates.

#include "AsyncSession.hpp" // For the class definition and accessors
#include "GameData.hpp"     // For all game data, registries, and utils
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <random>
#include <deque>
#include <set>
#include <queue>
#include <algorithm> 

/**
 * @brief Processes one tick of player movement.
 * Called by the session's move_timer_.
 */
void AsyncSession::process_movement()
{
    // Access session state
    PlayerState& player = getPlayerState();

    // Only move if not in combat and a path exists
    if (!player.isInCombat && !player.currentPath.empty()) {
        auto now = std::chrono::steady_clock::now();
        // Check if enough time has passed since the last move
        if (now - player.lastMoveTime >= MOVEMENT_DELAY) {
            Point next_pos = player.currentPath.front();
            player.currentPath.pop_front();

            // Update player position
            player.posX = next_pos.x;
            player.posY = next_pos.y;
            player.lastMoveTime = now;

            //checking for whehn u walk on an interactable object
            auto it = g_interactable_objects.find(player.currentArea);
            if (it != g_interactable_objects.end()) {
                for (const auto& obj : it->second) {
                    if (obj.position.x == player.posX && obj.position.y == player.posY) {
                        // Player has stepped on an interaction tile
                        if (obj.type == InteractableType::ZONE_TRANSITION) {
                            player.currentPath.clear(); // Stop any further movement
                            std::string command = "GO_TO:" + obj.data;
                            handle_message(command); // This will change the area
                            return;
                        }
                        // could add other crap here (maybe traps ;D?)
                    }
                }
            }
            // Update public broadcast data
            PlayerBroadcastData& broadcast = getBroadcastData();
            broadcast.posX = player.posX;
            broadcast.posY = player.posY;
            {
                std::lock_guard<std::mutex> lock(g_player_registry_mutex);
                g_player_registry[player.userId] = broadcast;
            }
            // Notify the client of their new stats (which include position)
            send_player_stats();
        }
    }
}

/**
 * @brief Sends the player's complete stat block to the client.
 */
void AsyncSession::send_player_stats() {
    // Access session state
    PlayerState& player = getPlayerState();
    auto& ws = getWebSocket();

    // Use ostringstream to build the JSON-like string
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

    // Add spells list only if they exist
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
    std::string stats_message = oss.str();
    ws.write(net::buffer(stats_message));
}

/**
 * @brief Sends a dynamically generated list of available areas.
 */
void AsyncSession::send_available_areas() {
    auto& ws = getWebSocket();

    std::vector<std::string> areas = ALL_AREAS; // Get from GameData
    unsigned seed = (unsigned int)std::chrono::system_clock::now().time_since_epoch().count();
    std::default_random_engine rng(seed);
    std::shuffle(areas.begin(), areas.end(), rng);

    // Send 2-4 random areas
    int count = (std::rand() % 3) + 2;
    std::string area_list;
    for (int i = 0; i < std::min((size_t)count, areas.size()); ++i) {
        if (!area_list.empty()) area_list += ",";
        area_list += areas[i];
    }
    std::string response = "SERVER:AREAS:" + area_list;
    ws.write(net::buffer(response));
}

//sends all interactable stuff to the client
void AsyncSession::send_interactables(const std::string& areaName) {
    auto& ws = getWebSocket();
    std::ostringstream oss;
    oss << "SERVER:INTERACTABLES:[";

    auto it = g_interactable_objects.find(areaName);
    if (it != g_interactable_objects.end()) {
        bool first = true;
        for (const auto& obj : it->second) {
            if (!first) oss << ",";
            oss << "{\"id\":\"" << obj.id << "\""
                << ",\"type\":" << static_cast<int>(obj.type) // Send enum as int
                << ",\"x\":" << obj.position.x
                << ",\"y\":" << obj.position.y
                << ",\"data\":\"" << obj.data << "\"}";
            first = false;
        }
    }

    oss << "]";
    std::string message = oss.str();
    ws.write(net::buffer(message));
}
/**
 * @brief Sends the current list of monsters to the client. i added coords instead oif it jus bein sprites
 */
void AsyncSession::send_current_monsters_list() {
    PlayerState& player = getPlayerState();
    auto& ws = getWebSocket();

    std::string json_monsters = "[";
    for (size_t i = 0; i < player.currentMonsters.size(); ++i) {
        const auto& monster = player.currentMonsters[i];
        if (i > 0) json_monsters += ",";
        json_monsters += "{\"id\":" + std::to_string(monster.id) +
            ",\"type\":\"" + monster.type +
            "\",\"asset\":\"" + monster.assetKey + "\"" +
            ",\"x\":" + std::to_string(monster.posX) +
            ",\"y\":" + std::to_string(monster.posY) + "}";
    }
    json_monsters += "]";
    std::string response = "SERVER:MONSTERS:" + json_monsters;
    ws.write(net::buffer(response));
}

/**
 * @brief Generates 2-4 new monsters for the area.
 */
void AsyncSession::generate_and_send_monsters() {
    PlayerState& player = getPlayerState();
    player.currentMonsters.clear();

    // Find the grid for the current area
    auto grid_it = g_area_grids.find(player.currentArea);
    if (grid_it == g_area_grids.end()) {
          //send empty list if we using an area where we dont havea  grid to walk around in
        send_current_monsters_list(); 
        return;
    }
    const auto& grid = grid_it->second;

    int monster_count = (std::rand() % 3) + 2;
    for (int i = 0; i < monster_count; ++i) {
        int template_index = std::rand() % MONSTER_KEYS.size();
        std::string key = MONSTER_KEYS[template_index]; 
        MonsterState monster;
        monster.id = global_monster_id_counter++; 
        monster.type = key;
        monster.assetKey = MONSTER_ASSETS.at(key); 

        // making the monsters have random walk patterns
        int x, y;
        do {
            x = std::rand() % GRID_COLS;
            y = std::rand() % GRID_ROWS;
        } while (y >= grid.size() || x >= grid[y].size() || grid[y][x] != 0); // make sure they only move on 0 grid tiles n not 1 so they cant move thru blocked grid cells

        monster.posX = x;
        monster.posY = y;
        

        player.currentMonsters.push_back(monster);
    }
    send_current_monsters_list();
}

/**
 * @brief Checks for and processes player level-ups.
 */
void AsyncSession::check_for_level_up() {
    PlayerState& player = getPlayerState();
    auto& ws = getWebSocket();

    while (player.stats.experience >= player.stats.experienceToNextLevel) {
        player.stats.level++;
        player.stats.experience -= player.stats.experienceToNextLevel;
        player.stats.experienceToNextLevel = static_cast<int>(player.stats.experienceToNextLevel * 1.5);

        // Apply stat gains
        player.availableSkillPoints += 3;
        player.stats.maxHealth += 10;
        player.stats.health = player.stats.maxHealth;
        player.stats.maxMana += 5;
        player.stats.mana = player.stats.maxMana;
        player.stats.attack += 2;
        player.stats.defense += 1;
        player.stats.speed += 1;

        std::cout << "[Level Up] Player " << player.playerName << " reached level " << player.stats.level << "\n";

        // Notify client
        std::string level_msg = "SERVER:LEVEL_UP:You have reached level " + std::to_string(player.stats.level) + "! You feel stronger!";
        ws.write(net::buffer(level_msg));
        std::string prompt_msg = "SERVER:PROMPT:You have " + std::to_string(player.availableSkillPoints) + " new skill points to spend.";
        ws.write(net::buffer(prompt_msg));
    }
}

/**
 * @brief Sends the collision map data for a given area.
 */
void AsyncSession::send_area_map_data(const std::string& areaName) {
    auto& ws = getWebSocket();
    std::ostringstream oss;
    oss << "SERVER:MAP_DATA:";

    // Look for the area in our new grid registry
    auto it = g_area_grids.find(areaName);

    if (it != g_area_grids.end()) {
        // ok now we're serizling all the found grids
        const auto& grid = it->second;
        for (int y = 0; y < GRID_ROWS; ++y) {
            for (int x = 0; x < GRID_COLS; ++x) {
                if (y < grid.size() && x < grid[y].size()) {
                    oss << grid[y][x];
                }
                else {
                    oss << '0'; // Fallback for safety
                }
            }
        }
    }
    else {
        // No grid defined for this area.
        // Send an all-open map (maintains behavior for combat zones)
        std::string open_map(GRID_COLS * GRID_ROWS, '0');
        oss << open_map;
    }
    std::string message = oss.str();
    std::cout << "[DEBUG] Map data message length: " << message.length() << std::endl;
    ws.write(net::buffer(message));
}


/**
 * @brief The main message router. Parses client commands and acts on them.
 */
void AsyncSession::handle_message(const std::string& message)
{
    // Get mutable access to this session's state
    PlayerState& player = getPlayerState();
    PlayerBroadcastData& broadcast_data = getBroadcastData();
    auto& ws = getWebSocket();
    std::string client_address = client_address_; // Get a copy for logging

    // --- Character Creation ---
    if (message.rfind("SET_NAME:", 0) == 0 && player.playerName.empty()) {
        std::string name = message.substr(9);
        if (name.length() < 2 || name.length() > 20) {
            ws.write(net::buffer("SERVER:ERROR:Name must be between 2 and 20 characters."));
        }
        else {
            player.playerName = name;
            broadcast_data.playerName = name;
            std::string response = "SERVER:NAME_SET:" + name;
            ws.write(net::buffer(response));
            std::string class_prompt = "SERVER:PROMPT:Welcome " + name + "! Choose your class: SELECT_CLASS:FIGHTER, SELECT_CLASS:WIZARD, or SELECT_CLASS:ROGUE";
            ws.write(net::buffer(class_prompt));
            std::cout << "[" << client_address << "] --- NAME SET: " << name << " ---\n";
            // Update the global registry
            { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }
        }
    }
    else if (message.rfind("SELECT_CLASS:", 0) == 0 && player.currentClass == PlayerClass::UNSELECTED) {
        std::string class_str = message.substr(13);
        if (class_str == "FIGHTER") { player.currentClass = PlayerClass::FIGHTER; broadcast_data.playerClass = player.currentClass; }
        else if (class_str == "WIZARD") { player.currentClass = PlayerClass::WIZARD; broadcast_data.playerClass = player.currentClass; player.spells = { "Fireball", "Lightning", "Freeze" }; }
        else if (class_str == "ROGUE") { player.currentClass = PlayerClass::ROGUE; broadcast_data.playerClass = player.currentClass; }
        else { ws.write(net::buffer("SERVER:ERROR:Invalid class.")); return; }

        player.stats = getStartingStats(player.currentClass); // from GameData
        player.availableSkillPoints = 3;
        player.hasSpentInitialPoints = false;

        std::cout << "[" << client_address << "] --- CLASS SET: " << class_str << " ---\n";
        ws.write(net::buffer("SERVER:CLASS_SET:" + class_str));
        send_player_stats();
        ws.write(net::buffer("SERVER:PROMPT:You have 3 skill points to distribute. Use UPGRADE_STAT:stat_name to spend points."));
        { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }
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
                ws.write(net::buffer("SERVER:STAT_UPGRADED:" + stat_name));
                send_player_stats();
                if (player.availableSkillPoints == 0 && !player.hasSpentInitialPoints) {
                    player.hasSpentInitialPoints = true;
                    player.isFullyInitialized = true;
                    ws.write(net::buffer("SERVER:CHARACTER_COMPLETE:Character creation complete! You can now explore."));
                    send_available_areas();
                }
                else if (player.availableSkillPoints > 0) { ws.write(net::buffer("SERVER:PROMPT:You have " + std::to_string(player.availableSkillPoints) + " skill points remaining.")); }
                else { ws.write(net::buffer("SERVER:STATUS:All skill points spent.")); }
            }
            else { ws.write(net::buffer("SERVER:ERROR:Invalid stat name.")); }
        }
    }

    // --- World Navigation ---
    else if (message.rfind("GO_TO:", 0) == 0) {
        if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot travel while in combat!")); }
        else {
            std::string target_area = message.substr(6);
            player.currentPath.clear();
            broadcast_data.currentArea = target_area;
            { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }

            if (target_area == "TOWN") {
                player.currentArea = "TOWN";
                player.currentMonsters.clear();
                player.stats.health = player.stats.maxHealth;
                player.stats.mana = player.stats.maxMana;

                // I HAVE BEEN TRYING TO FIX THIS FKING STRING CORRUPTION BUG FOR HOURS PLEASE WORK
                std::string response = "SERVER:AREA_CHANGED:TOWN";
                std::cout << "[DEBUG] Sending area change: '" << response << "'" << std::endl;
                std::cout << "[DEBUG] Response length: " << response.length() << std::endl;
                std::cout << "[DEBUG] Response bytes: ";
                for (char c : response) {
                    std::cout << (int)(unsigned char)c << " ";
                }
                std::cout << std::endl;
                ws.write(net::buffer(response));

                send_area_map_data(player.currentArea);
                send_interactables(player.currentArea);
                send_available_areas();
                send_player_stats();
            }
            else if (std::find(ALL_AREAS.begin(), ALL_AREAS.end(), target_area) != ALL_AREAS.end()) {
                player.currentArea = target_area;
                ws.write(net::buffer("SERVER:AREA_CHANGED:" + target_area));
                send_area_map_data(player.currentArea);
                generate_and_send_monsters();
            }
            else {
                ws.write(net::buffer("SERVER:ERROR:Invalid or unknown travel destination."));
                broadcast_data.currentArea = player.currentArea; // Revert
                { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }
            }
            std::cout << "[" << client_address << "] --- AREA CHANGED TO: " << player.currentArea << " ---\n";
        }
    }
    else if (message.rfind("MOVE_TO:", 0) == 0) {
        if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot move while in combat!")); }
        else {
            // we gotta check to see if players current area even has a grid for it yet
            auto it = g_area_grids.find(player.currentArea);
            if (it == g_area_grids.end()) {
                ws.write(net::buffer("SERVER:ERROR:Grid movement is not available in this area."));
                return; // Exit if no grid exists for this area
            }

            // Get the grid for the current area
            const auto& current_grid = it->second;


            try {
                std::string coords_str = message.substr(8);
                size_t comma_pos = coords_str.find(',');
                if (comma_pos == std::string::npos) throw std::invalid_argument("Invalid coordinate format.");

                int target_x = std::stoi(coords_str.substr(0, comma_pos));
                int target_y = std::stoi(coords_str.substr(comma_pos + 1));

                if (target_x < 0 || target_x >= GRID_COLS || target_y < 0 || target_y >= GRID_ROWS) {
                    ws.write(net::buffer("SERVER:ERROR:Target coordinates are out of bounds."));
                }
                //checkin walkability using the area we pass in grid instead of the old town grid
                else if (current_grid[target_y][target_x] != 0) {
                    ws.write(net::buffer("SERVER:ERROR:Cannot move to that location."));
                }
                else {
                    // Use A* to find the path
                    Point start_pos = { player.posX, player.posY };
                    Point end_pos = { target_x, target_y };


                    // we makin sure to paass the correct grid to the A* function
                    player.currentPath = A_Star_Search(start_pos, end_pos, current_grid); // from GameData


                    player.lastMoveTime = std::chrono::steady_clock::now() - MOVEMENT_DELAY; // Allow instant first move
                }
            }
            catch (const std::exception& e) {
                std::cerr << "Error parsing MOVE_TO: " << e.what() << "\n";
                ws.write(net::buffer("SERVER:ERROR:Invalid coordinate format."));
            }
        }
    }

    // --- Chat System ---
    else if (message.rfind("SEND_CHAT:", 0) == 0) {
        if (!player.isFullyInitialized) {
            ws.write(net::buffer("SERVER:ERROR:Must complete character creation to chat."));
            return;
        }
        std::string chat_text = message.substr(10);
        if (chat_text.empty() || chat_text.length() > 100) {
            ws.write(net::buffer("SERVER:ERROR:Chat message must be 1-100 characters."));
            return;
        }

        // Create a shared_ptr to the message string so it lives long enough
        auto shared_chat_msg = std::make_shared<std::string>(
            "SERVER:CHAT_MSG:{\"sender\":\"" + player.playerName + "\",\"text\":\"" + chat_text + "\"}");

        // Get a list of all active sessions
        std::vector<std::shared_ptr<AsyncSession>> all_sessions;
        {
            std::lock_guard<std::mutex> lock(g_session_registry_mutex);
            for (auto const& pair : g_session_registry) {
                if (auto session = pair.second.lock()) { // .lock() converts weak_ptr to shared_ptr
                    all_sessions.push_back(session);
                }
            }
        }

        // Dispatch a write task to *each session's own strand*
        for (auto& session : all_sessions) {
            net::dispatch(session->ws_.get_executor(), [session, shared_chat_msg]() {
                // This code runs on the *target's* strand, making it thread-safe.
                try {
                    session->ws_.write(net::buffer(*shared_chat_msg));
                }
                catch (std::exception const& e) {
                    std::cerr << "Chat broadcast write error: " << e.what() << "\n";
                }
                });
        }
    }
    else if (message.rfind("INTERACT_AT:", 0) == 0) {
        if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot interact while in combat!")); }
        else {
            try {
                std::string coords_str = message.substr(12);
                size_t comma_pos = coords_str.find(',');
                if (comma_pos == std::string::npos) throw std::invalid_argument("Invalid coordinate format.");

                int target_x = std::stoi(coords_str.substr(0, comma_pos));
                int target_y = std::stoi(coords_str.substr(comma_pos + 1));

                // Find the object in the current area our player is in
                InteractableObject* targetObject = nullptr;
                auto it = g_interactable_objects.find(player.currentArea);
                if (it != g_interactable_objects.end()) {
                    for (auto& obj : it->second) {
                        if (obj.position.x == target_x && obj.position.y == target_y) {
                            targetObject = const_cast<InteractableObject*>(&obj);
                            break;
                        }
                    }
                }

                if (!targetObject) {
                    ws.write(net::buffer("SERVER:ERROR:No object to interact with at that location."));
                    return;
                }

                // Check if player is adjacent to the intractble thing
                int dist = std::abs(player.posX - target_x) + std::abs(player.posY - target_y);
                if (dist > 1) {
                    ws.write(net::buffer("SERVER:ERROR:You are too far away to interact with that."));
                    // Optional: You could pathfind the player to an adjacent tile here
                    return;
                }

                // Player is adjacent lets start interaction
                player.currentPath.clear(); // Stop playr from moving

                if (targetObject->type == InteractableType::NPC) {
                    // Send an interaction event to the client with the NPC's data
                    ws.write(net::buffer("SERVER:NPC_INTERACT:" + targetObject->data));

                    // using yousafs great ass dialogue here :D
                    if (targetObject->data == "GUARD_DIALOGUE_1") {
                        ws.write(net::buffer("SERVER:PROMPT:Guard: \"This place gets scary at night\""));
                    }
                    else if (targetObject->data == "MERCHANT_SHOP_1") {
                        ws.write(net::buffer("SERVER:PROMPT:Merchant: \"You there, got some gold, I've got stuff that might appeal to you\""));
                    }
                }
                else if (targetObject->type == InteractableType::ZONE_TRANSITION) {
                    // This is normally a walk-on, but if clicked, just trigger it
                    handle_message("GO_TO:" + targetObject->data);
                }
                // we'll add shops here soon and other crap
                else {
                    ws.write(net::buffer("SERVER:ERROR:Unknown interaction type."));
                }
            }
            catch (const std::exception& e) {
                std::cerr << "Error parsing INTERACT_AT: " << e.what() << "\n";
                ws.write(net::buffer("SERVER:ERROR:Invalid coordinate format."));
            }
        }
    }
    // --- Combat System ---
    else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
        if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:You are already in combat!")); }
        else if (player.currentArea == "TOWN") { ws.write(net::buffer("SERVER:STATUS:No monsters to fight in TOWN.")); }
        else {
            try {
                int selected_id = std::stoi(message.substr(17));
                // Find the monster in the area list
                auto it = std::find_if(player.currentMonsters.begin(), player.currentMonsters.end(), [selected_id](const MonsterState& m) { return m.id == selected_id; });

                if (it != player.currentMonsters.end()) {
                    player.isInCombat = true;
                    // Create a full monster instance for combat
                    player.currentOpponent = create_monster(it->id, it->type); // from GameData
                    player.isDefending = false;
                    player.currentMonsters.erase(it); // Remove from world

                    std::cout << "[" << client_address << "] --- COMBAT STARTED vs " << player.currentOpponent->type << " ---\n";
                    std::ostringstream oss;
                    oss << "SERVER:COMBAT_START:"
                        << "{\"id\":" << player.currentOpponent->id << ",\"type\":\"" << player.currentOpponent->type
                        << "\",\"asset\":\"" << player.currentOpponent->assetKey << "\",\"health\":" << player.currentOpponent->health
                        << ",\"maxHealth\":" << player.currentOpponent->maxHealth << "}";
                    std::string combat_start_message = oss.str();
                    ws.write(net::buffer(combat_start_message));
                    ws.write(net::buffer("SERVER:COMBAT_LOG:You engaged the " + player.currentOpponent->type + "!"));
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

            // --- Player Turn ---
            int player_damage = 0; int mana_cost = 0; bool fled = false;
            if (action_type == "ATTACK") {
                int base_damage = std::max(1, player.stats.attack - player.currentOpponent->defense);
                float variance = 0.8f + ((float)(std::rand() % 41) / 100.0f); // 0.8 to 1.2
                player_damage = std::max(1, (int)(base_damage * variance));
                ws.write(net::buffer("SERVER:COMBAT_LOG:You attack the " + player.currentOpponent->type + " for " + std::to_string(player_damage) + " damage!"));
            }
            else if (action_type == "SPELL") {
                int base_damage = 0; mana_cost = 0; float variance = 1.0f;
                if (action_param == "Fireball") { mana_cost = 20; if (player.stats.mana >= mana_cost) { base_damage = (player.stats.maxMana / 8) + player.stats.attack; variance = 0.8f + ((float)(std::rand() % 41) / 100.0f); } }
                else if (action_param == "Lightning") { mana_cost = 15; if (player.stats.mana >= mana_cost) { base_damage = (player.stats.maxMana / 10) + player.stats.attack; variance = 0.7f + ((float)(std::rand() % 61) / 100.0f); } }
                else if (action_param == "Freeze") { mana_cost = 10; if (player.stats.mana >= mana_cost) { base_damage = (player.stats.maxMana / 12) + (player.stats.attack / 2); variance = 0.9f + ((float)(std::rand() % 21) / 100.0f); } }
                else { ws.write(net::buffer("SERVER:COMBAT_LOG:You don't know that spell!")); return; }

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
                flee_chance = std::max(0.1f, std::min(0.9f, flee_chance)); // Clamp chance
                if (((float)std::rand() / RAND_MAX) < flee_chance) { fled = true; }
                else { ws.write(net::buffer("SERVER:COMBAT_LOG:You failed to flee!")); }
            }

            // --- Check Flee ---
            if (fled) {
                ws.write(net::buffer("SERVER:COMBAT_LOG:You successfully fled from the " + player.currentOpponent->type + "!"));
                player.isInCombat = false; player.currentOpponent.reset();
                ws.write(net::buffer("SERVER:COMBAT_VICTORY:Fled"));
                send_current_monsters_list(); return;
            }

            // --- Check Monster Defeat ---
            if (player_damage > 0) { player.currentOpponent->health -= player_damage; }
            send_player_stats();
            ws.write(net::buffer("SERVER:COMBAT_UPDATE:" + std::to_string(player.currentOpponent->health)));

            if (player.currentOpponent->health <= 0) {
                ws.write(net::buffer("SERVER:COMBAT_LOG:You defeated the " + player.currentOpponent->type + "!"));
                int xp_gain = player.currentOpponent->xpReward;
                ws.write(net::buffer("SERVER:STATUS:Gained " + std::to_string(xp_gain) + " XP."));
                player.stats.experience += xp_gain;
                player.isInCombat = false; player.currentOpponent.reset();
                ws.write(net::buffer("SERVER:COMBAT_VICTORY:Defeated"));
                check_for_level_up(); send_player_stats();
                send_current_monsters_list(); return;
            }

            // --- Monster Turn ---
            int monster_damage = 0; int player_defense = player.stats.defense;
            if (player.isDefending) { player_defense *= 2; player.isDefending = false; }
            monster_damage = std::max(1, player.currentOpponent->attack - player_defense);
            player.stats.health -= monster_damage;
            ws.write(net::buffer("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " attacks you for " + std::to_string(monster_damage) + " damage!"));
            send_player_stats();

            // --- Check Player Defeat ---
            if (player.stats.health <= 0) {
                player.stats.health = 0;
                ws.write(net::buffer("SERVER:COMBAT_DEFEAT:You have been defeated!"));
                player.isInCombat = false; player.currentOpponent.reset();
                // Respawn in town
                player.currentArea = "TOWN"; player.currentMonsters.clear();
                player.stats.health = player.stats.maxHealth / 2; player.stats.mana = player.stats.maxMana;
                player.posX = 5; player.posY = 5; player.currentPath.clear();
                broadcast_data.currentArea = "TOWN"; broadcast_data.posX = player.posX; broadcast_data.posY = player.posY;
                { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }

                ws.write(net::buffer("SERVER:AREA_CHANGED:TOWN"));
                send_available_areas(); send_player_stats();
                return;
            }

            // Combat continues
            ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
        }
    }

    // --- Admin/Debug ---
    else if (message.rfind("GIVE_XP:", 0) == 0) {
        if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot gain XP in combat.")); }
        else {
            try {
                int xp_to_give = std::stoi(message.substr(8));
                if (xp_to_give > 0) {
                    player.stats.experience += xp_to_give;
                    ws.write(net::buffer("SERVER:STATUS:Gained " + std::to_string(xp_to_give) + " XP."));
                    check_for_level_up();
                    send_player_stats();
                }
                else { ws.write(net::buffer("SERVER:ERROR:Invalid XP amount.")); }
            }
            catch (const std::exception&) { ws.write(net::buffer("SERVER:ERROR:Invalid XP amount format.")); }
        }
    }

    else if (message == "REQUEST_PLAYERS") {
        // Send players only if the current area is a grid-based area
        if (g_area_grids.find(player.currentArea) == g_area_grids.end()) {
            ws.write(net::buffer("SERVER:PLAYERS_IN_AREA:[]")); // Send empty list
            return;
        }

        std::ostringstream oss;
        // Rename message for clarity (client must be updated)
        oss << "SERVER:PLAYERS_IN_AREA:[";
        bool first_player = true;
        std::string my_area = player.currentArea; // Get player's area
        {
            std::lock_guard<std::mutex> lock(g_player_registry_mutex);
            for (auto const& pair : g_player_registry) {
                if (pair.first == player.userId) continue; // Don't send self

                // The key change: check against my_area, not hardcoded "TOWN" instead of usin town like we did before we're dynamically,checking what area they are in
                //todo for mckay:: I NEED TO EVENTUALLY MAKE IT SO IT ONLY BROADCASTS/LISTENS WHEN THE SERVER KNOWS MORE THAN 1 (EXLCUDING URTSLEF) PERSON IS IN THE AREA
                if (pair.second.currentArea == my_area && pair.second.playerClass != PlayerClass::UNSELECTED) {
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
        // since buffer/websocket ops are async and we're passing it a string thats temporary meaning the buffer/websocket op has a pointer to that string in memory, but as its doing async operations that memory
        //can change and then our string gets corrupted and sends a corrupted string through to our client IF U HAVE QUUESTIONS ABOUT THIS ASK ME BECASUE IT WAS GIVING ME TROUBLE TIL I REALIZED
        //WHAT WAS HAPPENING
        std::string player_list_message = oss.str();
        ws.write(net::buffer(player_list_message));
    }

    // --- Fallback ---
    else {
        std::string echo = "SERVER:ECHO: " + message;
        ws.write(net::buffer(echo));
    }
}