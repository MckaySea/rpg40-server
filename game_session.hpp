// File: game_session.hpp
// Description: Defines all core game data structures and constants.
// This file is the "single source of truth" for what game objects look like.
#pragma once 

#include <string>
#include <vector>
#include <deque> 
#include <chrono> 
#include <map>
#include <memory> // For std::unique_ptr
#include <optional>  
#include <atomic>   
#include <boost/asio/ip/tcp.hpp> // For basic networking types
#include "Items.hpp"  // <-- Includes ItemInstance, Inventory, Equipment, etc.

namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- Global Game Constants ---
static const int GRID_COLS = 40; // Width of the town map
static const int GRID_ROWS = 22; // Height of the town map
static const std::chrono::milliseconds MOVEMENT_DELAY{ 150 }; // ms per tile
static const int SERVER_TICK_RATE_MS = 50; // How often the server "ticks" (20 ticks/sec)


/**
* @enum PlayerClass
* @brief Represents the character class choices.
*/
enum class PlayerClass : int {
    UNSELECTED = 0,
    FIGHTER = 1,
    WIZARD = 2,
    ROGUE = 3
};
enum class StatusType {
    NONE = 0,
    BURN,
    BLEED,
    DEFENSE_UP,
    ATTACK_UP,
    SPEED_UP,
    SPEED_DOWN,
    STUN
};

struct StatusEffect {
    StatusType type = StatusType::NONE;
    int remainingTurns = 0;   // how many turns left
    int magnitude = 0;        // strength of the effect (damage per turn, bonus DEF, etc.)
    bool appliedByPlayer = false;
};

/**
 * @struct PlayerBroadcastData
 * @brief A lightweight struct containing only the data needed
 * to show other players in the "TOWN" area.
 */
struct PlayerBroadcastData {
    std::string userId;
    std::string playerName;
    PlayerClass playerClass = PlayerClass::UNSELECTED;
    std::string currentArea = "TOWN";
    int posX = 0;
    int posY = 0;
};

//i removed attack to make more interesting encounters based on different stats and make some fights harder! Added int,dex,str,luck
struct PlayerStats {
    int health = 0, maxHealth = 0;
    int mana = 0, maxMana = 0;
    int defense = 0;
    int speed = 0;
    int level = 1, experience = 0, experienceToNextLevel = 100;

    int strength = 0;
    int dexterity = 0;
    int intellect = 0;
    int luck = 0;
    int gold = 0;

    PlayerStats() = default;

    //changed and added more stats
    PlayerStats(int h, int m, int def, int spd, int str, int dex, int intl, int lck)
        : health(h), maxHealth(h), mana(m), maxMana(m), defense(def), speed(spd),
        level(1), experience(0), experienceToNextLevel(100),
        strength(str), dexterity(dex), intellect(intl), luck(lck), gold(10) {
    }
};

/**
 * @struct MonsterState
 * @brief A lightweight struct representing a monster "in the world"
 * before combat starts.
 */
struct MonsterState {
    int id;
    std::string type;
    std::string assetKey;
    int posX = 0;
    int posY = 0;
};

//mobs now also have defensde,speed, str, dex, int, and luck. think imma add spell casting eventually so theres harder fights
struct MonsterInstance {
    int id;
    std::string type;
    std::string assetKey;
    int health, maxHealth;
    int defense;
    int speed;
    int xpReward;
    int strength;
    int dexterity;
    int intellect;
    int luck;
    int lootTier;
    int dropChance; //imma make use of these as 0-100 representing percentage chance to drop loot from mobs i think
    std::vector<StatusEffect> activeStatusEffects;
    MonsterInstance(int id, std::string type, std::string assetKey, int h, int def, int spd, int str, int dex, int intl, int lck, int xp, int lTier, int dChance)
        : id(id), type(type), assetKey(assetKey), health(h), maxHealth(h),
        defense(def), speed(spd), xpReward(xp),
        strength(str), dexterity(dex), intellect(intl), luck(lck),
        lootTier(lTier), dropChance(dChance) {
    }
};

/**
 * @struct Point
 * @brief A simple 2D coordinate for grid movement.
 */
struct Point {
    int x, y;
    bool operator==(const Point& other) const {
        return x == other.x && y == other.y;
    }
    bool operator!=(const Point& other) const {
        return !(*this == other);
    }
    // Added for use in std::map if needed
    bool operator<(const Point& other) const {
        return std::tie(x, y) < std::tie(other.x, other.y);
    }
};

//we can add to this eventually
enum class InteractableType {
    NPC,
    ZONE_TRANSITION,
    QUEST_ITEM,
    SHOP
};

//definin what we can interact with come back and look at this or ask me (McKay for help with creeating new ones)
struct InteractableObject {
    std::string id;      // Unique ID, e.g., "TOWN_MERCHANT"
    InteractableType type;  // What kind of object this is
    Point position;      // The (x, y) grid coordinate
    std::string data;      // Flexible data (e.g., Dialogue ID, Shop ID, or Target Area Name)
};

// --- NEW: Struct to hold all skill data from the 'skills' JSONB column ---
struct PlayerSkills {
    // For permanently learned spells
    std::vector<std::string> spells;

    // For "Woodcutting: 10", "Fishing: 5", etc.
    std::map<std::string, int> life_skills;
};
// --- END NEW ---


/**
 * @struct PlayerState
 * @brief The complete state for a single connected player.
 * This is the main "game state" object for a session.
 */
struct PlayerState {
    PlayerClass currentClass = PlayerClass::UNSELECTED;
    std::string userId = "UNKNOWN";
    std::string playerName = "";
    std::string currentArea = "TOWN";

    int posX = 0;
    int posY = 0;

    // List of monsters available to fight in the current area
    std::vector<MonsterState> currentMonsters;

    // Stats and progression
    PlayerStats stats;

    // --- MODIFIED: Replaced old 'spells' vector ---
    // Holds data loaded from the DB (learned spells, life skills)
    PlayerSkills skills;
    // Holds the *runtime* list of spells (learned + item-granted)
    // This is populated by getCalculatedStats()
    std::vector<std::string> temporary_spells_list;
    // --- END MODIFIED ---

    int availableSkillPoints = 0;
    bool hasSpentInitialPoints = false;
    bool isFullyInitialized = false;

    // --- These structs are now included from Items.hpp ---
    Inventory inventory;
    Equipment equipment;
    // --- ---

    // Combat State
    bool isInCombat = false;
	//using optional to cyheck if there's a current opponent
    std::optional<MonsterInstance> currentOpponent;
    bool isDefending = false;

    std::vector<StatusEffect> activeStatusEffects;

    // Movement State
    std::deque<Point> currentPath; // The A* path
    std::chrono::steady_clock::time_point lastMoveTime; // Timer
};