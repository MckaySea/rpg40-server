
// Description: Implements all game logic handlers for the AsyncSession class.
// This includes message parsing, combat, movement, and state updates.
#include "AsyncSession.hpp" 
#include "GameData.hpp"     
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
#include "Items.hpp"
#include <random> 
#include <map>

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


auto applyStat = [](PlayerStats& stats, const std::string& statName, int value) {
    if (statName == "health") { stats.maxHealth += value; }
    else if (statName == "mana") { stats.maxMana += value; }
    else if (statName == "defense") { stats.defense += value; }
    else if (statName == "speed") { stats.speed += value; }
    else if (statName == "strength") { stats.strength += value; }
    else if (statName == "dexterity") { stats.dexterity += value; }
    else if (statName == "intellect") { stats.intellect += value; }
    else if (statName == "luck") { stats.luck += value; }
    };

PlayerStats AsyncSession::getCalculatedStats() {
    PlayerState& player = getPlayerState();
    PlayerStats finalStats = player.stats; // Start with base stats

    // Iterate over all equipment slots
    for (const auto& slotPair : player.equipment.slots) {
        if (slotPair.second.has_value()) { // if an item is equipped
            uint64_t instanceId = slotPair.second.value();

            if (player.inventory.count(instanceId)) {
                const ItemInstance& instance = player.inventory.at(instanceId);
                const ItemDefinition& def = instance.getDefinition();

                // 1. Apply base stats from ItemDefinition
                for (const auto& statMod : def.stats) {
                    applyStat(finalStats, statMod.first, statMod.second);
                }

                // 2. Apply custom random stats from ItemInstance
                for (const auto& statMod : instance.customStats) {
                    applyStat(finalStats, statMod.first, statMod.second);
                }
            }
        }
    }

    // Ensure current health/mana aren't above the new max
    if (finalStats.health > finalStats.maxHealth) {
        finalStats.health = finalStats.maxHealth;
    }
    if (finalStats.mana > finalStats.maxMana) {
        finalStats.mana = finalStats.maxMana;
    }

    // update the player's actual current health/mana just in case max changed
    player.stats.health = finalStats.health;
    player.stats.mana = finalStats.mana;

    return finalStats;
}


/**
 * @brief Adds an item to inventory, rolling for random modifiers.
 */
void AsyncSession::addItemToInventory(const std::string& itemId, int quantity) {
    PlayerState& player = getPlayerState();
    if (quantity <= 0) return;

    if (itemDatabase.count(itemId) == 0) {
        std::cerr << "Error: Attempted to add non-existent item ID: " << itemId << std::endl;
        return;
    }

    const ItemDefinition& def = itemDatabase.at(itemId);
   
    // gotta handle the stackable items theres only like 6 i made tho
    if (def.stackable) {
        for (auto& pair : player.inventory) {
            ItemInstance& instance = pair.second;
            if (instance.itemId == itemId) {
                instance.quantity += quantity;
                send_inventory_and_equipment(); // Send update
                return;
            }
        }

    }
    int numInstancesToAdd = def.stackable ? 1 : quantity;
    int qtyPerInstance = def.stackable ? quantity : 1;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> bonusRoll(1, 100); // 

    for (int i = 0; i < numInstancesToAdd; ++i) {
        uint64_t newInstanceId = g_item_instance_id_counter++;
        ItemInstance newInstance = { newInstanceId, itemId, qtyPerInstance, {} };

        // only equippable stuff should get bonuses obv
        if (def.equipSlot != EquipSlot::None) {

            //im looping over to see what base stats can even get a bonus roll if any
            for (const auto& baseStatPair : def.stats) {
                const std::string& statName = baseStatPair.first;
                int baseValue = baseStatPair.second;
                if (baseValue == 0) continue;

                // there s a 30 prcnt chance they even hit an extra roll 
                if (bonusRoll(gen) <= 30) {
                    int roll = bonusRoll(gen);
                    int bonus = 0;
                    if (roll <= 50) bonus = 1;      // 50% chance
                    else if (roll <= 80) bonus = 2; // 30% chance
                    else bonus = 3;                 // 20% chance

                    // just 2 be funny if they had a negative base imma make it go lower lol
                    if (baseValue < 0) bonus = -bonus;

                    newInstance.customStats[statName] += bonus;
                }
            }

            //i had a good ass idea to make it so they have a 5 percent chance to roll another stat that that item normally couldnt even have :D
            if (bonusRoll(gen) >= 96) {

                
                const std::vector<std::string> rareStatPool = {
                    "health", "mana", "strength", "dexterity", "intellect", "defense", "speed", "luck"
                };

                // then we jus randomly pick one to apply to the item
                std::uniform_int_distribution<> statPicker(0, rareStatPool.size() - 1);
                std::string chosenStat = rareStatPool[statPicker(gen)];

                
                int bonusValue = 0;
                if (chosenStat == "health") bonusValue = 5; 
                else if (chosenStat == "mana") bonusValue = 10; 
                else bonusValue = 2; 
                

                newInstance.customStats[chosenStat] += bonusValue;
            }
        }

        player.inventory[newInstanceId] = newInstance;
    }

    send_inventory_and_equipment(); 
}

