// D// Description: Implements all game logic handlers for the AsyncSession class.
// This includes message parsing, combat, movement, and state updates.
#include "AsyncSession.hpp"
#include "GameData.hpp"
#include "Items.hpp"
#include "AreaData.hpp"
#include <cmath> 
#include "game_session.hpp"
#include <algorithm>
#include <deque>
#include <iostream>
#include <map>
#include <queue>
#include <unordered_set>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <sodium.h>
#include <pqxx/pqxx>
#include <pqxx/transaction>
#include "pqxx/connection.hxx"
#include <nlohmann/json.hpp>
#include <optional>
#include <algorithm>

using namespace std;

// --- Combat math helpers ---

static const float DEF_SCALE = 1.0f;
static const float GLOBAL_CRIT_CAP = 0.40f;

// Class-based crit tuning
struct ClassCritTuning {
	float baseCrit;
	float dexCritScale;
	float luckCritScale;
	float critMultiplier;
};

ClassCritTuning getCritTuning(PlayerClass cls) {
	switch (cls) {
	case PlayerClass::FIGHTER:
		return { 0.05f, 0.0015f, 0.0025f, 1.6f };
	case PlayerClass::ROGUE:
		return { 0.08f, 0.0030f, 0.0020f, 1.9f };
	case PlayerClass::WIZARD:
		return { 0.05f, 0.0010f, 0.0020f, 1.7f };
	default:
		return { 0.05f, 0.0015f, 0.0020f, 1.5f };
	}
}

// Scales attack power by defense in a soft way (no “brick wall”)
int damage_after_defense(float attackPower, int defense) {
	if (attackPower <= 0.0f) return 1;
	int def = std::max(defense, 0);
	float multiplier = 100.0f / (100.0f + def * DEF_SCALE);
	float raw = attackPower * multiplier;
	if (raw < 1.0f) raw = 1.0f;
	return static_cast<int>(std::round(raw));
}

// Crit chance depends on class + DEX + LUCK
float crit_chance_for_player(const PlayerStats& stats, PlayerClass cls) {
	ClassCritTuning t = getCritTuning(cls);
	float crit = t.baseCrit
		+ t.dexCritScale * static_cast<float>(stats.dexterity)
		+ t.luckCritScale * static_cast<float>(stats.luck);

	if (crit > GLOBAL_CRIT_CAP) crit = GLOBAL_CRIT_CAP;
	if (crit < 0.0f) crit = 0.0f;
	return crit;
}

// Attack power scales differently per class
float attack_power_for_player(const PlayerStats& stats, PlayerClass cls) {
	switch (cls) {
	case PlayerClass::FIGHTER:
		return stats.strength * 1.4f
			+ stats.dexterity * 0.3f
			+ stats.speed * 0.2f;
	case PlayerClass::ROGUE:
		return stats.dexterity * 1.4f
			+ stats.strength * 0.3f
			+ stats.speed * 0.3f;
	case PlayerClass::WIZARD:
		return stats.intellect * 0.6f
			+ stats.dexterity * 0.4f
			+ stats.strength * 0.2f;
	default:
		return stats.strength * 1.0f;
	}
}
// Monster attack power: STR-heavy with a bit of DEX
float attack_power_for_monster(const MonsterInstance& m) {
	return m.strength * 1.3f + m.dexterity * 0.4f;
}

// Monster crit chance: based on DEX + LUCK, with a global cap
float crit_chance_for_monster(const MonsterInstance& m) {
	float base = 0.05f; // 5%
	float fromDex = m.dexterity * 0.0015f; // 0.15% per DEX
	float fromLuck = m.luck * 0.0020f;     // 0.2% per LUCK

	float crit = base + fromDex + fromLuck;
	if (crit > GLOBAL_CRIT_CAP) crit = GLOBAL_CRIT_CAP;
	if (crit < 0.0f) crit = 0.0f;
	return crit;
}
// Forward declaration – implemented later in this file
void SyncPlayerMonsters(PlayerState& player);

// -----------------------------------------------------------------------------
// Sends the current list of monsters (player.currentMonsters) to the client.
// -----------------------------------------------------------------------------
void AsyncSession::send_current_monsters_list() {
	// Use the session's own player_ and ws_ members
	nlohmann::json payload;
	payload["area"] = player_.currentArea;
	payload["monsters"] = nlohmann::json::array();

	for (const auto& m : player_.currentMonsters) {
		payload["monsters"].push_back({
			{"id", m.id},
			{"name", m.type},
			{"asset", m.assetKey},
			{"x", m.posX},
			{"y", m.posY}
			});
	}

	std::ostringstream oss;
	oss << "SERVER:MONSTERS:" << payload.dump();
	ws_.write(net::buffer(oss.str()));

	std::cerr << "[Sent] " << player_.currentMonsters.size()
		<< " monsters for area " << player_.currentArea << "\n";
}
static std::string sanitize_for_json(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s) {
		// keep printable ASCII
		if (c >= 0x20 && c <= 0x7E) {
			out.push_back(static_cast<char>(c));
		}
		else {
			out.push_back('?');
		}
	}
	return out;
}


// -----------------------------------------------------------------------------
// Processes one tick of player movement (called by move_timer_).
// -----------------------------------------------------------------------------
void AsyncSession::process_movement()
{
	PlayerState& player = getPlayerState();

	// No movement if in combat or no path
	if (player.isInCombat || player.currentPath.empty()) {
		return;
	}

	auto now = std::chrono::steady_clock::now();
	if (now - player.lastMoveTime < MOVEMENT_DELAY) {
		return;
	}

	// --- Move one step along the path ---
	Point next_pos = player.currentPath.front();
	player.currentPath.pop_front();

	player.posX = next_pos.x;
	player.posY = next_pos.y;
	player.lastMoveTime = now;

	// --- INTERACTABLE CHECK: NPCs, shops, zone transitions, etc. ---
	try {
		auto& ws = getWebSocket();

		auto it = g_interactable_objects.find(player.currentArea);
		if (it != g_interactable_objects.end()) {
			for (const auto& obj : it->second) {
				if (obj.position.x == player.posX && obj.position.y == player.posY) {

					// --- Zone transitions ---
					if (obj.type == InteractableType::ZONE_TRANSITION) {
						player.currentPath.clear(); // stop further movement

						std::string command = "GO_TO:" + obj.data;
						std::cout << "[DEBUG] Zone transition: " << player.currentArea
							<< " -> " << obj.data << " via " << obj.id << std::endl;

						handle_message(command);    // area change logic
						return;
					}

					// --- NPC: trigger dialogue when you arrive on their tile ---
					if (obj.type == InteractableType::NPC) {
						std::cout << "[DEBUG] NPC stepped on: id=" << obj.id
							<< " data=" << obj.data
							<< " area=" << player.currentArea << std::endl;

						auto dlgIt = g_dialogues.find(obj.data); // e.g. "MAYOR_WELCOME_DIALOGUE"
						if (dlgIt == g_dialogues.end()) {
							std::cerr << "[Dialogue] No dialogue found for key: "
								<< obj.data << std::endl;
							ws.write(net::buffer("SERVER:PROMPT:They have nothing to say right now."));
							return;
						}

						const auto& dialogueLines = dlgIt->second;
						if (dialogueLines.empty()) {
							std::cerr << "[Dialogue] Dialogue is empty for key: "
								<< obj.data << std::endl;
							ws.write(net::buffer("SERVER:PROMPT:They have nothing to say right now."));
							return;
						}

						nlohmann::json j;
						j["npcId"] = sanitize_for_json(obj.id);
						j["dialogueId"] = sanitize_for_json(obj.data);
						j["lines"] = nlohmann::json::array();

						for (const auto& line : dlgIt->second) {
							nlohmann::json jline;
							jline["speaker"] = sanitize_for_json(line.speaker);
							jline["text"] = sanitize_for_json(line.text);
							jline["portrait"] = sanitize_for_json(line.portraitKey);
							j["lines"].push_back(jline);
						}

						std::ostringstream oss;
						oss << "SERVER:DIALOGUE:" << j.dump();
						ws.write(net::buffer(oss.str()));
						std::cout << "[Dialogue] Sent dialogue '" << obj.data
							<< "' with " << dlgIt->second.size() << " lines.\n";
						return;
					}

					// --- SHOP: auto-open when you step onto the merchant tile ---
					if (obj.type == InteractableType::SHOP) {
						std::cout << "[DEBUG] Shop stepped on: id=" << obj.id
							<< " data=" << obj.data
							<< " area=" << player.currentArea << std::endl;

						std::string msg = "SERVER:SHOW_SHOP:" + obj.data; // your shop key
						ws.write(net::buffer(msg));
						return;
					}

					// future: QUEST_GIVER, SHRINE, etc…
				}
			}
		}
	}
	catch (const std::exception& e) {
		// This prevents unhandled exceptions from aborting your server
		std::cerr << "[process_movement] Exception during interactable handling: "
			<< e.what() << std::endl;
		auto& ws = getWebSocket();
		ws.write(net::buffer("SERVER:ERROR:An error occurred while processing interaction."));
		// fall through and still update position/broadcast below
	}
	catch (...) {
		std::cerr << "[process_movement] Unknown exception during interactable handling.\n";
		auto& ws = getWebSocket();
		ws.write(net::buffer("SERVER:ERROR:Unknown interaction error."));
	}

	// --- Update broadcast data (position) ---
	PlayerBroadcastData& broadcast = getBroadcastData();
	broadcast.posX = player.posX;
	broadcast.posY = player.posY;
	{
		std::lock_guard<std::mutex> lock(g_player_registry_mutex);
		g_player_registry[player.userId] = broadcast;
	}

	// Send updated stats (position etc.)
	send_player_stats();
}


auto applyStat = [](PlayerStats& stats, const std::string& statName, int value) {
	if (statName == "health" || statName == "maxHealth") {
		stats.maxHealth += value;
	}
	else if (statName == "mana" || statName == "maxMana") {
		stats.maxMana += value;
	}
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

	//Clear and populate runtime spell list ---
	player.temporary_spells_list.clear();
	player.temporary_spells_list.insert(
		player.temporary_spells_list.end(),
		player.skills.spells.begin(),
		player.skills.spells.end()
	);

	auto applyEffectAsStat = [&](const ItemEffect& effect) {
		if (effect.type == "GRANT_STAT") {
			auto itStat = effect.params.find("stat");
			auto itValue = effect.params.find("value");
			if (itStat != effect.params.end() && itValue != effect.params.end()) {
				int v = std::stoi(itValue->second);
				applyStat(finalStats, itStat->second, v);
			}
		}
		};
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


				// 3. Apply static effects from ItemDefinition
				for (const auto& effect : def.effects) {
					if (effect.type == "GRANT_SPELL" && effect.params.count("spell_id")) {
						player.temporary_spells_list.push_back(effect.params.at("spell_id"));
					}
					applyEffectAsStat(effect);
				}

				// 4. Apply custom effects from ItemInstance
				for (const auto& effect : instance.customEffects) {
					if (effect.type == "GRANT_SPELL" && effect.params.count("spell_id")) {
						player.temporary_spells_list.push_back(effect.params.at("spell_id"));
					}
					applyEffectAsStat(effect);
				}

			}
		}
	}


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


