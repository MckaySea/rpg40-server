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
#include <set>  
#include <cstdint> // For uint64_t// --- NEW: For GRID_BASED_AREAS ---
#include "DatabaseManager.hpp"
#include "AreaData.hpp"
#include "GameData.hpp"
#include <cassert>
#include <unordered_map> 
#include <chrono>
// Forward-declare the session class to avoid circular includes
class AsyncSession;



// --- Server Data Definitions ---
// 'extern' means "this variable is defined in a .cpp file somewhere else"
extern const std::vector<std::string> ALL_AREAS;

// --- Grid Map Data ---
extern const std::vector<std::vector<int>> OVERWORLD_GRID;
extern const std::vector<std::vector<int>> TOWN_GRID;
extern const std::vector<std::vector<int>> FOREST_GRID;
extern const std::vector<std::vector<int>> DESERT_GRID;
extern const std::vector<std::vector<int>> CAVES_GRID;
extern const std::vector<std::vector<int>> VOLCANO_GRID;
extern const std::vector<std::vector<int>> LAKE_GRID;
extern const std::vector<std::vector<int>> CASTLEINSIDE_GRID;
//this is our global grid registry, we'll keep all grids in hur goin forward :D
extern const std::map<std::string, std::vector<std::vector<int>>> g_area_grids;
extern std::unordered_map<std::string, AreaData> g_areas;
extern std::atomic<uint64_t> g_item_instance_id_counter;
extern std::map<std::string, std::vector<std::string>> g_effect_suffix_pools;
// This is what holds all interactable objects for each area (map/zone/grid).
extern const std::map<std::string, std::vector<InteractableObject>> g_interactable_objects;
//dialogue stuff

struct DialogueLine {
	std::string speaker;
	std::string text;
	std::string portraitKey; // e.g. "MAYOR", "HUNTER", maps to a PNG on client
};

extern const std::map<std::string, std::vector<DialogueLine>> g_dialogues;

// --- Monster Data ---
extern std::map<std::string, std::shared_ptr<Party>> g_parties;
extern std::mutex g_parties_mutex;
extern const std::map<std::string, std::string> MONSTER_ASSETS;
extern const std::map<std::string, MonsterInstance> MONSTER_TEMPLATES;
extern const std::vector<std::string> MONSTER_KEYS;
int calculateItemSellPrice(const ItemInstance& instance, const ItemDefinition& def);
extern int global_monster_id_counter;
//we're just mapping items to a sell price (which will be whatever tier of gear theyre in)
extern std::map<std::string, int> g_item_buy_prices;
extern std::atomic<uint64_t> g_item_instance_id_counter;
extern std::map<std::string, PlayerBroadcastData> g_player_registry;
extern std::mutex g_player_registry_mutex;

extern std::map<std::string, std::weak_ptr<AsyncSession>> g_session_registry;
extern std::mutex g_session_registry_mutex;
extern std::map<std::string, std::shared_ptr<TradeSession>> g_active_trades;
extern std::mutex g_active_trades_mutex;
// --- NEW: Definition for a rollable effect ---
// This struct links a cosmetic suffix pool (e.g., "FIRE_EFFECT")
// to a tangible gameplay mechanic (e.g., +5 strength) and a rarity.
struct RandomEffectDefinition {
	std::string effect_key; // Links to a suffix pool, e.g., "FIRE_EFFECT"
	ItemEffect gameplay_effect; // The actual effect, e.g., {"GRANT_STAT", {{"stat", "strength"}, {"value", "5"}}}
	int rarity_weight; // How common? (Higher = more common, e.g., 100)
	int power_level;   // Tier of the effect (1-5)
};

enum class SkillTarget {
	SELF,
	ENEMY
};

enum class SkillClass {
	ANY,
	WARRIOR,
	ROGUE,
	WIZARD
};

// NEW: explicitly distinguishes between spell-like and physical abilities
enum class SkillType {
	SPELL,
	ABILITY
};

struct SkillDefinition {
	std::string name;
	SkillClass requiredClass = SkillClass::ANY;
	SkillType type = SkillType::ABILITY; // default to ABILITY unless defined otherwise
	int manaCost = 0;
	int cooldownTurns = 0;
	SkillTarget target = SkillTarget::ENEMY;

	// Stat scaling
	float strScale = 0.0f;
	float dexScale = 0.0f;
	float intScale = 0.0f;
	float flatDamage = 0.0f;

	// Status effect
	bool appliesStatus = false;
	StatusType statusType = StatusType::NONE;
	int statusMagnitude = 0;
	int statusDuration = 0;

	bool isDefensive = false;
	bool isMagic = false;
	bool autoGranted = false;
};
struct SpawnPoint
{
	int x;
	int y;
};

struct ResourceDefinition {
	LifeSkillType skill;     // e.g. WOODCUTTING
	int requiredLevel;       // Level needed to gather
	int xpReward;            // XP gained per gather
	std::string dropItemId;  // The standard drop
	int dropChance;          // % chance (0-100)
	std::string rareItemId;  // Rare drop (optional)
	int rareChance;          // % chance for rare drop
};

struct CraftingRecipe {
	std::string resultItemId;
	int quantityCreated;
	std::string requiredSkill; // e.g. "Crafting"
	int requiredLevel;
	std::map<std::string, int> ingredients; // ItemID -> Quantity needed
	int xpReward;
};

// Global Registries
extern std::map<std::string, ResourceDefinition> g_resource_defs;
extern std::map<std::string, CraftingRecipe> g_crafting_recipes;
extern std::unordered_map<std::string, SkillDefinition> g_skill_defs;
extern std::unordered_map<std::string, SkillDefinition> g_monster_spell_defs;
// Declaration only – no initializer here
const std::unordered_map<std::string, SpawnPoint>& get_area_spawns();
// Global registry of all skills by name (e.g. "Ignite", "BloodStrike")
extern std::unordered_map<std::string, SkillDefinition> g_skill_defs;

// Initializes all skills. Call this during server startup.
void initialize_skill_definitions();


std::shared_ptr<AsyncSession> get_session_by_id(const std::string& userId);
void cleanup_trade_session(std::string playerAId, std::string playerBId);
// --- END NEW ---
//maps a shop id to a list of item ids it sells for the shops imma make alterable later on so we can have different shops sell different stuff
extern const std::map<std::string, std::vector<std::string>> g_shops;
//maps a loot tier to a list of item ids that can drop from that tier, way easier than going through all the items i made lol
extern const std::map<std::string, std::vector<std::string>> g_loot_tables_by_tier;
// --- NEW: Master list of all random effects ---
extern std::vector<RandomEffectDefinition> g_random_effect_pool;
extern std::atomic<int> g_session_id_counter;

// --- Utility Function Declarations ---
void initialize_item_prices();
void initialize_item_id_counter(DatabaseManager& db_manager);
void initialize_suffix_pools();
void initialize_monster_spell_definitions();
void initialize_random_effect_pool();
void initializeAreas();

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
std::optional<MonsterInstance> create_monster(int id, const std::string& type);

Point find_random_spawn_point(const AreaData& area);

void broadcast_monster_despawn(const std::string& areaName, int spawn_id, const std::string& exclude_user_id);

void broadcast_monster_list(const std::string& areaName); // <-- ADD THIS
void broadcast_party_update(std::shared_ptr<Party> party);
void respawn_monster_immediately(const std::string& areaName, int spawn_id);
void check_party_timeouts();