//equips an item if its allowed to be equipped
std::string AsyncSession::equipItem(uint64_t itemInstanceId) {
    PlayerState& player = getPlayerState();

    if (player.inventory.count(itemInstanceId) == 0) {
        return "Item instance not found in inventory.";
    }

    const ItemInstance& instance = player.inventory.at(itemInstanceId);
    const ItemDefinition& def = instance.getDefinition();
    EquipSlot slot = def.equipSlot;

    if (slot == EquipSlot::None) {
        return def.name + " cannot be equipped.";
    }
    if (player.equipment.slots.count(slot) == 0) {
        return "Invalid equipment slot."; // Should not happen
    }

    std::string msg = "";
    // Check if an item is already in that slot
    if (player.equipment.slots[slot].has_value()) {
        uint64_t oldInstanceId = player.equipment.slots[slot].value();
        const auto& oldDef = player.inventory.at(oldInstanceId).getDefinition();
        msg = " (replacing " + oldDef.name + ")";
    }

    // Equip the new itemz instanceId into the slot
    player.equipment.slots[slot] = itemInstanceId;

    
  
    send_player_stats();
    send_inventory_and_equipment();

    return "Equipped " + def.name + "." + msg;
}

/**
 * @brief Attempts to unequip an item from a specific slot.
 */
std::string AsyncSession::unequipItem(EquipSlot slotToUnequip) {
    PlayerState& player = getPlayerState();

    if (player.equipment.slots.count(slotToUnequip) == 0 || slotToUnequip == EquipSlot::None) {
        return "Invalid equipment slot.";
    }
    if (!player.equipment.slots[slotToUnequip].has_value()) {
        return "No item equipped in that slot.";
    }

    uint64_t instanceId = player.equipment.slots[slotToUnequip].value();
    const ItemDefinition& def = player.inventory.at(instanceId).getDefinition();

    // Unequip it by setting the slot to empty
    player.equipment.slots[slotToUnequip] = std::nullopt;

    // --- VITAL ---
    send_player_stats();
    send_inventory_and_equipment();

    return "Unequipped " + def.name + ".";
}

