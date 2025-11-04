// File: LocalWebSocketServer/game_session.cpp
//
// Additions:
// 1. New function `check_for_level_up` to handle leveling logic.
// 2. Updated `send_player_stats` to include level and XP.
// 3. Added debug command `GIVE_XP:` to test leveling.
// 4. ADDED: Combat logic, MonsterInstance, and combat handlers.
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
#include <map> // ADDED

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- Server Data Definitions ---
static const std::vector<std::string> ALL_AREAS = {
    "FOREST", "CAVES", "RUINS", "SWAMP", "MOUNTAINS", "DESERT", "VOLCANO"
};

// Monster Templates (Type, AssetKey)
static const std::map<std::string, std::string> MONSTER_ASSETS = {
    {"SLIME", "SLM"},
    {"GOBLIN", "GB"},
    {"WOLF", "WLF"},
    {"BAT", "BAT"},
    {"SKELETON", "SKL"},
    {"GIANT SPIDER", "SPDR"},
    {"ORC BRUTE", "ORC"}
};

// ADDED: Monster Base Stats (Type -> Stats)
// Using MonsterInstance constructor: (id, type, asset, hp, atk, def, spd, xp)
// ID, Type, and Asset are set dynamically.
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

// ADDED: Create a monster instance from a template
MonsterInstance create_monster(int id, std::string type) {
    MonsterInstance monster = MONSTER_TEMPLATES.at(type); // Get base stats
    monster.id = id;
    monster.type = type;
    monster.assetKey = MONSTER_ASSETS.at(type);
    // TODO: Add level-based scaling
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
        if (!area_list.empty()) {
            area_list += ",";
        }
        area_list += areas[i];
    }

    std::string response = "SERVER:AREAS:" + area_list;
    ws.write(net::buffer(response));
}

// ADDED: Helper to send the current monster list
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
        << "{\"health\":" << player.stats.health
        << ",\"maxHealth\":" << player.stats.maxHealth // ADDED
        << ",\"mana\":" << player.stats.mana
        << ",\"maxMana\":" << player.stats.maxMana // ADDED
        << ",\"attack\":" << player.stats.attack
        << ",\"defense\":" << player.stats.defense
        << ",\"speed\":" << player.stats.speed
        << ",\"level\":" << player.stats.level
        << ",\"experience\":" << player.stats.experience
        << ",\"experienceToNextLevel\":" << player.stats.experienceToNextLevel
        << ",\"availableSkillPoints\":" << player.availableSkillPoints;

    // ADDED: Include spells for wizards
    if (player.currentClass == PlayerClass::WIZARD && !player.spells.empty()) {
        oss << ",\"spells\":[";
        for (size_t i = 0; i < player.spells.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "\"" << player.spells[i] << "\"";
        }
        oss << "]";
    }
    else {
        oss << ",\"spells\":[]"; // Always include spells array
    }

    oss << "}";

    ws.write(net::buffer(oss.str()));
}

