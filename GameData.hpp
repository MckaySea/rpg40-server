// File: GameData.hpp
// Description: Declares global game data, registries, and utility functions.
// This is the "interface" for accessing shared game assets and systems.
#pragma once

#include "game_session.hpp" // For Point, MonsterInstance, etc.
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <deque>
#include <atomic>
#include <memory> // For std::weak_ptr
#include <set>    // --- NEW: For GRID_BASED_AREAS ---

// Forward-declare the session class to avoid circular includes
class AsyncSession;

// --- Server Data Definitions ---
// 'extern' means "this variable is defined in a .cpp file somewhere else"
extern const std::vector<std::string> ALL_AREAS;

// --- Grid Map Data ---
extern const std::vector<std::vector<int>> TOWN_GRID;
// --- NEW: Declare new grids ---
extern const std::vector<std::vector<int>> FOREST_GRID;
extern const std::vector<std::vector<int>> DESERT_GRID;

//this is our global grid registry, we'll keep all grids in hur goin forward :D
extern const std::map<std::string, std::vector<std::vector<int>>> g_area_grids;



// --- Monster Data ---
extern const std::map<std::string, std::string> MONSTER_ASSETS;
extern const std::map<std::string, MonsterInstance> MONSTER_TEMPLATES;
extern const std::vector<std::string> MONSTER_KEYS;
extern int global_monster_id_counter;

// --- Multiplayer Registries ---
// These are shared by all sessions, so they must be thread-safe.
extern std::map<std::string, PlayerBroadcastData> g_player_registry;
extern std::mutex g_player_registry_mutex;

extern std::map<std::string, std::weak_ptr<AsyncSession>> g_session_registry;
extern std::mutex g_session_registry_mutex;

// --- Global ID Counter ---
extern std::atomic<int> g_session_id_counter;

// --- Utility Function Declarations ---

/**
 * @brief Finds the shortest walkable path from start to end on a given grid.
 * @param start The starting grid coordinate.
 * @param end The target grid coordinate.
 * @param grid The collision grid (e.g., TOWN_GRID) to pathfind on.
 * @return A deque of Points representing the path. Empty if no path found.
 */
 
std::deque<Point> A_Star_Search(Point start, Point end, const std::vector<std::vector<int>>& grid); // i made it so u can pass in what grid instead oif it jus being town

/**
 * @brief Gets the base stats for a chosen player class.
 * @param playerClass The class (FIGHTER, WIZARD, ROGUE).
 * @return A PlayerStats object.
 */
PlayerStats getStartingStats(PlayerClass playerClass);

/**
 * @brief Creates a full MonsterInstance from a template.
 * @param id The unique ID for this new monster.
 * @param type The monster's template key (e.g., "SLIME").
 * @return A MonsterInstance object.
 */
MonsterInstance create_monster(int id, std::string type);