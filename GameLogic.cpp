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

std::shared_ptr<AsyncSession> get_session_by_id(const std::string& userId) {
	std::lock_guard<std::mutex> lock(g_session_registry_mutex);
	auto it = g_session_registry.find(userId);
	if (it != g_session_registry.end()) {
		if (auto session = it->second.lock()) {
			return session;
		}
	}
	return nullptr;
}
void broadcast_monster_despawn(const std::string& areaName, int spawn_id, const std::string& exclude_user_id) {
	// ... (This function is correct, no changes)
	auto shared_msg = std::make_shared<std::string>(
		"SERVER:MONSTER_DESPAWNED:" + std::to_string(spawn_id)
	);

	std::vector<std::shared_ptr<AsyncSession>> sessions_to_notify;
	{
		std::lock_guard<std::mutex> lock_reg(g_session_registry_mutex);
		std::lock_guard<std::mutex> lock_data(g_player_registry_mutex);

		for (auto const& [id, weak_session] : g_session_registry) {
			if (id == exclude_user_id) continue;
			auto data_it = g_player_registry.find(id);
			if (data_it == g_player_registry.end()) continue;

			if (data_it->second.currentArea == areaName) {
				if (auto session = weak_session.lock()) {
					sessions_to_notify.push_back(session);
				}
			}
		}
	}

	for (auto& session : sessions_to_notify) {
		net::dispatch(session->getWebSocket().get_executor(), [session, shared_msg]() {
			session->send(*shared_msg);
			});
	}
}

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
float dodge_chance_for_player(const PlayerStats& stats, PlayerClass cls) {
	// Base dodge chance (e.g., 5%)
	float dodge = 0.05f;

	// Scaling factors (can be tuned per class, but applying globally for now)
	static const float DEX_SCALE = 0.0020f; // +0.2% per DEX
	static const float SPEED_SCALE = 0.0015f; // +0.15% per SPEED
	static const float LUCK_SCALE = 0.0010f; // +0.1% per LUCK

	dodge += stats.dexterity * DEX_SCALE
		+ stats.speed * SPEED_SCALE
		+ stats.luck * LUCK_SCALE;

	// Cap the dodge chance so it's not guaranteed (e.g., 30%)
	static const float GLOBAL_DODGE_CAP = 0.20f;

	if (cls == PlayerClass::ROGUE) {
		// Rogues get a slight bonus to emphasize their agility
		dodge += 0.05f;
	}

	if (dodge > GLOBAL_DODGE_CAP) dodge = GLOBAL_DODGE_CAP;
	if (dodge < 0.0f) dodge = 0.0f;

	return dodge; // Returns a float between 0.0 and 0.3 (0% to 30%)
}
float magic_resistance_for_player(const PlayerStats& stats) {
	// Base resistance (e.g., 5%)
	float resistance = 0.05f;

	// Scaling factors
	static const float INTELLECT_SCALE = 0.0020f; // +0.2% per INT (Main source)
	static const float LUCK_SCALE = 0.0010f;    // +0.1% per LUCK
	static const float DEFENSE_SCALE = 0.0005f;  // +0.05% per DEF (Minor utility)

	resistance += stats.intellect * INTELLECT_SCALE
		+ stats.luck * LUCK_SCALE
		+ stats.defense * DEFENSE_SCALE; // Added Defense contribution

	// Cap the maximum reduction (e.g., 50%)
	static const float GLOBAL_RESIST_CAP = 0.50f;

	if (resistance > GLOBAL_RESIST_CAP) resistance = GLOBAL_RESIST_CAP;
	if (resistance < 0.0f) resistance = 0.0f;

	return resistance; // Returns a float between 0.0 and 0.5 (0% to 50% reduction)
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
void broadcast_monster_list(const std::string& areaName) {
	// 1. Get the area data
	auto areaIt = g_areas.find(areaName);
	if (areaIt == g_areas.end()) return;
	AreaData& area = areaIt->second;

	// 2. Build the monster list payload
	nlohmann::json payload;
	payload["area"] = areaName;
	payload["monsters"] = nlohmann::json::array();

	{
		// Lock this area's monster list while we read it
		std::lock_guard<std::mutex> lock(area.monster_mutex);

		for (const auto& pair : area.live_monsters) {
			const LiveMonster& lm = pair.second;
			if (lm.is_alive) {
				payload["monsters"].push_back({
					{"id", lm.spawn_id},
					{"name", lm.monster_type},
					{"asset", lm.asset_key},
					{"x", lm.position.x},
					{"y", lm.position.y}
					});
			}
		}
	} // Monster mutex unlocks here

	// 3. Create the shared message
	auto shared_msg = std::make_shared<std::string>(
		"SERVER:MONSTERS:" + payload.dump()
	);

	// 4. Get the list of sessions to notify (using the safe lock order)
	std::vector<std::shared_ptr<AsyncSession>> sessions_to_notify;
	{
		std::lock_guard<std::mutex> lock_reg(g_session_registry_mutex);
		std::lock_guard<std::mutex> lock_data(g_player_registry_mutex);

		for (auto const& [id, weak_session] : g_session_registry) {
			auto data_it = g_player_registry.find(id);
			if (data_it == g_player_registry.end()) continue;

			if (data_it->second.currentArea == areaName) {
				if (auto session = weak_session.lock()) {
					sessions_to_notify.push_back(session);
				}
			}
		}
	} // All locks released

	// 5. Dispatch the message to everyone
	std::cout << "[Broadcast] Sending full monster list (" << payload["monsters"].size()
		<< " monsters) to " << sessions_to_notify.size()
		<< " players in " << areaName << std::endl;

	for (auto& session : sessions_to_notify) {
		net::dispatch(session->getWebSocket().get_executor(), [session, shared_msg]() {
			session->send(*shared_msg);
			});
	}
}
/**
 * @brief Sets a monster's respawn timer.
 * @param areaName The area the monster is in.
 * @param spawn_id The monster's ID.
 * @param seconds The number of seconds until respawn.
 */
void set_monster_respawn_timer(const std::string& areaName, int spawn_id, int seconds) {
	auto areaIt = g_areas.find(areaName);
	if (areaIt == g_areas.end()) return;

	AreaData& area = areaIt->second;
	std::lock_guard<std::mutex> lock(area.monster_mutex);

	auto monsterIt = area.live_monsters.find(spawn_id);
	if (monsterIt != area.live_monsters.end()) {
		monsterIt->second.is_alive = false; // Ensure it's marked as dead
		monsterIt->second.respawn_time = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	}
}
// Global Party Registry (Matches GameData.hpp extern)
std::map<std::string, std::shared_ptr<Party>> g_parties;
std::mutex g_parties_mutex;

// --- Helper: Get Party Safely ---
std::shared_ptr<Party> get_party_by_id(const std::string& partyId) {
	std::lock_guard<std::mutex> lock(g_parties_mutex);
	auto it = g_parties.find(partyId);
	return (it != g_parties.end()) ? it->second : nullptr;
}

// --- Helper: Broadcast to Party Members ---
void broadcast_to_party(std::shared_ptr<Party> party, const std::string& msg) {
	if (!party) return;
	for (const auto& memId : party->memberIds) {
		if (auto sess = get_session_by_id(memId)) {
			sess->send(msg);
		}
	}
}
void broadcast_party_update(std::shared_ptr<Party> party) {
	if (!party) return;

	nlohmann::json j;
	j["members"] = nlohmann::json::array();

	// Collect all member names
	for (const auto& mid : party->memberIds) {
		if (auto s = get_session_by_id(mid)) {
			j["members"].push_back(s->getPlayerState().playerName);
		}
	}

	std::string msg = "SERVER:PARTY_UPDATE:" + j.dump();

	// Reuse the existing helper to send to everyone
	broadcast_to_party(party, msg);
}
// --- Helper: Resolve Party Round (Full Logic) ---
// --- Helper: Resolve Party Round (Full Logic) ---
// --- Helper: Resolve Party Round (Full Logic) ---
// File: GameLogic.cpp

// --- Helper: Resolve Party Round (Full Logic) ---
// --- Helper: Resolve Party Round (Full Logic) ---
void resolve_party_round(std::shared_ptr<Party> party) {
	if (!party || !party->activeCombat) return;

	auto combat = party->activeCombat;
	auto monster = combat->monster; // Shared monster instance

	// ==================================================
	// 0. UPKEEP PHASE (Status Effects & DoTs)
	// ==================================================

	// A. Monster Upkeep
	{
		int totalDot = 0;
		auto& mEffects = monster->activeStatusEffects;
		for (auto it = mEffects.begin(); it != mEffects.end(); ) {
			// Apply DoTs
			if (it->type == StatusType::BURN || it->type == StatusType::BLEED) {
				int dmg = std::max(1, it->magnitude);
				totalDot += dmg;
				monster->health -= dmg;
			}

			// Tick Duration
			it->remainingTurns--;
			if (it->remainingTurns <= 0) {
				it = mEffects.erase(it);
			}
			else {
				++it;
			}
		}
		if (totalDot > 0) {
			broadcast_to_party(party, "SERVER:COMBAT_LOG:The " + monster->type + " takes " + std::to_string(totalDot) + " damage from effects!");
		}
	}

	// B. Player Upkeep
	std::vector<std::string> participants = combat->participantIds;
	for (const auto& pid : participants) {
		if (auto s = get_session_by_id(pid)) {
			PlayerState& p = s->getPlayerState();
			int totalDot = 0;
			auto& pEffects = p.activeStatusEffects;

			for (auto it = pEffects.begin(); it != pEffects.end(); ) {
				if (it->type == StatusType::BURN || it->type == StatusType::BLEED) {
					int dmg = std::max(1, it->magnitude);
					totalDot += dmg;
					p.stats.health -= dmg;
				}

				it->remainingTurns--;
				if (it->remainingTurns <= 0) {
					it = pEffects.erase(it);
				}
				else {
					++it;
				}
			}

			if (totalDot > 0) {
				broadcast_to_party(party, "SERVER:COMBAT_LOG:" + p.playerName + " takes " + std::to_string(totalDot) + " damage from effects!");
				s->send_player_stats();
			}

			// --- CHECK DEATH (UPKEEP) ---
			if (p.stats.health <= 0) {
				p.stats.health = 0;
				s->send("SERVER:COMBAT_DEFEAT:You have succumbed to your wounds!");
				broadcast_to_party(party, "SERVER:COMBAT_LOG:" + p.playerName + " has died from status effects!");
				p.isInCombat = false;

				// Capture area BEFORE reset for respawn logic if wipe occurs
				std::string fightArea = p.currentArea;

				// 1. Remove from Combat
				auto& parts = combat->participantIds;
				parts.erase(std::remove(parts.begin(), parts.end(), pid), parts.end());
				combat->threatMap.erase(pid);
				combat->pendingActions.erase(pid);

				// 2. Remove from Party (Treat as Solo Death)
				auto& mems = party->memberIds;
				mems.erase(std::remove(mems.begin(), mems.end(), pid), mems.end());
				p.partyId = "";

				// 3. Update Clients
				broadcast_party_update(party); // Notify survivors
				s->send("SERVER:PARTY_UPDATE:{\"members\":[]}"); // Clear victim's HUD

				// 4. FULL RESET: Use existing logic to teleport, heal, and reload map
				s->handle_message("GO_TO:TOWN");

				// 5. Check Wipe
				if (combat->participantIds.empty()) {
					party->activeCombat = nullptr;
					// FIX: Use fightArea instead of assetKey
					set_monster_respawn_timer(fightArea, monster->id, 15);
					return; // Combat Over
				}
			}
		}
	}

	// Check if Monster died from DoTs
	if (monster->health <= 0) {
		// Fall through to victory logic at bottom
	}

	// ==================================================
	// 1. AGGREGATE ACTIONS
	// ==================================================
	std::vector<CombatAction> turnOrder;

	for (const auto& pid : combat->participantIds) {
		if (combat->pendingActions.count(pid)) {
			turnOrder.push_back(combat->pendingActions[pid]);
		}
		else {
			// Default to DEFEND
			CombatAction def;
			def.actorId = pid;
			def.type = "DEFEND";
			def.speed = 0;
			turnOrder.push_back(def);
		}
	}

	// Boss Action Logic
	std::string targetId = "";
	int maxThreat = -1;

	if (!combat->participantIds.empty()) {
		targetId = combat->participantIds[0];
		for (const auto& [pid, threat] : combat->threatMap) {
			// Verify player is in combat list
			if (std::find(combat->participantIds.begin(), combat->participantIds.end(), pid) == combat->participantIds.end()) continue;

			if (threat > maxThreat) {
				if (auto s = get_session_by_id(pid)) {
					if (s->getPlayerState().stats.health > 0) {
						maxThreat = threat;
						targetId = pid;
					}
				}
			}
		}
	}

	CombatAction bossAction;
	bossAction.actorId = "BOSS";
	bossAction.speed = monster->speed;
	bossAction.param = targetId;

	if (!monster->skills.empty() && (rand() % 100 < 30)) {
		std::string chosenSkill = monster->skills[rand() % monster->skills.size()];
		bossAction.type = "SKILL";
		bossAction.param = chosenSkill + ":" + targetId;
	}
	else {
		bossAction.type = "ATTACK";
		bossAction.param = targetId;
	}
	turnOrder.push_back(bossAction);

	// Sort by Speed
	std::sort(turnOrder.begin(), turnOrder.end(), [](const CombatAction& a, const CombatAction& b) {
		return a.speed > b.speed;
		});

	// Log Round
	std::string roundLog = "SERVER:COMBAT_LOG:--- Round " + std::to_string(combat->roundNumber++) + " ---";
	broadcast_to_party(party, roundLog);

	// ==================================================
	// 2. EXECUTE ACTIONS
	// ==================================================
	for (const auto& act : turnOrder) {
		if (monster->health <= 0) break;

		// --- BOSS TURN ---
		if (act.actorId == "BOSS") {
			std::string tId = act.param;
			std::string skillName = "";

			if (act.type == "SKILL") {
				size_t colon = act.param.find(':');
				if (colon != std::string::npos) {
					skillName = act.param.substr(0, colon);
					tId = act.param.substr(colon + 1);
				}
				else {
					skillName = act.param;
					tId = targetId;
				}
			}

			auto targetSession = get_session_by_id(tId);
			if (!targetSession) continue;

			// Re-check participation
			if (std::find(combat->participantIds.begin(), combat->participantIds.end(), tId) == combat->participantIds.end()) continue;

			PlayerState& pState = targetSession->getPlayerState();
			if (pState.stats.health <= 0) continue;

			// USE CALCULATED STATS
			PlayerStats targetStats = targetSession->getCalculatedStats();

			int damage = 0;
			std::string logMsg;

			if (act.type == "ATTACK") {
				float pwr = attack_power_for_monster(*monster);
				int pDef = targetStats.defense;

				if (pState.isDefending) {
					pDef *= 2;
					pState.isDefending = false;
				}

				damage = damage_after_defense(pwr, pDef);

				float crit = crit_chance_for_monster(*monster);
				if (((float)rand() / RAND_MAX) < crit) {
					damage = (int)(damage * 1.5f);
					broadcast_to_party(party, "SERVER:COMBAT_LOG:The " + monster->type + " lands a CRITICAL hit!");
				}

				if (damage < 1) damage = 1;
				pState.stats.health -= damage;
				logMsg = "The " + monster->type + " attacks " + pState.playerName + " for " + std::to_string(damage) + "!";
			}
			else if (act.type == "SKILL") {
				float pwr = attack_power_for_monster(*monster) * 1.2f;

				const SkillDefinition* skillDef = nullptr;
				if (g_monster_spell_defs.count(skillName)) skillDef = &g_monster_spell_defs.at(skillName);
				else if (g_skill_defs.count(skillName)) skillDef = &g_skill_defs.at(skillName);

				if (skillDef) {
					pwr = attack_power_for_monster(*monster) * 1.2f;
				}

				damage = damage_after_defense(pwr, targetStats.defense);
				if (damage < 1) damage = 1;

				pState.stats.health -= damage;
				logMsg = "The " + monster->type + " uses " + skillName + " on " + pState.playerName + " for " + std::to_string(damage) + "!";

				if (skillDef && skillDef->appliesStatus) {
					StatusEffect eff;
					eff.type = skillDef->statusType;
					eff.magnitude = skillDef->statusMagnitude;
					eff.remainingTurns = skillDef->statusDuration;
					eff.appliedByPlayer = false;
					pState.activeStatusEffects.push_back(eff);
					broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " is affected by " + skillName + "!");
				}
			}

			broadcast_to_party(party, "SERVER:COMBAT_LOG:" + logMsg);
			targetSession->send_player_stats(); // Update UI for victim

			// --- CHECK PLAYER DEATH (BOSS TURN) ---
			if (pState.stats.health <= 0) {
				pState.stats.health = 0;
				targetSession->send("SERVER:COMBAT_DEFEAT:You have fallen!");
				pState.isInCombat = false;
				broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " has been defeated!");

				// Capture area BEFORE reset
				std::string fightArea = pState.currentArea;

				// 1. Remove from Combat
				auto& parts = combat->participantIds;
				parts.erase(std::remove(parts.begin(), parts.end(), tId), parts.end());
				combat->threatMap.erase(tId);
				combat->pendingActions.erase(tId);

				// 2. Remove from Party (Treat as Solo Death)
				auto& mems = party->memberIds;
				mems.erase(std::remove(mems.begin(), mems.end(), tId), mems.end());
				pState.partyId = "";

				// 3. Update Clients
				broadcast_party_update(party); // Update survivors HUD
				targetSession->send("SERVER:PARTY_UPDATE:{\"members\":[]}"); // Clear victim HUD

				// 4. FULL RESET: Use GO_TO:TOWN
				targetSession->handle_message("GO_TO:TOWN");

				// 5. Check Wipe
				if (combat->participantIds.empty()) {
					party->activeCombat = nullptr;
					// FIX: Use fightArea instead of assetKey
					set_monster_respawn_timer(fightArea, monster->id, 15);
					return; // End function
				}
			}
		}
		// --- PLAYER TURN ---
		else {
			if (std::find(combat->participantIds.begin(), combat->participantIds.end(), act.actorId) == combat->participantIds.end()) continue;

			auto session = get_session_by_id(act.actorId);
			if (!session) continue;
			PlayerState& pState = session->getPlayerState();

			if (pState.stats.health <= 0) continue;

			// USE CALCULATED STATS
			PlayerStats playerCalcStats = session->getCalculatedStats();

			if (act.type == "DEFEND") {
				pState.isDefending = true;
				broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " braces for impact.");
			}
			else if (act.type == "FLEE") {
				if (rand() % 100 < 50) {
					pState.isInCombat = false;
					combat->pendingActions.erase(act.actorId);
					auto& parts = combat->participantIds;
					parts.erase(std::remove(parts.begin(), parts.end(), act.actorId), parts.end());
					combat->threatMap.erase(act.actorId);

					session->send("SERVER:COMBAT_VICTORY:Fled");
					broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " fled the battle!");

					if (combat->participantIds.empty()) {
						party->activeCombat = nullptr;
						return;
					}
				}
				else {
					broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " failed to flee!");
				}
			}
			else if (act.type == "ATTACK") {
				float atk = attack_power_for_player(playerCalcStats, pState.currentClass);
				int dmg = damage_after_defense(atk, monster->defense);

				float crit = crit_chance_for_player(playerCalcStats, pState.currentClass);
				if (((float)rand() / RAND_MAX) < crit) {
					dmg = (int)(dmg * 1.5f);
					broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " lands a CRITICAL hit!");
				}

				if (dmg < 1) dmg = 1;
				monster->health -= dmg;
				combat->threatMap[act.actorId] += dmg;
				broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " attacks for " + std::to_string(dmg) + "!");
			}
			else if (act.type == "SKILL" || act.type == "SPELL") {
				std::string skillName = act.param;
				const SkillDefinition* sDef = nullptr;
				if (g_skill_defs.count(skillName)) sDef = &g_skill_defs.at(skillName);

				if (sDef) {
					float atk = playerCalcStats.strength * sDef->strScale +
						playerCalcStats.dexterity * sDef->dexScale +
						playerCalcStats.intellect * sDef->intScale +
						sDef->flatDamage;

					int dmg = damage_after_defense(atk, monster->defense);

					if (sDef->isMagic) {
						int penDef = (int)(monster->defense * 0.6f);
						dmg = damage_after_defense(atk, penDef);
					}

					if (dmg < 1) dmg = 1;

					monster->health -= dmg;
					combat->threatMap[act.actorId] += dmg;
					broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " uses " + skillName + " for " + std::to_string(dmg) + "!");

					if (sDef->appliesStatus) {
						StatusEffect eff;
						eff.type = sDef->statusType;
						eff.magnitude = sDef->statusMagnitude;
						eff.remainingTurns = sDef->statusDuration;
						eff.appliedByPlayer = true;
						monster->activeStatusEffects.push_back(eff);
						broadcast_to_party(party, "SERVER:COMBAT_LOG:The " + monster->type + " is affected by " + skillName + "!");
					}
				}
				else {
					broadcast_to_party(party, "SERVER:COMBAT_LOG:" + pState.playerName + " tries to use " + skillName + " but fails!");
				}
			}
		}
	}

	// 5. Post-Round Updates
	std::string bossUpdate = "SERVER:COMBAT_UPDATE:" + std::to_string(monster->health);
	broadcast_to_party(party, bossUpdate);

	// 6. Win Check
	if (monster->health <= 0) {
		// Determine the area name for respawn (grab from first participant)
		std::string combatArea = "";
		if (!combat->participantIds.empty()) {
			if (auto s = get_session_by_id(combat->participantIds[0])) {
				combatArea = s->getPlayerState().currentArea;
			}
		}

		int xpShare = monster->xpReward;
		for (const auto& pid : combat->participantIds) {
			if (auto s = get_session_by_id(pid)) {
				s->getPlayerState().isInCombat = false;
				s->getPlayerState().stats.experience += xpShare;
				s->check_for_level_up();
				s->send("SERVER:COMBAT_VICTORY:Defeated");
				s->send("SERVER:STATUS:Gained " + std::to_string(xpShare) + " XP.");
				s->send_player_stats();
			}
		}

		// Loot Distribution
		std::vector<std::string> droppedItems;
		int lootTier = monster->lootTier;
		int totalLuck = 0;
		int memberCount = 0;
		for (const auto& pid : combat->participantIds) {
			if (auto s = get_session_by_id(pid)) {
				totalLuck += s->getCalculatedStats().luck;
				memberCount++;
			}
		}
		int avgLuck = (memberCount > 0) ? totalLuck / memberCount : 5;

		const std::vector<std::string> ALL_SKILL_BOOKS = {
			 "BOOK_SUNDER_ARMOR", "BOOK_PUMMEL", "BOOK_ENRAGE", "BOOK_WHIRLWIND",
			 "BOOK_SECOND_WIND", "BOOK_VENOMOUS_SHANK", "BOOK_CRIPPLING_STRIKE",
			 "BOOK_EVASION", "BOOK_GOUGE", "BOOK_BACKSTAB", "BOOK_FROST_NOVA",
			 "BOOK_ARCANE_INTELLECT", "BOOK_LESSER_HEAL", "BOOK_MANA_SHIELD", "BOOK_PYROBLAST"
		};

		if (lootTier >= 2 && (rand() % 1000 < 5)) {
			droppedItems.push_back(ALL_SKILL_BOOKS[rand() % ALL_SKILL_BOOKS.size()]);
		}

		if (lootTier != -1) {
			double luckMult = 1.0 + (std::sqrt(avgLuck) / 15.0);
			if (luckMult > 1.8) luckMult = 1.8;
			double tierMod = 1.0 - (std::max(0, lootTier - 1) * 0.15);
			if (tierMod < 0.4) tierMod = 0.4;
			double chance = monster->dropChance * luckMult * tierMod;
			if (chance < 5.0) chance = 5.0; if (chance > 75.0) chance = 75.0;

			if ((rand() % 100) < chance) {
				std::vector<std::string> tierItems;
				for (const auto& [id, def] : itemDatabase) { if (def.item_tier == lootTier) tierItems.push_back(id); }
				if (!tierItems.empty()) droppedItems.push_back(tierItems[rand() % tierItems.size()]);
			}
		}

		if (!droppedItems.empty() && !combat->participantIds.empty()) {
			for (const auto& itemId : droppedItems) {
				std::string winnerId = combat->participantIds[rand() % combat->participantIds.size()];
				if (auto winnerSess = get_session_by_id(winnerId)) {
					winnerSess->addItemToInventory(itemId, 1);
					std::string itemName = itemId;
					if (itemDatabase.count(itemId)) itemName = itemDatabase.at(itemId).name;
					broadcast_to_party(party, "SERVER:STATUS:" + winnerSess->getPlayerState().playerName + " won the " + itemName + "!");
				}
			}
		}

		party->activeCombat = nullptr;

		// FIX: Use the combatArea we captured, NOT assetKey
		if (!combatArea.empty()) {
			set_monster_respawn_timer(combatArea, monster->id, 15);
			broadcast_monster_list(combatArea);
		}
	}
	else {
		combat->pendingActions.clear();
		combat->roundStartTime = std::chrono::steady_clock::now();
		broadcast_to_party(party, "SERVER:COMBAT_TURN:Your turn.");
	}
}
/**
 * @brief Respawns a monster immediately (for flee/player death).
 * @param areaName The area the monster is in.
 * @param spawn_id The monster's ID.
 */