void AsyncSession::addItemToInventory(const std::string& itemId, int quantity) {
	PlayerState& player = getPlayerState();
	if (quantity <= 0) return;

	if (itemDatabase.count(itemId) == 0) {
		std::cerr << "Error: Attempted to add non-existent item ID: " << itemId << std::endl;
		return;
	}

	const ItemDefinition& def = itemDatabase.at(itemId);

	if (def.stackable) {
		for (auto& pair : player.inventory) {
			ItemInstance& instance = pair.second;
			if (instance.itemId == itemId) {
				instance.quantity += quantity;
				send_inventory_and_equipment();
				return;
			}
		}
	}

	int numInstancesToAdd = def.stackable ? 1 : quantity;
	int qtyPerInstance = def.stackable ? quantity : 1;

	// Set up random number generator
	std::random_device rd;
	std::mt19937 gen(rd());

	for (int i = 0; i < numInstancesToAdd; ++i) {
		uint64_t newInstanceId = g_item_instance_id_counter++;
		ItemInstance newInstance = { newInstanceId, itemId, qtyPerInstance, {}, {} };

		// --- Weighted Random Effect Roll ---
		if (def.equipSlot != EquipSlot::None && !g_random_effect_pool.empty()) {
			std::uniform_int_distribution<> initial_roll(1, 100);
			if (initial_roll(gen) <= 20) { // 15% chance to roll
				int item_tier = std::max(1, def.item_tier);
				std::vector<const RandomEffectDefinition*> available_effects;
				int total_weight = 0;

				for (const auto& effect_def : g_random_effect_pool) {
					if (effect_def.power_level <= item_tier) {
						available_effects.push_back(&effect_def);
						total_weight += effect_def.rarity_weight;
					}
				}

				if (!available_effects.empty() && total_weight > 0) {
					std::uniform_int_distribution<> effect_roll(0, total_weight - 1);
					int roll_result = effect_roll(gen);

					const RandomEffectDefinition* chosen_effect_def = nullptr;
					for (const auto* effect_def_ptr : available_effects) {
						if (roll_result < effect_def_ptr->rarity_weight) {
							chosen_effect_def = effect_def_ptr;
							break;
						}
						roll_result -= effect_def_ptr->rarity_weight;
					}

					if (chosen_effect_def) {
						newInstance.customEffects.push_back(chosen_effect_def->gameplay_effect);

						try {
							const auto& suffix_pool = g_effect_suffix_pools.at(chosen_effect_def->effect_key);
							if (!suffix_pool.empty()) {
								std::string chosen_suffix = suffix_pool[gen() % suffix_pool.size()];
								ItemEffect suffixEffect;
								suffixEffect.type = "SUFFIX";
								suffixEffect.params["value"] = chosen_suffix;
								newInstance.customEffects.push_back(suffixEffect);
							}
						}
						catch (const std::out_of_range&) {
							std::cerr << "Warning: No suffix pool for key: "
								<< chosen_effect_def->effect_key << std::endl;
						}

						std::cout << "[EFFECT ROLL] New item " << def.name
							<< " gained effect: " << chosen_effect_def->effect_key << std::endl;
					}
				}
			}
		}

		// Add the new item to the player's inventory
		player.inventory[newInstanceId] = newInstance;

		// --- DEBUG BLOCK ---
		std::cout << "[DEBUG] Added new item to memory inventory:"
			<< "\n  Instance ID: " << newInstanceId
			<< "\n  Item ID: " << itemId
			<< "\n  Quantity: " << qtyPerInstance
			<< "\n  Current Inventory Count: " << player.inventory.size()
			<< std::endl;
	}

	// Send update to client (we rely on autosave for persistence)
	send_inventory_and_equipment();
}


string AsyncSession::equipItem(uint64_t itemInstanceId) {
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
		return "Invalid equipment slot.";
	}

	string msg = "";
	if (player.equipment.slots[slot].has_value()) {
		uint64_t oldInstanceId = player.equipment.slots[slot].value();
		const auto& oldDef = player.inventory.at(oldInstanceId).getDefinition();
		msg = " (replacing " + oldDef.name + ")";
	}
	player.equipment.slots[slot] = itemInstanceId;

	send_player_stats();
	send_inventory_and_equipment();

	return "Equipped " + def.name + "." + msg;
}

string AsyncSession::unequipItem(EquipSlot slotToUnequip) {
	PlayerState& player = getPlayerState();

	if (player.equipment.slots.count(slotToUnequip) == 0 || slotToUnequip == EquipSlot::None) {
		return "Invalid equipment slot.";
	}
	if (!player.equipment.slots[slotToUnequip].has_value()) {
		return "No item equipped in that slot.";
	}

	uint64_t instanceId = player.equipment.slots[slotToUnequip].value();
	const ItemDefinition& def = player.inventory.at(instanceId).getDefinition();
	player.equipment.slots[slotToUnequip] = std::nullopt;

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
	string effectMsg = "";

	if (instance.itemId == "SMALL_HEALTH_POTION") {
		PlayerStats finalStats = getCalculatedStats();
		int healAmount = 50;
		if (player.stats.health < finalStats.maxHealth) {
			player.stats.health = min(finalStats.maxHealth, player.stats.health + healAmount);
			itemUsed = true;
			effectMsg = "You restore " + to_string(healAmount) + " health.";
		}
		else {
			effectMsg = "Your health is already full.";
		}
	}
	else if (instance.itemId == "LARGE_HEALTH_POTION") {
		PlayerStats finalStats = getCalculatedStats();
		int healAmount = 250;
		if (player.stats.health < finalStats.maxHealth) {
			player.stats.health = min(finalStats.maxHealth, player.stats.health + healAmount);
			itemUsed = true;
			effectMsg = "You restore " + to_string(healAmount) + " health.";
		}
		else {
			effectMsg = "Your health is already full.";
		}
	}
	else if (instance.itemId == "SMALL_MANA_POTION") {
		PlayerStats finalStats = getCalculatedStats();
		int manaAmount = 50;
		if (player.stats.mana < finalStats.maxMana) {
			player.stats.mana = min(finalStats.maxMana, player.stats.mana + manaAmount);
			itemUsed = true;
			effectMsg = "You restore " + to_string(manaAmount) + " mana.";
		}
		else {
			effectMsg = "Your mana is already full.";
		}
	}
	else {
		effectMsg = "That item has no use.";
	}

	ws.write(net::buffer("SERVER:STATUS:" + effectMsg));

	if (itemUsed) {
		instance.quantity--;
		if (instance.quantity <= 0) {
			player.inventory.erase(itemInstanceId);
		}
		send_inventory_and_equipment();
		send_player_stats();
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
			ws.write(net::buffer("SERVER:STATUS:Dropped " + to_string(instance.quantity) + "x " + def.name + "."));
			player.inventory.erase(itemInstanceId);
		}
		else {
			instance.quantity -= quantity;
			ws.write(net::buffer("SERVER:STATUS:Dropped " + to_string(quantity) + "x " + def.name + "."));
		}
	}
	send_inventory_and_equipment();
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
	catch (const out_of_range&) {
		cerr << "WARNING: Item " << def.id << " has no defined price. Defaulting to 1." << endl;
	}
	int sellPricePerItem = max(1, buyPrice / 4);
	int totalSellPrice = 0;

	if (!def.stackable) {
		totalSellPrice = sellPricePerItem;
		ws.write(net::buffer("SERVER:STATUS:Sold " + def.name + " for " + to_string(totalSellPrice) + " gold."));
		player.inventory.erase(itemInstanceId);
	}
	else {
		if (quantity >= instance.quantity) {
			totalSellPrice = sellPricePerItem * instance.quantity;
			ws.write(net::buffer("SERVER:STATUS:Sold " + to_string(instance.quantity) + "x " + def.name + " for " + to_string(totalSellPrice) + " gold."));
			player.inventory.erase(itemInstanceId);
		}
		else {
			totalSellPrice = sellPricePerItem * quantity;
			instance.quantity -= quantity;
			ws.write(net::buffer("SERVER:STATUS:Sold " + to_string(quantity) + "x " + def.name + " for " + to_string(totalSellPrice) + " gold."));
		}
	}

	player.stats.gold += totalSellPrice;
	send_player_stats();
	send_inventory_and_equipment();
}


void AsyncSession::send_player_stats() {
	PlayerState& player = getPlayerState();
	auto& ws = getWebSocket();

	PlayerStats finalStats = getCalculatedStats();
	std::vector<std::string> serverSpells;
	std::vector<std::string> serverSkills;

	for (const auto& name : player.skills.spells) {
		auto it = g_skill_defs.find(name);
		if (it == g_skill_defs.end()) {
			// unknown entry in DB, ignore instead of crashing
			continue;
		}

		const SkillDefinition& def = it->second;
		if (def.type == SkillType::SPELL) {
			serverSpells.push_back(name);
		}
		else if (def.type == SkillType::ABILITY) {
			serverSkills.push_back(name);
		}
	}


	ostringstream oss;
	oss << "SERVER:STATS:"
		<< "{\"playerName\":" << nlohmann::json(player.playerName).dump()
		<< ",\"health\":" << player.stats.health
		<< ",\"maxHealth\":" << finalStats.maxHealth
		<< ",\"mana\":" << player.stats.mana
		<< ",\"maxMana\":" << finalStats.maxMana
		<< ",\"defense\":" << finalStats.defense
		<< ",\"speed\":" << finalStats.speed
		<< ",\"level\":" << player.stats.level
		<< ",\"experience\":" << player.stats.experience
		<< ",\"experienceToNextLevel\":" << player.stats.experienceToNextLevel
		<< ",\"availableSkillPoints\":" << player.availableSkillPoints
		<< ",\"strength\":" << finalStats.strength
		<< ",\"dexterity\":" << finalStats.dexterity
		<< ",\"intellect\":" << finalStats.intellect
		<< ",\"luck\":" << finalStats.luck
		<< ",\"gold\":" << finalStats.gold
		<< ",\"posX\":" << player.posX
		<< ",\"posY\":" << player.posY;
	oss << ",\"playerClass\":" << static_cast<int>(player.currentClass);

	// --- Include both spells and skills ---
	oss << ",\"spells\":" << nlohmann::json(serverSpells).dump();
	oss << ",\"skills\":" << nlohmann::json(serverSkills).dump();

	// still send life_skills for future use
	oss << ",\"life_skills\":" << nlohmann::json(player.skills.life_skills).dump();

	oss << "}";
	string stats_message = oss.str();
	ws.write(net::buffer(stats_message));
}
void AsyncSession::send_inventory_and_equipment() {
	PlayerState& player = getPlayerState();
	auto& ws = getWebSocket();

	using json = nlohmann::json;
	json payload;
	json inventory = json::array();
	json equipment = json::object();

	// --- Collect equipped item instance IDs ---
	std::set<uint64_t> equipped_item_ids;
	for (const auto& slot_pair : player.equipment.slots) {
		if (slot_pair.second.has_value()) {
			equipped_item_ids.insert(slot_pair.second.value());
		}
	}

	// --- Build inventory JSON array ---
	for (const auto& [instanceId, instance] : player.inventory) {
		if (equipped_item_ids.count(instance.instanceId))
			continue; // skip equipped items in the inventory list

		const ItemDefinition& def = instance.getDefinition();

		// --- Find suffix (e.g. " of Flames") ---
		std::string suffix;
		for (const auto& effect : instance.customEffects) {
			if (effect.type == "SUFFIX" && effect.params.count("value")) {
				suffix = " " + effect.params.at("value");
				break;
			}
		}

		// --- Build JSON for custom effects ---
		json effects_json = json::array();
		for (const auto& eff : instance.customEffects) {
			json eff_json;
			eff_json["type"] = eff.type;
			eff_json["params"] = eff.params;
			effects_json.push_back(eff_json);
		}

		// --- Add inventory item entry ---
		json item_json = {
			{"instanceId", instance.instanceId},
			{"itemId", instance.itemId},
			{"name", def.name + suffix},
			{"desc", def.description},
			{"imagePath", def.imagePath},
			{"quantity", instance.quantity},
			{"slot", static_cast<int>(def.equipSlot)},
			{"baseStats", def.stats},
			{"customStats", instance.customStats},
			{"customEffects", effects_json}
		};

		inventory.push_back(item_json);
	}

	// --- Build equipment JSON (full objects, not just IDs) ---
	for (const auto& [slot, optId] : player.equipment.slots) {
		int slotInt = static_cast<int>(slot);

		if (optId.has_value()) {
			uint64_t instanceId = optId.value();

			if (player.inventory.count(instanceId)) {
				const ItemInstance& instance = player.inventory.at(instanceId);
				const ItemDefinition& def = instance.getDefinition();

				// --- Find suffix ---
				std::string suffix;
				for (const auto& eff : instance.customEffects) {
					if (eff.type == "SUFFIX" && eff.params.count("value")) {
						suffix = " " + eff.params.at("value");
						break;
					}
				}

				// --- Build JSON for custom effects ---
				json effects_json = json::array();
				for (const auto& eff : instance.customEffects) {
					json eff_json;
					eff_json["type"] = eff.type;
					eff_json["params"] = eff.params;
					effects_json.push_back(eff_json);
				}

				// --- Add full equipment object ---
				equipment[std::to_string(slotInt)] = {
					{"instanceId", instance.instanceId},
					{"itemId", instance.itemId},
					{"name", def.name + suffix},
					{"desc", def.description},
					{"imagePath", def.imagePath},
					{"quantity", instance.quantity},
					{"slot", slotInt},
					{"baseStats", def.stats},
					{"customStats", instance.customStats},
					{"customEffects", effects_json}
				};
			}
			else {
				equipment[std::to_string(slotInt)] = nullptr;
			}
		}
		else {
			equipment[std::to_string(slotInt)] = nullptr;
		}
	}

	// --- Combine payload ---
	payload["inventory"] = inventory;
	payload["equipment"] = equipment;

	// --- Send to client ---
	std::string msg = "SERVER:INVENTORY_UPDATE:" + payload.dump();
	ws.write(net::buffer(msg));

	std::cout << "[DEBUG] Sent inventory + equipment update: "
		<< "Inventory=" << player.inventory.size()
		<< " Equipped=" << player.equipment.slots.size()
		<< std::endl;
}


