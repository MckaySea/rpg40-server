// File: game_session.hpp
// Description: Defines all core game data structures and constants.
// This file is the "single source of truth" for what game objects look like.
#pragma once 

#include <string>
#include <vector>
#include <deque> 
#include <chrono> 
#include <boost/optional.hpp> // Used for currentOpponent
#include <boost/asio/ip/tcp.hpp> // For basic networking types

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


    PlayerStats() = default;

    //changed and added more stats
    PlayerStats(int h, int m, int def, int spd, int str, int dex, int intl, int lck)
        : health(h), maxHealth(h), mana(m), maxMana(m), defense(def), speed(spd),
        level(1), experience(0), experienceToNextLevel(100),
        strength(str), dexterity(dex), intellect(intl), luck(lck) {
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

    MonsterInstance(int id, std::string type, std::string assetKey, int h, int def, int spd, int str, int dex, int intl, int lck, int xp)
        : id(id), type(type), assetKey(assetKey), health(h), maxHealth(h),
        defense(def), speed(spd), xpReward(xp),
        strength(str), dexterity(dex), intellect(intl), luck(lck) {
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

    // Combat State
    bool isInCombat = false;
    // We use boost::optional because a player may not be in combat
    boost::optional<MonsterInstance> currentOpponent;
    bool isDefending = false;

    // Movement State
    std::deque<Point> currentPath; // The A* path
    std::chrono::steady_clock::time_point lastMoveTime; // Timer
};