void AsyncSession::useItem(uint64_t itemInstanceId) {
    PlayerState& player = getPlayerState();
    auto& ws = getWebSocket();

    if (player.inventory.count(itemInstanceId) == 0) {
        ws.write(net::buffer("SERVER:ERROR:You do not have that item."));
        return;
    }

    ItemInstance& instance = player.inventory.at(itemInstanceId);
    const ItemDefinition& def = instance.getDefinition();

   
    if (def.equipSlot != EquipSlot::None) {
        ws.write(net::buffer("SERVER:ERROR:This item cannot be 'used'. Try equipping it."));
        return;
    }

    bool itemUsed = false;
    std::string effectMsg = "";

    
    if (instance.itemId == "SMALL_HEALTH_POTION") {
        PlayerStats finalStats = getCalculatedStats(); 
        int healAmount = 50;
        if (player.stats.health < finalStats.maxHealth) {
            player.stats.health = std::min(finalStats.maxHealth, player.stats.health + healAmount);
            itemUsed = true;
            effectMsg = "You restore " + std::to_string(healAmount) + " health.";
        }
        else {
            effectMsg = "Your health is already full.";
        }
    }
    else if (instance.itemId == "LARGE_HEALTH_POTION") {
        PlayerStats finalStats = getCalculatedStats();
        int healAmount = 250;
        if (player.stats.health < finalStats.maxHealth) {
            player.stats.health = std::min(finalStats.maxHealth, player.stats.health + healAmount);
            itemUsed = true;
            effectMsg = "You restore " + std::to_string(healAmount) + " health.";
        }
        else {
            effectMsg = "Your health is already full.";
        }
    }
    else if (instance.itemId == "SMALL_MANA_POTION") {
        PlayerStats finalStats = getCalculatedStats(); // Get max mana
        int manaAmount = 50;
        if (player.stats.mana < finalStats.maxMana) {
            player.stats.mana = std::min(finalStats.maxMana, player.stats.mana + manaAmount);
            itemUsed = true;
            effectMsg = "You restore " + std::to_string(manaAmount) + " mana.";
        }
        else {
            effectMsg = "Your mana is already full.";
        }
    }
    // ... ill add more consumable crap here later
    else {
        effectMsg = "That item has no use.";
    }

    ws.write(net::buffer("SERVER:STATUS:" + effectMsg));

    if (itemUsed) {
        instance.quantity--;
        if (instance.quantity <= 0) {
            // Remove the item from inventory
            player.inventory.erase(itemInstanceId);
        }
        send_inventory_and_equipment(); // Update client's inventory view
        send_player_stats();            // Update client's health/mana
    }
}

void AsyncSession::dropItem(uint64_t itemInstanceId, int quantity) {
    PlayerState& player = getPlayerState();
    auto& ws = getWebSocket();

    if (quantity <= 0) {
        ws.write(net::buffer("SERVER:ERROR:Invalid quantity."));
        return;
    }

    if (player.inventory.count(itemInstanceId) == 0) {
        ws.write(net::buffer("SERVER:ERROR:You do not have that item."));
        return;
    }

    // needa make sure they cant drop stuff they have on
    for (const auto& slotPair : player.equipment.slots) {
        if (slotPair.second.has_value() && slotPair.second.value() == itemInstanceId) {
            ws.write(net::buffer("SERVER:ERROR:Cannot drop an equipped item. Unequip it first."));
            return;
        }
    }

    ItemInstance& instance = player.inventory.at(itemInstanceId);
    const ItemDefinition& def = instance.getDefinition();

    if (!def.stackable) {
        ws.write(net::buffer("SERVER:STATUS:Dropped " + def.name + "."));
        player.inventory.erase(itemInstanceId);
    }
    else {
        if (quantity >= instance.quantity) {
            ws.write(net::buffer("SERVER:STATUS:Dropped " + std::to_string(instance.quantity) + "x " + def.name + "."));
            player.inventory.erase(itemInstanceId);
        }
        else {
            instance.quantity -= quantity;
            ws.write(net::buffer("SERVER:STATUS:Dropped " + std::to_string(quantity) + "x " + def.name + "."));
        }
    }

    send_inventory_and_equipment(); // Update client
}
void AsyncSession::sellItem(uint64_t itemInstanceId, int quantity) {
    PlayerState& player = getPlayerState();
    auto& ws = getWebSocket();

    if (quantity <= 0) {
        ws.write(net::buffer("SERVER:ERROR:Invalid quantity."));
        return;
    }

    if (player.inventory.count(itemInstanceId) == 0) {
        ws.write(net::buffer("SERVER:ERROR:You do not have that item."));
        return;
    }

    // Check if item is equipped ALWAYS dont forget if u wanna look at these func's
    for (const auto& slotPair : player.equipment.slots) {
        if (slotPair.second.has_value() && slotPair.second.value() == itemInstanceId) {
            ws.write(net::buffer("SERVER:ERROR:Cannot sell an equipped item. Unequip it first."));
            return;
        }
    }

    ItemInstance& instance = player.inventory.at(itemInstanceId);
    const ItemDefinition& def = instance.getDefinition();

    
    int buyPrice = 1; 
    try {
        buyPrice = g_item_buy_prices.at(def.id);
    }
    catch (const std::out_of_range&) {
        std::cerr << "WARNING: Item " << def.id << " has no defined price. Defaulting to 1." << std::endl;
    }

    int sellPricePerItem = std::max(1, buyPrice / 4); // Sell for 1/4 of buy price, 1 gold min

    int totalSellPrice = 0;

    if (!def.stackable) {
        totalSellPrice = sellPricePerItem;
        ws.write(net::buffer("SERVER:STATUS:Sold " + def.name + " for " + std::to_string(totalSellPrice) + " gold."));
        player.inventory.erase(itemInstanceId);
    }
    else {
        if (quantity >= instance.quantity) {
            totalSellPrice = sellPricePerItem * instance.quantity;
            ws.write(net::buffer("SERVER:STATUS:Sold " + std::to_string(instance.quantity) + "x " + def.name + " for " + std::to_string(totalSellPrice) + " gold."));
            player.inventory.erase(itemInstanceId);
        }
        else {
            totalSellPrice = sellPricePerItem * quantity;
            instance.quantity -= quantity;
            ws.write(net::buffer("SERVER:STATUS:Sold " + std::to_string(quantity) + "x " + def.name + " for " + std::to_string(totalSellPrice) + " gold."));
        }
    }

    
    player.stats.gold += totalSellPrice;
    send_player_stats();
    send_inventory_and_equipment();
}
/**
 * @brief Sends the player's full inventory and equipment list to the client.
 */