void AsyncSession::send_available_areas() {
	auto& ws = getWebSocket();
	vector<string> areas = ALL_AREAS;
	unsigned seed = (unsigned int)chrono::system_clock::now().time_since_epoch().count();
	default_random_engine rng(seed);
	shuffle(areas.begin(), areas.end(), rng);

	int count = (rand() % 3) + 2;
	string area_list;
	for (int i = 0; i < (int)min((size_t)count, areas.size()); ++i) { // FIXED: Added (int) cast
		if (!area_list.empty()) area_list += ",";
		area_list += areas[i];
	}
	string response = "SERVER:AREAS:" + area_list;
	ws.write(net::buffer(response));
}

void AsyncSession::send_interactables(const string& areaName) {
	auto& ws = getWebSocket();
	ostringstream oss;
	oss << "SERVER:INTERACTABLES:[";

	auto it = g_interactable_objects.find(areaName);
	if (it != g_interactable_objects.end()) {
		bool first = true;
		for (const auto& obj : it->second) {
			if (!first) oss << ",";
			oss << "{\"id\":" << nlohmann::json(obj.id).dump()
				<< ",\"type\":" << static_cast<int>(obj.type)
				<< ",\"x\":" << obj.position.x
				<< ",\"y\":" << obj.position.y
				<< ",\"data\":" << nlohmann::json(obj.data).dump() << "}";
			first = false;
		}
	}
	oss << "]";
	string message = oss.str();
	ws.write(net::buffer(message));
}



void AsyncSession::generate_and_send_monsters() {
	PlayerState& player = getPlayerState();
	player.currentMonsters.clear();

	auto grid_it = g_area_grids.find(player.currentArea);
	if (grid_it == g_area_grids.end()) {
		send_current_monsters_list();
		return;
	}
	const auto& grid = grid_it->second;

	int monster_count = (rand() % 3) + 2;
	for (int i = 0; i < monster_count; ++i) {
		int template_index = rand() % MONSTER_KEYS.size();
		string key = MONSTER_KEYS[template_index];
		MonsterState monster;
		monster.id = global_monster_id_counter++;
		monster.type = key;
		monster.assetKey = MONSTER_ASSETS.at(key);

		int x, y;
		do {
			x = rand() % GRID_COLS;
			y = rand() % GRID_ROWS;
		} while (y >= grid.size() || x >= grid[y].size() || grid[y][x] != 0);

		monster.posX = x;
		monster.posY = y;

		player.currentMonsters.push_back(monster);
	}
	send_current_monsters_list();
}

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
		cout << "[Level Up] Player " << player.playerName << " reached level " << player.stats.level << endl;

		string level_msg = "SERVER:LEVEL_UP:You have reached level " + to_string(player.stats.level) + "! You feel stronger!";
		ws.write(net::buffer(level_msg));
		string prompt_msg = "SERVER:PROMPT:You have " + to_string(player.availableSkillPoints) + " new skill points to spend.";
		ws.write(net::buffer(prompt_msg));
	}
}

//void AsyncSession::send_area_map_data(const string& areaName) {
//    auto& ws = getWebSocket();
//    ostringstream oss;
//    oss << "SERVER:MAP_DATA:";
//
//    auto it = g_area_grids.find(areaName);
//    if (it != g_area_grids.end()) {
//        const auto& grid = it->second;
//        for (int y = 0; y < GRID_ROWS; ++y) {
//            for (int x = 0; x < GRID_COLS; ++x) {
//                if (y < grid.size() && x < grid[y].size()) {
//                    oss << grid[y][x];
//                }
//                else {
//                    oss << '0';
//                }
//            }
//        }
//    }#
//    else {
//        string open_map(GRID_COLS * GRID_ROWS, '0');
//        oss << open_map;
//    }
//    string message = oss.str();
//    cout << "[DEBUG] Map data message length: " << message.length() << endl;
//    ws.write(net::buffer(message));
//}




void AsyncSession::send_area_map_data(const std::string& areaName) {
	auto& ws = getWebSocket();
	std::ostringstream oss;
	oss << "SERVER:MAP_DATA:";

	nlohmann::json payload;
	payload["area"] = areaName;

	// --- Check area
	auto areaIt = g_areas.find(areaName);
	if (areaIt == g_areas.end()) {
		payload["error"] = "Unknown area";
		oss << payload.dump();
		ws.write(net::buffer(oss.str()));
		return;
	}

	const auto& area = areaIt->second;


	// --- Grid
	if (area.grid) {
		std::string gridData;
		for (const auto& row : *area.grid) {
			for (int cell : row) {
				gridData.push_back(cell ? '1' : '0');
			}
		}
		payload["grid"] = gridData;
	}
	else {
		payload["grid"] = std::string(GRID_COLS * GRID_ROWS, '0');
	}

	// --- Interactables (includes NPCs, shops, transitions, etc.)
	nlohmann::json interactables = nlohmann::json::array();
	for (const auto& obj : area.interactables) {
		interactables.push_back({
			{"id", obj.id},
			{"type", static_cast<int>(obj.type)},
			{"x", obj.position.x},
			{"y", obj.position.y},
			{"data", obj.data}
			});
	}
	payload["interactables"] = interactables;

	// --- Zones (optional)
	nlohmann::json zones = nlohmann::json::array();
	for (const auto& z : area.zones) {
		zones.push_back({
			{"x", z.x},
			{"y", z.y},
			{"target", z.targetArea}
			});
	}
	payload["zones"] = zones;

	// --- Monsters
	nlohmann::json monsters = nlohmann::json::array();
	for (const auto& m : area.monsters) {
		// 'm.name' is the template key (e.g., "SLIME", "GIANT_SPIDER")
		auto templateIt = MONSTER_TEMPLATES.find(m.name);
		if (templateIt == MONSTER_TEMPLATES.end()) {
			std::cerr << "[Warning] Area '" << areaName << "' has invalid monster key '" << m.name << "'\n";
			continue;
		}

		// Get the actual template data
		const auto& monsterTemplate = templateIt->second;
		monsters.push_back({
			{"id", m.id},
			{"name", monsterTemplate.type},   // <-- Send the display name (e.g., "Slime")
			{"asset", monsterTemplate.assetKey},
			{"x", m.x},
			{"y", m.y},
			{"minCount", m.minCount},
			{"maxCount", m.maxCount}
			});
	}
	payload["monsters"] = monsters;

	// --- Send JSON payload
	oss << payload.dump();
	ws.write(net::buffer(oss.str()));
}



// --- NEW: Auth and DB Functions ---