void respawn_monster_immediately(const std::string& areaName, int spawn_id) {
	auto areaIt = g_areas.find(areaName);
	if (areaIt == g_areas.end()) return;

	AreaData& area = areaIt->second;
	bool found = false;

	{
		std::lock_guard<std::mutex> lock(area.monster_mutex);
		auto monsterIt = area.live_monsters.find(spawn_id);
		if (monsterIt != area.live_monsters.end()) {
			monsterIt->second.is_alive = true;
			monsterIt->second.respawn_time = std::chrono::steady_clock::time_point::max();
			monsterIt->second.position = monsterIt->second.original_spawn_point;
			// No need to copy the monster, just set the flag
			found = true;
		}
	}

	if (found) {
		// --- THIS IS THE FIX ---
		// We no longer broadcast just one monster, we broadcast the
		// entire new list for the area to ensure everyone is in sync.
		broadcast_monster_list(areaName);
		// --- END FIX ---
	}
}
// Helper function to safely end a trade and reset player states
void cleanup_trade_session(std::string playerAId, std::string playerBId) {
	// 1. Remove the trade from the global map
	{
		std::lock_guard<std::mutex> lock(g_active_trades_mutex);
		g_active_trades.erase(playerAId);
		g_active_trades.erase(playerBId);
	}

	// 2. Find sessions (if they are still online) and reset their flags
	if (auto sessionA = get_session_by_id(playerAId)) {
		sessionA->getPlayerState().isTrading = false;
		sessionA->getPlayerState().tradePartnerId.clear();
	}
	if (auto sessionB = get_session_by_id(playerBId)) {
		sessionB->getPlayerState().isTrading = false;
		sessionB->getPlayerState().tradePartnerId.clear();
	}
}

// Helper function to send the current trade state to both players
void send_trade_update(std::shared_ptr<TradeSession> trade) {
	if (!trade) return;

	// Find both sessions
	auto sessionA = get_session_by_id(trade->playerAId);
	auto sessionB = get_session_by_id(trade->playerBId);

	// Build the JSON payload
	using json = nlohmann::json;
	json j;

	// --- Player A's Offer ---
	json offerA;
	json itemsA = json::array();
	if (sessionA) { // Need sessionA to get item details
		PlayerState& playerA = sessionA->getPlayerState();
		for (auto const& [instanceId, quantity] : trade->offerAItems) {
			if (playerA.inventory.count(instanceId)) {
				const auto& item = playerA.inventory.at(instanceId);
				itemsA.push_back({
					{"instanceId", instanceId},
					{"name", item.getDefinition().name}, // Get name from definition
					{"quantity", quantity}
					});
			}
		}
	}
	offerA["items"] = itemsA;
	offerA["gold"] = trade->offerAGold;
	offerA["confirmed"] = trade->confirmA;

	// --- Player B's Offer ---
	json offerB;
	json itemsB = json::array();
	if (sessionB) { // Need sessionB to get item details
		PlayerState& playerB = sessionB->getPlayerState();
		for (auto const& [instanceId, quantity] : trade->offerBItems) {
			if (playerB.inventory.count(instanceId)) {
				const auto& item = playerB.inventory.at(instanceId);
				itemsB.push_back({
					{"instanceId", instanceId},
					{"name", item.getDefinition().name}, // Get name from definition
					{"quantity", quantity}
					});
			}
		}
	}
	offerB["items"] = itemsB;
	offerB["gold"] = trade->offerBGold;
	offerB["confirmed"] = trade->confirmB;

	j["offerA"] = offerA;
	j["offerB"] = offerB;

	std::string payload = "SERVER:TRADE_UPDATE:" + j.dump();

	// Send to both players (if they are still online)
	if (sessionA) sessionA->send(payload);
	if (sessionB) sessionB->send(payload);
}
// --- Combat math helpers ---


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
	send(oss.str());

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

