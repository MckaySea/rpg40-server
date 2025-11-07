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

/**
 * @struct PlayerStats
 * @brief Holds all combat and progression stats for a player.
 */
struct PlayerStats {
    int health = 0;
    int maxHealth = 0;
    int mana = 0;
    int maxMana = 0;
    int attack = 0;
    int defense = 0;
    int speed = 0;
    int level = 1;
    int experience = 0;
    int experienceToNextLevel = 100;

    PlayerStats() = default;
    PlayerStats(int h, int m, int a, int d, int s, int l = 1, int xp = 0, int nextXp = 100)
        : health(h), maxHealth(h), mana(m), maxMana(m), attack(a), defense(d), speed(s),
        level(l), experience(xp), experienceToNextLevel(nextXp)
    {
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

/**
 * @struct MonsterInstance
 * @brief A full monster object, created when combat begins.
 * Includes health and stats.
 */
struct MonsterInstance {
    int id;
    std::string type;
    std::string assetKey;
    int health;
    int maxHealth;
    int attack;
    int defense;
    int speed;
    int xpReward;

    MonsterInstance(int i, std::string t, std::string a, int h, int atk, int def, int spd, int xp)
        : id(i), type(t), assetKey(a), health(h), maxHealth(h), attack(atk), defense(def), speed(spd), xpReward(xp) {
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