void AsyncSession::handle_register(const string& credentials) {
	auto& ws = ws_;
	string username, password;
	stringstream ss(credentials);
	if (!getline(ss, username, ':') || !getline(ss, password)) {
		ws.write(net::buffer("SERVER:ERROR:Invalid registration format."));
		return;
	}
	if (username.length() < 3 || username.length() > 20) {
		ws.write(net::buffer("SERVER:ERROR:Username must be 3-20 characters."));
		return;
	}
	if (password.length() < 6) {
		ws.write(net::buffer("SERVER:ERROR:Password must be at least 6 characters."));
		return;
	}

	try {
		char hashed_password[crypto_pwhash_STRBYTES];
		if (crypto_pwhash_str(hashed_password, password.c_str(), password.length(),
			crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {

			throw runtime_error("Failed to hash password.");
		}
		string password_hash_str(hashed_password);

		pqxx::connection C = db_manager_->get_connection();
		pqxx::work W(C);

		string sql =
			"INSERT INTO accounts (username, password_hash, player_name, "
			"base_health, base_mana, base_defense, base_speed, "
			"base_strength, base_dexterity, base_intellect, base_luck) "
			"VALUES ($1, $2, $3, 100, 50, 10, 10, 10, 10, 10, 5)";

		W.exec(pqxx::zview(sql), pqxx::params(username, password_hash_str, username));
		W.commit();

		ws.write(net::buffer("SERVER:REGISTRATION_SUCCESS:Account created. Please log in."));

	}
	catch (const pqxx::unique_violation& e) {
		cerr << "Registration failed (unique_violation): " << e.what() << endl;
		ws.write(net::buffer("SERVER:ERROR:Username is already taken."));
	}
	catch (const exception&) {
		cerr << "Registration error: " << endl;
		ws.write(net::buffer("SERVER:ERROR:An internal error occurred."));
	}
}

void AsyncSession::handle_login(const string& credentials) {
	auto& ws = ws_;

	string username, password;
	stringstream ss(credentials);
	if (!getline(ss, username, ':') || !getline(ss, password)) {
		ws.write(net::buffer("SERVER:ERROR:Invalid login format."));
		return;
	}

	try {
		pqxx::connection C = db_manager_->get_connection();
		pqxx::nontransaction N(C);
		string sql = "SELECT id, password_hash, player_class FROM accounts WHERE username = $1";
		pqxx::result R = N.exec(pqxx::zview(sql), pqxx::params(username));

		if (R.empty()) {
			ws.write(net::buffer("SERVER:ERROR:Invalid username or password."));
			return;
		}

		string stored_hash = R[0]["password_hash"].as<string>();

		if (crypto_pwhash_str_verify(stored_hash.c_str(), password.c_str(), password.length()) != 0) {
			ws.write(net::buffer("SERVER:ERROR:Invalid username or password."));
			return;
		}

		is_authenticated_ = true;
		account_id_ = R[0]["id"].as<int>();
		string player_class_str = R[0]["player_class"].as<string>();
		string was_good = "SERVER:LOGIN_SUCCESS";
		ws.write(net::buffer(was_good));

		load_character(account_id_.load());
		ensureAutoGrantedSkillsForClass();

		if (player_class_str == "UNSELECTED") {
			send_player_stats();
			ws.write(net::buffer("SERVER:PROMPT:Welcome! Please pick a class!"));
		}
		else {
			send_player_stats();
			send_inventory_and_equipment();
			string did_load = "SERVER:CHARACTER_LOADED";
			ws.write(net::buffer(did_load));
			handle_message("GO_TO:" + getPlayerState().currentArea);

		}

	}
	catch (const exception&) {
		cerr << "Login error: " << endl;
		ws.write(net::buffer("SERVER:ERROR:An internal server error occurred."));
	}
}
void AsyncSession::ensureAutoGrantedSkillsForClass()
{
	PlayerState& player = getPlayerState();

	if (player.currentClass == PlayerClass::UNSELECTED) {
		return;
	}

	// Convert PlayerClass -> SkillClass
	SkillClass playerSkillClass = SkillClass::ANY;
	switch (player.currentClass) {
	case PlayerClass::FIGHTER: playerSkillClass = SkillClass::WARRIOR; break;
	case PlayerClass::ROGUE:   playerSkillClass = SkillClass::ROGUE;   break;
	case PlayerClass::WIZARD:  playerSkillClass = SkillClass::WIZARD;  break;
	default:                   playerSkillClass = SkillClass::ANY;     break;
	}

	// Loop all defined skills and grant those that:
	// - are flagged autoGranted
	// - match this class (or ANY)
	for (const auto& kv : g_skill_defs) {
		const std::string& skillName = kv.first;
		const SkillDefinition& def = kv.second;

		if (!def.autoGranted) continue;

		if (def.requiredClass != SkillClass::ANY &&
			def.requiredClass != playerSkillClass) {
			continue;
		}

		std::string err;
		// This function already:
		//  - checks class again
		//  - checks "already known"
		//  - saves to DB
		//  - calls send_player_stats()
		grantSkillToPlayer(skillName, err);
		// We ignore false here – it will fail for “already known”
		// or DB issues, which is fine for a best-effort starter sync.
	}
}


void AsyncSession::load_character(int accountId) {
	PlayerState& player = getPlayerState();
	using json = nlohmann::json;

	try {
		pqxx::connection C = db_manager_->get_connection();
		pqxx::nontransaction N(C);

		string sql_accounts = "SELECT * FROM accounts WHERE id = $1";
		pqxx::result R = N.exec(pqxx::zview(sql_accounts), pqxx::params(accountId));

		if (R.empty()) {
			throw runtime_error("No account found for loaded ID.");
		}

		auto row = R[0];
		player.playerName = row["player_name"].as<string>();
		player.currentArea = row["current_area"].as<string>();
		player.posX = row["pos_x"].as<int>();
		player.posY = row["pos_y"].as<int>();

		string class_str = row["player_class"].as<string>();
		if (class_str == "FIGHTER") player.currentClass = PlayerClass::FIGHTER;
		else if (class_str == "WIZARD") player.currentClass = PlayerClass::WIZARD;
		else if (class_str == "ROGUE") player.currentClass = PlayerClass::ROGUE;
		else player.currentClass = PlayerClass::UNSELECTED;

		player.stats.maxHealth = row["base_health"].as<int>();
		player.stats.health = player.stats.maxHealth;
		player.stats.maxMana = row["base_mana"].as<int>();
		player.stats.mana = player.stats.maxMana;
		player.stats.defense = row["base_defense"].as<int>();
		player.stats.speed = row["base_speed"].as<int>();
		player.stats.strength = row["base_strength"].as<int>();
		player.stats.dexterity = row["base_dexterity"].as<int>();
		player.stats.intellect = row["base_intellect"].as<int>();
		player.stats.luck = row["base_luck"].as<int>();
		player.stats.level = row["level"].as<int>();
		player.stats.experience = row["experience"].as<int>();
		player.stats.experienceToNextLevel = row["experience_to_next_level"].as<int>();
		player.stats.gold = row["gold"].as<int>();

		player.availableSkillPoints = row["available_skill_points"].as<int>();

		if (player.currentClass != PlayerClass::UNSELECTED) {
			player.isFullyInitialized = true;
			player.hasSpentInitialPoints = true;
		}


		string skills_json_str = row["skills"].as<string>();
		json skills_json = json::parse(skills_json_str);
		if (skills_json.contains("spells")) {
			player.skills.spells = skills_json["spells"].get<vector<string>>();
		}
		if (skills_json.contains("life_skills")) {
			player.skills.life_skills = skills_json["life_skills"].get<map<string, int>>();
		}



		player.inventory.clear();
		std::string sql_items = "SELECT * FROM player_items WHERE account_id = $1";
		pqxx::result R_items = N.exec(pqxx::zview(sql_items), pqxx::params(accountId));

		for (auto item_row : R_items) {
			ItemInstance instance;
			instance.instanceId = item_row["instance_id"].as<uint64_t>();
			instance.itemId = item_row["item_id"].as<std::string>();
			instance.quantity = item_row["quantity"].as<int>();

			std::string stats_str = item_row["custom_stats"].as<std::string>();
			if (!stats_str.empty()) {
				try {
					instance.customStats = nlohmann::json::parse(stats_str).get<std::map<std::string, int>>();
				}
				catch (...) {
					instance.customStats.clear();
				}
			}

			std::string effects_str = item_row["custom_effects"].as<std::string>();
			if (!effects_str.empty()) {
				try {
					nlohmann::json effects_json = nlohmann::json::parse(effects_str);
					for (const auto& e : effects_json) {
						ItemEffect eff;
						eff.type = e.value("type", "");
						eff.params = e.value("params", std::map<std::string, std::string>{});
						instance.customEffects.push_back(eff);
					}
				}
				catch (...) {
					instance.customEffects.clear();
				}
			}

			player.inventory[instance.instanceId] = instance;

			if (!item_row["equipped_slot"].is_null()) {
				std::string slot_str = item_row["equipped_slot"].as<std::string>();
				if (slot_str == "Weapon") player.equipment.slots[EquipSlot::Weapon] = instance.instanceId;
				else if (slot_str == "Hat") player.equipment.slots[EquipSlot::Hat] = instance.instanceId;
				else if (slot_str == "Top") player.equipment.slots[EquipSlot::Top] = instance.instanceId;
				else if (slot_str == "Bottom") player.equipment.slots[EquipSlot::Bottom] = instance.instanceId;
				else if (slot_str == "Boots") player.equipment.slots[EquipSlot::Boots] = instance.instanceId;
			}
		}



		broadcast_data_.playerName = player.playerName;
		broadcast_data_.playerClass = player.currentClass;
		broadcast_data_.currentArea = player.currentArea;
		broadcast_data_.posX = player.posX;
		broadcast_data_.posY = player.posY;
		{
			lock_guard<mutex> lock(g_player_registry_mutex);
			g_player_registry[player.userId] = broadcast_data_;
		}
		cout << "Loaded character: " << player.playerName << endl;

	}
	catch (const exception&) {
		cerr << "FATAL: load_character error: " << endl;
		ws_.close(websocket::close_code::internal_error);
	}
}
bool AsyncSession::grantSkillToPlayer(const std::string& skillName, std::string& outError)
{
	PlayerState& player = getPlayerState();

	// 1) Check that the skill exists in your global registry
	auto itSkill = g_skill_defs.find(skillName);
	if (itSkill == g_skill_defs.end()) {
		outError = "Unknown skill: " + skillName;
		return false;
	}
	const SkillDefinition& skill = itSkill->second;

	// 2) Check class requirement
	SkillClass playerSkillClass = SkillClass::ANY;
	switch (player.currentClass) {
	case PlayerClass::FIGHTER: playerSkillClass = SkillClass::WARRIOR; break;
	case PlayerClass::ROGUE:   playerSkillClass = SkillClass::ROGUE;   break;
	case PlayerClass::WIZARD:  playerSkillClass = SkillClass::WIZARD;  break;
	default:                   playerSkillClass = SkillClass::ANY;     break;
	}

	if (skill.requiredClass != SkillClass::ANY &&
		skill.requiredClass != playerSkillClass) {
		outError = "Class cannot learn this skill.";
		return false;
	}

	// 3) Already known?
	auto& known = player.skills.spells;
	if (std::find(known.begin(), known.end(), skillName) != known.end()) {
		outError = "Skill already known.";
		return false;
	}

	// 4) Add to in-memory list
	known.push_back(skillName);

	// 5) Persist skills JSON to DB
	try {
		pqxx::connection C = db_manager_->get_connection();
		pqxx::work W(C);

		nlohmann::json skills_json;
		skills_json["spells"] = player.skills.spells;
		skills_json["life_skills"] = player.skills.life_skills;

		std::string skills_str = skills_json.dump();

		std::string sql = "UPDATE accounts SET skills = $1 WHERE id = $2";
		W.exec(pqxx::zview(sql),
			pqxx::params(skills_str, account_id_.load()));
		W.commit();
	}
	catch (const std::exception& e) {
		std::cerr << "grantSkillToPlayer DB error: " << e.what() << std::endl;
		// Roll back local change for safety
		known.pop_back();
		outError = "Failed to save skill to database.";
		return false;
	}

	// 6) Refresh stats/spell list for client usage
	send_player_stats();   // repopulates temporary_spells_list inside

	return true;
}

void SyncPlayerMonsters(PlayerState& player)
{
	player.currentMonsters.clear();

	auto areaIt = g_areas.find(player.currentArea);
	if (areaIt == g_areas.end()) {
		std::cerr << "[Warning] Tried to sync monsters for unknown area: "
			<< player.currentArea << "\n";
		return;
	}
	const auto& area = areaIt->second;

	const std::vector<std::vector<int>>* gridPtr = nullptr;
	auto gridIt = g_area_grids.find(player.currentArea);
	if (gridIt != g_area_grids.end()) {
		gridPtr = &gridIt->second;
	}

	for (const auto& m : area.monsters)
	{
		MonsterState state;

		// ✅ Use area configuration ID, not a fresh global one
		state.id = m.id;
		state.type = m.name;

		int x = 0;
		int y = 0;
		bool placed = false;
		int attempts = 0;

		while (!placed && attempts < 256) {
			attempts++;

			x = rand() % GRID_COLS;
			y = rand() % GRID_ROWS;

			if (gridPtr) {
				if (y < 0 || y >= static_cast<int>(gridPtr->size())) continue;
				if (x < 0 || x >= static_cast<int>((*gridPtr)[y].size())) continue;
				if ((*gridPtr)[y][x] != 0) continue; // must be walkable
			}

			placed = true;
		}

		if (!placed) {
			x = 0;
			y = 0;
		}

		state.posX = x;
		state.posY = y;

		auto tpl = MONSTER_TEMPLATES.find(m.name);
		if (tpl != MONSTER_TEMPLATES.end()) {
			state.assetKey = tpl->second.assetKey;
		}
		else {
			state.assetKey = "UNKNOWN";
			std::cerr << "[Warning] No template found for monster '"
				<< m.name << "' in area " << player.currentArea << "\n";
		}

		player.currentMonsters.push_back(state);
	}

	std::cout << "[Sync] Synced " << player.currentMonsters.size()
		<< " monsters for area " << player.currentArea
		<< " with random positions" << std::endl;
}


void AsyncSession::save_character() {
	if (!is_authenticated_ || account_id_ == 0)
		return;

	using json = nlohmann::json;
	PlayerState& player = getPlayerState();
	int current_account_id = account_id_.load();

	std::cout << "[SAVE] Saving character for account " << current_account_id << "..." << std::endl;

	try {
		pqxx::connection C = db_manager_->get_connection();

		// reduce lock contention – commit each section separately
		{
			pqxx::work W(C);

			json skills_json;
			skills_json["spells"] = player.skills.spells;
			skills_json["life_skills"] = player.skills.life_skills;
			std::string skills_json_string = skills_json.dump();

			std::string class_str = "UNSELECTED";
			if (player.currentClass == PlayerClass::FIGHTER) class_str = "FIGHTER";
			else if (player.currentClass == PlayerClass::WIZARD) class_str = "WIZARD";
			else if (player.currentClass == PlayerClass::ROGUE)  class_str = "ROGUE";

			std::string sql_accounts = R"(
                UPDATE accounts SET
                    player_name = $1, player_class = $2, current_area = $3,
                    pos_x = $4, pos_y = $5,
                    base_health = $6, base_mana = $7, base_defense = $8,
                    base_speed = $9, base_strength = $10, base_dexterity = $11,
                    base_intellect = $12, base_luck = $13,
                    level = $14, experience = $15, experience_to_next_level = $16,
                    gold = $17, available_skill_points = $18, skills = $19
                WHERE id = $20
            )";

			W.exec(pqxx::zview(sql_accounts), pqxx::params(
				player.playerName, class_str, player.currentArea,
				player.posX, player.posY,
				player.stats.maxHealth, player.stats.maxMana, player.stats.defense,
				player.stats.speed, player.stats.strength, player.stats.dexterity,
				player.stats.intellect, player.stats.luck,
				player.stats.level, player.stats.experience, player.stats.experienceToNextLevel,
				player.stats.gold, player.availableSkillPoints, skills_json_string,
				current_account_id
			));
			W.commit();
		}

		// --- Replace player_items in its own transaction ---
		{
			pqxx::work W(C);
			W.exec(pqxx::zview("DELETE FROM player_items WHERE account_id = $1"),
				pqxx::params(current_account_id));

			std::string sql_insert_item = R"(
                INSERT INTO player_items (
                    instance_id, account_id, item_id, quantity,
                    custom_stats, custom_effects, equipped_slot
                ) VALUES ($1, $2, $3, $4, $5, $6, $7)
            )";

			for (const auto& [instanceId, instance] : player.inventory) {
				json custom_stats_json = instance.customStats;
				std::string custom_stats_str = custom_stats_json.dump();

				json custom_effects_json = json::array();
				for (const auto& effect : instance.customEffects) {
					json j_effect;
					j_effect["type"] = effect.type;
					j_effect["params"] = effect.params;
					custom_effects_json.push_back(j_effect);
				}
				std::string custom_effects_str = custom_effects_json.dump();

				std::string equipped_slot_str;
				bool has_slot = false;
				for (const auto& [slot, optInstance] : player.equipment.slots) {
					if (optInstance.has_value() && optInstance.value() == instanceId) {
						switch (slot) {
						case EquipSlot::Weapon: equipped_slot_str = "Weapon"; break;
						case EquipSlot::Hat:    equipped_slot_str = "Hat";    break;
						case EquipSlot::Top:    equipped_slot_str = "Top";    break;
						case EquipSlot::Bottom: equipped_slot_str = "Bottom"; break;
						case EquipSlot::Boots:  equipped_slot_str = "Boots";  break;
						default: break;
						}
						has_slot = true;
						break;
					}
				}

				if (has_slot) {
					W.exec(pqxx::zview(sql_insert_item),
						pqxx::params(static_cast<long long>(instanceId),
							current_account_id,
							instance.itemId,
							instance.quantity,
							custom_stats_str,
							custom_effects_str,
							equipped_slot_str));
				}
				else {
					// pass a real SQL NULL via string literal "null" casted as zview
					W.exec(pqxx::zview(sql_insert_item),
						pqxx::params(static_cast<long long>(instanceId),
							current_account_id,
							instance.itemId,
							instance.quantity,
							custom_stats_str,
							custom_effects_str,
							pqxx::zview("null")));
				}
			}
			W.commit();
		}

		std::cout << "[SAVE] Successfully saved character for account " << current_account_id << std::endl;
	}
	catch (const std::exception& e) {
		std::cerr << "[SAVE SQL ERROR] " << e.what() << std::endl;
	}
}





