#pragma once 

#include <string>
#include <map>
#include <vector>

// Enum for equipment slots
enum class EquipSlot {
    None,
    Weapon,
    Hat,
    Top,
    Bottom,
    Boots
};

// A static definition for an item
struct ItemDefinition {
    std::string id;          // Unique ID, e.g., "RUSTY_SWORD"
    std::string name;        // Display name, e.g., "Rusty Sword"
    std::string description; // Tooltip text
    std::string imagePath;   // ID to be matched with a .png on the client
    EquipSlot equipSlot = EquipSlot::None;
    std::map<std::string, int> stats; // Maps directly to your player stats
    bool stackable = false;
};

// Central database mapping item IDs to their definitions.
// DECLARED here, but DEFINED in GameData.cpp
extern const std::map<std::string, ItemDefinition> itemDatabase;