// --- Level Up Handler ---
void check_for_level_up(websocket::stream<tcp::socket>& ws, PlayerState& player) {
    // Check if player has enough XP to level up, loop in case of multiple levels
    while (player.stats.experience >= player.stats.experienceToNextLevel) {
        player.stats.level++;
        player.stats.experience -= player.stats.experienceToNextLevel;
        // Increase XP required for next level (e.g., 50% more)
        player.stats.experienceToNextLevel = static_cast<int>(player.stats.experienceToNextLevel * 1.5);
        player.availableSkillPoints += 3;

        // ADDED: Stat boosts on level up
        player.stats.maxHealth += 10;
        player.stats.health = player.stats.maxHealth; // Full heal
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
    // Note: The calling function is responsible for sending the final updated stats.
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

            // --- Check for non-combat commands first ---

            // Step 1: Set Player Name
            if (message.rfind("SET_NAME:", 0) == 0 && player.playerName.empty()) {
                std::string name = message.substr(9);

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
                // ... (omitted: same as your file)
                std::string class_str = message.substr(13);

                if (class_str == "FIGHTER") {
                    player.currentClass = PlayerClass::FIGHTER;
                }
                else if (class_str == "WIZARD") {
                    player.currentClass = PlayerClass::WIZARD;
                    player.spells = { "Fireball", "Lightning", "Freeze" };
                }
                else if (class_str == "ROGUE") {
                    player.currentClass = PlayerClass::ROGUE;
                }
                else {
                    ws.write(net::buffer("SERVER:ERROR:Invalid class. Choose FIGHTER, WIZARD, or ROGUE."));
                    buffer.consume(buffer.size());
                    continue;
                }

                player.stats = getStartingStats(player.currentClass);
                player.availableSkillPoints = 3;
                player.hasSpentInitialPoints = false;
                std::cout << "[" << client_address << "] --- CLASS SET: " << class_str << " ---\n";
                std::string response = "SERVER:CLASS_SET:" + class_str;
                ws.write(net::buffer(response));
                send_player_stats(ws, player);
                std::string prompt = "SERVER:PROMPT:You have 3 skill points to distribute. Use UPGRADE_STAT:stat_name to spend points.";
                ws.write(net::buffer(prompt));
            }
            // Step 3: Upgrade Stats
            else if (message.rfind("UPGRADE_STAT:", 0) == 0) {
                // ... (omitted: same as your file, but with maxHealth/maxMana)
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
                    player.stats.maxHealth += 5;
                    player.stats.health += 5; // Also increase current health
                    valid_stat = true;
                }
                else if (stat_name == "mana") {
                    player.stats.maxMana += 5;
                    player.stats.mana += 5; // Also increase current mana
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
                    send_player_stats(ws, player); // Send updated stats

                    if (player.availableSkillPoints == 0 && !player.hasSpentInitialPoints) {
                        player.hasSpentInitialPoints = true;
                        player.isFullyInitialized = true;
                        std::string complete = "SERVER:CHARACTER_COMPLETE:Character creation complete! You can now explore.";
                        ws.write(net::buffer(complete));
                        send_available_areas(ws);
                    }
                    else if (player.availableSkillPoints > 0) {
                        std::string remaining = "SERVER:PROMPT:You have " + std::to_string(player.availableSkillPoints) + " skill points remaining.";
                        ws.write(net::buffer(remaining));
                    }
                    else {
                        std::string remaining = "SERVER:STATUS:All skill points spent.";
                        ws.write(net::buffer(remaining));
                    }
                }
                else {
                    ws.write(net::buffer("SERVER:ERROR:Invalid stat name."));
                }
            }
            // Area Travel (only if not in combat)
            else if (message.rfind("GO_TO:", 0) == 0) {
                if (!player.isFullyInitialized) {
                    ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
                }
                else if (player.isInCombat) { // ADDED
                    ws.write(net::buffer("SERVER:ERROR:Cannot travel while in combat!"));
                }
                else {
                    std::string target_area = message.substr(6);
                    if (target_area == "TOWN") {
                        player.currentArea = "TOWN";
                        player.currentMonsters.clear();
                        // ADDED: Heal player in town
                        player.stats.health = player.stats.maxHealth;
                        player.stats.mana = player.stats.maxMana;
                        std::string response = "SERVER:AREA_CHANGED:TOWN";
                        ws.write(net::buffer(response));
                        send_available_areas(ws);
                        send_player_stats(ws, player); // Send updated (healed) stats
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
            }
            // Monster Selection (Starts Combat)
            else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
                if (!player.isFullyInitialized) {
                    ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
                }
                else if (player.isInCombat) { // ADDED
                    ws.write(net::buffer("SERVER:ERROR:You are already in combat!"));
                }
                else if (player.currentArea == "TOWN") {
                    ws.write(net::buffer("SERVER:STATUS:No monsters to fight in TOWN."));
                }
                else {
                    try {
                        int selected_id = std::stoi(message.substr(17));
                        auto it = std::find_if(player.currentMonsters.begin(), player.currentMonsters.end(),
                            [selected_id](const MonsterState& m) { return m.id == selected_id; });

                        if (it != player.currentMonsters.end()) {
                            // --- START COMBAT ---
                            player.isInCombat = true;
                            player.currentOpponent = create_monster(it->id, it->type);
                            player.isDefending = false;

                            // Remove monster from area list
                            player.currentMonsters.erase(it);

                            std::cout << "[" << client_address << "] --- COMBAT STARTED vs " << player.currentOpponent->type << " ---\n";

                            // Send combat start message with monster data
                            std::ostringstream oss;
                            oss << "SERVER:COMBAT_START:"
                                << "{\"id\":" << player.currentOpponent->id
                                << ",\"type\":\"" << player.currentOpponent->type
                                << "\",\"asset\":\"" << player.currentOpponent->assetKey
                                << "\",\"health\":" << player.currentOpponent->health
                                << ",\"maxHealth\":" << player.currentOpponent->maxHealth
                                << "}";
                            ws.write(net::buffer(oss.str()));

                            ws.write(net::buffer("SERVER:COMBAT_LOG:You engaged the " + player.currentOpponent->type + "!"));
                            ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
                        }
                        else {
                            ws.write(net::buffer("SERVER:ERROR:Selected monster ID not found. It might have been defeated."));
                        }
                    }
                    catch (const std::exception&) {
                        ws.write(net::buffer("SERVER:ERROR:Invalid monster ID format."));
                    }
                }
            }
            // --- ADDED: COMBAT ACTION HANDLER ---
            else if (message.rfind("COMBAT_ACTION:", 0) == 0) {
                if (!player.isInCombat || !player.currentOpponent) {
                    ws.write(net::buffer("SERVER:ERROR:You are not in combat."));
                    buffer.consume(buffer.size());
                    continue;
                }

                std::string action_command = message.substr(14);
                std::string action_type;
                std::string action_param;

                size_t colon_pos = action_command.find(':');
                if (colon_pos != std::string::npos) {
                    action_type = action_command.substr(0, colon_pos);
                    action_param = action_command.substr(colon_pos + 1);
                }
                else {
                    action_type = action_command;
                }

                // --- 1. Player Turn ---
                int player_damage = 0;
                int mana_cost = 0;
                bool fled = false;

                if (action_type == "ATTACK") {
                    // ADDED: Basic randomness to attack (80% - 120%)
                    int base_damage = std::max(1, player.stats.attack - player.currentOpponent->defense);
                    float variance = 0.8f + ((float)(std::rand() % 41) / 100.0f); // 0.80 to 1.20
                    player_damage = std::max(1, (int)(base_damage * variance));

                    ws.write(net::buffer("SERVER:COMBAT_LOG:You attack the " + player.currentOpponent->type + " for " + std::to_string(player_damage) + " damage!"));
                }
                // --- MODIFIED: SPELL BLOCK ---
                else if (action_type == "SPELL") {

                    int base_damage = 0;
                    mana_cost = 0;
                    float variance = 1.0f;

                    if (action_param == "Fireball") {
                        mana_cost = 20;
                        if (player.stats.mana >= mana_cost) {
                            // High damage formula
                            base_damage = (player.stats.maxMana / 8) + player.stats.attack;
                            // Variance: 80% to 120%
                            variance = 0.8f + ((float)(std::rand() % 41) / 100.0f);
                        }
                    }
                    else if (action_param == "Lightning") {
                        mana_cost = 15;
                        if (player.stats.mana >= mana_cost) {
                            // Medium damage, high variance formula
                            base_damage = (player.stats.maxMana / 10) + player.stats.attack;
                            // Variance: 70% to 130%
                            variance = 0.7f + ((float)(std::rand() % 61) / 100.0f);
                        }
                    }
                    else if (action_param == "Freeze") {
                        mana_cost = 10;
                        if (player.stats.mana >= mana_cost) {
                            // Low damage, low variance formula
                            base_damage = (player.stats.maxMana / 12) + (player.stats.attack / 2);
                            // Variance: 90% to 110%
                            variance = 0.9f + ((float)(std::rand() % 21) / 100.0f);
                        }
                    }
                    else {
                        // Unknown spell
                        ws.write(net::buffer("SERVER:COMBAT_LOG:You don't know that spell!"));
                        buffer.consume(buffer.size()); // Stop processing this turn
                        continue;
                    }

                    // Now check mana and apply damage
                    if (mana_cost > 0 && player.stats.mana >= mana_cost) {
                        player.stats.mana -= mana_cost;
                        player_damage = std::max(1, (int)(base_damage * variance));
                        ws.write(net::buffer("SERVER:COMBAT_LOG:You cast " + action_param + " for " + std::to_string(player_damage) + " damage!"));
                    }
                    else if (mana_cost > 0) {
                        // Not enough mana
                        ws.write(net::buffer("SERVER:COMBAT_LOG:Not enough mana to cast " + action_param + "! (Needs " + std::to_string(mana_cost) + ")"));
                    }
                    else {
                        // This branch is hit if mana_cost is 0 (e.g., failed check)
                        ws.write(net::buffer("SERVER:COMBAT_LOG:Cannot cast " + action_param + "."));
                    }
                }
                else if (action_type == "DEFEND") {
                    player.isDefending = true;
                    ws.write(net::buffer("SERVER:COMBAT_LOG:You brace for the next attack."));
                }
                else if (action_type == "FLEE") {
                    float flee_chance = 0.5f + ((float)player.stats.speed - (float)player.currentOpponent->speed) * 0.05f;
                    if (flee_chance < 0.1f) flee_chance = 0.1f;
                    if (flee_chance > 0.9f) flee_chance = 0.9f;

                    if (((float)std::rand() / RAND_MAX) < flee_chance) {
                        fled = true;
                    }
                    else {
                        ws.write(net::buffer("SERVER:COMBAT_LOG:You failed to flee!"));
                    }
                }

                // Apply player action
                if (fled) {
                    ws.write(net::buffer("SERVER:COMBAT_LOG:You successfully fled from the " + player.currentOpponent->type + "!"));
                    player.isInCombat = false;
                    player.currentOpponent.reset();
                    ws.write(net::buffer("SERVER:COMBAT_VICTORY:Fled")); // Use VICTORY to close UI
                    send_current_monsters_list(ws, player); // Send updated monster list
                    buffer.consume(buffer.size());
                    continue;
                }

                if (player_damage > 0) {
                    player.currentOpponent->health -= player_damage;
                }

                // Send updated stats (for mana cost)
                send_player_stats(ws, player);
                // Send monster health update
                ws.write(net::buffer("SERVER:COMBAT_UPDATE:" + std::to_string(player.currentOpponent->health)));


                // --- 2. Check Monster Status ---
                if (player.currentOpponent->health <= 0) {
                    ws.write(net::buffer("SERVER:COMBAT_LOG:You defeated the " + player.currentOpponent->type + "!"));
                    int xp_gain = player.currentOpponent->xpReward;
                    ws.write(net::buffer("SERVER:STATUS:Gained " + std::to_string(xp_gain) + " XP."));

                    player.stats.experience += xp_gain;

                    // Reset combat state
                    player.isInCombat = false;
                    player.currentOpponent.reset();

                    ws.write(net::buffer("SERVER:COMBAT_VICTORY:Defeated")); // Tell client combat is over

                    check_for_level_up(ws, player);
                    send_player_stats(ws, player); // Send final stats
                    send_current_monsters_list(ws, player); // Send updated monster list

                    buffer.consume(buffer.size());
                    continue;
                }

                // --- 3. Monster Turn ---
                int monster_damage = 0;
                int player_defense = player.stats.defense;
                if (player.isDefending) {
                    player_defense *= 2; // Double defense if defending
                    player.isDefending = false; // Reset defend state
                }

                monster_damage = std::max(1, player.currentOpponent->attack - player_defense);
                player.stats.health -= monster_damage;

                ws.write(net::buffer("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " attacks you for " + std::to_string(monster_damage) + " damage!"));
                send_player_stats(ws, player); // Send updated player health

                // --- 4. Check Player Status ---
                if (player.stats.health <= 0) {
                    player.stats.health = 0;
                    ws.write(net::buffer("SERVER:COMBAT_DEFEAT:You have been defeated!"));

                    // Reset combat
                    player.isInCombat = false;
                    player.currentOpponent.reset();

                    // Send to town to recover
                    player.currentArea = "TOWN";
                    player.currentMonsters.clear();
                    player.stats.health = player.stats.maxHealth / 2; // Recover 50% HP
                    player.stats.mana = player.stats.maxMana;

                    ws.write(net::buffer("SERVER:AREA_CHANGED:TOWN"));
                    send_available_areas(ws);
                    send_player_stats(ws, player); // Send updated (healed) stats

                    buffer.consume(buffer.size());
                    continue;
                }

                // --- 5. Next Turn ---
                ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
            }
            // DEBUG: Give XP
            else if (message.rfind("GIVE_XP:", 0) == 0) {
                // ... (omitted: same as your file)
                if (!player.isFullyInitialized) {
                    ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
                }
                else if (player.isInCombat) {
                    ws.write(net::buffer("SERVER:ERROR:Cannot gain XP in combat."));
                }
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
                        else {
                            ws.write(net::buffer("SERVER:ERROR:Invalid XP amount."));
                        }
                    }
                    catch (const std::exception&) {
                        ws.write(net::buffer("SERVER:ERROR:Invalid XP amount format."));
                    }
                }
            }
            // Echo/Debug
            else {
                std::string echo = "SERVER:ECHO: " + message;
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