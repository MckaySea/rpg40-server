// File: game_session.hpp
// Description: Defines all core game data structures and constants.
// This file is the "single source of truth" for what game objects look like.
#pragma once 

#include <string>
#include <vector>
#include <deque> 
#include <chrono> 
#include <map>
#include <boost/optional.hpp> // Used for currentOpponent
#include <boost/asio/ip/tcp.hpp> // For basic networking types
#include <optional>  
#include <atomic>     
#include "Items.hpp"  
namespace net = boost::asio;
using tcp = net::ip::tcp;

// --- Global Game Constants ---
static const int GRID_COLS = 40; // Width of the town map
static const int GRID_ROWS = 22; // Height of the town map
static const std::chrono::milliseconds MOVEMENT_DELAY{ 150 }; // ms per tile
static const int SERVER_TICK_RATE_MS = 50; // How often the server "ticks" (20 ticks/sec)
extern std::atomic<uint64_t> g_item_instance_id_counter;






struct ItemInstance {
    uint64_t instanceId;         // EVERY ITEM WLL HAVE A UNIQUE ID FOR  A VRY SPECIAL REASON XD
    std::string itemId;          // name of the item basically
    int quantity;

    // every item has a unique id so we can roll variance to items that would otherwise be the same, so u could get multiple of the same item names but all have different worse n better stats
    std::map<std::string, int> customStats;

    // this is to get the actual items (without modifiers or anything basically the base versionns of em
    const ItemDefinition& getDefinition() const {
        return itemDatabase.at(itemId); 
    }
};


using Inventory = std::map<uint64_t, ItemInstance>; 


struct Equipment {

    // Maps a specific equipment slot to the unique instanceId of the item
    std::map<EquipSlot, std::optional<uint64_t>> slots;

    Equipment() {
        slots[EquipSlot::Weapon] = std::nullopt;
        slots[EquipSlot::Hat] = std::nullopt;
        slots[EquipSlot::Top] = std::nullopt;
        slots[EquipSlot::Bottom] = std::nullopt;
        slots[EquipSlot::Boots] = std::nullopt;
    }

    std::optional<uint64_t> getEquippedItemId(EquipSlot slot) const {
        if (slots.count(slot)) {
            return slots.at(slot);
        }
        return std::nullopt;
    }
};
    
    
    /**
* 
* 
* 
* 
 * @enum PlayerClass
 * @brief Represents the character class choices.
 */
enum class PlayerClass {
    UNSELECTED,
    FIGHTER,
    WIZARD,
    ROGUE
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
    std::string id;         // Unique ID, e.g., "TOWN_MERCHANT"
    InteractableType type;  // What kind of object this is
    Point position;         // The (x, y) grid coordinate
    std::string data;       // Flexible data (e.g., Dialogue ID, Shop ID, or Target Area Name)
};
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
    std::vector<std::string> spells;
    int availableSkillPoints = 0;
    bool hasSpentInitialPoints = false;
    bool isFullyInitialized = false;


    Inventory inventory;
    Equipment equipment;


    // Combat State
    bool isInCombat = false;
    // We use boost::optional because a player may not be in combat
    boost::optional<MonsterInstance> currentOpponent;
    bool isDefending = false;

    // Movement State
    std::deque<Point> currentPath; // The A* path
    std::chrono::steady_clock::time_point lastMoveTime; // Timer
};