void AsyncSession::handle_message(const string& message)
{
	PlayerState& player = getPlayerState();
	PlayerBroadcastData& broadcast_data = getBroadcastData();
	auto& ws = ws_;
	string client_address = client_address_;

	// --- 1. AUTH COMMANDS (Allowed *before* login) ---
	if (message.rfind("REGISTER:", 0) == 0) {
		handle_register(message.substr(9));
		return;
	}
	if (message.rfind("LOGIN:", 0) == 0) {
		handle_login(message.substr(6));
		return;
	}

	// --- 2. AUTHENTICATION GATE ---
	if (!is_authenticated_) {
		ws.write(net::buffer("SERVER:ERROR:You must be logged in to do that."));
		return;
	}

	// --- 3. ALL *EXISTING* GAME LOGIC ---
   /* else if (message.rfind("SET_NAME:", 0) == 0 && player.currentClass == PlayerClass::UNSELECTED) {
		string name = message.substr(9);
		if (name.length() < 2 || name.length() > 20) {
			ws.write(net::buffer("SERVER:ERROR:Name must be between 2 and 20 characters."));
		}
		else {
			try {
				pqxx::connection C = db_manager_->get_connection();
				pqxx::work W(C);
				W.exec(pqxx::zview("UPDATE accounts SET player_name = $1 WHERE id = $2"), pqxx::params(name, account_id_.load()));
				W.commit();

				player.playerName = name;
				broadcast_data.playerName = name;
				string response = "SERVER:NAME_SET:" + name;
				ws.write(net::buffer(response));
				string class_prompt = "SERVER:PROMPT:Welcome " + name + "! Choose your class: SELECT_CLASS:FIGHTER, SELECT_CLASS:WIZARD, or SELECT_CLASS:ROGUE";
				ws.write(net::buffer(class_prompt));
				cout << "[" << client_address << "] --- NAME SET: " << name << " ---" << endl;
				{ lock_guard<mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }

			}
			catch (const pqxx::unique_violation& e) {
				ws.write(net::buffer("SERVER:ERROR:That name is already taken."));
			}
			catch (const exception& e) {
				ws.write(net::buffer("SERVER:ERROR:An error occurred setting name."));
				cerr << "SET_NAME error: " << e.what() << endl;
			}
		}
	}*/
	else if (message.rfind("SELECT_CLASS:", 0) == 0 && player.currentClass == PlayerClass::UNSELECTED) {
		string class_str = message.substr(13);
		string class_to_db;

		if (class_str == "FIGHTER") {
			player.currentClass = PlayerClass::FIGHTER;
			class_to_db = "FIGHTER";
			broadcast_data.playerClass = player.currentClass;
		}
		else if (class_str == "WIZARD") {
			player.currentClass = PlayerClass::WIZARD;
			class_to_db = "WIZARD";
			broadcast_data.playerClass = player.currentClass;
		}
		else if (class_str == "ROGUE") {
			player.currentClass = PlayerClass::ROGUE;
			class_to_db = "ROGUE";
			broadcast_data.playerClass = player.currentClass;
		}
		else { ws.write(net::buffer("SERVER:ERROR:Invalid class.")); return; }

		player.stats = getStartingStats(player.currentClass);
		player.availableSkillPoints = 3;
		player.hasSpentInitialPoints = false;

		try {
			pqxx::connection C = db_manager_->get_connection();
			pqxx::work W(C);

			// Note the added available_skill_points field
			std::string sql =
				"UPDATE accounts SET "
				"player_class = $1, "
				"base_health = $2, base_mana = $3, base_defense = $4, "
				"base_speed = $5, base_strength = $6, base_dexterity = $7, "
				"base_intellect = $8, base_luck = $9, "
				"available_skill_points = $10 "
				"WHERE id = $11";

			W.exec(
				pqxx::zview(sql),
				pqxx::params(
					class_to_db,                     // player_class
					player.stats.maxHealth,          // base_health
					player.stats.maxMana,            // base_mana
					player.stats.defense,            // base_defense
					player.stats.speed,              // base_speed
					player.stats.strength,           // base_strength
					player.stats.dexterity,          // base_dexterity
					player.stats.intellect,          // base_intellect
					player.stats.luck,               // base_luck
					player.availableSkillPoints,     // available_skill_points
					account_id_.load()               // id
				)
			);

			W.commit();
		}
		catch (const std::exception&) {
			ws.write(net::buffer("SERVER:ERROR:An error occurred saving your class."));
			std::cerr << "SELECT_CLASS error: " << std::endl;
			player.currentClass = PlayerClass::UNSELECTED;
			player.skills.spells.clear();
			return;
		}

		//  Give autoGranted skills for this class (Fireball, BloodStrike, etc.)
		ensureAutoGrantedSkillsForClass();

		std::cout << "[" << client_address << "] --- CLASS SET: " << class_str << " ---" << endl;
		ws.write(net::buffer("SERVER:CLASS_SET:" + class_str));
		send_player_stats();
		ws.write(net::buffer("SERVER:PROMPT:You have 3 skill points to distribute. Use UPGRADE_STAT:stat_name to spend points."));
		{ lock_guard<mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }
	}
	else if (message.rfind("UPGRADE_STAT:", 0) == 0) {
		if (player.currentClass == PlayerClass::UNSELECTED) {
			ws.write(net::buffer("SERVER:ERROR:You must select a class first."));
		}
		else if (player.availableSkillPoints <= 0) { ws.write(net::buffer("SERVER:ERROR:You have no skill points available.")); }
		else {
			string stat_name = message.substr(13);
			bool valid_stat = false;
			if (stat_name == "health") { player.stats.maxHealth += 5; player.stats.health += 5; valid_stat = true; }
			else if (stat_name == "mana") { player.stats.maxMana += 5; player.stats.mana += 5; valid_stat = true; }
			else if (stat_name == "defense") { player.stats.defense += 1; valid_stat = true; }
			else if (stat_name == "speed") { player.stats.speed += 1; valid_stat = true; }
			else if (stat_name == "strength") { player.stats.strength += 1; valid_stat = true; }
			else if (stat_name == "dexterity") { player.stats.dexterity += 1; valid_stat = true; }
			else if (stat_name == "intellect") { player.stats.intellect += 1; valid_stat = true; }
			else if (stat_name == "luck") { player.stats.luck += 1; valid_stat = true; }

			if (valid_stat) {
				player.availableSkillPoints--;

				try {
					pqxx::connection C = db_manager_->get_connection();
					pqxx::work W(C);
					string sql = "UPDATE accounts SET "
						"base_health = $1, base_mana = $2, base_defense = $3, "
						"base_speed = $4, base_strength = $5, base_dexterity = $6, "
						"base_intellect = $7, base_luck = $8, available_skill_points = $9 "
						"WHERE id = $10";
					W.exec(pqxx::zview(sql), pqxx::params(
						player.stats.maxHealth, player.stats.maxMana, player.stats.defense,
						player.stats.speed, player.stats.strength, player.stats.dexterity,
						player.stats.intellect, player.stats.luck, player.availableSkillPoints,
						account_id_.load()
					));
					W.commit();
				}
				catch (const exception&) {
					ws.write(net::buffer("SERVER:ERROR:An error occurred saving your stats."));
					cerr << "UPGRADE_STAT error: " << endl;
					player.availableSkillPoints++;
					return;
				}

				ws.write(net::buffer("SERVER:STAT_UPGRADED:" + stat_name));
				send_player_stats();
				if (player.availableSkillPoints == 0 && !player.isFullyInitialized) {
					player.isFullyInitialized = true;
					player.hasSpentInitialPoints = true;
					ws.write(net::buffer("SERVER:CHARACTER_COMPLETE:Character creation complete! You can now explore."));
					send_available_areas();
				}
				else if (player.availableSkillPoints > 0) { ws.write(net::buffer("SERVER:PROMPT:You have " + to_string(player.availableSkillPoints) + " skill points remaining.")); }
				else { ws.write(net::buffer("SERVER:STATUS:All skill points spent.")); }
			}
			else { ws.write(net::buffer("SERVER:ERROR:Invalid stat name.")); }
		}
	}
	/*   else if (message.rfind("GO_TO:", 0) == 0) {
		   if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
		   else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot travel while in combat!")); }
		   else {
			   string target_area = message.substr(6);
			   player.currentPath.clear();
			   broadcast_data.currentArea = target_area;
			   { lock_guard<mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }

			   if (target_area == "TOWN") {
				   player.currentArea = "TOWN";
				   player.currentMonsters.clear();
				   player.stats.health = player.stats.maxHealth;
				   player.stats.mana = player.stats.maxMana;

				   string response = "SERVER:AREA_CHANGED:TOWN";
				   cout << "[DEBUG] Sending area change: '" << response << "'" << endl;
				   ws.write(net::buffer(response));

				   send_area_map_data(player.currentArea);
				   send_interactables(player.currentArea);
				   send_available_areas();
				   send_player_stats();
			   }
			   else if (find(ALL_AREAS.begin(), ALL_AREAS.end(), target_area) != ALL_AREAS.end()) {
				   player.currentArea = target_area;
				   ws.write(net::buffer("SERVER:AREA_CHANGED:" + target_area));
				   send_area_map_data(player.currentArea);
				   generate_and_send_monsters();
			   }
			   else {
				   ws.write(net::buffer("SERVER:ERROR:Invalid or unknown travel destination."));
				   broadcast_data.currentArea = player.currentArea;
				   { lock_guard<mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }
			   }
			   cout << "[" << client_address << "] --- AREA CHANGED TO: " << player.currentArea << " ---" << endl;
		   }
	   }*/else if (message.rfind("GO_TO:", 0) == 0)
	{
		PlayerState& player = getPlayerState();
		PlayerBroadcastData& broadcast_data = getBroadcastData();
		auto& ws = getWebSocket();

		if (!player.isFullyInitialized)
		{
			ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
			return;
		}

		if (player.isInCombat)
		{
			ws.write(net::buffer("SERVER:ERROR:Cannot travel while in combat!"));
			return;
		}

		std::string target_area = message.substr(6);
		player.currentPath.clear();

		// --- Validate area exists ---
		auto areaIt = g_areas.find(target_area);
		if (areaIt == g_areas.end())
		{
			ws.write(net::buffer("SERVER:ERROR:Invalid or unknown travel destination."));
			return;
		}

		// --- Update player + broadcast data ---
		player.currentArea = target_area;
		broadcast_data.currentArea = target_area;
		const auto& spawns = get_area_spawns();

		auto spawnIt = spawns.find(target_area);
		if (spawnIt != spawns.end())
		{
			player.posX = spawnIt->second.x;
			player.posY = spawnIt->second.y;
		}


		{
			std::lock_guard<std::mutex> lock(g_player_registry_mutex);
			g_player_registry[player.userId] = broadcast_data;
		}

		// --- Handle special zones (Town heals & clears combat) ---
		if (target_area == "TOWN")
		{
			PlayerStats finalStats = getCalculatedStats(); // Calculate maxes WITH equipment
			player.isInCombat = false;
			player.currentMonsters.clear();
			// Now, heal to the newly calculated maxes
			player.stats.health = finalStats.maxHealth;
			player.stats.mana = finalStats.maxMana;
		}

		// --- Notify client of area change ---
		ws.write(net::buffer("SERVER:AREA_CHANGED:" + target_area));
		std::cout << "[" << client_address_ << "] --- AREA CHANGED TO: "
			<< player.currentArea << " ---" << std::endl;

		// --- Send area data and monsters ---
		send_area_map_data(player.currentArea);
		SyncPlayerMonsters(player);
		send_current_monsters_list();
		send_player_stats();
	}

	   else if (message.rfind("MOVE_TO:", 0) == 0) {
		if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
		else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot move while in combat!")); }
		else {
			auto it = g_area_grids.find(player.currentArea);
			if (it == g_area_grids.end()) {
				ws.write(net::buffer("SERVER:ERROR:Grid movement is not available in this area."));
				return;
			}
			const auto& current_grid = it->second;

			try {
				string coords_str = message.substr(8);
				size_t comma_pos = coords_str.find(',');
				if (comma_pos == string::npos) throw invalid_argument("Invalid coordinate format.");

				int target_x = stoi(coords_str.substr(0, comma_pos));
				int target_y = stoi(coords_str.substr(comma_pos + 1));

				if (target_x < 0 || target_x >= GRID_COLS || target_y < 0 || target_y >= GRID_ROWS) {
					ws.write(net::buffer("SERVER:ERROR:Target coordinates are out of bounds."));
				}
				else if (current_grid[target_y][target_x] != 0) {
					ws.write(net::buffer("SERVER:ERROR:Cannot move to that location."));
				}
				else {
					Point start_pos = { player.posX, player.posY };
					Point end_pos = { target_x, target_y };
					player.currentPath = A_Star_Search(start_pos, end_pos, current_grid);
					player.lastMoveTime = chrono::steady_clock::now() - MOVEMENT_DELAY;
				}
			}
			catch (const exception&) {
				cerr << "Error parsing MOVE_TO: " << "\n";
				ws.write(net::buffer("SERVER:ERROR:Invalid coordinate format."));
			}
		}
	}
	   else if (message.rfind("SELL_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
		}
		else {
			try {
				// Expect: SELL_ITEM:instanceId:quantity
				std::string params = message.substr(10);
				size_t colon_pos = params.find(':');

				if (colon_pos == std::string::npos) {
					throw std::invalid_argument("Invalid format. Expected SELL_ITEM:instanceId:quantity");
				}

				uint64_t instanceId = std::stoull(params.substr(0, colon_pos));
				int quantity = std::stoi(params.substr(colon_pos + 1));

				sellItem(instanceId, quantity);
			}
			catch (const std::exception& e) {
				std::cerr << "Sell item error: " << e.what() << std::endl;
				ws.write(net::buffer("SERVER:ERROR:Invalid sell command format."));
			}
		}
	}
	   else if (message.rfind("SEND_CHAT:", 0) == 0) {
		if (!player.isFullyInitialized) {
			ws.write(net::buffer("SERVER:ERROR:Must complete character creation to chat."));
			return;
		}
		string chat_text = message.substr(10);
		if (chat_text.empty() || chat_text.length() > 100) {
			ws.write(net::buffer("SERVER:ERROR:Chat message must be 1-100 characters."));
			return;
		}

		auto shared_chat_msg = make_shared<string>(
			"SERVER:CHAT_MSG:{\"sender\":" + nlohmann::json(player.playerName).dump() + ",\"text\":" + nlohmann::json(chat_text).dump() + "}");

		vector<shared_ptr<AsyncSession>> all_sessions;
		{
			lock_guard<mutex> lock(g_session_registry_mutex);
			for (auto const& pair : g_session_registry) {
				if (auto session = pair.second.lock()) {
					all_sessions.push_back(session);
				}
			}
		}

		for (auto& session : all_sessions) {
			net::dispatch(session->ws_.get_executor(), [session, shared_chat_msg]() {
				try {
					session->ws_.write(net::buffer(*shared_chat_msg));
				}
				catch (exception const&) {
					cerr << "Chat broadcast write error: " << "\n";
				}
				});
		}
	}
	   else if (message.rfind("INTERACT_AT:", 0) == 0) {
		if (player.isInCombat) {
			ws.write(net::buffer("SERVER:ERROR:Cannot interact while in combat!")); {
			}
		}
		else {
			try {
				string coords_str = message.substr(12);
				size_t comma_pos = coords_str.find(',');
				if (comma_pos == string::npos) throw invalid_argument("Invalid coordinate format.");

				int target_x = stoi(coords_str.substr(0, comma_pos));
				int target_y = stoi(coords_str.substr(comma_pos + 1));

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

				int dist = abs(player.posX - target_x) + abs(player.posY - target_y);
				if (dist > 1) {
					ws.write(net::buffer("SERVER:ERROR:You are too far away to interact with that."));
					return;
				}

				player.currentPath.clear();

				if (targetObject->type == InteractableType::NPC) {
					ws.write(net::buffer("SERVER:NPC_INTERACT:" + targetObject->data));
					if (targetObject->data == "GUARD_DIALOGUE_1") {
						ws.write(net::buffer("SERVER:PROMPT:Guard: \"This place gets scary at night\""));
					}
					else if (targetObject->data == "MERCHANT_SHOP_1") {
						ws.write(net::buffer("SERVER:PROMPT:Merchant: \"You there, got some gold, I've got stuff that might appeal to you\""));
						auto shop_it = g_shops.find("MERCHANT_SHOP_1");
						if (shop_it == g_shops.end()) {
							ws.write(net::buffer("SERVER:ERROR:Shop inventory not found."));
							return;
						}

						ostringstream oss;
						oss << "SERVER:SHOW_SHOP:{\"shopId\":\"" << shop_it->first << "\",\"items\":[";

						bool firstItem = true;
						for (const string& itemId : shop_it->second) {
							if (itemDatabase.count(itemId) == 0) continue;
							const ItemDefinition& def = itemDatabase.at(itemId);

							int price = 1;
							try {
								price = g_item_buy_prices.at(itemId);
							}
							catch (const out_of_range&) {
								cerr << "WARNING: Shop item " << itemId << " has no price. Defaulting to 1." << endl;
							}

							if (!firstItem) oss << ",";
							oss << "{\"itemId\":" << nlohmann::json(def.id).dump()
								<< ",\"name\":" << nlohmann::json(def.name).dump()
								<< ",\"desc\":" << nlohmann::json(def.description).dump()
								<< ",\"imagePath\":" << nlohmann::json(def.imagePath).dump()
								<< ",\"price\":" << price
								<< ",\"slot\":" << static_cast<int>(def.equipSlot)
								<< ",\"baseStats\":" << nlohmann::json(def.stats).dump()
								<< "}";
							firstItem = false;
						}
						oss << "]}";
						ws.write(net::buffer(oss.str()));
					}
				}
				else if (targetObject->type == InteractableType::ZONE_TRANSITION) {
					handle_message("GO_TO:" + targetObject->data);
					send_area_map_data(player.currentArea);
					SyncPlayerMonsters(player);
					send_current_monsters_list();
					// --- Optional: confirm area change
					auto& ws = getWebSocket();
					ws.write(net::buffer("SERVER:AREA_CHANGED:" + player.currentArea));
				}
				else {
					ws.write(net::buffer("SERVER:ERROR:Unknown interaction type."));
				}
			}
			catch (const exception&) {
				cerr << "Error parsing INTERACT_AT: " << "\n";
				ws.write(net::buffer("SERVER:ERROR:Invalid coordinate format."));
			}
		}
	}
	   else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
		if (!player.isFullyInitialized) {
			ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
		}
		else if (player.isInCombat) {
			ws.write(net::buffer("SERVER:ERROR:You are already in combat!"));
		}
		else if (player.currentArea == "TOWN") {
			ws.write(net::buffer("SERVER:STATUS:No monsters to fight in TOWN."));
		}
		else {
			try {
				int selected_id = stoi(message.substr(17));
				auto it = std::find_if(
					player.currentMonsters.begin(),
					player.currentMonsters.end(),
					[selected_id](const MonsterState& m) { return m.id == selected_id; }
				);

				if (it != player.currentMonsters.end()) {
					player.isInCombat = true;

					// Create combat instance of the monster
					player.currentOpponent = create_monster(it->id, it->type);
					player.isDefending = false;
					player.currentMonsters.erase(it); // Remove monster from world

					// Check create_monster result
					if (!player.currentOpponent) {
						player.isInCombat = false;
						std::cerr << "CRITICAL: Failed to create monster instance for combat." << std::endl;
						ws.write(net::buffer("SERVER:ERROR:Internal error starting combat."));
						return;
					}

					std::cout << "[" << client_address << "] --- COMBAT STARTED vs "
						<< player.currentOpponent->type << " ---" << std::endl;

					std::ostringstream oss;
					oss << "SERVER:COMBAT_START:"
						<< "{\"id\":" << player.currentOpponent->id
						<< ",\"name\":" << nlohmann::json(player.currentOpponent->type).dump()
						<< ",\"asset\":" << nlohmann::json(player.currentOpponent->assetKey).dump()
						<< ",\"health\":" << player.currentOpponent->health
						<< ",\"maxHealth\":" << player.currentOpponent->maxHealth << "}";
					std::string combat_start_message = oss.str();
					ws.write(net::buffer(combat_start_message));
					ws.write(net::buffer("SERVER:COMBAT_LOG:You engaged the " + player.currentOpponent->type + "!"));

					PlayerStats finalStats = getCalculatedStats();

					// Speed check: who acts first?
					if (finalStats.speed >= player.currentOpponent->speed) {
						ws.write(net::buffer("SERVER:COMBAT_LOG:You are faster! You attack first."));
						ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
					}
					else {
						ws.write(net::buffer("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " is faster! It attacks first."));

						// --- Monster attacks first using new combat math ---

						float monsterAttackPower = attack_power_for_monster(*player.currentOpponent);
						int player_defense = finalStats.defense;
						int base_monster_damage = damage_after_defense(monsterAttackPower, player_defense);

						float variance = 0.85f + ((float)(rand() % 31) / 100.0f); // 0.85–1.15
						int monster_damage = std::max(1, (int)std::round(base_monster_damage * variance));

						float monster_crit_chance = crit_chance_for_monster(*player.currentOpponent);
						if (((float)rand() / RAND_MAX) < monster_crit_chance) {
							monster_damage = (int)std::round(monster_damage * 1.6f);
							ws.write(net::buffer(
								"SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " lands a critical hit!"
							));
						}

						player.stats.health -= monster_damage;
						ws.write(net::buffer(
							"SERVER:COMBAT_LOG:The " + player.currentOpponent->type +
							" attacks you for " + std::to_string(monster_damage) + " damage!"
						));
						send_player_stats();

						if (player.stats.health <= 0) {
							player.stats.health = 0;
							ws.write(net::buffer("SERVER:COMBAT_DEFEAT:You have been defeated!"));
							player.isInCombat = false;
							player.currentOpponent.reset();
							player.currentArea = "TOWN";
							player.currentMonsters.clear();
							player.stats.health = player.stats.maxHealth / 2;
							player.stats.mana = player.stats.maxMana;
							player.posX = 26;
							player.posY = 12;
							player.currentPath.clear();

							broadcast_data.currentArea = "TOWN";
							{
								std::lock_guard<std::mutex> lock(g_player_registry_mutex);
								g_player_registry[player.userId] = broadcast_data;
							}

							ws.write(net::buffer("SERVER:AREA_CHANGED:TOWN"));
							send_area_map_data(player.currentArea);
							send_available_areas();
							send_player_stats();
							broadcast_data.posX = player.posX;
							broadcast_data.posY = player.posY;
						}
						else {
							ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
						}
					}
				}
				else {
					ws.write(net::buffer("SERVER:ERROR:Selected monster ID not found."));
				}
			}
			catch (const std::exception&) {
				ws.write(net::buffer("SERVER:ERROR:Invalid monster ID format."));
			}
		}
	}

	   else if (message.rfind("COMBAT_ACTION:", 0) == 0) {
		if (!player.isInCombat || !player.currentOpponent) { ws.write(net::buffer("SERVER:ERROR:You are not in combat.")); }
		else {
			PlayerStats finalStats = getCalculatedStats();
			int extraDefFromBuffs = 0;
			bool monsterStunnedThisTurn = false;
			auto apply_statuses_to_player = [&]() {
				int totalDefBuff = 0;
				int totalDot = 0;

				// shorthand reference to the vector of effects
				auto& effects = player.activeStatusEffects;

				for (auto it = effects.begin(); it != effects.end(); ) {
					StatusEffect& eff = *it;

					switch (eff.type) {
					case StatusType::BURN:
					case StatusType::BLEED: {
						int dmg = eff.magnitude;
						if (eff.type == StatusType::BURN) {
							dmg += finalStats.intellect / 20;
						}
						else { // BLEED
							dmg += finalStats.dexterity / 25;
						}
						if (dmg < 1) dmg = 1;

						totalDot += dmg;
						player.stats.health = std::max(1, player.stats.health - dmg);
						break;
					}
					case StatusType::DEFENSE_UP: {
						totalDefBuff += eff.magnitude;
						break;
					}
					default:
						break;
					}

					eff.remainingTurns--;
					if (eff.remainingTurns <= 0) {
						it = effects.erase(it);
					}
					else {
						++it;
					}
				}

				if (totalDot > 0) {
					ws.write(net::buffer(
						"SERVER:COMBAT_LOG:You suffer " + std::to_string(totalDot) +
						" damage from ongoing effects!"
					));
					send_player_stats();
				}

				extraDefFromBuffs = totalDefBuff;
				};

			auto apply_statuses_to_monster = [&]() {
				if (!player.currentOpponent) return;

				int totalDot = 0;
				auto& effects = player.currentOpponent->activeStatusEffects;

				for (auto it = effects.begin(); it != effects.end(); ) {
					StatusEffect& eff = *it;

					switch (eff.type) {
					case StatusType::BURN:
					case StatusType::BLEED: {
						int dmg = eff.magnitude;
						if (dmg < 1) dmg = 1;

						totalDot += dmg;
						player.currentOpponent->health =
							std::max(1, player.currentOpponent->health - dmg);
						break;
					}
					case StatusType::STUN: {
						// Monster will skip its attack this turn
						monsterStunnedThisTurn = true;
						break;
					}
					default:
						break;
					}

					eff.remainingTurns--;
					if (eff.remainingTurns <= 0) {
						it = effects.erase(it);
					}
					else {
						++it;
					}
				}

				if (totalDot > 0) {
					ws.write(net::buffer(
						"SERVER:COMBAT_LOG:The " + player.currentOpponent->type +
						" suffers " + std::to_string(totalDot) +
						" damage from ongoing effects!"
					));
					std::string update =
						"SERVER:COMBAT_UPDATE:" + std::to_string(player.currentOpponent->health);
					ws.write(net::buffer(update));
				}
				};

			// Tick statuses at the start of every player action
			apply_statuses_to_player();
			apply_statuses_to_monster();



			string action_command = message.substr(14);
			string action_type; string action_param;
			size_t colon_pos = action_command.find(':');
			if (colon_pos != string::npos) { action_type = action_command.substr(0, colon_pos); action_param = action_command.substr(colon_pos + 1); }
			else { action_type = action_command; }

			int player_damage = 0; int mana_cost = 0; bool fled = false;
			if (action_type == "ATTACK") {
				float attackPower = attack_power_for_player(finalStats, player.currentClass);
				int base_damage = damage_after_defense(attackPower, player.currentOpponent->defense);

				float variance = 0.85f + ((float)(rand() % 31) / 100.0f); // 0.85–1.15
				int dmg = std::max(1, (int)(base_damage * variance));

				float crit_chance = crit_chance_for_player(finalStats, player.currentClass);
				if (((float)rand() / RAND_MAX) < crit_chance) {
					ClassCritTuning t = getCritTuning(player.currentClass);
					dmg = (int)std::round(dmg * t.critMultiplier);
					ws.write(net::buffer("SERVER:COMBAT_LOG:A critical hit!"));
				}

				player_damage = dmg; // then use your existing code that subtracts HP and logs
				std::string log_msg =
					"SERVER:COMBAT_LOG:You attack the " + player.currentOpponent->type +
					" for " + std::to_string(player_damage) + " damage!";
				ws.write(net::buffer(log_msg));
			}
			else if (action_type == "SPELL") {
				std::string spellName = action_param;

				// Look up the spell definition
				auto itSkill = g_skill_defs.find(spellName);
				if (itSkill == g_skill_defs.end() || itSkill->second.type != SkillType::SPELL) {
					ws.write(net::buffer("SERVER:COMBAT_LOG:Unknown spell: " + spellName));
					return;
				}
				const SkillDefinition& spell = itSkill->second;

				// Make sure the player actually knows this spell
				bool knows_spell = false;
				for (const auto& s : player.temporary_spells_list) {
					if (s == spellName) {
						knows_spell = true;
						break;
					}
				}
				if (!knows_spell) {
					ws.write(net::buffer("SERVER:COMBAT_LOG:You don't know that spell!"));
					return;
				}

				// Class requirement check
				SkillClass playerSkillClass = SkillClass::ANY;
				switch (player.currentClass) {
				case PlayerClass::FIGHTER: playerSkillClass = SkillClass::WARRIOR; break;
				case PlayerClass::ROGUE:   playerSkillClass = SkillClass::ROGUE;   break;
				case PlayerClass::WIZARD:  playerSkillClass = SkillClass::WIZARD;  break;
				default:                   playerSkillClass = SkillClass::ANY;     break;
				}

				if (spell.requiredClass != SkillClass::ANY &&
					spell.requiredClass != playerSkillClass) {
					ws.write(net::buffer("SERVER:COMBAT_LOG:You cannot cast that spell with your class."));
					return;
				}

				// Mana check
				if (player.stats.mana < spell.manaCost) {
					ws.write(net::buffer(
						"SERVER:COMBAT_LOG:Not enough mana to cast " + spellName +
						"! (Needs " + std::to_string(spell.manaCost) + ")"
					));
					return;
				}

				player.stats.mana -= spell.manaCost;

				bool targetIsSelf = (spell.target == SkillTarget::SELF);

				// Compute spell power using stat scales
				float scaledAttack =
					finalStats.strength * spell.strScale +
					finalStats.dexterity * spell.dexScale +
					finalStats.intellect * spell.intScale +
					spell.flatDamage;

				int base_damage = 0;
				float variance = 1.0f;

				if (!targetIsSelf) {
					// Magic partially ignores defense instead of fully ignoring it
					int targetDefense = player.currentOpponent->defense;
					if (spell.isMagic) {
						// 40% defense penetration: spells feel good vs tanky enemies
						targetDefense = static_cast<int>(std::round(targetDefense * 0.4f));
					}

					base_damage = damage_after_defense(scaledAttack, targetDefense);

					// Tight-ish variance for consistency (0.9–1.1)
					variance = 0.9f + (static_cast<float>(rand() % 21) / 100.0f);

					int dmg = std::max(1, static_cast<int>(std::round(base_damage * variance)));

					// Spells can crit too (fun!)
					float critChance = crit_chance_for_player(finalStats, player.currentClass);
					if (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) < critChance) {
						ClassCritTuning t = getCritTuning(player.currentClass);
						dmg = static_cast<int>(std::round(dmg * t.critMultiplier));
						ws.write(net::buffer("SERVER:COMBAT_LOG:Your " + spellName + " critically hits!"));
					}

					player_damage = dmg;

					std::string log_msg =
						"SERVER:COMBAT_LOG:You cast " + spellName +
						" for " + std::to_string(player_damage) + " damage!";
					ws.write(net::buffer(log_msg));
				}
				else {
					// Self-target spells (none for wizard yet, but future-proof)
					player_damage = 0;
					ws.write(net::buffer("SERVER:COMBAT_LOG:You cast " + spellName + " on yourself."));
				}

				// Apply status effects from the spell definition
				if (spell.appliesStatus) {
					bool applyStatus = true;

					StatusEffect eff;
					eff.type = spell.statusType;
					eff.magnitude = spell.statusMagnitude;
					eff.remainingTurns = spell.statusDuration;
					eff.appliedByPlayer = true;

					// Special behavior: Lightning stun is chance-based
					if (spellName == "Lightning" && spell.statusType == StatusType::STUN) {
						int baseChance = 20;                   // 20% base
						int fromLuck = finalStats.luck / 2;  // +0.5% per LUCK
						int finalChance = baseChance + fromLuck;
						if (finalChance > 70) finalChance = 70;
						if (finalChance < 20) finalChance = 20;

						int roll = rand() % 100;
						if (roll >= finalChance) {
							applyStatus = false;
							ws.write(net::buffer("SERVER:COMBAT_LOG:The lightning crackles, but fails to stun."));
						}
					}

					// Fireball burn scales a bit with INT
					if (spellName == "Fireball" && spell.statusType == StatusType::BURN) {
						eff.magnitude += std::max(1, finalStats.intellect / 10);
					}

					if (applyStatus) {
						if (targetIsSelf) {
							player.activeStatusEffects.push_back(eff);
						}
						else {
							player.currentOpponent->activeStatusEffects.push_back(eff);
						}

						std::string statusMsg;
						switch (spell.statusType) {
						case StatusType::BURN:
							statusMsg = targetIsSelf
								? "You are burning!"
								: "The " + player.currentOpponent->type + " is set ablaze!";
							break;
						case StatusType::STUN:
							statusMsg = targetIsSelf
								? "You are stunned!"
								: "The " + player.currentOpponent->type + " is stunned!";
							break;
						default:
							statusMsg = targetIsSelf
								? "You are affected by a status effect."
								: "The " + player.currentOpponent->type + " is affected by a status effect.";
							break;
						}

						ws.write(net::buffer("SERVER:COMBAT_LOG:" + statusMsg));
					}
				}
			}

			else if (action_type == "SKILL") {
				std::string skillName = action_param;

				// Look up the skill in the global registry
				auto itSkill = g_skill_defs.find(skillName);
				if (itSkill == g_skill_defs.end()) {
					ws.write(net::buffer("SERVER:COMBAT_LOG:Unknown skill: " + skillName));
					return;
				}

				const SkillDefinition& skill = itSkill->second;

				// Optional: require that the player actually "knows" this skill
				bool knows_skill = false;
				for (const auto& s : player.temporary_spells_list) {
					if (s == skillName) {
						knows_skill = true;
						break;
					}
				}
				if (!knows_skill) {
					ws.write(net::buffer("SERVER:COMBAT_LOG:You don't know that skill!"));
					return;
				}

				// Check class match
				SkillClass playerSkillClass = SkillClass::ANY;
				switch (player.currentClass) {
				case PlayerClass::FIGHTER: playerSkillClass = SkillClass::WARRIOR; break;
				case PlayerClass::ROGUE:   playerSkillClass = SkillClass::ROGUE;   break;
				case PlayerClass::WIZARD:  playerSkillClass = SkillClass::WIZARD;  break;
				default:                   playerSkillClass = SkillClass::ANY;     break;
				}

				if (skill.requiredClass != SkillClass::ANY &&
					skill.requiredClass != playerSkillClass) {
					ws.write(net::buffer("SERVER:COMBAT_LOG:You cannot use that skill with your class."));
					return;
				}

				// Check mana cost
				if (player.stats.mana < skill.manaCost) {
					ws.write(net::buffer("SERVER:COMBAT_LOG:Not enough mana to use " + skillName + "!"));
					return;
				}

				player.stats.mana -= skill.manaCost;

				bool targetIsSelf = (skill.target == SkillTarget::SELF);

				// Compute skill-based attack power using player stats and skill scales
				float scaledAttack =
					finalStats.strength * skill.strScale +
					finalStats.dexterity * skill.dexScale +
					finalStats.intellect * skill.intScale +
					skill.flatDamage;

				int base_damage = 0;

				if (!targetIsSelf) {
					// Offensive skill at the opponent
					int targetDefense = player.currentOpponent->defense;
					base_damage = damage_after_defense(scaledAttack, targetDefense);

					float variance = 0.9f + ((float)(rand() % 21) / 100.0f); // 0.9–1.1
					int dmg = std::max(1, (int)std::round(base_damage * variance));

					// Let skills crit like basic attacks
					float critChance = crit_chance_for_player(finalStats, player.currentClass);
					if (((float)rand() / RAND_MAX) < critChance) {
						ClassCritTuning t = getCritTuning(player.currentClass);
						dmg = (int)std::round(dmg * t.critMultiplier);
						ws.write(net::buffer("SERVER:COMBAT_LOG:A critical " + skillName + "!"));
					}

					player_damage = dmg;

					std::string log_msg =
						"SERVER:COMBAT_LOG:You use " + skillName + " on the " +
						player.currentOpponent->type + " for " +
						std::to_string(player_damage) + " damage!";
					ws.write(net::buffer(log_msg));
				}
				else {
					// Defensive / self-target skill (e.g., ShieldWall)
					player_damage = 0;
					std::string log_msg =
						"SERVER:COMBAT_LOG:You use " + skillName + " on yourself.";
					ws.write(net::buffer(log_msg));
				}

				// Apply status effect if the skill has one
				if (skill.appliesStatus) {
					StatusEffect eff;
					eff.type = skill.statusType;
					eff.magnitude = skill.statusMagnitude;
					eff.appliedByPlayer = true;

					// Base duration from the skill definition
					int baseDuration = skill.statusDuration;
					int bonusTurns = 0;

					// Small stat-based chance to extend duration by +1 turn
					switch (skill.statusType) {
					case StatusType::BURN: {
						// Wizards with higher INT get better burns
						int chance = std::min(50, finalStats.intellect); // 0–50%
						if ((rand() % 100) < chance) {
							bonusTurns = 1;
						}
						break;
					}
					case StatusType::BLEED: {
						// Rogues with higher DEX get better bleeds
						int chance = std::min(50, finalStats.dexterity); // 0–50%
						if ((rand() % 100) < chance) {
							bonusTurns = 1;
						}
						break;
					}
					case StatusType::DEFENSE_UP: {
						// Warriors with higher LUCK might keep their shield up longer
						int chance = std::min(40, finalStats.luck); // up to 40%
						if ((rand() % 100) < chance) {
							bonusTurns = 1;
						}
						break;
					}
					default:
						// other status types: no bonus for now
						break;
					}

					eff.remainingTurns = baseDuration + bonusTurns;
					if (eff.remainingTurns < 1) eff.remainingTurns = 1;

					if (targetIsSelf) {
						player.activeStatusEffects.push_back(eff);
					}
					else {
						player.currentOpponent->activeStatusEffects.push_back(eff);
					}
					if (!targetIsSelf &&
						(skill.statusType == StatusType::BURN || skill.statusType == StatusType::BLEED)) {
						apply_statuses_to_monster();
					}

					if (bonusTurns > 0) {
						ws.write(net::buffer(
							"SERVER:COMBAT_LOG:The effects of " + skillName +
							" linger longer than usual!"
						));
					}

					// Flavor log about the effect itself
					std::string statusMsg;
					switch (skill.statusType) {
					case StatusType::BURN:       statusMsg = "burns";                break;
					case StatusType::BLEED:      statusMsg = "makes bleed";          break;
					case StatusType::DEFENSE_UP: statusMsg = "bolsters the defense of"; break;
					default:                     statusMsg = "affects";              break;
					}

					if (targetIsSelf) {
						ws.write(net::buffer(
							"SERVER:COMBAT_LOG:Your " + skillName + " " + statusMsg + " you."
						));
					}
					else {
						ws.write(net::buffer(
							"SERVER:COMBAT_LOG:Your " + skillName + " " + statusMsg +
							" the " + player.currentOpponent->type + "."
						));
					}
				}
			}
			else if (action_type == "DEFEND") {
				player.isDefending = true;
				ws.write(net::buffer("SERVER:COMBAT_LOG:You brace for the next attack."));
			}

			else if (action_type == "FLEE") {
				// FIX: Use -> access and std::max/min
				float flee_chance = 0.5f + ((float)finalStats.speed - (float)player.currentOpponent->speed) * 0.05f + ((float)finalStats.luck * 0.01f);
				flee_chance = std::max(0.1f, std::min(0.9f, flee_chance));
				if (((float)rand() / RAND_MAX) < flee_chance) { fled = true; }
				else { ws.write(net::buffer("SERVER:COMBAT_LOG:You failed to flee!")); }
			}

			if (fled) {
				// FIX: Store string
				std::string log_msg = "SERVER:COMBAT_LOG:You successfully fled from the " + player.currentOpponent->type + "!";
				ws.write(net::buffer(log_msg));
				player.isInCombat = false; player.currentOpponent.reset();
				ws.write(net::buffer("SERVER:COMBAT_VICTORY:Fled"));
				SyncPlayerMonsters(player);
				send_current_monsters_list(); return;
			}

			// FIX: Use -> access
			if (player_damage > 0) { player.currentOpponent->health -= player_damage; }
			send_player_stats();
			// FIX: Store string
			std::string combat_update = "SERVER:COMBAT_UPDATE:" + to_string(player.currentOpponent->health);
			ws.write(net::buffer(combat_update));

			// FIX: Use -> access
			if (player.currentOpponent->health <= 0) {
				std::string log_msg_1 = "SERVER:COMBAT_LOG:You defeated the " + player.currentOpponent->type + "!";
				ws.write(net::buffer(log_msg_1));

				int xp_gain = player.currentOpponent->xpReward;
				std::string log_msg_2 = "SERVER:STATUS:Gained " + to_string(xp_gain) + " XP.";
				ws.write(net::buffer(log_msg_2));

				player.stats.experience += xp_gain;
				int lootTier = player.currentOpponent->lootTier;

				if (lootTier != -1) {
					// 1) Base drop chance defined per monster (0–100)
					int baseDropChance = player.currentOpponent->dropChance;


					//    Example: 0 luck = 1.0x, 10 luck ≈ 1.3x, 25 luck ≈ 1.5x, 50 luck ≈ 1.7x
					double luckMultiplier = 1.0 + (std::sqrt(static_cast<double>(player.stats.luck)) / 15.0);
					if (luckMultiplier > 1.8) {
						luckMultiplier = 1.8;
					}


					//    Tier 1 = 1.0, Tier 2 ≈ 0.85, Tier 3 ≈ 0.70, Tier 4 ≈ 0.55, Tier 5 ≈ 0.40 (min)
					int tierIndex = std::max(0, lootTier - 1);
					double tierModifier = 1.0 - (tierIndex * 0.15);
					if (tierModifier < 0.4) {
						tierModifier = 0.4;
					}

					// 4) Final chance
					double finalDropChance = baseDropChance * luckMultiplier * tierModifier;

					// Clamp between 5% and 75% so it never feels impossible or guaranteed
					if (finalDropChance < 5.0) finalDropChance = 5.0;
					if (finalDropChance > 75.0) finalDropChance = 75.0;

					// 5) Roll 0–99
					int roll = rand() % 100;

					std::cout << "[DEBUG] Drop roll: " << roll
						<< " | Chance: " << finalDropChance
						<< "% | Luck: " << player.stats.luck
						<< " | Tier: " << lootTier << std::endl;

					if (roll < finalDropChance) {
						// 6) Build a candidate list of items for this tier
						std::vector<std::string> possibleItems;
						for (const auto& [itemId, def] : itemDatabase) {
							if (def.item_tier == lootTier) {
								possibleItems.push_back(itemId);
							}
						}

						if (!possibleItems.empty()) {
							int itemIndex = rand() % static_cast<int>(possibleItems.size());
							std::string itemId = possibleItems[itemIndex];

							addItemToInventory(itemId, 1);

							const ItemDefinition& def = itemDatabase.at(itemId);
							std::string log_msg_3 =
								"SERVER:STATUS:The " + player.currentOpponent->type +
								" dropped: " + def.name + "!";
							ws.write(net::buffer(log_msg_3));

							std::cout << "[LOOT DEBUG] roll=" << roll
								<< " finalDropChance=" << finalDropChance
								<< " possibleItems=" << possibleItems.size()
								<< " item=" << itemId
								<< std::endl;
						}
						else {
							std::cout << "[LOOT DEBUG] No items defined for tier "
								<< lootTier << std::endl;
						}
					}
				}

				ws.write(net::buffer("SERVER:COMBAT_VICTORY:Defeated"));
				player.isInCombat = false;
				player.currentOpponent.reset();
				check_for_level_up();
				send_player_stats();
				send_current_monsters_list();
				return;
			}
			if (monsterStunnedThisTurn) {
				ws.write(net::buffer(
					"SERVER:COMBAT_LOG:The " + player.currentOpponent->type +
					" is stunned and cannot act!"
				));
				ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
				return;
			}
			int monster_damage = 0;

			// include DEF_UP buff if you added extraDefFromBuffs above
			int player_defense = finalStats.defense + extraDefFromBuffs;

			if (player.isDefending) {
				player_defense *= 2;
				player.isDefending = false;
			}

			// Use shared monster helpers
			float monsterAttackPower = attack_power_for_monster(*player.currentOpponent);
			int base_monster_damage = damage_after_defense(monsterAttackPower, player_defense);

			float monster_variance = 0.85f + ((float)(rand() % 31) / 100.0f); // 0.85–1.15
			monster_damage = std::max(1, (int)std::round(base_monster_damage * monster_variance));

			float monster_crit_chance = crit_chance_for_monster(*player.currentOpponent);
			if (((float)rand() / RAND_MAX) < monster_crit_chance) {
				monster_damage = (int)std::round(monster_damage * 1.6f);
				std::string log_msg =
					"SERVER:COMBAT_LOG:The " + player.currentOpponent->type +
					" lands a critical hit!";
				ws.write(net::buffer(log_msg));
			}

			player.stats.health -= monster_damage;
			std::string log_msg_attack =
				"SERVER:COMBAT_LOG:The " + player.currentOpponent->type +
				" attacks you for " + std::to_string(monster_damage) + " damage!";
			ws.write(net::buffer(log_msg_attack));
			send_player_stats();
			if (player.stats.health <= 0) {
				player.stats.health = 0;
				ws.write(net::buffer("SERVER:COMBAT_DEFEAT:You have been defeated!"));
				player.isInCombat = false; player.currentOpponent.reset();
				player.currentArea = "TOWN"; player.currentMonsters.clear();
				player.stats.health = player.stats.maxHealth / 2; player.stats.mana = player.stats.maxMana;
				player.posX = 26; player.posY = 12; player.currentPath.clear();
				broadcast_data.currentArea = "TOWN"; broadcast_data.posX = player.posX; broadcast_data.posY = player.posY;
				{ lock_guard<mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }

				ws.write(net::buffer("SERVER:AREA_CHANGED:TOWN"));
				send_area_map_data(player.currentArea);
				SyncPlayerMonsters(player);
				send_current_monsters_list();
				send_available_areas();
				send_player_stats();
				return;
			}
			ws.write(net::buffer("SERVER:COMBAT_TURN:Your turn."));
		}
	}
	   else if (message.rfind("GIVE_XP:", 0) == 0) {
		if (!player.isFullyInitialized) { ws.write(net::buffer("SERVER:ERROR:Complete character creation first.")); }
		else if (player.isInCombat) { ws.write(net::buffer("SERVER:ERROR:Cannot gain XP in combat.")); }
		else {
			try {
				int xp_to_give = stoi(message.substr(8));
				if (xp_to_give > 0) {
					player.stats.experience += xp_to_give;
					ws.write(net::buffer("SERVER:STATUS:Gained " + to_string(xp_to_give) + " XP."));
					check_for_level_up();
					send_player_stats();
				}
				else { ws.write(net::buffer("SERVER:ERROR:Invalid XP amount.")); }
			}
			catch (const exception&) { ws.write(net::buffer("SERVER:ERROR:Invalid XP amount format.")); }
		}
	}
	   else if (message == "REQUEST_PLAYERS") {

		if (g_area_grids.find(player.currentArea) == g_area_grids.end()) {
			ws.write(net::buffer("SERVER:PLAYERS_IN_AREA:[]"));
			return;
		}

		std::ostringstream oss;
		oss << "SERVER:PLAYERS_IN_AREA:[";
		bool first_player = true;
		std::string my_area = player.currentArea;

		{
			std::lock_guard<std::mutex> lock(g_player_registry_mutex);
			for (auto const& pair : g_player_registry) {
				if (pair.first == player.userId) continue;
				if (pair.second.currentArea == my_area && pair.second.playerClass != PlayerClass::UNSELECTED) {
					if (!first_player) oss << ",";
					oss << "{\"id\":" << nlohmann::json(pair.second.userId).dump()
						<< ",\"name\":" << nlohmann::json(pair.second.playerName).dump()
						<< ",\"class\":" << static_cast<int>(pair.second.playerClass)
						<< ",\"x\":" << pair.second.posX
						<< ",\"y\":" << pair.second.posY
						<< "}";
					first_player = false;
				}
			}
		}

		oss << "]";
		std::string player_list_message = oss.str();
		ws.write(net::buffer(player_list_message));
	}
	   else if (message.rfind("USE_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			ws.write(net::buffer("SERVER:ERROR:Complete character creation first."));
		}
		else {
			try {
				uint64_t instanceId = stoull(message.substr(9));
				useItem(instanceId);
			}
			catch (const exception&) {
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
				uint64_t instanceId = stoull(message.substr(11));
				string equipMsg = equipItem(instanceId);
				ws.write(net::buffer("SERVER:STATUS:" + equipMsg));
			}
			catch (const exception&) {
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
				string params = message.substr(10);
				size_t colon_pos = params.find(':');

				if (colon_pos == string::npos) {
					throw invalid_argument("Invalid format. Expected DROP_ITEM:instanceId:quantity");
				}

				uint64_t instanceId = stoull(params.substr(0, colon_pos));
				int quantity = stoi(params.substr(colon_pos + 1));

				dropItem(instanceId, quantity);
			}
			catch (const exception& e) {
				cerr << "Drop item error: " << e.what() << endl;
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
				int slotInt = stoi(message.substr(13));
				EquipSlot slotToUnequip = static_cast<EquipSlot>(slotInt);

				string result = unequipItem(slotToUnequip);
				ws.write(net::buffer("SERVER:STATUS:" + result));
			}
			catch (const exception& e) {
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
				string params = message.substr(9);
				size_t colon_pos = params.find(':');

				if (colon_pos == string::npos) {
					throw invalid_argument("Invalid format. Expected BUY_ITEM:shopId:itemId");
				}

				string shopId = params.substr(0, colon_pos);
				string itemId = params.substr(colon_pos + 1);

				if (g_shops.count(shopId) == 0) {
					ws.write(net::buffer("SERVER:ERROR:Unknown shop."));
					return;
				}
				if (itemDatabase.count(itemId) == 0) {
					ws.write(net::buffer("SERVER:ERROR:Unknown item."));
					return;
				}

				const auto& shopItems = g_shops.at(shopId);
				if (find(shopItems.begin(), shopItems.end(), itemId) == shopItems.end()) {
					ws.write(net::buffer("SERVER:ERROR:This shop does not sell that item."));
					return;
				}

				const ItemDefinition& def = itemDatabase.at(itemId);

				int price = 1;
				try {
					price = g_item_buy_prices.at(itemId);
				}
				catch (const out_of_range&) {
					cerr << "WARNING: Player tried to buy " << itemId << " which has no price." << endl;
					ws.write(net::buffer("SERVER:ERROR:That item is not for sale."));
					return;
				}

				if (player.stats.gold >= price) {
					player.stats.gold -= price;
					addItemToInventory(itemId, 1);
					ws.write(net::buffer("SERVER:STATUS:Bought " + def.name + " for " + to_string(price) + " gold."));
					send_player_stats();

				}
				else {
					ws.write(net::buffer("SERVER:ERROR:Not enough gold. You need " + to_string(price) + "."));
				}
			}
			catch (const exception&) {
				cerr << "Buy item error: " << endl;
				ws.write(net::buffer("SERVER:ERROR:Invalid buy command format."));
			}
		}
	}
	   else {
		string echo = "SERVER:ECHO: " + message;
		ws.write(net::buffer(echo));
	}
}