void AsyncSession::process_gathering() {
	PlayerState& player = getPlayerState();

	// 1. Basic Checks
	if (!player.isGathering) return;

	// Safety: Stop gathering if combat starts or if valid node is lost
	if (player.isInCombat) {
		player.isGathering = false;
		return;
	}

	// 2. Time Check
	auto now = std::chrono::steady_clock::now();
	// GATHER_INTERVAL is defined in game_session.hpp (e.g., 2000ms)
	if (now - player.lastGatherTime < std::chrono::milliseconds(5000)) {
		return; // Not ready for the next "swing" yet
	}

	// 3. Reset Timer
	player.lastGatherTime = now;

	// 4. Get Resource Data
	// The player.gatheringResourceNode string holds the key, e.g., "OAK_TREE"
	if (g_resource_defs.count(player.gatheringResourceNode) == 0) {
		player.isGathering = false;
		return;
	}
	const ResourceDefinition& def = g_resource_defs.at(player.gatheringResourceNode);

	// 5. Execute One "Tick" of Gathering
	bool gatheredSomething = false;
	std::string statusMsg = "";
	auto& ws = getWebSocket();

	// Roll Normal Drop
	if ((rand() % 100) < def.dropChance) {
		addItemToInventory(def.dropItemId, 1);
		gatheredSomething = true;
		statusMsg = "You gathered " + def.dropItemId + ".";
	}

	// Roll Rare Drop
	if (!def.rareItemId.empty() && (rand() % 100) < def.rareChance) {
		addItemToInventory(def.rareItemId, 1);
		gatheredSomething = true;
		statusMsg += " Rare find! " + def.rareItemId + "!";
	}

	// 6. Rewards
	if (gatheredSomething) {
		// Determine skill name for XP
		std::string skillName;
		switch (def.skill) {
		case LifeSkillType::WOODCUTTING: skillName = "Woodcutting"; break;
		case LifeSkillType::MINING:      skillName = "Mining"; break;
		case LifeSkillType::FISHING:     skillName = "Fishing"; break;
		default:                         skillName = "Gathering"; break;
		}

		player.skills.life_skills[skillName] += def.xpReward;
		send("SERVER:STATUS:" + statusMsg + " (+" + std::to_string(def.xpReward) + " XP)");

		// Update client with new items and XP
		send_inventory_and_equipment();
		send_player_stats();
	}
	else {
		send("SERVER:STATUS:You attempt to gather, but find nothing.");
	}
}
// -----------------------------------------------------------------------------
// Processes one tick of player movement (called by move_timer_).
// -----------------------------------------------------------------------------
void AsyncSession::process_movement()
{
	process_gathering();

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

					// --- ZONE TRANSITIONS ---
					if (obj.type == InteractableType::ZONE_TRANSITION) {
						player.currentPath.clear(); // stop further movement

						std::string command = "GO_TO:" + obj.data;
						std::cout << "[DEBUG] Zone transition: " << player.currentArea
							<< " -> " << obj.data << " via " << obj.id << std::endl;

						handle_message(command);    // area change logic
						return;
					}

					// --- NPC / SHOP INTERACTION ---
					if (obj.type == InteractableType::NPC || obj.type == InteractableType::SHOP) {

						std::cout << "[DEBUG] Stepped on interactable: id=" << obj.id
							<< " data=" << obj.data << std::endl;

						player.currentPath.clear(); // Stop movement

						// 1. SHOP LOGIC
						if (obj.data.rfind("SHOP_", 0) == 0) {
							string shop_id = obj.data;
							auto shop_it = g_shops.find(shop_id);

							if (shop_it == g_shops.end()) {
								send("SERVER:ERROR:Shop inventory not found for ID: " + shop_id);
								return;
							}

							// Serialize shop data (using existing logic from previous request)
							ostringstream oss;
							oss << "SERVER:SHOP_DATA:{\"shopId\":" << nlohmann::json(shop_id).dump() << ",\"items\":[";
							bool firstItem = true;

							for (const string& itemId : shop_it->second) {
								if (itemDatabase.count(itemId) == 0) continue;
								const ItemDefinition& def = itemDatabase.at(itemId);
								int price = g_item_buy_prices.count(itemId) ? g_item_buy_prices.at(itemId) : 1;

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
							send(oss.str());
							return;
						}

						// 2. DIALOGUE LOGIC
						auto dlgIt = g_dialogues.find(obj.data);
						if (dlgIt != g_dialogues.end() && !dlgIt->second.empty()) {
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
							send(oss.str());
							return;
						}

						// 3. FALLBACK
						send("SERVER:PROMPT:The object hums silently.");
						return;
					}

					// future: RESOURCE_NODE, CRAFTING_STATION, etc.
				}
			}
		}
	}
	catch (const std::exception& e) {
		// This prevents unhandled exceptions from aborting your server
		std::cerr << "[process_movement] Exception during interactable handling: "
			<< e.what() << std::endl;
		auto& ws = getWebSocket();
		send("SERVER:ERROR:An error occurred while processing interaction.");
		// fall through and still update position/broadcast below
	}
	catch (...) {
		std::cerr << "[process_movement] Unknown exception during interactable handling.\n";
		auto& ws = getWebSocket();
		send("SERVER:ERROR:Unknown interaction error.");
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

	// Lambda to apply stat modifications from items
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

	// --- 1. Apply Stats from Equipment and Item Effects ---
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

	// --- 2. Apply Active Status Effects (Buffs/Debuffs) ---
	for (const auto& eff : player.activeStatusEffects) {
		if (eff.remainingTurns > 0) {
			switch (eff.type) {
				// ATTACK_UP/DOWN modifies combat-related core stats (STR/DEX)
			case StatusType::ATTACK_UP: {
				finalStats.strength += eff.magnitude;
				finalStats.dexterity += eff.magnitude;
				break;
			}
			case StatusType::ATTACK_DOWN: {
				finalStats.strength -= eff.magnitude;
				finalStats.dexterity -= eff.magnitude;
				break;
			}
										// DEFENSE_DOWN decreases defense (DEF_UP is handled in combat loop for defense doubling)
			case StatusType::DEFENSE_DOWN: {
				finalStats.defense -= eff.magnitude;
				break;
			}
			case StatusType::SPEED_UP: {
				finalStats.speed += eff.magnitude;
				break;
			}
			case StatusType::SPEED_DOWN: {
				finalStats.speed -= eff.magnitude;
				break;
			}
									   // MANA_UP/DOWN are primarily handled during the combat tick, but can affect max mana/regen here if needed.
									   // Assuming magnitude applies to mana pool or regen over time (leaving base stats unchanged here).
			default:
				break;
			}
		}
	}

	// --- 3. Safety Clamp Final Stats ---
	finalStats.strength = std::max(0, finalStats.strength);
	finalStats.dexterity = std::max(0, finalStats.dexterity);
	finalStats.intellect = std::max(0, finalStats.intellect);
	finalStats.defense = std::max(0, finalStats.defense);
	finalStats.speed = std::max(0, finalStats.speed);

	// Max Health/Mana must be clamped
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

//void AsyncSession::addCraftedItemToInventory(const std::string& itemId, int quantity, int bonusEffectChance) {
//	PlayerState& player = getPlayerState();
//	if (quantity <= 0) return;
//
//	if (itemDatabase.count(itemId) == 0) {
//		std::cerr << "Error: Attempted to craft non-existent item ID: " << itemId << std::endl;
//		return;
//	}
//
//	const ItemDefinition& def = itemDatabase.at(itemId);
//
//	// 1. Handle Stackables (Potions, Ingots) - usually no effects
//	if (def.stackable) {
//		for (auto& pair : player.inventory) {
//			ItemInstance& instance = pair.second;
//			if (instance.itemId == itemId) {
//				instance.quantity += quantity;
//				send_inventory_and_equipment();
//				return;
//			}
//		}
//	}
//
//	// 2. Handle Non-Stackables (Weapons, Armor) - Apply Boosts!
//	int numInstancesToAdd = def.stackable ? 1 : quantity;
//	int qtyPerInstance = def.stackable ? quantity : 1;
//
//	std::random_device rd;
//	std::mt19937 gen(rd());
//
//	for (int i = 0; i < numInstancesToAdd; ++i) {
//		uint64_t newInstanceId = g_item_instance_id_counter++;
//		ItemInstance newInstance = { newInstanceId, itemId, qtyPerInstance, {}, {} };
//
//		// --- BOOSTING LOGIC ---
//		// Only roll effects if it's equipment or a high-tier item
//		bool canHaveEffects = (def.equipSlot != EquipSlot::None) || (def.item_tier > 0);
//
//		if (canHaveEffects && !g_random_effect_pool.empty()) {
//			std::uniform_int_distribution<> percentRoll(1, 100);
//
//			// Base chance (20%) + The Booster Bonus (e.g. +50%)
//			int totalChance = 20 + bonusEffectChance;
//
//			// Cap at 100% to prevent weirdness
//			if (totalChance > 100) totalChance = 100;
//
//			if (percentRoll(gen) <= totalChance) {
//				// --- SUCCESS! Pick an effect ---
//				int item_tier = std::max(1, def.item_tier);
//				std::vector<const RandomEffectDefinition*> available_effects;
//				int total_weight = 0;
//
//				for (const auto& effect_def : g_random_effect_pool) {
//					if (effect_def.power_level <= item_tier) {
//						available_effects.push_back(&effect_def);
//						total_weight += effect_def.rarity_weight;
//					}
//				}
//
//				if (!available_effects.empty() && total_weight > 0) {
//					std::uniform_int_distribution<> effect_roll(0, total_weight - 1);
//					int roll_result = effect_roll(gen);
//
//					const RandomEffectDefinition* chosen = nullptr;
//					for (const auto* ptr : available_effects) {
//						if (roll_result < ptr->rarity_weight) {
//							chosen = ptr;
//							break;
//						}
//						roll_result -= ptr->rarity_weight;
//					}
//
//					if (chosen) {
//						// Apply the gameplay effect
//						newInstance.customEffects.push_back(chosen->gameplay_effect);
//
//						// Apply the name suffix
//						try {
//							const auto& suffix_pool = g_effect_suffix_pools.at(chosen->effect_key);
//							if (!suffix_pool.empty()) {
//								std::string suffix = suffix_pool[gen() % suffix_pool.size()];
//								ItemEffect suffixEffect;
//								suffixEffect.type = "SUFFIX";
//								suffixEffect.params["value"] = suffix;
//								newInstance.customEffects.push_back(suffixEffect);
//							}
//						}
//						catch (...) {}
//					}
//				}
//			}
//		}
//		// --- END BOOSTING LOGIC ---
//
//		// (Optional) Add a "Crafted" tag?
//		// ItemEffect craftedTag;
//		// craftedTag.type = "CRAFTED_BY";
//		// craftedTag.params["name"] = player.playerName;
//		// newInstance.customEffects.push_back(craftedTag);
//
//		player.inventory[newInstanceId] = newInstance;
//	}
//
//	send_inventory_and_equipment();
//}
void AsyncSession::addCraftedItemToInventory(const std::string& itemId, int quantity, int bonusEffectChance) {
	PlayerState& player = getPlayerState();
	if (quantity <= 0) return;

	if (itemDatabase.count(itemId) == 0) {
		std::cerr << "Error: Attempted to craft non-existent item ID: " << itemId << std::endl;
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

	uint64_t startingInstanceId = 0;

	if (numInstancesToAdd > 0) {
		try {
			pqxx::connection C = db_manager_->get_connection();
			pqxx::nontransaction N(C);

			startingInstanceId = N.exec("SELECT nextval('item_instance_id_seq')").one_row()[0].as<uint64_t>();

			if (numInstancesToAdd > 1) {
				N.exec("SELECT setval('item_instance_id_seq', currval('item_instance_id_seq') + "
					+ std::to_string(numInstancesToAdd - 1) + ")");
			}

		}
		catch (const std::exception& e) {
			std::cerr << "CRITICAL: Could not batch fetch item instance IDs: " << e.what() << std::endl;
			send("SERVER:ERROR:Could not create item. Please try again.");
			return;
		}
	}
	else {
		return;
	}

	uint64_t currentInstanceId = startingInstanceId;

	std::random_device rd;
	std::mt19937 gen(rd());

	for (int i = 0; i < numInstancesToAdd; ++i) {

		uint64_t newInstanceId = currentInstanceId;
		currentInstanceId++;

		ItemInstance newInstance = { newInstanceId, itemId, qtyPerInstance, {}, {} };

		bool canHaveEffects = (def.equipSlot != EquipSlot::None) || (def.item_tier > 0);

		if (canHaveEffects && !g_random_effect_pool.empty()) {
			std::uniform_int_distribution<> percentRoll(1, 100);
			int totalChance = 5 + bonusEffectChance;
			if (totalChance > 100) totalChance = 100;

			if (percentRoll(gen) <= totalChance) {
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

					const RandomEffectDefinition* chosen = nullptr;
					for (const auto* ptr : available_effects) {
						if (roll_result < ptr->rarity_weight) {
							chosen = ptr;
							break;
						}
						roll_result -= ptr->rarity_weight;
					}

					if (chosen) {
						newInstance.customEffects.push_back(chosen->gameplay_effect);

						try {
							const auto& suffix_pool = g_effect_suffix_pools.at(chosen->effect_key);
							if (!suffix_pool.empty()) {
								std::string suffix = suffix_pool[gen() % suffix_pool.size()];
								ItemEffect suffixEffect;
								suffixEffect.type = "SUFFIX";
								suffixEffect.params["value"] = suffix;
								newInstance.customEffects.push_back(suffixEffect);
							}
						}
						catch (...) {}
					}
				}
			}
		}

		player.inventory[newInstanceId] = newInstance;
	}

	send_inventory_and_equipment();
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

		//we gotta get id from postgres sequencing atomic wasnt enuf
	// uint64_t newInstanceId = g_item_instance_id_counter++;


		uint64_t newInstanceId;
		try {
			pqxx::connection C = db_manager_->get_connection();
			pqxx::nontransaction N(C);
			// --- THIS IS THE CORRECTED LINE ---
			newInstanceId = N.exec("SELECT nextval('item_instance_id_seq')").one_row()[0].as<uint64_t>();
		}
		catch (const std::exception& e) {
			std::cerr << "CRITICAL: Could not fetch new item instance ID: " << e.what() << std::endl;
			send("SERVER:ERROR:Could not create item. Please try again.");
			return; // Stop here, we failed to get an ID
		}


		ItemInstance newInstance = { newInstanceId, itemId, qtyPerInstance, {}, {} };

		// --- (Your existing random effect roll logic) ---
		if (def.equipSlot != EquipSlot::None && !g_random_effect_pool.empty()) {
			std::uniform_int_distribution<> initial_roll(1, 100);
			if (initial_roll(gen) <= 5) { //5 percent chance to roll unique
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
		// --- (End of random effect logic) ---

		// Add the new item to the player's inventory
		player.inventory[newInstanceId] = newInstance;

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
		send("SERVER:ERROR:You do not have that item.");
		return;
	}

	ItemInstance& instance = player.inventory.at(itemInstanceId);
	const ItemDefinition& def = instance.getDefinition();

	// 1. Check if item is equippable (cannot be "used")
	if (def.equipSlot != EquipSlot::None) {
		send("SERVER:ERROR:This item cannot be 'used'. Try equipping it.");
		return;
	}

	// 2. Check if item has any "USE" effects
	if (def.effects.empty()) {
		send("SERVER:STATUS:That item has no use.");
		return;
	}

	bool itemUsed = false;
	std::string effectMsg = ""; // We will build this message
	PlayerStats finalStats = getCalculatedStats(); // Get stats *once*

	// 3. Process all effects on the item
	for (const auto& effect : def.effects) {
		if (effect.type != "USE") continue; // Skip non-use effects (like GRANT_STAT)

		auto params = effect.params; // Get a copy of the params map
		std::string action = params.count("action") ? params["action"] : "";

		// --- Handle RESTORE_HEALTH (Potions/Food) ---
		if (action == "RESTORE_HEALTH") {
			try {
				int healAmount = std::stoi(params["amount"]);
				if (player.stats.health < finalStats.maxHealth) {
					player.stats.health = std::min(finalStats.maxHealth, player.stats.health + healAmount);
					itemUsed = true;
					effectMsg += "You restore " + std::to_string(healAmount) + " health. ";
				}
				else if (effectMsg.empty()) { // Only show if no other effect happened
					effectMsg = "Your health is already full. ";
				}
			}
			catch (...) { /* bad data */ }
		}

		// --- Handle RESTORE_MANA (Potions/Food) ---
		else if (action == "RESTORE_MANA") {
			try {
				int manaAmount = std::stoi(params["amount"]);
				if (player.stats.mana < finalStats.maxMana) {
					player.stats.mana = std::min(finalStats.maxMana, player.stats.mana + manaAmount);
					itemUsed = true;
					effectMsg += "You restore " + std::to_string(manaAmount) + " mana. ";
				}
				else if (effectMsg.empty()) {
					effectMsg = "Your mana is already full. ";
				}
			}
			catch (...) { /* bad data */ }
		}

		// --- Handle APPLY_BUFF (Elixirs) ---
		else if (action == "APPLY_BUFF") {
			try {
				std::string stat = params["stat"];
				int amount = std::stoi(params["amount"]);
				int duration = std::stoi(params["duration"]);

				StatusEffect buff;
				if (stat == "speed") buff.type = StatusType::SPEED_UP;
				else if (stat == "strength") buff.type = StatusType::ATTACK_UP;
				// Add more buff types here if needed
				else continue;

				buff.magnitude = amount;
				buff.remainingTurns = duration * 10; // * 10 turns for out-of-combat duration
				buff.appliedByPlayer = true;

				player.activeStatusEffects.push_back(buff);
				itemUsed = true;
				effectMsg += "You feel a temporary surge of " + stat + "! ";

			}
			catch (...) { /* bad data */ }
		}

		// --- Handle GRANT_SKILL (Skill Books) ---
		else if (action == "GRANT_SKILL") {
			if (params.count("skill_id")) {
				std::string skill_id = params["skill_id"];
				std::string outError = "";

				if (grantSkillToPlayer(skill_id, outError)) {
					itemUsed = true; // Consume the book
					effectMsg = "You read the tome and learn a new skill: " + skill_id + "! ";
				}
				else {
					itemUsed = false; // Do NOT consume the book
					effectMsg = outError; // Send the error (e.g., "Skill already known.")
					break; // Stop processing other effects if this one failed
				}
			}
		}
	} // End for loop

	// 4. Send final feedback
	if (effectMsg.empty()) {
		effectMsg = "That item doesn't seem to do anything.";
	}
	send("SERVER:STATUS:" + effectMsg);

	// 5. Consume item if it was successfully used
	if (itemUsed) {
		instance.quantity--;
		if (instance.quantity <= 0) {
			// Check if it was equipped (e.g., a bug) and unequip it
			for (auto& slotPair : player.equipment.slots) {
				if (slotPair.second.has_value() && slotPair.second.value() == itemInstanceId) {
					slotPair.second = std::nullopt;
					break;
				}
			}
			player.inventory.erase(itemInstanceId);
		}
		send_inventory_and_equipment();
		send_player_stats(); // Send updated stats (health/mana/skills)
	}
}
void AsyncSession::dropItem(uint64_t itemInstanceId, int quantity) {
	PlayerState& player = getPlayerState();
	auto& ws = getWebSocket();
	if (player.isTrading) {
		send("SERVER:ERROR:Cannot drop items while trading.");
		return;
	}
	if (quantity <= 0) {
		send("SERVER:ERROR:Invalid quantity.");
		return;
	}
	if (player.inventory.count(itemInstanceId) == 0) {
		send("SERVER:ERROR:You do not have that item.");
		return;
	}

	for (const auto& slotPair : player.equipment.slots) {
		if (slotPair.second.has_value() && slotPair.second.value() == itemInstanceId) {
			send("SERVER:ERROR:Cannot drop an equipped item. Unequip it first.");
			return;
		}
	}

	ItemInstance& instance = player.inventory.at(itemInstanceId);
	const ItemDefinition& def = instance.getDefinition();

	if (!def.stackable) {
		send("SERVER:STATUS:Dropped " + def.name + ".");
		player.inventory.erase(itemInstanceId);
	}
	else {
		if (quantity >= instance.quantity) {
			send("SERVER:STATUS:Dropped " + to_string(instance.quantity) + "x " + def.name + ".");
			player.inventory.erase(itemInstanceId);
		}
		else {
			instance.quantity -= quantity;
			send("SERVER:STATUS:Dropped " + to_string(quantity) + "x " + def.name + ".");
		}
	}
	send_inventory_and_equipment();
}

void AsyncSession::sellItem(uint64_t itemInstanceId, int quantity) {
	PlayerState& player = getPlayerState();
	auto& ws = getWebSocket();
	if (player.isTrading) {
		send("SERVER:ERROR:Cannot sell items while trading.");
		return;
	}
	if (quantity <= 0) {
		send("SERVER:ERROR:Invalid quantity.");
		return;
	}
	if (player.inventory.count(itemInstanceId) == 0) {
		send("SERVER:ERROR:You do not have that item.");
		return;
	}
	for (const auto& slotPair : player.equipment.slots) {
		if (slotPair.second.has_value() && slotPair.second.value() == itemInstanceId) {
			send("SERVER:ERROR:Cannot sell an equipped item. Unequip it first.");
			return;
		}
	}

	ItemInstance& instance = player.inventory.at(itemInstanceId);
	const ItemDefinition& def = instance.getDefinition();

	// --- REFACTORED LOGIC ---
	// Price is now calculated by the global helper function
	int sellPricePerItem = calculateItemSellPrice(instance, def);
	// --- END REFACTORED LOGIC ---

	// --- 3. Process Sale ---
	int totalSellPrice = 0;

	if (!def.stackable) {
		totalSellPrice = sellPricePerItem;
		send("SERVER:STATUS:Sold " + def.name + " for " + to_string(totalSellPrice) + " gold.");
		player.inventory.erase(itemInstanceId);
	}
	else {
		if (quantity >= instance.quantity) {
			totalSellPrice = sellPricePerItem * instance.quantity;
			send("SERVER:STATUS:Sold " + to_string(instance.quantity) + "x " + def.name + " for " + to_string(totalSellPrice) + " gold.");
			player.inventory.erase(itemInstanceId);
		}
		else {
			totalSellPrice = sellPricePerItem * quantity;
			instance.quantity -= quantity;
			send("SERVER:STATUS:Sold " + to_string(quantity) + "x " + def.name + " for " + to_string(totalSellPrice) + " gold.");
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
	send(stats_message);
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

		int sellPrice = calculateItemSellPrice(instance, def);

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

			{"customEffects", effects_json},

			{"sellPrice", sellPrice}

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

				int sellPrice = calculateItemSellPrice(instance, def);

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

					{"customEffects", effects_json},

					{"sellPrice", sellPrice}

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


	{

		// Get a reference to the session's local broadcast_data_

		auto& broadcastData = getBroadcastData();


		// Populate core data

		broadcastData.userId = player.userId;

		broadcastData.playerName = player.playerName;

		broadcastData.currentArea = player.currentArea;

		broadcastData.posX = player.posX;

		broadcastData.posY = player.posY;

		broadcastData.playerClass = player.currentClass;


		// --- START VERBOSE EQUIPMENT UPDATE ---


		// Slot 1: Weapon

		if (player.equipment.slots.count(EquipSlot::Weapon) && player.equipment.slots.at(EquipSlot::Weapon).has_value()) {

			uint64_t instanceId = player.equipment.slots.at(EquipSlot::Weapon).value();

			if (player.inventory.count(instanceId)) {

				broadcastData.weaponItemId = player.inventory.at(instanceId).itemId;

			}

			else {

				broadcastData.weaponItemId = "";

			}

		}

		else {

			broadcastData.weaponItemId = "";

		}


		// Slot 2: Hat

		if (player.equipment.slots.count(EquipSlot::Hat) && player.equipment.slots.at(EquipSlot::Hat).has_value()) {

			uint64_t instanceId = player.equipment.slots.at(EquipSlot::Hat).value();

			if (player.inventory.count(instanceId)) {

				broadcastData.hatItemId = player.inventory.at(instanceId).itemId;

			}

			else {

				broadcastData.hatItemId = "";

			}

		}

		else {

			broadcastData.hatItemId = "";

		}


		// Slot 3: Top

		if (player.equipment.slots.count(EquipSlot::Top) && player.equipment.slots.at(EquipSlot::Top).has_value()) {

			uint64_t instanceId = player.equipment.slots.at(EquipSlot::Top).value();

			if (player.inventory.count(instanceId)) {

				broadcastData.torsoItemId = player.inventory.at(instanceId).itemId;

			}

			else {

				broadcastData.torsoItemId = "";

			}

		}

		else {

			broadcastData.torsoItemId = "";

		}


		// Slot 4: Bottom

		if (player.equipment.slots.count(EquipSlot::Bottom) && player.equipment.slots.at(EquipSlot::Bottom).has_value()) {

			uint64_t instanceId = player.equipment.slots.at(EquipSlot::Bottom).value();

			if (player.inventory.count(instanceId)) {

				broadcastData.legsItemId = player.inventory.at(instanceId).itemId;

			}

			else {

				broadcastData.legsItemId = "";

			}

		}

		else {

			broadcastData.legsItemId = "";

		}


		// Slot 5: Boots

		if (player.equipment.slots.count(EquipSlot::Boots) && player.equipment.slots.at(EquipSlot::Boots).has_value()) {

			uint64_t instanceId = player.equipment.slots.at(EquipSlot::Boots).value();

			if (player.inventory.count(instanceId)) {

				broadcastData.bootsItemId = player.inventory.at(instanceId).itemId;

			}

			else {

				broadcastData.bootsItemId = "";

			}

		}

		else {

			broadcastData.bootsItemId = "";

		}



		std::lock_guard<std::mutex> lock(g_player_registry_mutex);

		g_player_registry[player.userId] = broadcastData;

	}


	// --- Send to client ---

	std::string msg = "SERVER:INVENTORY_UPDATE:" + payload.dump();

	send(msg);


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
	send(response);
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
	send(message);
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
		send(level_msg);
		string prompt_msg = "SERVER:PROMPT:You have " + to_string(player.availableSkillPoints) + " new skill points to spend.";
		send(prompt_msg);
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
		send(oss.str());
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
	send(oss.str());
}



// --- NEW: Auth and DB Functions ---

void AsyncSession::handle_register(const string& credentials) {
	auto& ws = ws_;
	string username, password;
	stringstream ss(credentials);
	if (!getline(ss, username, ':') || !getline(ss, password)) {
		send("SERVER:ERROR:Invalid registration format.");
		return;
	}
	if (username.length() < 3 || username.length() > 20) {
		send("SERVER:ERROR:Username must be 3-20 characters.");
		return;
	}
	if (password.length() < 6) {
		send("SERVER:ERROR:Password must be at least 6 characters.");
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

		send("SERVER:REGISTRATION_SUCCESS:Account created. Please log in.");

	}
	catch (const pqxx::unique_violation& e) {
		cerr << "Registration failed (unique_violation): " << e.what() << endl;
		send("SERVER:ERROR:Username is already taken.");
	}
	catch (const exception&) {
		cerr << "Registration error: " << endl;
		send("SERVER:ERROR:An internal error occurred.");
	}
}



void AsyncSession::handle_login(const string& credentials) {
	string username, password;
	stringstream ss(credentials);
	if (!getline(ss, username, ':') || !getline(ss, password)) {
		send("SERVER:ERROR:Invalid login format.");
		return;
	}

	// Capture 'self' (shared_ptr) to keep the session alive.
	auto self = shared_from_this();

	// --- Step 1: Enqueue the blocking work ---
	// This function now returns INSTANTLY. The work happens on the db_pool_.
	db_pool_->enqueue([self, username, password] {

		// --- This code runs on a DB THREAD ---
		LoginResult result;
		try {
			pqxx::connection C = self->db_manager_->get_connection();
			pqxx::nontransaction N(C);
			string sql = "SELECT id, password_hash, player_class FROM accounts WHERE username = $1";
			pqxx::result R = N.exec(pqxx::zview(sql), pqxx::params(username));

			if (R.empty()) {
				result.error_message = "Invalid username or password.";
			}
			else {
				std::string stored_hash = R[0]["password_hash"].as<string>();

				// This slow hash now safely blocks the DB thread
				if (crypto_pwhash_str_verify(stored_hash.c_str(), password.c_str(), password.length()) != 0) {
					result.error_message = "Invalid username or password.";
				}
				else {
					// SUCCESS!
					result.success = true;
					result.account_id = R[0]["id"].as<int>();
					result.player_class_str = R[0]["player_class"].as<string>();
				}
			}
		}
		catch (const exception& e) {
			std::cerr << "Login DB error: " << e.what() << std::endl;
			result.error_message = "An internal server error occurred.";
		}

		// --- Step 2: Post the result back to the Asio thread ---
		// We use net::post to run on_login_finished() on the session's strand.
		net::post(self->ws_.get_executor(), [self, result] {
			// --- This code runs back on the Asio THREAD ---
			self->on_login_finished(result);
			});
		});
}


void AsyncSession::on_login_finished(const LoginResult& result) {
	// This code is now 100% async and runs safely on the Asio thread.
	if (!result.success) {
		send("SERVER:ERROR:" + result.error_message);
		return;
	}

	is_authenticated_ = true;
	account_id_ = result.account_id;

	send("SERVER:LOGIN_SUCCESS");

	// load_character is ALSO blocking, so we must refactor it.
	// We'll use the same pattern: enqueue, run sync, post back.
	auto self = shared_from_this();
	db_pool_->enqueue([self, account_id = result.account_id, class_str = result.player_class_str] {

		// --- This code runs on a DB THREAD ---
		// We call the *original* load_character, which is fine
		// because it's blocking on this separate thread.
		self->load_character(account_id);

		// Post the final step back to the Asio thread
		net::post(self->ws_.get_executor(), [self, class_str] {
			// --- This code runs back on the Asio THREAD ---
			self->ensureAutoGrantedSkillsForClass();

			if (class_str == "UNSELECTED") {
				self->send_player_stats();
				self->send("SERVER:PROMPT:Welcome! Please pick a class!");
			}
			else {
				self->send_player_stats();
				self->send_inventory_and_equipment();
				self->send_crafting_recipes();
				self->send("SERVER:CHARACTER_LOADED");
				self->handle_message("GO_TO:" + self->getPlayerState().currentArea);
			}
			});
		});
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


		auto getItemId = [&](const std::optional<uint64_t>& optId) -> std::string {
			if (optId.has_value() && player.inventory.count(optId.value())) {
				return player.inventory.at(optId.value()).itemId;
			}
			return "";
			};

		// We must populate the broadcast data with equipment IDs *on load*
		broadcast_data_.weaponItemId = player.equipment.slots.count(EquipSlot::Weapon) ?
			getItemId(player.equipment.slots.at(EquipSlot::Weapon)) : "";

		broadcast_data_.hatItemId = player.equipment.slots.count(EquipSlot::Hat) ?
			getItemId(player.equipment.slots.at(EquipSlot::Hat)) : "";

		broadcast_data_.torsoItemId = player.equipment.slots.count(EquipSlot::Top) ?
			getItemId(player.equipment.slots.at(EquipSlot::Top)) : "";

		broadcast_data_.legsItemId = player.equipment.slots.count(EquipSlot::Bottom) ?
			getItemId(player.equipment.slots.at(EquipSlot::Bottom)) : "";

		broadcast_data_.bootsItemId = player.equipment.slots.count(EquipSlot::Boots) ?
			getItemId(player.equipment.slots.at(EquipSlot::Boots)) : "";
		// --- END ADDED LOGIC ---

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

	AreaData& area = areaIt->second;

	// Lock the area's monster list while we read it
	std::lock_guard<std::mutex> lock(area.monster_mutex);

	for (const auto& pair : area.live_monsters) {
		const LiveMonster& lm = pair.second;

		// Only add monsters that are currently alive
		if (lm.is_alive) {
			MonsterState ms;
			ms.id = lm.spawn_id; // The client knows this as the unique ID
			ms.type = lm.monster_type;
			ms.assetKey = lm.asset_key;
			ms.posX = lm.position.x;
			ms.posY = lm.position.y;
			player.currentMonsters.push_back(ms);
		}
	}

	std::cout << "[Sync] Synced " << player.currentMonsters.size()
		<< " LIVE monsters for player " << player.playerName
		<< " in area " << player.currentArea << std::endl;
}


void AsyncSession::save_character() {
	if (!is_authenticated_ || account_id_ == 0)
		return;

	// --- Step 1: Copy all data ---
	// This copy happens on whatever thread called save_character()
	// (either an Asio thread or the auto-save timer thread).
	PlayerState player_copy = getPlayerState();
	int account_id_copy = account_id_.load();
	std::string user_id_copy = player_copy.userId;

	// --- Step 2: Capture 'self' (shared_ptr) to fix crash ---
	auto self = shared_from_this();

	// --- Step 3: Enqueue on the 1-thread 'save_pool_' queue ---
	save_pool_->enqueue([self, player_copy, account_id_copy, user_id_copy] {

		// --- ALL OF THIS RUNS ON THE SAVE THREAD ---

		std::cout << "[SAVE QUEUE] Saving character for account " << account_id_copy
			<< " (User: " << user_id_copy << ")" << std::endl;

		try {
			pqxx::connection C = self->db_manager_->get_connection();
			pqxx::work W(C);

			// --- 1. Save Player Account Data ---
			nlohmann::json skills_json;
			skills_json["spells"] = player_copy.skills.spells;
			skills_json["life_skills"] = player_copy.skills.life_skills;
			std::string skills_json_string = skills_json.dump();

			std::string class_str = "UNSELECTED";
			if (player_copy.currentClass == PlayerClass::FIGHTER) class_str = "FIGHTER";
			else if (player_copy.currentClass == PlayerClass::WIZARD) class_str = "WIZARD";
			else if (player_copy.currentClass == PlayerClass::ROGUE)  class_str = "ROGUE";

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
				player_copy.playerName, class_str, player_copy.currentArea,
				player_copy.posX, player_copy.posY,
				player_copy.stats.maxHealth, player_copy.stats.maxMana, player_copy.stats.defense,
				player_copy.stats.speed, player_copy.stats.strength, player_copy.stats.dexterity,
				player_copy.stats.intellect, player_copy.stats.luck,
				player_copy.stats.level, player_copy.stats.experience, player_copy.stats.experienceToNextLevel,
				player_copy.stats.gold, player_copy.availableSkillPoints, skills_json_string,
				account_id_copy
			));

			// --- 2. Clear Old Inventory ---
			W.exec(pqxx::zview("DELETE FROM player_items WHERE account_id = $1"),
				pqxx::params(account_id_copy));

			// --- 3. Insert Current Inventory ---
			std::string sql_insert_item = R"(
                INSERT INTO player_items (
                    instance_id, account_id, item_id, quantity,
                    custom_stats, custom_effects, equipped_slot
                ) VALUES ($1, $2, $3, $4, $5, $6, $7)
            )";

			for (const auto& [instanceId, instance] : player_copy.inventory) {
				nlohmann::json custom_stats_json = instance.customStats;
				std::string custom_stats_str = custom_stats_json.dump();

				nlohmann::json custom_effects_json = nlohmann::json::array();
				for (const auto& effect : instance.customEffects) {
					nlohmann::json j_effect;
					j_effect["type"] = effect.type;
					j_effect["params"] = effect.params;
					custom_effects_json.push_back(j_effect);
				}
				std::string custom_effects_str = custom_effects_json.dump();

				std::string equipped_slot_str;
				bool has_slot = false;
				for (const auto& [slot, optInstance] : player_copy.equipment.slots) {
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
							account_id_copy,
							instance.itemId,
							instance.quantity,
							custom_stats_str,
							custom_effects_str,
							equipped_slot_str));
				}
				else {
					W.exec(pqxx::zview(sql_insert_item),
						pqxx::params(static_cast<long long>(instanceId),
							account_id_copy,
							instance.itemId,
							instance.quantity,
							custom_stats_str,
							custom_effects_str,
							pqxx::zview("null")));
				}
			} // end for loop

			// --- 4. Commit Transaction ---
			W.commit();

			std::cout << "[SAVE SUCCESS] Saved account " << account_id_copy << std::endl;

		}
		catch (const std::exception& e) {
			std::cerr << "[SAVE FAILED] (Transaction Rolled Back) for account "
				<< account_id_copy << ": " << e.what() << std::endl;
		}
		}); // end save_pool_->enqueue
}


