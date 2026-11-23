#pragma once

#include "common.hpp"

namespace bridge {

// Text of an engine CPersistentName (the name object of fleets, armies, ...), rendered by the
// engine's CPersistentName::BuildString.
std::string PersistentNameText(const void* persistent_name);

// TPdxNullObject-style references: whether the pointee is a real object (its vtable slot 1).
bool IsRealObject(void* obj);

// NHabitability::CalcHabitability(species, planet's colony carrier, country): 0..1, with the
// country's modifiers (what the expansion planner shows). -1 when the call fails.
double Habitability(uintptr_t base, const void* species, const void* planet, const void* country);

// PdxLocalize with one named parameter ("$NAME$" / "$NAME|fmt$" in the text); falls back to the
// plain localized key.
std::string LocalizeWithParam(uintptr_t base, const std::string& key, const char* param, const std::string& value);

}  // namespace bridge

namespace bridge::fleets {

// EShipClass (CFleet::ship_class), from GetShipClassTokenFromEnum.
enum class ShipClass : uint8_t {
    Military = 0,
    Constructor = 1,
    Colonizer = 2,
    ScienceShip = 3,
    Transport = 4,
    MiningStation = 5,
    ResearchStation = 6,
    MilitaryStation = 7,
    HabitatStation = 8,
    ObservationStation = 9,
    Starbase = 10,
    MilitarySpecial = 11,
    GravitySnare = 12,
    EntropyConduit = 13,
    Unknown = 0xFF,
};

// Script key of a ship class ("shipclass_military", ...).
const char* ShipClassKey(ShipClass c);
bool IsCivilianShip(ShipClass c);  // constructor, colonizer, science ship, transport
bool IsStation(ShipClass c);       // starbases and orbital stations

// The CFleet with this id from TPdxRef<CFleet>::_pDatabase, or nullptr (id checked, generation included).
void* Find(uintptr_t base, uint32_t fleet_id);

// Fleets the country owns (CCountryFleetsManager::owned_fleets).
std::vector<uint32_t> Owned(void* country);

// owned_fleets 的完整条目：这条数组里**也包含借入/借出的舰队**，靠 ownership_status / debtor /
// previous_owner 区分（2026-11-21 加，给「借来的舰队 / 借走的舰队」用）。
struct OwnedFleetEntry {
    uint32_t fleet_id{ 0xFFFFFFFF };
    uint32_t ownership_status{ 0 };
    int32_t lease_period{ 0 };
    uint32_t debtor{ 0xFFFFFFFF };
    uint32_t previous_owner{ 0xFFFFFFFF };
};
std::vector<OwnedFleetEntry> OwnedEntries(void* country);

// 舰队库里的**全部**舰队 id（用于找「控制权在本国、owner 却是别人」的借入舰队——
// 那种舰队不会出现在借入国的 owned_fleets 里，模板驱动的那份列表也看不到它）。
std::vector<uint32_t> AllIds(uintptr_t base);

// 舰队对象上的两个国家字段：owner 与 controller（借出期间 owner 不变、controller 换成借用国）。
uint32_t Owner(void* fleet);
uint32_t Controller(void* fleet);

ShipClass ClassOf(void* fleet);
double MilitaryPower(void* fleet);   // CFleet::military_power, normalized
uint32_t TemplateId(void* fleet);    // CFleet::fleet_template, 0xFFFFFFFF if none
std::string Name(void* fleet);       // CFleet::GetLocalizedName (engine CPersistentName::BuildString)
std::string OrdersText(void* fleet); // current orders as the outliner shows them (CFleet::BuildOrdersString)

}  // namespace bridge::fleets