void AsyncSession::send_inventory_and_equipment() {
    PlayerState& player = getPlayerState();
    auto& ws = getWebSocket();

    std::ostringstream oss;
    oss << "SERVER:INVENTORY_UPDATE:";

    // --- 1. Serialize Inventory ---
    oss << "{\"inventory\":[";
    bool firstItem = true;
    for (const auto& pair : player.inventory) {
        const ItemInstance& instance = pair.second;
        const ItemDefinition& def = instance.getDefinition();
        if (!firstItem) oss << ",";
        oss << "{\"instanceId\":" << instance.instanceId
            << ",\"itemId\":\"" << instance.itemId << "\""
            << ",\"name\":\"" << def.name << "\""
            << ",\"desc\":\"" << def.description << "\""
            << ",\"imagePath\":\"" << def.imagePath << "\""
            << ",\"quantity\":" << instance.quantity
            << ",\"slot\":" << static_cast<int>(def.equipSlot)
            << ",\"baseStats\":{";
        // Add base stats
        bool firstStat = true;
        for (const auto& statPair : def.stats) {
            if (!firstStat) oss << ",";
            oss << "\"" << statPair.first << "\":" << statPair.second;
            firstStat = false;
        }
        oss << "},\"customStats\":{";
        // Add custom stats
        firstStat = true;
        for (const auto& statPair : instance.customStats) {
            if (!firstStat) oss << ",";
            oss << "\"" << statPair.first << "\":" << statPair.second;
            firstStat = false;
        }
        oss << "}}";
        firstItem = false;
    }
    oss << "]"; // End inventory array

    // --- 2. Serialize Equipment ---
    oss << ",\"equipment\":{";
    firstItem = true;
    for (const auto& pair : player.equipment.slots) {
        if (!firstItem) oss << ",";
        // Send slot enum int as key and instanceId (or null) as value
        oss << "\"" << static_cast<int>(pair.first) << "\":";
        if (pair.second.has_value()) {
            oss << pair.second.value();
        }
        else {
            oss << "null";
        }
        firstItem = false;
    }
    oss << "}}"; // End equipment object and main object
    
    ws.write(net::buffer(oss.str()));
}
void AsyncSession::send_player_stats() {
    // Access session state
    PlayerState& player = getPlayerState(); // This has BASE stats
    auto& ws = getWebSocket();

    // --- NEW ---
    // Get the final, calculated stats *including* equipment
    PlayerStats finalStats = getCalculatedStats();
    // --- END NEW ---

    // Use ostringstream to build the JSON-like string
    std::ostringstream oss;
    oss << "SERVER:STATS:"
        << "{\"playerName\":\"" << player.playerName << "\""

        // --- MODIFIED ---
        // Use finalStats for max values, but player.stats for current values
        << ",\"health\":" << player.stats.health // Current health
        << ",\"maxHealth\":" << finalStats.maxHealth
        << ",\"mana\":" << player.stats.mana // Current mana
        << ",\"maxMana\":" << finalStats.maxMana
        << ",\"defense\":" << finalStats.defense
        << ",\"speed\":" << finalStats.speed
        << ",\"level\":" << player.stats.level // Level is base
        << ",\"experience\":" << player.stats.experience // XP is base
        << ",\"experienceToNextLevel\":" << player.stats.experienceToNextLevel
        << ",\"availableSkillPoints\":" << player.availableSkillPoints
        << ",\"strength\":" << finalStats.strength
        << ",\"dexterity\":" << finalStats.dexterity
        << ",\"intellect\":" << finalStats.intellect
        << ",\"luck\":" << finalStats.luck
        << ",\"gold\":" << finalStats.gold
        // --- END MODIFIED ---

        << ",\"posX\":" << player.posX
        << ",\"posY\":" << player.posY;

    // Add spells list only if they exist (for wizards)
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
        player.availableSkillPoints += 3;
        player.stats.maxHealth += 5;
        player.stats.health = player.stats.maxHealth;
        player.stats.maxMana += 5;
        player.stats.mana = player.stats.maxMana;
        player.stats.defense += 1;
        player.stats.speed += 1;
        player.stats.dexterity += 1;
        player.stats.luck += 1;
        player.stats.intellect += 1;
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
            // else if (stat_name == "attack") { player.stats.attack += 1; valid_stat = true; } // REMOVED
            else if (stat_name == "defense") { player.stats.defense += 1; valid_stat = true; }
            else if (stat_name == "speed") { player.stats.speed += 1; valid_stat = true; }
            // ADDED
            else if (stat_name == "strength") { player.stats.strength += 1; valid_stat = true; }
            else if (stat_name == "dexterity") { player.stats.dexterity += 1; valid_stat = true; }
            else if (stat_name == "intellect") { player.stats.intellect += 1; valid_stat = true; }
            else if (stat_name == "luck") { player.stats.luck += 1; valid_stat = true; }


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
                        // --- NEW SHOP LOGIC ---
                        ws.write(net::buffer("SERVER:PROMPT:Merchant: \"You there, got some gold, I've got stuff that might appeal to you\""));

                        // Find the shop's inventory
                        auto shop_it = g_shops.find("MERCHANT_SHOP_1");
                        if (shop_it == g_shops.end()) {
                            ws.write(net::buffer("SERVER:ERROR:Shop inventory not found."));
                            return;
                        }

                        // Serialize the shop data
                        std::ostringstream oss;
                        oss << "SERVER:SHOW_SHOP:{\"shopId\":\"" << shop_it->first << "\",\"items\":[";

                        bool firstItem = true;
                        for (const std::string& itemId : shop_it->second) {
                            if (itemDatabase.count(itemId) == 0) continue;
                            const ItemDefinition& def = itemDatabase.at(itemId);

                            int price = 1; 
                            try {
                                price = g_item_buy_prices.at(itemId);
                            }
                            catch (const std::out_of_range&) {
                                std::cerr << "WARNING: Shop item " << itemId << " has no price. Defaulting to 1." << std::endl;
                            }


                            if (!firstItem) oss << ",";
                            oss << "{\"itemId\":\"" << def.id << "\""
                                << ",\"name\":\"" << def.name << "\""
                                << ",\"desc\":\"" << def.description << "\""
                                << ",\"imagePath\":\"" << def.imagePath << "\""
                                << ",\"price\":" << price
                                << ",\"slot\":" << static_cast<int>(def.equipSlot)
                                << ",\"baseStats\":{";
                            bool firstStat = true;
                            for (const auto& statPair : def.stats) {
                                if (!firstStat) oss << ",";
                                oss << "\"" << statPair.first << "\":" << statPair.second;
                                firstStat = false;
                            }
                            oss << "}}";
                            firstItem = false;
                        }
                        oss << "]}";
                        ws.write(net::buffer(oss.str()));
                        // --- END SHOP LOGIC ---
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
        //gonna keep reivising the combat system and make it more intricate :P
    else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
        if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
        else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:You are already in combat!")); }
        else if (player.currentArea == "TOWN") { ws.write(net::buffer("SERVER:STATUS:No monsters to fight in TOWN.")); }
        else {
            try {
                int selected_id = std::stoi(message.substr(17));
                // we find the mob from the area we're in and its in
                auto it = std::find_if(player.currentMonsters.begin(), player.currentMonsters.end(), [selected_id](const MonsterState& m) { return m.id == selected_id; });
                if (it != player.currentMonsters.end()) {
                    player.isInCombat = true;
                    player.currentOpponent = create_monster(it->id, it->type); // 
                    player.isDefending = false;
                    player.currentMonsters.erase(it); // we gotta delete the mob from the world 
                    std::cout << "[" << client_address << "] --- COMBAT STARTED vs " << player.currentOpponent->type << " ---\n";
                    std::ostringstream oss;
                    oss << "SERVER:COMBAT_START:"
                        << "{\"id\":" << player.currentOpponent->id << ",\"type\":\"" << player.currentOpponent->type
                        << "\",\"asset\":\"" << player.currentOpponent->assetKey << "\",\"health\":" << player.currentOpponent->health
                        << ",\"maxHealth\":" << player.currentOpponent->maxHealth << "}";
                    std::string combat_start_message = oss.str();
                    ws.write(net::buffer(combat_start_message));
                    ws.write(net::buffer("SERVER:COMBAT_LOG:You engaged the " + player.currentOpponent->type + "!"));
                    if (player.stats.speed >= player.currentOpponent->speed) {
                        ws.write(net::buffer("SERVER:COMBAT_LOG:You are faster! You attack first."));
                        ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
                    }
                    else {
                        // Monster is faster
                        ws.write(net::buffer("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " is faster! It attacks first."));

                        //subject to change but i wanted more stats to have more effects on coimbat
                        int monster_attack_value = player.currentOpponent->strength + (player.currentOpponent->dexterity / 4);
                        int monster_damage = 0;
                        int player_defense = player.stats.defense;
                        monster_damage = std::max(1, monster_attack_value - player_defense);

                            //i added the ability for the mob to crit u
                        float monster_crit_chance = (player.currentOpponent->luck * 0.005f) + (player.currentOpponent->dexterity * 0.0025f);
                        if (((float)std::rand() / RAND_MAX) < monster_crit_chance) {
                            monster_damage *= 2;
                            ws.write(net::buffer("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " lands a critical hit!"));
                        }
                        player.stats.health -= monster_damage;
                        ws.write(net::buffer("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " attacks you for " + std::to_string(monster_damage) + " damage!"));
                        send_player_stats();

                        // incase a noob tries to fight a rlly high level mob and gets one shot i just copied logic from combat action lol
                        if (player.stats.health <= 0) {
                            player.stats.health = 0;
                            ws.write(net::buffer("SERVER:COMBAT_DEFEAT:You have been defeated!"));
                            player.isInCombat = false; player.currentOpponent.reset();
                            player.currentArea = "TOWN"; player.currentMonsters.clear();
                            player.stats.health = player.stats.maxHealth / 2; player.stats.mana = player.stats.maxMana;
                            player.posX = 5; player.posY = 5; player.currentPath.clear();
                            broadcast_data.currentArea = "TOWN"; broadcast_data.posX = player.posX; broadcast_data.posY = player.posY;
                            { std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }

                            ws.write(net::buffer("SERVER:AREA_CHANGED:TOWN"));
                            send_area_map_data(player.currentArea);
                            send_interactables(player.currentArea); 
                            send_available_areas();
                            send_player_stats();
                        }
                        else {
                            ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
                        }
                    }
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
                // --- NEW PLAYER ATTACK CALCULATION ---
                int player_attack_value = player.stats.strength + (player.stats.dexterity / 2);
                int base_damage = std::max(1, player_attack_value - player.currentOpponent->defense);

                float variance = 0.8f + ((float)(std::rand() % 41) / 100.0f); // 0.8 to 1.2
                player_damage = std::max(1, (int)(base_damage * variance));

                // --- NEW: CRITICAL HIT LOGIC ---
                float crit_chance = (player.stats.luck * 0.005f) + (player.stats.dexterity * 0.0025f);
                if (((float)std::rand() / RAND_MAX) < crit_chance) {
                    player_damage *= 2; // Double damage!
                    ws.write(net::buffer("SERVER:COMBAT_LOG:A critical hit!"));
                }
                // --- END CRITICAL HIT LOGIC ---

                ws.write(net::buffer("SERVER:COMBAT_LOG:You attack the " + player.currentOpponent->type + " for " + std::to_string(player_damage) + " damage!"));
            }
            else if (action_type == "SPELL") {
                int base_damage = 0; mana_cost = 0; float variance = 1.0f;

                // --- NEW SPELL DAMAGE CALCULATION ---
                if (action_param == "Fireball") {
                    mana_cost = 25;
                    if (player.stats.mana >= mana_cost) {
                        base_damage = static_cast<int>(player.stats.intellect * 1.3) + (player.stats.maxMana / 10);
                        variance = 0.8f + ((float)(std::rand() % 41) / 100.0f);
                    }
                }
                else if (action_param == "Lightning") {
                    mana_cost = 20;
                    if (player.stats.mana >= mana_cost) {
                        base_damage = static_cast<int>(player.stats.intellect * 1.1) + (player.stats.maxMana / 8);
                        variance = 0.7f + ((float)(std::rand() % 61) / 100.0f);
                    }
                }
                else if (action_param == "Freeze") {
                    mana_cost = 10;
                    if (player.stats.mana >= mana_cost) {
                        base_damage = (player.stats.intellect) + (player.stats.maxMana / 10);
                        variance = 0.9f + ((float)(std::rand() % 21) / 100.0f);
                    }
                }
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
               
                float flee_chance = 0.5f + ((float)player.stats.speed - (float)player.currentOpponent->speed) * 0.05f + ((float)player.stats.luck * 0.01f);
                flee_chance = std::max(0.1f, std::min(0.9f, flee_chance)); // Clamp chance
                if (((float)std::rand() / RAND_MAX) < flee_chance) { fled = true; }
                else { ws.write(net::buffer("SERVER:COMBAT_LOG:You failed to flee!")); }
            }

            
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
                int lootTier = player.currentOpponent->lootTier;

                if (lootTier != -1) { // -1 means no loot
                    // Player's luck slightly increases drop chance... still gottta figure out a better forumla
                    int baseDropChance = player.currentOpponent->dropChance;
                    int finalDropChance = baseDropChance + (player.stats.luck * 10); //  20 luck = +5% chance butttt subject to change if i remmeber

                    if ((std::rand() % 100) < finalDropChance) {
                        
                        auto table_it = g_loot_tables.find(lootTier);
                        if (table_it != g_loot_tables.end()) {
                            const std::vector<std::string>& lootTable = table_it->second;

                            if (!lootTable.empty()) {
                                // Pick a random item from the table that monster belonged to
                                int itemIndex = std::rand() % lootTable.size();
                                std::string itemId = lootTable[itemIndex];

                                
                                addItemToInventory(itemId, 1);

                              
                                const ItemDefinition& def = itemDatabase.at(itemId);
                                ws.write(net::buffer("SERVER:STATUS:The " + player.currentOpponent->type + " dropped: " + def.name + "!"));
                            }
                        }
                    }
                }
                player.isInCombat = false; player.currentOpponent.reset();
                ws.write(net::buffer("SERVER:COMBAT_VICTORY:Defeated"));
                check_for_level_up(); send_player_stats();
                send_current_monsters_list(); return;
            }

            
            // --- Monster Turn ---
            int monster_damage = 0; int player_defense = player.stats.defense;
            if (player.isDefending) { player_defense *= 2; player.isDefending = false; }

           
            int monster_attack_value = player.currentOpponent->strength + (player.currentOpponent->dexterity / 4);
            monster_damage = std::max(1, monster_attack_value - player_defense);

            // crit for monster chance
            float monster_crit_chance = (player.currentOpponent->luck * 0.005f) + (player.currentOpponent->dexterity * 0.0025f);
            if (((float)std::rand() / RAND_MAX) < monster_crit_chance) {
                monster_damage *= 2;
                ws.write(net::buffer("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " lands a critical hit!"));
            }

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
    else if (message.rfind("USE_ITEM:", 0) == 0) {
        if (!player.isFullyInitialized) {
            ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
        }
        else {
            try {
                uint64_t instanceId = std::stoull(message.substr(9));
                useItem(instanceId);
            }
            catch (const std::exception&) {
                ws.write(net::buffer("SERVER:ERROR:Invalid item ID format."));
            }
        }
    }
    else if (message.rfind("EQUIP_ITEM:", 0) == 0) {
        if (!player.isFullyInitialized) {
            ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
        }
        else {
            try {
                uint64_t instanceId = std::stoull(message.substr(11)); // "EQUIP_ITEM:" is 11 chars
                // Call your existing equipItem function
                std::string equipMsg = equipItem(instanceId);
                // Send the success message back
                ws.write(net::buffer("SERVER:STATUS:" + equipMsg));
            }
            catch (const std::exception&) {
                ws.write(net::buffer("SERVER:ERROR:Invalid item ID format."));
            }
        }
    }
    else if (message.rfind("DROP_ITEM:", 0) == 0) {
        if (!player.isFullyInitialized) {
            ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
        }
        else {
            try {
                std::string params = message.substr(10);
                size_t colon_pos = params.find(':');

                if (colon_pos == std::string::npos) {
                    throw std::invalid_argument("Invalid format. Expected DROP_ITEM:instanceId:quantity");
                }

                uint64_t instanceId = std::stoull(params.substr(0, colon_pos));
                int quantity = std::stoi(params.substr(colon_pos + 1));

                dropItem(instanceId, quantity);
            }
            catch (const std::exception& e) {
                std::cerr << "Drop item error: " << e.what() << std::endl;
                ws.write(net::buffer("SERVER:ERROR:Invalid drop command format."));
            }
        }
    }
    else if (message.rfind("UNEQUIP_ITEM:", 0) == 0) {
        if (!player.isFullyInitialized) {
            ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
        }
        else {
            try {
                int slotInt = std::stoi(message.substr(13));
                EquipSlot slotToUnequip = static_cast<EquipSlot>(slotInt);

                std::string result = unequipItem(slotToUnequip);
                ws.write(net::buffer("SERVER:STATUS:" + result));
            }
            catch (const std::exception& e) {
                ws.write(net::buffer("SERVER:ERROR:Invalid slot format."));
            }
        }
        }
    else if (message.rfind("BUY_ITEM:", 0) == 0) {
        if (!player.isFullyInitialized) {
            ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
        }
        else {
            try {
                std::string params = message.substr(9);
                size_t colon_pos = params.find(':');

                if (colon_pos == std::string::npos) {
                    throw std::invalid_argument("Invalid format. Expected BUY_ITEM:shopId:itemId");
                }

                std::string shopId = params.substr(0, colon_pos);
                std::string itemId = params.substr(colon_pos + 1);

                // Verify shop and item exist
                if (g_shops.count(shopId) == 0) {
                    ws.write(net::buffer("SERVER:ERROR:Unknown shop."));
                    return;
                }
                if (itemDatabase.count(itemId) == 0) {
                    ws.write(net::buffer("SERVER:ERROR:Unknown item."));
                    return;
                }

                // Check if this shop actually sells this item
                const auto& shopItems = g_shops.at(shopId);
                if (std::find(shopItems.begin(), shopItems.end(), itemId) == shopItems.end()) {
                    ws.write(net::buffer("SERVER:ERROR:This shop does not sell that item."));
                    return;
                }

                const ItemDefinition& def = itemDatabase.at(itemId);

                // --- Price Calculation (same as before) ---
                int price = 1; // Default
                try {
                    price = g_item_buy_prices.at(itemId);
                }
                catch (const std::out_of_range&) {
                    std::cerr << "WARNING: Player tried to buy " << itemId << " which has no price." << std::endl;
                    ws.write(net::buffer("SERVER:ERROR:That item is not for sale."));
                    return;
                }

                if (player.stats.gold >= price) {
                    player.stats.gold -= price;
                    addItemToInventory(itemId, 1);
                    ws.write(net::buffer("SERVER:STATUS:Bought " + def.name + " for " + std::to_string(price) + " gold."));
                    send_player_stats(); // To update gold   <__- dont forget this if u guys wanna make  a shop
                    
                }
                else {
                    ws.write(net::buffer("SERVER:ERROR:Not enough gold. You need " + std::to_string(price) + "."));
                }
            }
            catch (const std::exception& e) {
                std::cerr << "Buy item error: " << e.what() << std::endl;
                ws.write(net::buffer("SERVER:ERROR:Invalid buy command format."));
            }
        }
        }
    // --- Fallback ---
    else {
        std::string echo = "SERVER:ECHO: " + message;
        ws.write(net::buffer(echo));
    }
}