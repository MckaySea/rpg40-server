#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <optional> // Use <optional> instead of <boost/optional.hpp>

// Defines which equipment slot an item can go into.
// This MUST be defined before any struct that uses it.
enum class EquipSlot : int {
    None = 0,
    Weapon = 1,
    Hat = 2,
    Top = 3,
    Bottom = 4,
    Boots = 5
};

// Defines what a randomly-generated effect on an item looks like
struct ItemEffect {
    std::string type; // e.g., "GRANT_STAT", "GRANT_SPELL", "SUFFIX"
    std::map<std::string, std::string> params;
};

// Defines the base template for an item
struct ItemDefinition {
    std::string id;
    std::string name;
    std::string description;
    std::string imagePath;
    EquipSlot equipSlot;
    bool stackable;
    int item_tier;
    std::map<std::string, int> stats; // Base stats
    std::vector<ItemEffect> effects; // Base effects (e.g., potions)
};

// The global database of all item templates
// This is extern, meaning it is *defined* in another file (GameData.cpp)
extern std::map<std::string, ItemDefinition> itemDatabase;

// Function to initialize the database
void initialize_item_database();


// Defines a unique instance of an item in a player's inventory
struct ItemInstance {
    uint64_t instanceId;
    std::string itemId;
    int quantity;
    std::map<std::string, int> customStats;
    std::vector<ItemEffect> customEffects; // Randomly rolled effects

    // Helper to get the base definition
    // This is just the DECLARATION. The DEFINITION is in GameData.cpp
    const ItemDefinition& getDefinition() const;
};

// Type aliases that depend on ItemInstance
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