void AsyncSession::send_crafting_recipes() {
	nlohmann::json j = nlohmann::json::array();

	for (const auto& [recipeId, recipe] : g_crafting_recipes) {
		// Look up the result item to get its Name and Description
		std::string name = "Unknown Item";
		std::string desc = "Craftable item.";

		if (itemDatabase.count(recipe.resultItemId) != 0) {
			const auto& def = itemDatabase.at(recipe.resultItemId);
			name = def.name;
			desc = def.description;
		}

		j.push_back({
			{"id", recipeId},
			{"name", name},
			{"description", desc},
			{"resultItemId", recipe.resultItemId},
			{"resultQuantity", recipe.quantityCreated},
			{"requiredSkill", recipe.requiredSkill},
			{"requiredLevel", recipe.requiredLevel},
			{"ingredients", recipe.ingredients}, // Automatically maps to JSON object
			{"xpReward", recipe.xpReward}
			});
	}

	std::ostringstream oss;
	oss << "SERVER:RECIPES:" << j.dump();
	send(oss.str());
}


void AsyncSession::handle_message(const string& message)
{
	PlayerState& player = getPlayerState();
	PlayerBroadcastData& broadcast_data = getBroadcastData();
	auto& ws = ws_;
	string client_address = client_address_;
	auto get_current_trade = [&]() -> std::shared_ptr<TradeSession> {
		if (!player.isTrading) {
			send("SERVER:ERROR:You are not in a trade.");
			return nullptr;
		}
		std::lock_guard<std::mutex> lock(g_active_trades_mutex);
		auto it = g_active_trades.find(player.userId);
		if (it == g_active_trades.end()) {
			send("SERVER:ERROR:Trade session not found.");
			player.isTrading = false; // Fix broken state
			return nullptr;
		}
		return it->second;
		};
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
		send("SERVER:ERROR:You must be logged in to do that.");
		return;
	}
	if (message.rfind("/", 0) == 0) {
		// Extract command (e.g., "/additem") and arguments (e.g., "IRON_SWORD 1")
		std::string command_with_args = message.substr(1); // Remove leading slash
		size_t space_pos = command_with_args.find(' ');
		std::string command = (space_pos == std::string::npos)
			? command_with_args
			: command_with_args.substr(0, space_pos);
		std::string args = (space_pos == std::string::npos)
			? ""
			: command_with_args.substr(space_pos + 1);

		bool isAdmin = (player.playerName == "Admin");

		// --- ADMIN COMMAND: /additem ---
		if (command == "additem" && isAdmin) {
			std::stringstream ss(args);
			std::string itemId;
			int quantity = 1;

			if (ss >> itemId) {
				ss >> quantity;
			}

			if (itemDatabase.count(itemId) == 0) {
				send("SERVER:STATUS:Admin: Unknown item ID: " + itemId);
				return;
			}

			addItemToInventory(itemId, std::max(1, quantity));
			send("SERVER:STATUS:Admin: Granted " + std::to_string(std::max(1, quantity)) + "x " + itemId + ".");
			return;
		}
		// --- END ADMIN COMMAND: /additem ---

		// Handle other general slash commands here if needed
		send("SERVER:STATUS:Unknown command: /" + command);
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
	else if (message.rfind("TRADE_REQUEST:", 0) == 0) {
		if (player.isTrading) {
			send("SERVER:ERROR:You are already in a trade.");
			return;
		}
		std::string targetId = message.substr(14);
		if (targetId == player.userId) {
			send("SERVER:ERROR:You cannot trade with yourself.");
			return;
		}

		auto targetSession = get_session_by_id(targetId);
		if (!targetSession) {
			send("SERVER:ERROR:Player not found or offline.");
			return;
		}

		if (targetSession->getPlayerState().isTrading) {
			send("SERVER:ERROR:That player is busy.");
			return;
		}

		// Mark self as busy
		player.isTrading = true;
		player.tradePartnerId = targetId;

		// Send request to target
		nlohmann::json req;
		req["from"] = player.playerName;
		req["fromId"] = player.userId;
		targetSession->send("SERVER:TRADE_REQUEST:" + req.dump());

		send("SERVER:STATUS:Trade request sent to " + targetSession->getPlayerState().playerName + ".");
	}
	else if (message.rfind("TRADE_DECLINE:", 0) == 0) {
		std::string initiatorId = message.substr(14);

		// Clear self's trading status (if we were the one declining)
		if (player.isTrading && player.tradePartnerId == initiatorId) {
			player.isTrading = false;
			player.tradePartnerId.clear();
		}

		auto initiatorSession = get_session_by_id(initiatorId);
		if (initiatorSession) {
			// Tell initiator they were declined
			initiatorSession->send("SERVER:TRADE_DECLINED:" + nlohmann::json(player.playerName).dump());

			// Clear initiator's trading status
			initiatorSession->getPlayerState().isTrading = false;
			initiatorSession->getPlayerState().tradePartnerId.clear();
		}
	}
	else if (message.rfind("TRADE_ACCEPT:", 0) == 0) {
		std::string initiatorId = message.substr(13);

		auto initiatorSession = get_session_by_id(initiatorId);
		if (!initiatorSession) {
			send("SERVER:ERROR:That player is no longer available.");
			return;
		}

		// Final check: are both players still available?
		if (!initiatorSession->getPlayerState().isTrading || initiatorSession->getPlayerState().tradePartnerId != player.userId) {
			send("SERVER:ERROR:The trade request expired.");
			return;
		}
		if (player.isTrading) {
			send("SERVER:ERROR:You are already busy.");
			return;
		}

		// --- Both are valid! Create the session ---
		player.isTrading = true;
		player.tradePartnerId = initiatorId;
		// Initiator is already set to isTrading=true, tradePartnerId=player.userId

		auto trade = std::make_shared<TradeSession>();
		trade->playerAId = initiatorId;
		trade->playerBId = player.userId;

		{
			std::lock_guard<std::mutex> lock(g_active_trades_mutex);
			g_active_trades[initiatorId] = trade;
			g_active_trades[player.userId] = trade;
		}

		// Notify both clients to open the trade window
		nlohmann::json j_to_A;
		j_to_A["partnerName"] = player.playerName;
		j_to_A["partnerId"] = player.userId;
		initiatorSession->send("SERVER:TRADE_STARTED:" + j_to_A.dump());

		nlohmann::json j_to_B;
		j_to_B["partnerName"] = initiatorSession->getPlayerState().playerName;
		j_to_B["partnerId"] = initiatorId;
		send("SERVER:TRADE_STARTED:" + j_to_B.dump());
	}
	else if (message.rfind("TRADE_ADD_ITEM:", 0) == 0) {
		auto trade = get_current_trade();
		if (!trade) return;

		try {
			std::string params = message.substr(15); // "TRADE_ADD_ITEM:"
			size_t colon_pos = params.find(':');
			if (colon_pos == std::string::npos) throw std::invalid_argument("Invalid format");
			uint64_t instanceId = std::stoull(params.substr(0, colon_pos));
			int quantity = std::stoi(params.substr(colon_pos + 1));

			if (quantity <= 0) throw std::invalid_argument("Qty <= 0");

			if (player.inventory.count(instanceId) == 0) {
				send("SERVER:ERROR:Item not in inventory.");
				return;
			}
			// Check if equipped
			for (const auto& slotPair : player.equipment.slots) {
				if (slotPair.second.has_value() && slotPair.second.value() == instanceId) {
					send("SERVER:ERROR:Cannot trade an equipped item.");
					return;
				}
			}

			auto& offerItems = (trade->playerAId == player.userId) ? trade->offerAItems : trade->offerBItems;

			// Check total quantity needed vs. what's in inventory
			int currentOffered = offerItems.count(instanceId) ? offerItems[instanceId] : 0;
			if (quantity > player.inventory.at(instanceId).quantity) {
				send("SERVER:ERROR:Not enough quantity.");
				return;
			}

			// This logic *sets* the offer to the specified quantity
			offerItems[instanceId] = quantity;
			trade->confirmA = false;
			trade->confirmB = false;
			send_trade_update(trade);

		}
		catch (const std::exception& e) {
			std::cerr << "TRADE_ADD_ITEM error: " << e.what() << std::endl;
			send("SERVER:ERROR:Invalid item format.");
		}
	}
	else if (message.rfind("TRADE_REMOVE_ITEM:", 0) == 0) {
		auto trade = get_current_trade();
		if (!trade) return;

		try {
			uint64_t instanceId = std::stoull(message.substr(18)); // "TRADE_REMOVE_ITEM:"
			auto& offerItems = (trade->playerAId == player.userId) ? trade->offerAItems : trade->offerBItems;

			if (offerItems.count(instanceId)) {
				offerItems.erase(instanceId);
				trade->confirmA = false;
				trade->confirmB = false;
				send_trade_update(trade);
			}
		}
		catch (const std::exception&) {
			send("SERVER:ERROR:Invalid item ID.");
		}
	}
	else if (message.rfind("TRADE_OFFER_GOLD:", 0) == 0) {
		auto trade = get_current_trade();
		if (!trade) return;

		try {
			int amount = std::stoi(message.substr(17)); // "TRADE_OFFER_GOLD:"
			if (amount < 0) throw std::invalid_argument("Amount < 0");

			if (player.stats.gold < amount) {
				send("SERVER:ERROR:You do not have that much gold.");
				return;
			}

			if (trade->playerAId == player.userId) {
				trade->offerAGold = amount;
			}
			else {
				trade->offerBGold = amount;
			}

			trade->confirmA = false;
			trade->confirmB = false;
			send_trade_update(trade);

		}
		catch (const std::exception&) {
			send("SERVER:ERROR:Invalid gold amount.");
		}
	}
	else if (message == "TRADE_CONFIRM") {
		auto trade = get_current_trade();
		if (!trade) return;

		bool isPlayerA = (trade->playerAId == player.userId);
		if (isPlayerA) {
			trade->confirmA = true;
		}
		else {
			trade->confirmB = true;
		}

		// Check if both players have confirmed
		if (trade->confirmA && trade->confirmB) {
			// --- FINAL EXCHANGE ---
			auto sessionA = get_session_by_id(trade->playerAId);
			auto sessionB = get_session_by_id(trade->playerBId);

			if (!sessionA || !sessionB) {
				send("SERVER:ERROR:Partner disconnected. Trade cancelled.");
				if (sessionA) sessionA->send("SERVER:ERROR:Partner disconnected. Trade cancelled.");
				cleanup_trade_session(trade->playerAId, trade->playerBId);
				return;
			}

			PlayerState& playerA = sessionA->getPlayerState();
			PlayerState& playerB = sessionB->getPlayerState();

			// 1. Re-validate Gold
			if (playerA.stats.gold < trade->offerAGold) {
				sessionA->send("SERVER:ERROR:You no longer have the required gold. Trade cancelled.");
				sessionB->send("SERVER:ERROR:Partner does not have the required gold. Trade cancelled.");
				cleanup_trade_session(trade->playerAId, trade->playerBId);
				return;
			}
			if (playerB.stats.gold < trade->offerBGold) {
				sessionB->send("SERVER:ERROR:You no longer have the required gold. Trade cancelled.");
				sessionA->send("SERVER:ERROR:Partner does not have the required gold. Trade cancelled.");
				cleanup_trade_session(trade->playerAId, trade->playerBId);
				return;
			}

			// 2. Re-validate Items
			for (auto const& [id, qty] : trade->offerAItems) {
				if (playerA.inventory.count(id) == 0 || playerA.inventory.at(id).quantity < qty) {
					sessionA->send("SERVER:ERROR:You no longer have the offered items. Trade cancelled.");
					sessionB->send("SERVER:ERROR:Partner no longer has the offered items. Trade cancelled.");
					cleanup_trade_session(trade->playerAId, trade->playerBId);
					return;
				}
			}
			for (auto const& [id, qty] : trade->offerBItems) {
				if (playerB.inventory.count(id) == 0 || playerB.inventory.at(id).quantity < qty) {
					sessionB->send("SERVER:ERROR:You no longer have the offered items. Trade cancelled.");
					sessionA->send("SERVER:ERROR:Partner no longer has the offered items. Trade cancelled.");
					cleanup_trade_session(trade->playerAId, trade->playerBId);
					return;
				}
			}

			// --- VALIDATION PASSED - PERFORM EXCHANGE ---

			// 3. Gold Transfer
			playerA.stats.gold = (playerA.stats.gold - trade->offerAGold) + trade->offerBGold;
			playerB.stats.gold = (playerB.stats.gold - trade->offerBGold) + trade->offerAGold;

			// 4. Item Transfer (A -> B)
			std::vector<uint64_t> itemsToRemoveA;
			std::vector<ItemInstance> itemsToAddB;
			for (auto const& [id, qty] : trade->offerAItems) {
				ItemInstance& itemA = playerA.inventory.at(id);

				uint64_t newInstanceId;
				try {
					pqxx::connection C = db_manager_->get_connection();
					pqxx::nontransaction N(C);
					newInstanceId = N.exec("SELECT nextval('item_instance_id_seq')").one_row()[0].as<uint64_t>();
				}
				catch (const std::exception& e) {
					std::cerr << "CRITICAL: Trade failed, could not get new item ID: " << e.what() << std::endl;
					sessionA->send("SERVER:ERROR:Database error during trade. Trade cancelled.");
					sessionB->send("SERVER:ERROR:Database error during trade. Trade cancelled.");
					cleanup_trade_session(trade->playerAId, trade->playerBId);
					return;
				}

				ItemInstance newItemForB = itemA; // Copy all data
				newItemForB.instanceId = newInstanceId;
				newItemForB.quantity = qty;
				itemsToAddB.push_back(newItemForB);

				// Decrement from A
				itemA.quantity -= qty;
				if (itemA.quantity <= 0) {
					itemsToRemoveA.push_back(id);
				}
			}

			// 5. Item Transfer (B -> A)
			std::vector<uint64_t> itemsToRemoveB;
			std::vector<ItemInstance> itemsToAddA;
			for (auto const& [id, qty] : trade->offerBItems) {
				ItemInstance& itemB = playerB.inventory.at(id);

				uint64_t newInstanceId;
				try {
					pqxx::connection C = db_manager_->get_connection();
					pqxx::nontransaction N(C);
					newInstanceId = N.exec("SELECT nextval('item_instance_id_seq')").one_row()[0].as<uint64_t>();
				}
				catch (const std::exception& e) {
					std::cerr << "CRITICAL: Trade (B->A) failed, could not get new item ID: " << e.what() << std::endl;
					sessionA->send("SERVER:ERROR:Database error during trade. Trade cancelled.");
					sessionB->send("SERVER:ERROR:Database error during trade. Trade cancelled.");
					// Don't cleanup, as A->B may have succeeded
					return;
				}

				ItemInstance newItemForA = itemB; // Copy
				newItemForA.instanceId = newInstanceId;
				newItemForA.quantity = qty;
				itemsToAddA.push_back(newItemForA);

				// Decrement from B
				itemB.quantity -= qty;
				if (itemB.quantity <= 0) {
					itemsToRemoveB.push_back(id);
				}
			}

			// 6. Now that all DB calls are done, apply changes to memory
			for (auto id : itemsToRemoveA) playerA.inventory.erase(id);
			for (auto id : itemsToRemoveB) playerB.inventory.erase(id);
			for (const auto& item : itemsToAddA) playerA.inventory[item.instanceId] = item;
			for (const auto& item : itemsToAddB) playerB.inventory[item.instanceId] = item;

			// 7. Notify and Cleanup
			sessionA->send("SERVER:TRADE_COMPLETE");
			sessionB->send("SERVER:TRADE_COMPLETE");

			sessionA->send_inventory_and_equipment();
			sessionB->send_inventory_and_equipment();
			sessionA->send_player_stats();
			sessionB->send_player_stats();

			cleanup_trade_session(trade->playerAId, trade->playerBId);

		}
		else {
			// Only one player confirmed, send update to show "Ready" status
			send_trade_update(trade);
		}
	}
	else if (message == "TRADE_CANCEL") {
		auto trade = get_current_trade();
		if (!trade) return;

		// Find the partner's ID
		std::string partnerId = (trade->playerAId == player.userId) ? trade->playerBId : trade->playerAId;

		// Notify the partner (if they are online)
		auto partnerSession = get_session_by_id(partnerId);
		if (partnerSession) {
			partnerSession->send("SERVER:TRADE_CANCELLED:Partner cancelled the trade.");
		}

		send("SERVER:TRADE_CANCELLED:You cancelled the trade.");

		// Use the helper to clean up
		cleanup_trade_session(trade->playerAId, trade->playerBId);
	}
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
		else { send("SERVER:ERROR:Invalid class."); return; }

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
			send("SERVER:ERROR:An error occurred saving your class.");
			std::cerr << "SELECT_CLASS error: " << std::endl;
			player.currentClass = PlayerClass::UNSELECTED;
			player.skills.spells.clear();
			return;
		}

		//  Give autoGranted skills for this class (Fireball, BloodStrike, etc.)
		ensureAutoGrantedSkillsForClass();

		std::cout << "[" << client_address << "] --- CLASS SET: " << class_str << " ---" << endl;
		send("SERVER:CLASS_SET:" + class_str);
		send_player_stats();
		send("SERVER:PROMPT:You have 3 skill points to distribute. Use UPGRADE_STAT:stat_name to spend points.");
		{ lock_guard<mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }
	}
	else if (message.rfind("UPGRADE_STAT:", 0) == 0) {
		if (player.currentClass == PlayerClass::UNSELECTED) {
			send("SERVER:ERROR:You must select a class first.");
		}
		else if (player.availableSkillPoints <= 0) { send("SERVER:ERROR:You have no skill points available."); }
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
					send("SERVER:ERROR:An error occurred saving your stats.");
					cerr << "UPGRADE_STAT error: " << endl;
					player.availableSkillPoints++;
					return;
				}

				send("SERVER:STAT_UPGRADED:" + stat_name);
				send_player_stats();
				if (player.availableSkillPoints == 0 && !player.isFullyInitialized) {
					player.isFullyInitialized = true;
					player.hasSpentInitialPoints = true;
					send("SERVER:CHARACTER_COMPLETE:Character creation complete! You can now explore.");
					send_inventory_and_equipment();
					handle_message("GO_TO:" + player.currentArea);

				}
				else if (player.availableSkillPoints > 0) { send("SERVER:PROMPT:You have " + to_string(player.availableSkillPoints) + " skill points remaining."); }
				else { send("SERVER:STATUS:All skill points spent."); }
			}
			else { send("SERVER:ERROR:Invalid stat name."); }
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
		player.isGathering = false;
		if (player.isTrading) {
			send("SERVER:ERROR:Cannot travel while trading.");
			return;
		}
		if (!player.isFullyInitialized)
		{
			send("SERVER:ERROR:Complete character creation first.");
			return;
		}

		if (player.isInCombat)
		{
			send("SERVER:ERROR:Cannot travel while in combat!");
			return;
		}

		std::string target_area = message.substr(6);
		player.currentPath.clear();

		// --- Validate area exists ---
		auto areaIt = g_areas.find(target_area);
		if (areaIt == g_areas.end())
		{
			send("SERVER:ERROR:Invalid or unknown travel destination.");
			return;
		}

		// --- Update player + broadcast data ---

		// Update private area (no lock needed, only this thread writes to player_)
		player.currentArea = target_area;

		broadcast_data.currentArea = target_area;
		const auto& spawns = get_area_spawns();

		auto spawnIt = spawns.find(target_area);
		if (spawnIt != spawns.end())
		{
			player.posX = spawnIt->second.x;
			player.posY = spawnIt->second.y;
			broadcast_data.posX = player.posX;
			broadcast_data.posY = player.posY;
		}

		// Update public broadcast data
		{
			std::lock_guard<std::mutex> lock(g_player_registry_mutex);
			g_player_registry[player.userId] = broadcast_data;
		}

		// --- Handle special zones (Town heals & clears combat) ---
		if (target_area == "TOWN")
		{
			PlayerStats finalStats = getCalculatedStats();
			player.isInCombat = false;
			player.currentMonsters.clear();
			player.stats.health = finalStats.maxHealth;
			player.stats.mana = finalStats.maxMana;
		}

		// --- Notify client of area change ---
		send("SERVER:AREA_CHANGED:" + target_area);
		std::cout << "[" << client_address_ << "] --- AREA CHANGED TO: "
			<< player.currentArea << " ---" << std::endl;

		// --- Send area data and monsters ---
		send_area_map_data(player.currentArea);
		SyncPlayerMonsters(player);
		send_current_monsters_list();
		send_player_stats();

		// --- 1. Get all other sessions in the area ---
		// --- MODIFICATION: Lock A then C ---
		std::vector<std::shared_ptr<AsyncSession>> sessions_in_area;
		std::vector<PlayerBroadcastData> broadcast_data_list; // Need to store data
		{
			std::lock_guard<std::mutex> lock_reg(g_session_registry_mutex); // Lock A
			std::lock_guard<std::mutex> lock_data(g_player_registry_mutex); // Lock C

			for (auto const& pair : g_session_registry) {
				if (pair.first == player.userId) continue; // Skip yourself

				auto data_it = g_player_registry.find(pair.first);
				if (data_it == g_player_registry.end()) continue;

				if (data_it->second.currentArea == player.currentArea) {
					if (auto session = pair.second.lock()) {
						sessions_in_area.push_back(session);
						broadcast_data_list.push_back(data_it->second); // Save their data
					}
				}
			}
		}


		// --- 2. Build "PLAYER_SPAWNED" message (for others) ---
		std::ostringstream oss_spawn;
		oss_spawn << "SERVER:PLAYER_SPAWNED:{";
		oss_spawn << "\"id\":" << nlohmann::json(broadcast_data.userId).dump()
			<< ",\"name\":" << nlohmann::json(broadcast_data.playerName).dump()
			<< ",\"class\":" << static_cast<int>(broadcast_data.playerClass)
			<< ",\"x\":" << broadcast_data.posX
			<< ",\"y\":" << broadcast_data.posY
			<< ",\"action\":" << nlohmann::json(broadcast_data.currentAction).dump()
			<< ",\"weaponItemId\":" << nlohmann::json(broadcast_data.weaponItemId).dump()
			<< ",\"hatItemId\":" << nlohmann::json(broadcast_data.hatItemId).dump()
			<< ",\"torsoItemId\":" << nlohmann::json(broadcast_data.torsoItemId).dump()
			<< ",\"legsItemId\":" << nlohmann::json(broadcast_data.legsItemId).dump()
			<< ",\"bootsItemId\":" << nlohmann::json(broadcast_data.bootsItemId).dump()
			<< "}";
		auto shared_spawn_msg = std::make_shared<std::string>(oss_spawn.str());

		// --- 3. Send this "spawn" message to all *other* players ---
		for (auto& session : sessions_in_area) {
			net::dispatch(session->ws_.get_executor(), [session, shared_spawn_msg]() {
				session->send(*shared_spawn_msg);
				});
		}

		// --- 4. Build "PLAYERS_IN_AREA" list (for *you*) ---
		std::ostringstream oss_area;
		oss_area << "SERVER:PLAYERS_IN_AREA:[";
		bool first = true;
		// --- MODIFICATION: Use the broadcast_data_list we saved ---
		for (auto& data : broadcast_data_list) {
			if (!first) {
				oss_area << ",";
			}
			oss_area << "{\"id\":" << nlohmann::json(data.userId).dump()
				<< ",\"name\":" << nlohmann::json(data.playerName).dump()
				<< ",\"class\":" << static_cast<int>(data.playerClass)
				<< ",\"x\":" << data.posX
				<< ",\"y\":" << data.posY
				<< ",\"action\":" << nlohmann::json(data.currentAction).dump()
				<< ",\"weaponItemId\":" << nlohmann::json(data.weaponItemId).dump()
				<< ",\"hatItemId\":" << nlohmann::json(data.hatItemId).dump()
				<< ",\"torsoItemId\":" << nlohmann::json(data.torsoItemId).dump()
				<< ",\"legsItemId\":" << nlohmann::json(data.legsItemId).dump()
				<< ",\"bootsItemId\":" << nlohmann::json(data.bootsItemId).dump()
				<< "}";
			first = false;
		}
		// --- END MODIFICATION ---
		oss_area << "]";
		send(oss_area.str()); // Send the full list
	}
	   else if (message.rfind("MOVE_TO:", 0) == 0) {
		if (player.isTrading) {
			send("SERVER:ERROR:Cannot move while trading.");
			player.currentPath.clear(); // Clear path just in case
			return;
		}
		if (!player.isFullyInitialized) { send("SERVER:ERROR:Complete character creation first."); }
		else if (player.isInCombat) { send("SERVER:ERROR:Cannot move while in combat!"); }
		else {
			PlayerBroadcastData& broadcast = getBroadcastData();
			if (!broadcast.currentAction.empty()) {
				broadcast.currentAction = "";
				{
					std::lock_guard<std::mutex> lock(g_player_registry_mutex);
					g_player_registry[player.userId] = broadcast;
				}
			}

			// 2. Reset the gathering flag
			if (player.isGathering) {
				player.isGathering = false;
				send("SERVER:STATUS:Gathering stopped.");
			}
			auto it = g_area_grids.find(player.currentArea);
			if (it == g_area_grids.end()) {
				send("SERVER:ERROR:Grid movement is not available in this area.");
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
					send("SERVER:ERROR:Target coordinates are out of bounds.");
				}
				else if (current_grid[target_y][target_x] != 0) {
					send("SERVER:ERROR:Cannot move to that location.");
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
				send("SERVER:ERROR:Invalid coordinate format.");
			}
		}
	}
	   else if (message.rfind("SELL_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Complete character creation first.");
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
				send("SERVER:ERROR:Invalid sell command format.");
			}
		}
	}
	   else if (message.rfind("SEND_CHAT:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Must complete character creation to chat.");
			return;
		}
		string chat_text = message.substr(10);
		if (chat_text.empty() || chat_text.length() > 100) {
			send("SERVER:ERROR:Chat message must be 1-100 characters.");
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
					session->send(*shared_chat_msg);
				}
				catch (exception const&) {
					cerr << "Chat broadcast write error: " << "\n";
				}
				});
		}
	}
	   else if (message.rfind("PARTY_INVITE:", 0) == 0) {
		std::string targetName = message.substr(13);
		if (targetName == player.playerName) {
			send("SERVER:ERROR:Cannot invite yourself.");
			return;
		}

		// Find target session
		std::shared_ptr<AsyncSession> targetSession = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_session_registry_mutex);
			for (auto const& [id, weak] : g_session_registry) {
				if (auto s = weak.lock()) {
					if (s->getPlayerState().playerName == targetName) {
						targetSession = s;
						break;
					}
				}
			}
		}

		if (targetSession) {
			// Send Invite Request
			targetSession->getPlayerState().pendingPartyInviteId = player.partyId.empty() ? "NEW:" + player.userId : player.partyId;
			targetSession->send("SERVER:PARTY_INVITE_REQ:" + player.playerName);
			send("SERVER:STATUS:Invite sent to " + targetName);
		}
		else {
			send("SERVER:ERROR:Player not found.");
		}
	}
	   else if (message.rfind("PARTY_ACCEPT:", 0) == 0) {
		if (player.pendingPartyInviteId.empty()) {
			send("SERVER:ERROR:No pending invite.");
			return;
		}

		std::string inviterName = message.substr(13);

		std::lock_guard<std::mutex> lock(g_parties_mutex);

		// CASE 1: Create New Party
		if (player.pendingPartyInviteId.rfind("NEW:", 0) == 0) {
			std::string inviterId = player.pendingPartyInviteId.substr(4);
			auto inviterSession = get_session_by_id(inviterId);

			if (inviterSession && inviterSession->getPlayerState().partyId.empty()) {
				auto newParty = std::make_shared<Party>();
				newParty->partyId = "PARTY_" + inviterId;
				newParty->leaderId = inviterId;
				newParty->memberIds.push_back(inviterId);
				newParty->memberIds.push_back(player.userId);

				g_parties[newParty->partyId] = newParty;

				inviterSession->getPlayerState().partyId = newParty->partyId;
				player.partyId = newParty->partyId;

				// Broadcast the DATA (for UI) and the STATUS (for Chat)
				broadcast_party_update(newParty);
				broadcast_to_party(newParty, "SERVER:STATUS:Party Formed!");
			}
		}
		// CASE 2: Join Existing Party
		else {
			std::string pId = player.pendingPartyInviteId;
			if (g_parties.count(pId)) {
				auto party = g_parties[pId];
				if (party->memberIds.size() < 4) {
					party->memberIds.push_back(player.userId);
					player.partyId = pId;

					// Broadcast the DATA (for UI) and the STATUS (for Chat)
					broadcast_party_update(party);
					broadcast_to_party(party, "SERVER:STATUS:" + player.playerName + " joined the party.");
				}
				else {
					send("SERVER:ERROR:Party is full.");
				}
			}
		}
		player.pendingPartyInviteId = "";
	}
	   else if (message == "PARTY_LEAVE") {
		if (player.partyId.empty()) {
			send("SERVER:ERROR:You are not in a party.");
			return;
		}

		std::lock_guard<std::mutex> lock(g_parties_mutex);
		auto it = g_parties.find(player.partyId);

		if (it != g_parties.end()) {
			auto party = it->second;

			// 1. Remove YOU (The Leaver) from Member List
			auto& mems = party->memberIds;
			mems.erase(std::remove(mems.begin(), mems.end(), player.userId), mems.end());

			// 2. Remove YOU from Active Combat (if applicable)
			if (party->activeCombat) {
				auto& parts = party->activeCombat->participantIds;
				parts.erase(std::remove(parts.begin(), parts.end(), player.userId), parts.end());
				party->activeCombat->pendingActions.erase(player.userId);
				party->activeCombat->threatMap.erase(player.userId);

				// If you were the last real person in the fight, kill the combat instance
				if (parts.empty()) {
					// Respawn monster if needed so it doesn't disappear forever
					if (party->activeCombat->monster) {
						respawn_monster_immediately(player.currentArea, party->activeCombat->monster->id);
					}
					party->activeCombat = nullptr;
				}
			}

			// 3. Check State of Remaining Party
			if (mems.empty()) {
				// Case A: Party is now empty -> Delete it
				g_parties.erase(player.partyId);
			}
			else if (mems.size() == 1) {
				// Case B: Only 1 person left -> AUTO-DISBAND
				// Leaving a party of 1 is pointless, so we free the survivor.
				std::string lastMemberId = mems[0];

				if (auto lastSess = get_session_by_id(lastMemberId)) {
					PlayerState& lastP = lastSess->getPlayerState();

					// Clear their party data
					lastP.partyId = "";

					// If they were in combat, force end it so they aren't stuck
					if (lastP.isInCombat) {
						lastP.isInCombat = false;
						lastSess->send("SERVER:COMBAT_VICTORY:Party Disbanded"); // Closes combat UI
						lastSess->send_current_monsters_list(); // Refresh map
					}

					// Notify client to clear HUD
					lastSess->send("SERVER:PARTY_LEFT");
					lastSess->send("SERVER:STATUS:The party has been disbanded.");
				}

				// Finally, delete the party object
				g_parties.erase(player.partyId);
			}
			else {
				// Case C: Party still valid (2+ people) -> Assign new leader if needed
				if (party->leaderId == player.userId) {
					party->leaderId = mems[0];
					std::string newLeaderName = "Unknown";
					if (auto s = get_session_by_id(mems[0])) {
						newLeaderName = s->getPlayerState().playerName;
					}
					broadcast_to_party(party, "SERVER:STATUS:" + player.playerName + " left. " + newLeaderName + " is now leader.");
				}
				else {
					broadcast_to_party(party, "SERVER:STATUS:" + player.playerName + " left the party.");
				}
				// Update HUD for remaining members
				broadcast_party_update(party);
			}
		}

		// 4. Reset YOUR (The Leaver) State
		player.partyId = "";
		player.isInCombat = false; // Ensure you drop out of combat state

		// Refresh your view (in case you were in combat)
		if (player.currentOpponent) {
			player.currentOpponent.reset();
		}
		send_current_monsters_list();
		send("SERVER:PARTY_LEFT");
	}
	   else if (message.rfind("INTERACT_AT:", 0) == 0) {
		if (player.isInCombat) {
			send("SERVER:ERROR:Cannot interact while in combat!");
		}
		else {
			try {
				// --- Parse coordinates ---
				std::string coords_str = message.substr(12);
				size_t comma_pos = coords_str.find(',');
				if (comma_pos == std::string::npos) {
					throw std::invalid_argument("Invalid coordinate format.");
				}

				int target_x = std::stoi(coords_str.substr(0, comma_pos));
				int target_y = std::stoi(coords_str.substr(comma_pos + 1));

				// --- Find interactable at those coords in the current area ---
				const InteractableObject* targetObject = nullptr;
				auto areaIt = g_interactable_objects.find(player.currentArea);
				if (areaIt != g_interactable_objects.end()) {
					for (const auto& obj : areaIt->second) {
						if (obj.position.x == target_x && obj.position.y == target_y) {
							targetObject = &obj;
							break;
						}
					}
				}

				if (!targetObject) {
					send("SERVER:ERROR:No object to interact with at that location.");
					return;
				}

				// --- Distance check ---
				int dx = std::abs(player.posX - target_x);
				int dy = std::abs(player.posY - target_y);
				bool tooFar = false;

				if (targetObject->type == InteractableType::ZONE_TRANSITION) {
					// Zone transitions: must be ON the tile
					int dist = std::max(dx, dy); // 0 only if on the exact tile
					if (dist > 0) {
						tooFar = true;
					}
				}
				else {
					// Everything else: Chebyshev <= 1 (8 surrounding tiles)
					int dist = std::max(dx, dy);
					if (dist > 1) {
						tooFar = true;
					}
				}

				if (tooFar) {
					send("SERVER:ERROR:You are too far away to interact with that.");
					return;
				}

				// Stop any current movement
				player.currentPath.clear();

				// --- Handle interaction by type ---
				if (targetObject->type == InteractableType::NPC) {
					// 1) Basic "you are interacting" message (for chat log)
					send("SERVER:NPC_INTERACT:" + targetObject->data);

					// 2) Look up dialogue by key (e.g. "MAYOR_WELCOME_DIALOGUE", "GUARD_DIALOGUE", etc.)
					auto dlgIt = g_dialogues.find(targetObject->data);
					if (dlgIt != g_dialogues.end()) {
						nlohmann::json dlgJson;
						dlgJson["npcId"] = targetObject->id;       // e.g. "TOWN_MAYOR"
						dlgJson["dialogueId"] = targetObject->data; // e.g. "MAYOR_WELCOME_DIALOGUE"
						dlgJson["lines"] = nlohmann::json::array();

						for (const auto& line : dlgIt->second) {
							nlohmann::json lineJson;
							lineJson["speaker"] = line.speaker;    // "Mayor"
							lineJson["text"] = line.text;       // dialogue text
							lineJson["portraitId"] = line.portraitKey; // "MAYOR" etc.
							dlgJson["lines"].push_back(lineJson);
						}

						// This is what the React client listens for to open the dialogue box
						send("SERVER:DIALOGUE:" + dlgJson.dump());
					}
					else {
						// Fallback if no dialogue is defined for this NPC key
						send("SERVER:PROMPT:They have nothing to say right now.");
					}
				}
				else if (targetObject->type == InteractableType::SHOP) {
					// targetObject->data holds the shop id like "SHOP_TOWN_POTIONS", "SHOP_TOWN_ARMOR", etc.
					auto shop_it = g_shops.find(targetObject->data);
					if (shop_it == g_shops.end()) {
						send("SERVER:ERROR:Shop inventory not found.");
						return;
					}

					send("SERVER:PROMPT:Merchant: \"You there, got some gold, I've got stuff that might appeal to you\"");

					std::ostringstream oss;
					oss << "SERVER:SHOW_SHOP:{\"shopId\":\"" << shop_it->first << "\",\"items\":[";
					bool firstItem = true;

					for (const std::string& itemId : shop_it->second) {
						auto itemIt = itemDatabase.find(itemId);
						if (itemIt == itemDatabase.end()) continue;
						const ItemDefinition& def = itemIt->second;

						int price = 1;
						try {
							price = g_item_buy_prices.at(itemId);
						}
						catch (const std::out_of_range&) {
							std::cerr << "WARNING: Shop item " << itemId << " has no price. Defaulting to 1." << std::endl;
						}

						if (!firstItem) oss << ",";
						oss << "{"
							<< "\"itemId\":" << nlohmann::json(def.id).dump()
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
					send(oss.str());
				}
				else if (targetObject->type == InteractableType::ZONE_TRANSITION) {
					// Area change
					handle_message("GO_TO:" + targetObject->data);
					send_area_map_data(player.currentArea);
					SyncPlayerMonsters(player);
					send_current_monsters_list();
					send("SERVER:AREA_CHANGED:" + player.currentArea);
				}
				else if (targetObject->type == InteractableType::RESOURCE_NODE) {
					// 1. Find definition
					auto itDef = g_resource_defs.find(targetObject->data);
					if (itDef == g_resource_defs.end()) {
						send("SERVER:ERROR:Unknown resource type.");
						return;
					}
					const ResourceDefinition& def = itDef->second;

					// 2. Check Skills
					std::string skillName;
					std::string actionStr;

					switch (def.skill) {
					case LifeSkillType::WOODCUTTING:
						skillName = "Woodcutting";
						actionStr = "WOODCUTTING";
						break;
					case LifeSkillType::MINING:
						skillName = "Mining";
						actionStr = "MINING";
						break;
					case LifeSkillType::FISHING:
						skillName = "Fishing";
						actionStr = "FISHING";
						break;
					default:
						skillName = "Gathering";
						actionStr = "GATHERING";
						break;
					}

					int currentXp = player.skills.life_skills[skillName];
					int currentLevel = 1 + static_cast<int>(std::sqrt(currentXp) / 5.0f);

					if (currentLevel < def.requiredLevel) {
						send("SERVER:ERROR:Requires " + skillName + " level " + std::to_string(def.requiredLevel));
						return;
					}

					// 4. START CONTINUOUS GATHERING
					player.isGathering = true;
					player.gatheringResourceNode = targetObject->data;
					player.lastGatherTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(6000);

					// Update broadcast
					PlayerBroadcastData& broadcast = getBroadcastData();
					broadcast.currentAction = actionStr;

					{
						std::lock_guard<std::mutex> lock(g_player_registry_mutex);
						g_player_registry[player.userId] = broadcast;
					}

					send("SERVER:STATUS:You start gathering from the " + def.dropItemId + "...");
					send_player_stats();
				}
				else if (targetObject->type == InteractableType::CRAFTING_STATION) {
					std::string msg = "SERVER:OPEN_CRAFTING";
					send(msg);
				}
				else {
					send("SERVER:ERROR:Unknown interaction type.");
				}
			}
			catch (const std::exception&) {
				std::cerr << "Error parsing INTERACT_AT\n";
				send("SERVER:ERROR:Invalid coordinate format.");
			}
		}
	}

	   else if (message.rfind("CRAFT_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Character not initialized.");
			return;
		}

		std::string content = message.substr(11);
		std::string recipeId = "";
		std::string boostItemId = "";
		int quantity = 1;

		std::stringstream ss(content);
		std::string segment;
		std::vector<std::string> parts;

		while (std::getline(ss, segment, ':')) {
			parts.push_back(segment);
		}

		if (parts.empty()) {
			send("SERVER:ERROR:Invalid craft command format.");
			return;
		}

		recipeId = parts[0];

		if (parts.size() >= 2) {
			boostItemId = parts[1];
			if (boostItemId == "NONE") {
				boostItemId = "";
			}
		}

		if (parts.size() >= 3) {
			try {
				quantity = std::stoi(parts[2]);
			}
			catch (const std::exception& e) {
				std::cerr << "Warning: Could not parse craft quantity, defaulting to 1." << std::endl;
				quantity = 1;
			}
			if (quantity <= 0) quantity = 1;
		}

		if (g_crafting_recipes.count(recipeId) == 0) {
			send("SERVER:ERROR:Unknown recipe: " + recipeId);
			return;
		}

		const CraftingRecipe& recipe = g_crafting_recipes.at(recipeId);

		std::string skillKey = recipe.requiredSkill;
		int currentXp = player.skills.life_skills[skillKey];
		int currentLevel = 1 + static_cast<int>(std::sqrt(currentXp) / 5.0f);

		if (currentLevel < recipe.requiredLevel) {
			send("SERVER:ERROR:Requires " + skillKey + " level " + std::to_string(recipe.requiredLevel));
			return;
		}

		int bonusChance = 0;
		if (!boostItemId.empty()) {
			bool hasBooster = false;
			uint64_t boosterInstanceId = 0;

			for (auto& pair : player.inventory) {
				if (pair.second.itemId == boostItemId) {
					hasBooster = true;
					boosterInstanceId = pair.first;
					break;
				}
			}

			if (!hasBooster) {
				send("SERVER:ERROR:You are missing the boosting material: " + boostItemId);
				return;
			}

			if (boostItemId == "RUBY") bonusChance = 1;
			else if (boostItemId == "GOLDEN_LEAF") bonusChance = 1;
			else if (boostItemId == "PEARL") bonusChance = 1;
			else {
				send("SERVER:ERROR:That item cannot be used as a booster.");
				return;
			}

			ItemInstance& b = player.inventory.at(boosterInstanceId);
			b.quantity--;
			if (b.quantity <= 0) {
				player.inventory.erase(boosterInstanceId);
			}
		}

		for (const auto& [ingId, reqQty] : recipe.ingredients) {
			int totalReqQty = reqQty * quantity;
			int playerHas = 0;
			for (const auto& pair : player.inventory) {
				if (pair.second.itemId == ingId) {
					playerHas += pair.second.quantity;
				}
			}
			if (playerHas < totalReqQty) {
				send("SERVER:ERROR:Missing material: " + ingId + " (" + std::to_string(playerHas) + "/" + std::to_string(totalReqQty) + ")");
				return;
			}
		}

		for (const auto& [ingId, reqQty] : recipe.ingredients) {
			int totalReqQty = reqQty * quantity;
			int remainingToRemove = totalReqQty;
			std::vector<uint64_t> toRemove;

			for (auto& pair : player.inventory) {
				if (remainingToRemove <= 0) break;
				ItemInstance& item = pair.second;
				if (item.itemId == ingId) {
					int take = std::min(item.quantity, remainingToRemove);
					item.quantity -= take;
					remainingToRemove -= take;
					if (item.quantity <= 0) toRemove.push_back(pair.first);
				}
			}
			for (uint64_t uid : toRemove) {
				player.inventory.erase(uid);
			}
		}

		addCraftedItemToInventory(
			recipe.resultItemId,
			recipe.quantityCreated * quantity,
			bonusChance
		);

		player.skills.life_skills[skillKey] += recipe.xpReward * quantity;

		std::string msg = "SERVER:STATUS:Crafted " + std::to_string(quantity) + "x " + recipe.resultItemId + "! (+" + std::to_string(recipe.xpReward * quantity) + " XP)";
		if (bonusChance > 0) msg += " [Boosted!]";

		send(msg);
		send_inventory_and_equipment();
		send_player_stats();
	}
	   else if (message.rfind("MONSTER_SELECTED:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Complete character creation first.");
		}
		else if (player.isInCombat) {
			send("SERVER:ERROR:You are already in combat!");
		}
		else if (player.currentArea == "TOWN") {
			send("SERVER:STATUS:No monsters to fight in TOWN.");
		}
		else {
			try {
				int selected_spawn_id = stoi(message.substr(17));

				// --- PARTY CHECK ---
				std::shared_ptr<Party> party = (!player.partyId.empty()) ? get_party_by_id(player.partyId) : nullptr;

				if (party) {
					// ==============================
					//        PARTY COMBAT PATH
					// ==============================
					std::lock_guard<std::mutex> lock(g_parties_mutex); // Safety

					// 1. Initialize Combat if not started
					if (!party->activeCombat) {
						// Find monster type from Global Area Data
						auto areaIt = g_areas.find(player.currentArea);
						if (areaIt == g_areas.end()) return;

						std::string mType = "";
						{
							std::lock_guard<std::mutex> mLock(areaIt->second.monster_mutex);
							auto mIt = areaIt->second.live_monsters.find(selected_spawn_id);
							if (mIt != areaIt->second.live_monsters.end() && mIt->second.is_alive) {
								mType = mIt->second.monster_type;
								// Mark engaged globally so others can't take it
								mIt->second.is_alive = false;
								broadcast_monster_despawn(player.currentArea, selected_spawn_id, "PARTY_ENGAGEMENT");
							}
							else {
								send("SERVER:ERROR:That monster is gone.");
								return;
							}
						}

						auto monsterOpt = create_monster(selected_spawn_id, mType);
						if (!monsterOpt) return;

						auto combat = std::make_shared<PartyCombat>();
						combat->monster = std::make_shared<MonsterInstance>(*monsterOpt);

						// --- FIX START: Only add party members in the SAME AREA ---
						std::vector<std::string> validParticipants;
						std::string combatArea = player.currentArea;

						for (const auto& memId : party->memberIds) {
							auto memSess = get_session_by_id(memId);
							// Check if online AND in the same area
							if (memSess && memSess->getPlayerState().currentArea == combatArea) {
								validParticipants.push_back(memId);
								memSess->getPlayerState().isInCombat = true; // Set state immediately
							}
						}
						combat->participantIds = validParticipants;
						// --- FIX END ---

						combat->roundStartTime = std::chrono::steady_clock::now();
						party->activeCombat = combat;

						// Prepare Combat Start Message
						std::ostringstream oss;
						oss << "SERVER:COMBAT_START:{"
							<< "\"id\":" << combat->monster->id
							<< ",\"name\":" << nlohmann::json(combat->monster->type).dump()
							<< ",\"health\":" << combat->monster->health
							<< ",\"maxHealth\":" << combat->monster->maxHealth << "}";

						std::string startMsg = oss.str();

						// --- NEW MESSAGING LOGIC ---
						// Iterate all party members. 
						// If they are in validParticipants -> Send Combat Start + Turn.
						// If NOT -> Send a status update.
						for (const auto& memId : party->memberIds) {
							auto sess = get_session_by_id(memId);
							if (!sess) continue;

							bool isParticipant = false;
							for (const auto& pId : validParticipants) {
								if (pId == memId) { isParticipant = true; break; }
							}

							if (isParticipant) {
								sess->send(startMsg);
								sess->send("SERVER:COMBAT_TURN:Your turn.");
							}
							else {
								sess->send("SERVER:STATUS:Your party engaged a " + mType + " in " + combatArea + ", but you are too far away!");
							}
						}
					}
					else {
						// --- JOINING EXISTING COMBAT ---
						bool canJoin = false;
						if (!party->activeCombat->participantIds.empty()) {
							// Check area against the first participant (usually the leader/initiator)
							auto leaderSess = get_session_by_id(party->activeCombat->participantIds[0]);
							if (leaderSess && leaderSess->getPlayerState().currentArea == player.currentArea) {
								canJoin = true;
							}
						}

						if (canJoin) {
							// Add self to combat if not already there
							bool alreadyIn = false;
							for (const auto& pid : party->activeCombat->participantIds) {
								if (pid == player.userId) { alreadyIn = true; break; }
							}

							if (!alreadyIn) {
								party->activeCombat->participantIds.push_back(player.userId);
							}
							player.isInCombat = true;

							// Send current combat state to the joiner
							std::ostringstream oss;
							oss << "SERVER:COMBAT_START:{"
								<< "\"id\":" << party->activeCombat->monster->id
								<< ",\"name\":" << nlohmann::json(party->activeCombat->monster->type).dump()
								<< ",\"health\":" << party->activeCombat->monster->health
								<< ",\"maxHealth\":" << party->activeCombat->monster->maxHealth << "}";
							send(oss.str());

							// Let the joiner know the current turn state immediately
							send("SERVER:COMBAT_TURN:Your turn.");
						}
						else {
							send("SERVER:ERROR:You are too far away to join this battle!");
						}
					}
				}
				else {
					// ==============================
					//    SOLO COMBAT PATH (Unchanged)
					// ==============================
					auto areaIt = g_areas.find(player.currentArea);
					if (areaIt == g_areas.end()) return;
					AreaData& area = areaIt->second;
					bool engaged = false;
					std::string monster_type;

					{
						std::lock_guard<std::mutex> lock(area.monster_mutex);
						auto monsterIt = area.live_monsters.find(selected_spawn_id);
						if (monsterIt != area.live_monsters.end() && monsterIt->second.is_alive) {
							monsterIt->second.is_alive = false;
							monster_type = monsterIt->second.monster_type;
							engaged = true;
						}
					}

					if (engaged) {
						player.isInCombat = true;
						player.currentOpponent = create_monster(selected_spawn_id, monster_type);
						player.isDefending = false;

						// Clean local list
						auto it_local = std::find_if(player.currentMonsters.begin(), player.currentMonsters.end(),
							[selected_spawn_id](const MonsterState& m) { return m.id == selected_spawn_id; });
						if (it_local != player.currentMonsters.end()) player.currentMonsters.erase(it_local);

						broadcast_monster_despawn(player.currentArea, selected_spawn_id, player.userId);

						std::ostringstream oss;
						oss << "SERVER:COMBAT_START:"
							<< "{\"id\":" << player.currentOpponent->id
							<< ",\"name\":" << nlohmann::json(player.currentOpponent->type).dump()
							<< ",\"asset\":" << nlohmann::json(player.currentOpponent->assetKey).dump()
							<< ",\"health\":" << player.currentOpponent->health
							<< ",\"maxHealth\":" << player.currentOpponent->maxHealth << "}";
						send(oss.str());
						send("SERVER:COMBAT_LOG:You engaged the " + player.currentOpponent->type + "!");

						// Solo Speed Check
						PlayerStats finalStats = getCalculatedStats();
						if (finalStats.speed >= player.currentOpponent->speed) {
							send("SERVER:COMBAT_LOG:You are faster! You attack first.");
							send("SERVER:COMBAT_TURN:Your turn.");
						}
						else {
							// Monster Attacks First
							send("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " is faster! It attacks first.");
							float pwr = attack_power_for_monster(*player.currentOpponent);
							int dmg = damage_after_defense(pwr, finalStats.defense);
							player.stats.health -= dmg;
							send("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " attacks you for " + std::to_string(dmg) + " damage!");
							send_player_stats();
							if (player.stats.health <= 0) {
								send("SERVER:COMBAT_DEFEAT:You have been defeated!");
								player.isInCombat = false;
								respawn_monster_immediately(player.currentArea, player.currentOpponent->id);
								player.currentOpponent.reset();
								handle_message("GO_TO:TOWN");
							}
							else {
								send("SERVER:COMBAT_TURN:Your turn.");
							}
						}
					}
					else {
						send("SERVER:ERROR:That monster is no longer available.");
						SyncPlayerMonsters(player);
						send_current_monsters_list();
					}
				}
			}
			catch (const std::exception&) {
				send("SERVER:ERROR:Invalid monster ID format.");
			}
		}
	}
	   else if (message.rfind("COMBAT_ACTION:", 0) == 0) {

		// --- 1. Check if Player is in a Party Combat ---
		std::shared_ptr<Party> party = (!player.partyId.empty()) ? get_party_by_id(player.partyId) : nullptr;

		if (party && party->activeCombat) {

			// Parse the command
			std::string action_command = message.substr(14);
			std::string action_type;
			std::string action_param;

			size_t colon_pos = action_command.find(':');
			if (colon_pos != std::string::npos) {
				action_type = action_command.substr(0, colon_pos);
				action_param = action_command.substr(colon_pos + 1);
			}
			else {
				action_type = action_command;
			}

			// Build the action struct
			CombatAction action;
			action.actorId = player.userId;
			action.type = action_type;
			action.param = action_param;
			action.speed = getCalculatedStats().speed; // Used for sorting turn order later

			// Queue the action in the shared party state
			{
				// Lock needed if multiple players write to pendingActions simultaneously
				// (g_parties_mutex is usually coarse, ideally Party has its own mutex, 
				// but for now we rely on the single-thread nature of the session logic 
				std::lock_guard<std::mutex> lock(g_parties_mutex);
				party->activeCombat->pendingActions[player.userId] = action;
			}

			send("SERVER:COMBAT_LOG:Action queued. Waiting for party...");

			// Check if everyone has acted
			bool all_acted = false;
			{
				std::lock_guard<std::mutex> lock(g_parties_mutex);
				if (party->activeCombat->pendingActions.size() >= party->activeCombat->participantIds.size()) {
					all_acted = true;
				}
			}

			// If everyone is ready, resolve the round immediately
			if (all_acted) {
				resolve_party_round(party);
			}
		}
		else {


			if (!player.isInCombat || !player.currentOpponent) {
				send("SERVER:ERROR:You are not in combat.");
				return; // Return early
			}

			// --- Get the ID of the monster you are fighting ---
			int current_monster_spawn_id = player.currentOpponent->id;

			int extraDefFromBuffs = 0;
			bool monsterStunnedThisTurn = false;

			// This lambda replaces the logic you removed from getCalculatedStats
			auto apply_statuses_to_player_and_get_stats = [&]() -> PlayerStats {
				// 1. Get base stats + equipment stats
				PlayerStats stats = getCalculatedStats();

				int totalDefBuff = 0;
				int totalDot = 0;

				// 2. Iterate the list ONCE
				auto& effects = player.activeStatusEffects;
				for (auto it = effects.begin(); it != effects.end(); ) {
					StatusEffect& eff = *it;

					// 3. Apply all buffs/debuffs/DoTs in one place
					switch (eff.type) {
					case StatusType::BURN:
					case StatusType::BLEED: {
						int dmg = eff.magnitude;
						if (eff.type == StatusType::BURN) { dmg += stats.intellect / 20; }
						else { dmg += stats.dexterity / 25; }
						if (dmg < 1) dmg = 1;
						totalDot += dmg;

						// --- FIX: Allow health to go to 0 or below ---
						player.stats.health -= dmg;
						// --- END FIX ---

						break;
					}
					case StatusType::DEFENSE_UP: {
						totalDefBuff += eff.magnitude;
						break;
					}
					case StatusType::ATTACK_UP: {
						stats.strength += eff.magnitude;
						stats.dexterity += eff.magnitude;
						break;
					}
					case StatusType::ATTACK_DOWN: {
						stats.strength -= eff.magnitude;
						stats.dexterity -= eff.magnitude;
						break;
					}
					case StatusType::DEFENSE_DOWN: {
						stats.defense -= eff.magnitude;
						break;
					}
					case StatusType::SPEED_UP: {
						stats.speed += eff.magnitude;
						break;
					}
					case StatusType::SPEED_DOWN: {
						stats.speed -= eff.magnitude;
						break;
					}
											   // MANA_UP/DOWN are primarily handled during the combat tick, but can affect max mana/regen here if needed.
											   // Assuming magnitude applies to mana pool or regen over time (leaving base stats unchanged here).
					case StatusType::MANA_UP: {
						player.stats.mana = std::min(stats.maxMana, player.stats.mana + eff.magnitude);
						break;
					}
					case StatusType::MANA_DOWN: {
						player.stats.mana = std::max(0, player.stats.mana - eff.magnitude);
						break;
					}
					default:
						break;
					}

					// 4. Tick down duration and erase if expired
					eff.remainingTurns--;
					if (eff.remainingTurns <= 0) {
						it = effects.erase(it); // Safe to erase here
					}
					else {
						++it;
					}
				}

				if (totalDot > 0) {
					send("SERVER:COMBAT_LOG:You suffer " + std::to_string(totalDot) +
						" damage from ongoing effects!");
					send_player_stats(); // Now safe, getCalculatedStats() doesn't loop
				}

				extraDefFromBuffs = totalDefBuff;

				// 5. Clamp stats and return them
				stats.strength = std::max(0, stats.strength);
				stats.dexterity = std::max(0, stats.dexterity);
				stats.defense = std::max(0, stats.defense);
				stats.speed = std::max(0, stats.speed);

				return stats;
				};

			// This lambda for the monster is mostly unchanged and safe
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
					send("SERVER:COMBAT_LOG:The " + player.currentOpponent->type +
						" suffers " + std::to_string(totalDot) + " damage!");
					std::string update = "SERVER:COMBAT_UPDATE:" + std::to_string(player.currentOpponent->health);
					send(update);
				}
				};


			PlayerStats finalStats = apply_statuses_to_player_and_get_stats();

			apply_statuses_to_monster(); // Apply monster DoTs/Stuns

			// --- NEW FIX: Check for player death from DoTs ---
			if (player.stats.health <= 0) {
				player.stats.health = 0;
				send("SERVER:COMBAT_DEFEAT:You have been defeated by your wounds!");
				player.isInCombat = false;

				// Respawn monster
				respawn_monster_immediately(player.currentArea, current_monster_spawn_id);

				player.currentOpponent.reset();
				player.currentArea = "TOWN"; player.currentMonsters.clear();
				player.stats.health = player.stats.maxHealth / 2; player.stats.mana = player.stats.maxMana;
				player.posX = 26; player.posY = 12; player.currentPath.clear();
				broadcast_data.currentArea = "TOWN"; broadcast_data.posX = player.posX; broadcast_data.posY = player.posY;
				{ std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }

				send("SERVER:AREA_CHANGED:TOWN");
				send_area_map_data(player.currentArea);
				SyncPlayerMonsters(player); // Syncs to TOWN
				send_current_monsters_list();
				send_available_areas();
				send_player_stats();
				return; // Exit combat action
			}
			// --- END NEW FIX ---


			// --- (Rest of your combat logic begins here) ---
			std::string action_command = message.substr(14);
			std::string action_type; std::string action_param;
			size_t colon_pos = action_command.find(':');
			if (colon_pos != std::string::npos) { action_type = action_command.substr(0, colon_pos); action_param = action_command.substr(colon_pos + 1); }
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
					send("SERVER:COMBAT_LOG:A critical hit!");
				}

				player_damage = dmg; // then use your existing code that subtracts HP and logs
				std::string log_msg =
					"SERVER:COMBAT_LOG:You attack the " + player.currentOpponent->type +
					" for " + std::to_string(player_damage) + " damage!";
				send(log_msg);
			}
			else if (action_type == "SPELL") {
				std::string spellName = action_param;

				// Look up the spell definition
				auto itSkill = g_skill_defs.find(spellName);
				if (itSkill == g_skill_defs.end() || itSkill->second.type != SkillType::SPELL) {
					send("SERVER:COMBAT_LOG:Unknown spell: " + spellName);
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
					send("SERVER:COMBAT_LOG:You don't know that spell!");
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
					send("SERVER:COMBAT_LOG:You cannot cast that spell with your class.");
					return;
				}

				// Mana check
				if (player.stats.mana < spell.manaCost) {
					send(
						"SERVER:COMBAT_LOG:Not enough mana to cast " + spellName +
						"! (Needs " + std::to_string(spell.manaCost) + ")"
					);
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
						send("SERVER:COMBAT_LOG:Your " + spellName + " critically hits!");
					}

					player_damage = dmg;

					std::string log_msg =
						"SERVER:COMBAT_LOG:You cast " + spellName +
						" for " + std::to_string(player_damage) + " damage!";
					send(log_msg);
				}
				else {
					// Self-target spells (none for wizard yet, but future-proof)
					player_damage = 0;
					send("SERVER:COMBAT_LOG:You cast " + spellName + " on yourself.");
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
						int baseChance = 20;                    // 20% base
						int fromLuck = finalStats.luck / 2;  // +0.5% per LUCK
						int finalChance = baseChance + fromLuck;
						if (finalChance > 70) finalChance = 70;
						if (finalChance < 20) finalChance = 20;

						int roll = rand() % 100;
						if (roll >= finalChance) {
							applyStatus = false;
							send("SERVER:COMBAT_LOG:The lightning crackles, but fails to stun.");
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

						send("SERVER:COMBAT_LOG:" + statusMsg);
					}
				}
			}

			else if (action_type == "SKILL") {
				std::string skillName = action_param;

				// Look up the skill in the global registry
				auto itSkill = g_skill_defs.find(skillName);
				if (itSkill == g_skill_defs.end()) {
					send("SERVER:COMBAT_LOG:Unknown skill: " + skillName);
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
					send("SERVER:COMBAT_LOG:You don't know that skill!");
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
					send("SERVER:COMBAT_LOG:You cannot use that skill with your class.");
					return;
				}

				// Check mana cost
				if (player.stats.mana < skill.manaCost) {
					send("SERVER:COMBAT_LOG:Not enough mana to use " + skillName + "!");
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
						send("SERVER:COMBAT_LOG:A critical " + skillName + "!");
					}

					player_damage = dmg;

					std::string log_msg =
						"SERVER:COMBAT_LOG:You use " + skillName + " on the " +
						player.currentOpponent->type + " for " +
						std::to_string(player_damage) + " damage!";
					send(log_msg);
				}
				else {
					// Defensive / self-target skill (e.g., ShieldWall)
					player_damage = 0;
					std::string log_msg =
						"SERVER:COMBAT_LOG:You use " + skillName + " on yourself.";
					send(log_msg);
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
						send(
							"SERVER:COMBAT_LOG:The effects of " + skillName +
							" linger longer than usual!"
						);
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
						send(
							"SERVER:COMBAT_LOG:Your " + skillName + " " + statusMsg + " you."
						);
					}
					else {
						send(
							"SERVER:COMBAT_LOG:Your " + skillName + " " + statusMsg +
							" the " + player.currentOpponent->type + "."
						);
					}
				}
			}
			else if (action_type == "DEFEND") {
				player.isDefending = true;
				send("SERVER:COMBAT_LOG:You brace for the next attack.");
			}

			else if (action_type == "FLEE") {
				// FIX: Use -> access and std::max/min
				float flee_chance = 0.5f + ((float)finalStats.speed - (float)player.currentOpponent->speed) * 0.05f + ((float)finalStats.luck * 0.01f);
				flee_chance = std::max(0.1f, std::min(0.9f, flee_chance));
				if (((float)rand() / RAND_MAX) < flee_chance) { fled = true; }
				else { send("SERVER:COMBAT_LOG:You failed to flee!"); }
			}

			if (fled) {
				// FIX: Store string
				std::string log_msg = "SERVER:COMBAT_LOG:You successfully fled from the " + player.currentOpponent->type + "!";
				send(log_msg);
				player.isInCombat = false;

				// --- MODIFICATION ---
				respawn_monster_immediately(player.currentArea, current_monster_spawn_id);

				player.currentOpponent.reset();
				send("SERVER:COMBAT_VICTORY:Fled");
				//SyncPlayerMonsters(player); // DO NOT SYNC
				send_current_monsters_list();
				return;
			}

			// FIX: Use -> access
			if (player_damage > 0) { player.currentOpponent->health -= player_damage; }
			send_player_stats();
			// FIX: Store string
			std::string combat_update = "SERVER:COMBAT_UPDATE:" + to_string(player.currentOpponent->health);
			send(combat_update);

			// FIX: Use -> access
			if (player.currentOpponent->health <= 0) {
				std::string log_msg_1 = "SERVER:COMBAT_LOG:You defeated the " + player.currentOpponent->type + "!";
				send(log_msg_1);

				int xp_gain = player.currentOpponent->xpReward;
				std::string log_msg_2 = "SERVER:STATUS:Gained " + to_string(xp_gain) + " XP.";
				send(log_msg_2);

				player.stats.experience += xp_gain;
				int lootTier = player.currentOpponent->lootTier;
				const std::vector<std::string> ALL_SKILL_BOOKS = {
				"BOOK_SUNDER_ARMOR", "BOOK_PUMMEL", "BOOK_ENRAGE", "BOOK_WHIRLWIND", "BOOK_SECOND_WIND",
				"BOOK_VENOMOUS_SHANK", "BOOK_CRIPPLING_STRIKE", "BOOK_EVASION", "BOOK_GOUGE", "BOOK_BACKSTAB",
				"BOOK_FROST_NOVA", "BOOK_ARCANE_INTELLECT", "BOOK_LESSER_HEAL", "BOOK_MANA_SHIELD", "BOOK_PYROBLAST"
				};

				// Check if monster tier is 2 or higher
				if (player.currentOpponent->lootTier >= 2) {
					// 0.5% chance = 5 in 1000 roll
					int skillBookDropChance = 5; // (0.5 * 1000)

					// Roll 0 to 999
					if ((rand() % 1000) < skillBookDropChance) {
						// Successful drop! Pick one book at random.
						int bookIndex = rand() % ALL_SKILL_BOOKS.size();
						std::string droppedBookId = ALL_SKILL_BOOKS[bookIndex];

						// Add the item to inventory
						addItemToInventory(droppedBookId, 1);

						send("SERVER:STATUS:A rare tome drops! You found a " + itemDatabase.at(droppedBookId).name + "!");

						std::cout << "[LOOT DEBUG] RARE DROP SUCCESS: Skill Book (" << droppedBookId << ") from Tier "
							<< player.currentOpponent->lootTier << " monster." << std::endl;
					}
				}
				if (lootTier != -1) {
					// 1) Base drop chance defined per monster (0–100)
					int baseDropChance = player.currentOpponent->dropChance;

					// Example: 0 luck = 1.0x, 10 luck ≈ 1.3x, 25 luck ≈ 1.5x, 50 luck ≈ 1.7x
					double luckMultiplier = 1.0 + (std::sqrt(static_cast<double>(player.stats.luck)) / 15.0);
					if (luckMultiplier > 1.8) {
						luckMultiplier = 1.8;
					}

					// Tier 1 = 1.0, Tier 2 ≈ 0.85, Tier 3 ≈ 0.70, Tier 4 ≈ 0.55, Tier 5 ≈ 0.40 (min)
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
							send(log_msg_3);

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

				send("SERVER:COMBAT_VICTORY:Defeated");
				player.isInCombat = false;

				// --- MODIFICATION ---
				const int RESPAWN_TIME_SECONDS = 15; // <-- CHANGED FROM 60
				set_monster_respawn_timer(player.currentArea, current_monster_spawn_id, RESPAWN_TIME_SECONDS);

				player.currentOpponent.reset();
				check_for_level_up();

				// --- THIS IS THE FIX ---
				// DELETE this line: send_current_monsters_list();
				// ADD this line:
				broadcast_monster_list(player.currentArea);
				// --- END FIX ---

				send_player_stats();
				// DELETE this line (it was a duplicate): send_curre
				return;
			}

			// --- Start of Monster Turn ---

			if (monsterStunnedThisTurn) {
				send(
					"SERVER:COMBAT_LOG:The " + player.currentOpponent->type +
					" is stunned and cannot act!"
				);
				send("SERVER:COMBAT_TURN:Your turn.");
				return;
			}

			// --- NEW: DODGE CHECK ---
			float dodge_chance = dodge_chance_for_player(finalStats, player.currentClass);
			if (((float)rand() / RAND_MAX) < dodge_chance) {
				// The player dodged the attack!
				player.isDefending = false; // Reset defend state
				send("SERVER:COMBAT_LOG:You swiftly dodged the " + player.currentOpponent->type + "'s attack!");
				send("SERVER:COMBAT_TURN:Your turn.");
				return;
			}
			// --- END DODGE CHECK ---

			int monster_damage = 0;
			std::string action_log;
			int healingDone = 0; // Tracks self-healing/lifesteal amount

			// --- MONSTER ACTION LOGIC: Attack vs. Spell/Ability ---
			bool usedSkill = false;

			// Calculate a spell chance based on Intellect dominance
			int primary_physical_stat = std::max(player.currentOpponent->strength, player.currentOpponent->dexterity);
			int magic_stat = player.currentOpponent->intellect;
			float int_diff = (float)magic_stat - (float)primary_physical_stat;

			// Base chance for a non-physical monster is 30%. Add 2% per point of Int lead.
			int spellChance = 30 + static_cast<int>(int_diff * 2.0f);
			spellChance = std::min(70, spellChance);
			spellChance = std::max(30, spellChance); // Min 30% if it has skills

			// Decide between skill and attack (skill attempt only; fallback handled below)
			if (!player.currentOpponent->skills.empty() && (rand() % 100 < spellChance)) {

				// --- MONSTER ATTEMPTS SKILL ---

				// Pick a random skill from the monster's list
				int skillIndex = rand() % player.currentOpponent->skills.size();
				std::string skillName = player.currentOpponent->skills[skillIndex];

				// --- [START] CRASH FIX ---
				const SkillDefinition* skill_ptr = nullptr; // Use a pointer
				bool skill_found = false;

				// 1. LOOKUP: Check Monster Spell Map first
				auto itMonsterSkill = g_monster_spell_defs.find(skillName);
				if (itMonsterSkill != g_monster_spell_defs.end()) {
					skill_ptr = &itMonsterSkill->second;
					skill_found = true;
				}
				// 2. Fallback: Check Player Spell Map
				else {
					auto itPlayerSkill = g_skill_defs.find(skillName);
					if (itPlayerSkill != g_skill_defs.end()) {
						skill_ptr = &itPlayerSkill->second;
						skill_found = true;
					}
				}

				// 3. THE REAL FIX: Check the boolean flag, not a mixed-up iterator
				if (skill_found && skill_ptr != nullptr) {
					const SkillDefinition& skill = *skill_ptr; // Dereference the pointer
					// --- [END] CRASH FIX ---

					bool targetIsSelf = (skill.target == SkillTarget::SELF);

					// --- VALIDATION AND ACTION ---

					if (skill.target == SkillTarget::ENEMY || targetIsSelf) {
						usedSkill = true;

						// --- A. OFFENSIVE SKILLS (TARGET ENEMY) ---
						if (skill.target == SkillTarget::ENEMY) {

							float scaledAttack =
								player.currentOpponent->strength * skill.strScale +
								player.currentOpponent->dexterity * skill.dexScale +
								player.currentOpponent->intellect * skill.intScale +
								skill.flatDamage;

							int base_damage = static_cast<int>(std::round(scaledAttack));

							// Magic Resistance Check
							if (skill.isMagic) {
								float resistance_multiplier = 1.0f - magic_resistance_for_player(finalStats);
								base_damage = static_cast<int>(std::round(base_damage * resistance_multiplier));

								if (resistance_multiplier < 1.0f) {
									send("SERVER:COMBAT_LOG:You resist some of the " + skillName + "'s magic!");
								}
							}

							float variance = 0.9f + ((float)(rand() % 21) / 100.0f); // 0.9–1.1
							monster_damage = std::max(1, (int)std::round(base_damage * variance));

							action_log = "The " + player.currentOpponent->type + " uses " + skillName +
								" for " + std::to_string(monster_damage) + " damage!";

							// Lifesteal/Self-Heal from Damage Dealt
							if (skillName == "BLOOD_LEECH" || skillName == "SOUL_DRAIN" || skillName == "LIFE_SIPHON") {
								healingDone += (int)std::round(monster_damage * 0.5f);
							}
						}

						// --- B. DEFENSIVE/SELF SKILLS (TARGET SELF) ---
						else if (targetIsSelf) {
							action_log = "The " + player.currentOpponent->type + " uses " + skillName + " on itself.";

							// Direct Healing Logic (REGENERATE)
							if (skillName == "REGENERATE") {
								healingDone += std::abs((int)std::round(skill.flatDamage + (player.currentOpponent->intellect * skill.intScale)));
							}

							// Special Berserk/Sacrificial Logic (Dual effect, applies debuff to self)
							if (skillName == "BERSERK") {
								// BERSERK applies ATK_UP via status below. Manually apply DEF_DOWN to monster.
								StatusEffect defDown;
								defDown.type = StatusType::DEFENSE_DOWN;
								defDown.magnitude = 5; // Example: -5 Def
								defDown.remainingTurns = skill.statusDuration;
								player.currentOpponent->activeStatusEffects.push_back(defDown);
							}
							if (skillName == "SACRIFICIAL_BITE") {
								// SACRIFICIAL_BITE deals damage to player above, but requires a self-burn.
								StatusEffect selfBurn;
								selfBurn.type = StatusType::BURN;
								selfBurn.magnitude = 5;
								selfBurn.remainingTurns = 2;
								player.currentOpponent->activeStatusEffects.push_back(selfBurn);
								send("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " burns itself with dark energy!");
							}
							// Note: SUMMON_MINION/TERRIFY require complex handler logic not implemented here.
						}

						// --- C. APPLY STATUS EFFECTS (BOTH TARGETS) ---
						if (skill.appliesStatus) {
							StatusEffect eff;
							eff.type = skill.statusType;
							eff.magnitude = skill.statusMagnitude;
							eff.remainingTurns = skill.statusDuration;
							eff.appliedByPlayer = false;

							if (skill.target == SkillTarget::ENEMY) {
								player.activeStatusEffects.push_back(eff);
								send("SERVER:COMBAT_LOG:You are afflicted by " + skillName + "!");
							}
							else if (skill.target == SkillTarget::SELF) {
								// Buffs/Debuffs on monster
								player.currentOpponent->activeStatusEffects.push_back(eff);
								send("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " is affected by " + skillName + "!");
							}
						}

					}
					else {
						// Unusable skill (e.g., targets SELF but has no self-effect, or invalid skill definition)
						usedSkill = false;
					}
				}
			}

			// --- If skill failed or if it was ignored, execute BASIC ATTACK ---
			if (!usedSkill) {

				int player_defense = finalStats.defense + extraDefFromBuffs;

				if (player.isDefending) {
					player_defense *= 2;
					player.isDefending = false;
				}

				// Use shared monster helpers (Basic Attack Logic)
				float monsterAttackPower = attack_power_for_monster(*player.currentOpponent);
				int base_monster_damage = damage_after_defense(monsterAttackPower, player_defense);

				float monster_variance = 0.85f + ((float)(rand() % 31) / 100.0f);
				monster_damage = std::max(1, (int)std::round(base_monster_damage * monster_variance));

				float monster_crit_chance = crit_chance_for_monster(*player.currentOpponent);
				if (((float)rand() / RAND_MAX) < monster_crit_chance) {
					monster_damage = (int)std::round(monster_damage * 1.6f);
					action_log = "The " + player.currentOpponent->type + " lands a critical hit!";
					send("SERVER:COMBAT_LOG:" + action_log);
				}

				action_log = "The " + player.currentOpponent->type + " attacks you for " + std::to_string(monster_damage) + " damage!";
			}
			// --- END MONSTER ACTION LOGIC ---

			// --- Apply Damage to Player ---
			player.stats.health -= monster_damage;
			send("SERVER:COMBAT_LOG:" + action_log);

			// --- Apply Monster Healing (after damage calculation) ---
			if (healingDone > 0) {
				player.currentOpponent->health =
					std::min(player.currentOpponent->maxHealth, player.currentOpponent->health + healingDone);
				send("SERVER:COMBAT_LOG:The " + player.currentOpponent->type + " heals for " + std::to_string(healingDone) + " health!");
				std::string update = "SERVER:COMBAT_UPDATE:" + std::to_string(player.currentOpponent->health);
				send(update);
			}

			send_player_stats();

			// --- Check for Player Defeat ---
			if (player.stats.health <= 0) {
				player.stats.health = 0;
				send("SERVER:COMBAT_DEFEAT:You have been defeated!");
				player.isInCombat = false;

				// --- MODIFICATION ---
				respawn_monster_immediately(player.currentArea, current_monster_spawn_id);

				player.currentOpponent.reset();
				player.currentArea = "TOWN"; player.currentMonsters.clear();
				player.stats.health = player.stats.maxHealth / 2; player.stats.mana = player.stats.maxMana;
				player.posX = 26; player.posY = 12; player.currentPath.clear();
				broadcast_data.currentArea = "TOWN"; broadcast_data.posX = player.posX; broadcast_data.posY = player.posY;
				{ std::lock_guard<std::mutex> lock(g_player_registry_mutex); g_player_registry[player.userId] = broadcast_data; }

				send("SERVER:AREA_CHANGED:TOWN");
				send_area_map_data(player.currentArea);
				SyncPlayerMonsters(player); // Syncs to TOWN
				send_current_monsters_list();
				send_available_areas();
				send_player_stats();
				return;
			}

			send("SERVER:COMBAT_TURN:Your turn.");
		}
	}

	   else if (message.rfind("GIVE_XP:", 0) == 0) {
		if (!player.isFullyInitialized) { send("SERVER:ERROR:Complete character creation first."); }
		else if (player.isInCombat) { send("SERVER:ERROR:Cannot gain XP in combat."); }
		else {
			try {
				int xp_to_give = stoi(message.substr(8));
				if (xp_to_give > 0) {
					player.stats.experience += xp_to_give;
					send("SERVER:STATUS:Gained " + to_string(xp_to_give) + " XP.");
					check_for_level_up();
					send_player_stats();
				}
				else { send("SERVER:ERROR:Invalid XP amount."); }
			}
			catch (const exception&) { send("SERVER:ERROR:Invalid XP amount format."); }
		}
	}
	   else if (message == "REQUEST_PLAYERS") {

		if (g_area_grids.find(player.currentArea) == g_area_grids.end()) {
			send("SERVER:PLAYERS_IN_AREA:[]");
			return;
		}

		std::string my_area = player.currentArea;


		std::vector<PlayerBroadcastData> players_in_area;
		{
			std::lock_guard<std::mutex> lock(g_player_registry_mutex);
			for (auto const& pair : g_player_registry) {
				if (pair.first == player.userId) continue;
				if (pair.second.currentArea == my_area && pair.second.playerClass != PlayerClass::UNSELECTED) {
					players_in_area.push_back(pair.second); // Copy the data
				}
			}
		} //


		std::ostringstream oss;
		oss << "SERVER:PLAYERS_IN_AREA:[";
		bool first_player = true;

		for (const auto& data : players_in_area) {
			if (!first_player) oss << ",";
			oss << "{\"id\":" << nlohmann::json(data.userId).dump()
				<< ",\"name\":" << nlohmann::json(data.playerName).dump()
				<< ",\"class\":" << static_cast<int>(data.playerClass)
				<< ",\"x\":" << data.posX
				<< ",\"y\":" << data.posY
				<< ",\"action\":" << nlohmann::json(data.currentAction).dump()

				// --- FIX: Added Equipment Fields ---
				<< ",\"weaponItemId\":" << nlohmann::json(data.weaponItemId).dump()
				<< ",\"hatItemId\":" << nlohmann::json(data.hatItemId).dump()
				<< ",\"torsoItemId\":" << nlohmann::json(data.torsoItemId).dump()
				<< ",\"legsItemId\":" << nlohmann::json(data.legsItemId).dump()
				<< ",\"bootsItemId\":" << nlohmann::json(data.bootsItemId).dump()
				// --- End of Fix ---

				<< "}";
			first_player = false;
		}

		oss << "]";
		std::string player_list_message = oss.str();
		send(player_list_message);
	}
	   else if (message.rfind("USE_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Complete character creation first.");
		}
		else {
			try {
				uint64_t instanceId = stoull(message.substr(9));
				useItem(instanceId);
			}
			catch (const exception&) {
				send("SERVER:ERROR:Invalid item ID format.");
			}
		}
	}
	   else if (message.rfind("EQUIP_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Complete character creation first.");
		}
		else {
			try {
				uint64_t instanceId = stoull(message.substr(11));
				string equipMsg = equipItem(instanceId);
				send("SERVER:STATUS:" + equipMsg);
			}
			catch (const exception&) {
				send("SERVER:ERROR:Invalid item ID format.");
			}
		}
	}
	   else if (message.rfind("DROP_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Complete character creation first.");
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
				send("SERVER:ERROR:Invalid drop command format.");
			}
		}
	}
	   else if (message.rfind("UNEQUIP_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Complete character creation first.");
		}
		else {
			try {
				int slotInt = stoi(message.substr(13));
				EquipSlot slotToUnequip = static_cast<EquipSlot>(slotInt);

				string result = unequipItem(slotToUnequip);
				send("SERVER:STATUS:" + result);
			}
			catch (const exception& e) {
				send("SERVER:ERROR:Invalid slot format.");
			}
		}
	}
	   else if (message.rfind("BUY_ITEM:", 0) == 0) {
		if (!player.isFullyInitialized) {
			send("SERVER:ERROR:Complete character creation first.");
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
					send("SERVER:ERROR:Unknown shop.");
					return;
				}
				if (itemDatabase.count(itemId) == 0) {
					send("SERVER:ERROR:Unknown item.");
					return;
				}

				const auto& shopItems = g_shops.at(shopId);
				if (find(shopItems.begin(), shopItems.end(), itemId) == shopItems.end()) {
					send("SERVER:ERROR:This shop does not sell that item.");
					return;
				}

				const ItemDefinition& def = itemDatabase.at(itemId);

				int price = 1;
				try {
					price = g_item_buy_prices.at(itemId);
				}
				catch (const out_of_range&) {
					cerr << "WARNING: Player tried to buy " << itemId << " which has no price." << endl;
					send("SERVER:ERROR:That item is not for sale.");
					return;
				}

				if (player.stats.gold >= price) {
					player.stats.gold -= price;
					addItemToInventory(itemId, 1);
					send("SERVER:STATUS:Bought " + def.name + " for " + to_string(price) + " gold.");
					send_player_stats();

				}
				else {
					send("SERVER:ERROR:Not enough gold. You need " + to_string(price) + ".");
				}
			}
			catch (const exception&) {
				cerr << "Buy item error: " << endl;
				send("SERVER:ERROR:Invalid buy command format.");
			}
		}
	}
	   else {
		string echo = "SERVER:ECHO: " + message;
		send(echo);
	}
}
void check_party_timeouts() {
	std::vector<std::shared_ptr<Party>> parties_to_resolve;

	// 1. Identify expired rounds (Lock briefly to read)
	{
		std::lock_guard<std::mutex> lock(g_parties_mutex);
		auto now = std::chrono::steady_clock::now();

		for (auto const& [id, party] : g_parties) {
			if (party && party->activeCombat) {
				// Check if 20 seconds have passed
				auto duration = std::chrono::duration_cast<std::chrono::seconds>(
					now - party->activeCombat->roundStartTime
				).count();

				if (duration >= 20) {
					parties_to_resolve.push_back(party);
				}
			}
		}
	}

	// 2. Resolve them (Unlocked, as resolve_party_round handles logic/sending)
	for (auto& party : parties_to_resolve) {
		// Double check combat still exists (race condition safety)
		if (party->activeCombat) {
			// Optional: Log that it was a forced timeout
			// broadcast_to_party(party, "SERVER:COMBAT_LOG:Time limit reached! Round ending...");
			resolve_party_round(party);
		}
	}
}