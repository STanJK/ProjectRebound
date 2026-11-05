// LoadoutSync.cpp
#include "LoadoutSync.h"
#include "B5WeaponConfig.h"
#include "CharCosmetics.h"
#include "../Logging/LogManager.h"
#include "../Logging/FileLog.h"
#include "../Libs/json.hpp"
#include <Windows.h>
#include <winhttp.h>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>

#pragma comment(lib, "winhttp.lib")
using namespace SDK;

thread_local bool g_InNativeCall = false;

extern uintptr_t BaseAddress;

template<typename T> static auto&  _D(T& a) { return *reinterpret_cast<void**>(&a); }
template<typename T> static int32& _N(T& a)  { return *reinterpret_cast<int32*>((char*)&a+8); }

static constexpr int S = 6;
static const char* SK[S] = {"primaryWeapon","secondaryWeapon","leftPylon","rightPylon","meleeWeapon","mobilityModule"};

// ---------------------------------------------------------------------------
// Game functions shared by both injection passes. RVAs are imagebase-relative.
// Resolved lazily: BaseAddress is not set yet when static initialisers run.
// ---------------------------------------------------------------------------
typedef void   (__fastcall *FnBuildV43) (void* out, void* roleFName);
typedef int*   (__fastcall *FnFind272)  (void* tmap, int* outIdx, uint64_t fname);
typedef void   (__fastcall *FnBuildSrc) (void* out, void* weaponFName);
typedef void*  (__fastcall *FnFind192)  (void* tmap, int hash, void* keyFName);
typedef void*  (__fastcall *FnInsert192)(void* tmap, int hash, void* keyFName);

struct GameFns
{
    FnBuildV43  BldV43 = (FnBuildV43) (BaseAddress + 0x1544D00); // role -> v43 buffer
    FnFind272   F272   = (FnFind272)  (BaseAddress + 0x15AEB20); // 272B role entry lookup
    FnBuildSrc  BSrc   = (FnBuildSrc) (BaseAddress + 0x1654C80); // weapon -> DataTable buffer
    FnFind192   F192   = (FnFind192)  (BaseAddress + 0x164F740); // 192B weapon entry lookup
    FnInsert192 I192   = (FnInsert192)(BaseAddress + 0x164ED60); // 192B weapon entry insert
};
static GameFns& Fns() { static GameFns g; return g; }

// The hash the game uses for its 192-byte TMap: ComparisonIndex bits + Number.
static int HashFName(uint64_t raw)
{
    uint32_t ci = (uint32_t)(raw & 0xFFFFFFFF);
    uint32_t h  = (uint32_t)((uint16_t)ci << 16) + (uint32_t)(uint16_t)ci
                + 524289u * (ci >> 16) + (uint32_t)((uint16_t)ci >> 4);
    return (int)((uint32_t)(raw >> 32) + h); // mod 2^32, matching the original int math
}

static void RunPhase2Injection(APBPlayerState* PS, const std::vector<FName>& roleFNs) {
    int nA = (int)roleFNs.size();
    // Deliberately no one-shot guard: the 192B map lives on the PlayerState
    // (+0x700), so every joining player needs their own entries. Already-present
    // entries are skipped below, which makes this safe to run per player.
    if (nA <= 0) return;
    GameFns& g = Fns();

    unsigned char* psRaw = (unsigned char*)PS;
    void* tmap272 = psRaw + 0x700;

    std::unordered_set<unsigned __int64> seenRoles;
    int wcInserted = 0, wcSkipped = 0;

    for (int ri = 0; ri < nA; ++ri) {
        uint64_t fnRaw = *(uint64_t*)&roleFNs[ri];
        if (seenRoles.count(fnRaw))
            continue;
        seenRoles.insert(fnRaw);

        int idx = -1;
        g.F272(tmap272, &idx, fnRaw);
        if (idx == -1)
            continue;

        void* entryData = *(void**)tmap272;
        if (!entryData) continue;
        unsigned char* roleEntry = (unsigned char*)entryData + 272 * idx;
        void* tmap192 = roleEntry + 0xB8;

        unsigned char v43[0x100] = {};
        g.BldV43(v43, (void*)&roleFNs[ri]);
        void* wmData = *(void**)(v43 + 0xB0);
        int   wmNum  = *(int*)(v43 + 0xB8);

        if (!wmData || wmNum <= 0) continue;

        for (int wi = 0; wi < wmNum && wi < 10; ++wi) {
            void* wepKey = (unsigned char*)wmData + 192 * wi;
            uint64_t wepFN = *(uint64_t*)wepKey;
            if (wepFN == 0) continue;

            int hash = HashFName(wepFN);
            void* existing = g.F192(tmap192, hash, wepKey);
            if (existing) {
                wcSkipped++;
                continue;
            }

            unsigned char srcBuf[0xC0] = {};
            g.BSrc(srcBuf, wepKey);
            g.I192(tmap192, hash, wepKey);
            wcInserted++;
        }
    }
    LogF("[LSYNC] 192B: %d inserted, %d skipped", wcInserted, wcSkipped);
}

static const EPBCharacterSlotType SE[S] = {
    EPBCharacterSlotType::FirstWeapon, EPBCharacterSlotType::SecondWeapon,
    EPBCharacterSlotType::LeftPod, EPBCharacterSlotType::RightPod,
    EPBCharacterSlotType::MeleeWeapon, EPBCharacterSlotType::Mobility,
};

std::vector<LoadoutSync::PendingLoadout> LoadoutSync::g_Pending;
std::mutex LoadoutSync::g_Mutex;

static FName MkN(const std::string& s) {
    if (s.empty() || s == "None") return FName();
    std::wstring w(s.begin(), s.end());
    return UKismetStringLibrary::Conv_StringToName(w.c_str());
}

// Intern `s` and return its raw FName (0 for empty / "None").
static uint64_t RawOf(const std::string& s) {
    FName fn = MkN(s);
    return *(uint64_t*)&fn;
}

// Read a string field, treating null / absent / "None" as empty.
static std::string JsonStr(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return {};
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    std::string v = it->get<std::string>();
    if (v.empty() || v == "null" || v == "None") return {};
    return v;
}

static std::string HttpGet(const std::string& pid) {
    std::wstring p(L"/api/loadout/" + std::wstring(pid.begin(), pid.end()));
    HINTERNET hS=WinHttpOpen(L"B/1.0",0,0,0,0); if(!hS)return{};
    WinHttpSetTimeouts(hS,3000,3000,3000,3000);
    HINTERNET hC=WinHttpConnect(hS,L"127.0.0.1",8000,0); if(!hC){WinHttpCloseHandle(hS);return{};}
    HINTERNET hR=WinHttpOpenRequest(hC,L"GET",p.c_str(),0,0,0,0); if(!hR){WinHttpCloseHandle(hC);WinHttpCloseHandle(hS);return{};}
    std::string r; BOOL ok=WinHttpSendRequest(hR,0,0,0,0,0,0);
    if(ok)ok=WinHttpReceiveResponse(hR,0);
    if(ok){
        DWORD br;char b[4096];bool truncated=false;
        while(WinHttpReadData(hR,b,4095,&br)&&br>0){b[br]=0;r.append(b,br);if(r.size()>256*1024){truncated=true;break;}}
        // Don't let a cap silently turn into a parse failure with no trace.
        if(truncated)LogF("[LSYNC][W] loadout for %s exceeds 256 KB, response truncated",pid.c_str());
    }
    WinHttpCloseHandle(hR);WinHttpCloseHandle(hC);WinHttpCloseHandle(hS); return r;
}


// B.5 production: apply custom weapon part configs from JSON to C1/C2
// ornType selects which cosmetic system ornId belongs to: "PartOri" (or absent)
// is a painting theme, anything else (e.g. "SMGTigerMZL") is a part skin mesh.
struct PartCfg { int slot; std::string partId; std::string ornId; std::string ornType; };
struct WeaponCfg { std::string weaponId; std::vector<PartCfg> parts; std::string ornId; std::string ornType; };
struct RoleCfg { std::vector<WeaponCfg> weapons; };

// ---------------------------------------------------------------------------
// Weapon suit expansion (receiver / "main body" cosmetic)
//
// The weapon-level ornament is {Type, Id} = {WeaponSuit, WeaponSuitePainting},
// e.g. {SkinSMGTiger, SkinSMGTiger_PTTiger} — the same skin/paint split as a
// part ornament, but at weapon scope.  A suit has NO network field of its own:
// FPBWeaponNetworkConfig (0x38) only carries per-part configs, and OrnamentID
// (+0x20) is the WeaponOrnament *badge* slot (its native default is WO-NONE,
// an EPBItemType::WeaponOrnament).  Writing the suit painting there is why the
// body never rendered — wrong item family, no consumer.
//
// The suit row in WeaponSuitDefinitionDataTable (@0x2B0) maps every slot to a
// part skin (FPBWeaponSuiteDefinitionRow::PartSlotAndSkinIDMap @0x128), e.g.
// SMGTigerMZL / SMGTigerGRP / SMGTigerRCVR.  Expanding those into the per-part
// WeaponPartSkinID (+0x08) reuses the path that already renders the Tiger
// muzzle/grip, and it replicates like any other part config.
//
// Only slots whose loadout ornament is the bare default (PTOriginal / absent)
// are filled, so explicit choices (ElectroRed barrel/sight/mag) keep winning.
// ---------------------------------------------------------------------------
static void ApplySuitExpansion(const std::string& sid, uint64_t wepFN,
                               const WeaponCfg& wc,
                               unsigned char* c1Data, int c1Num,
                               unsigned char* c2Data, int c2Num)
{
    auto* mgr = static_cast<UPBDataTableManager*>(
        USubsystemBlueprintLibrary::GetEngineSubsystem(
            TSubclassOf<UEngineSubsystem>(UPBDataTableManager::StaticClass())));
    UDataTable* dt = mgr ? mgr->WeaponSuitDefinitionDataTable.Get() : nullptr;
    if (!dt) { WriteLog("[SUIT] WeaponSuitDefinitionDataTable unavailable"); return; }

    FName suitFn = MkN(wc.ornType);
    FPBWeaponSuiteDefinitionRow* row = nullptr;
    int rowNum = dt->RowMap.Num();
    for (int i = 0; i < rowNum; ++i) {
        auto& pr = dt->RowMap[i];
        if (pr.Key() == suitFn) { row = (FPBWeaponSuiteDefinitionRow*)pr.Value(); break; }
    }
    if (!row) {
        LogF("[SUIT] %s -> NO ROW in WeaponSuitDefinitionDataTable (%d rows)",
             wc.ornType.c_str(), rowNum);
        return;
    }

    // slot -> C1 key, from the C2 entry list (same pairing the parts loop uses).
    std::unordered_map<int, uint64_t> slotKey;
    for (int e = 0; e < c2Num; ++e) {
        unsigned char* p = (unsigned char*)c2Data + 20 * e;
        slotKey[(int)*p] = *(uint64_t*)(p + 4);
    }

    // The loadout's own choice wins unless it is the bare default (PTOriginal /
    // absent). A slot the loadout never mentions at all (e.g. the receiver, slot
    // 12) counts as default too.
    auto isDefaultSlot = [&](int slot) -> bool {
        const PartCfg* pc = nullptr;
        for (auto& c : wc.parts) if (c.slot == slot) { pc = &c; break; }
        return !pc || (pc->ornType.empty() &&
                       (pc->ornId.empty() || pc->ornId == "PTOriginal"));
    };

    int applied = 0;
    int n = row->PartSlotAndSkinIDMap.Num();
    for (int i = 0; i < n; ++i) {
        auto& pair = row->PartSlotAndSkinIDMap[i];
        int slot = (int)pair.Key();
        FName skin = pair.Value();
        std::string skinStr = skin.ToString();

        if (!isDefaultSlot(slot)) continue;
        auto kit = slotKey.find(slot);
        if (kit == slotKey.end() || !kit->second) continue;
        if (skinStr.empty() || skinStr == "None") continue;

        B5WeaponConfig::CachePartOverride(sid, wepFN, kit->second, *(uint64_t*)&skin, 0);
        ++applied;
    }
    LogF("[SUIT] %s skins applied=%d/%d", wc.ornType.c_str(), applied, n);

    // -----------------------------------------------------------------------
    // Suit painting (the other half of the pair).
    //
    // A suit can change the MESH per slot (skins above) or just the COLOUR.
    // "Original" suits — SkinFOPOriginal, SkinAMROriginal, … — map their slots
    // to the unchanged part skin "PartOri", and the colour arrives from the
    // suit PAINTING instead: WeaponSuitPaintingDefinitionDataTable (@0x2D8),
    // row key = wc.ornId, map SlotAndWeaponPartSkinPaintingIDMap @0xD8 gives a
    // slot painting id per slot (e.g. GSW-FOP_ReceiverMain_PTElectroRed).
    // That goes to the same +0x18 the working per-part painting uses.
    // -----------------------------------------------------------------------
    if (!wc.ornId.empty()) {
        UDataTable* pdt = mgr->WeaponSuitPaintingDefinitionDataTable.Get();
        if (!pdt) { WriteLog("[SUIT] WeaponSuitPaintingDefinitionDataTable unavailable"); return; }

        FName paintFn = MkN(wc.ornId);
        FPBWeaponSuitePaintingDefinitionRow* prow = nullptr;
        int pNum = pdt->RowMap.Num();
        for (int i = 0; i < pNum; ++i) {
            auto& pr = pdt->RowMap[i];
            if (pr.Key() == paintFn) { prow = (FPBWeaponSuitePaintingDefinitionRow*)pr.Value(); break; }
        }
        if (!prow) {
            LogF("[SUIT] %s -> NO ROW in WeaponSuitPaintingDefinitionDataTable (%d rows)",
                 wc.ornId.c_str(), pNum);
            return;
        }

        int pApplied = 0;
        int pn = prow->SlotAndWeaponPartSkinPaintingIDMap.Num();
        for (int i = 0; i < pn; ++i) {
            auto& pair = prow->SlotAndWeaponPartSkinPaintingIDMap[i];
            int slot = (int)pair.Key();
            FName paint = pair.Value();
            std::string paintStr = paint.ToString();

            if (!isDefaultSlot(slot)) continue;
            auto kit = slotKey.find(slot);
            if (kit == slotKey.end() || !kit->second) continue;
            if (paintStr.empty() || paintStr == "None") continue;

            // Mirror the per-part painting path: C1 input at +8, output at +0x18.
            for (int ce = 0; ce < c1Num; ++ce) {
                unsigned char* cp = c1Data + 48 * ce;
                if (*(uint64_t*)(cp + 0) == kit->second) {
                    *(uint64_t*)(cp + 8) = *(uint64_t*)&paint; break;
                }
            }
            B5WeaponConfig::CachePartOverride(sid, wepFN, kit->second, 0, *(uint64_t*)&paint);
            ++pApplied;
        }
        LogF("[SUIT] %s paint applied=%d/%d", wc.ornId.c_str(), pApplied, pn);
    }
}

static void ApplyCustomWeaponConfigs(APBPlayerState* PS, const std::vector<FName>& roleFNs,
                                      const std::unordered_map<uint64_t, RoleCfg>& roleCfgMap) {
    int nA = (int)roleFNs.size();
    std::string sid = PS->GetDefaultIDStr().ToString(); // per-player cache key
    GameFns& g = Fns();

    unsigned char* psRaw = (unsigned char*)PS;
    void* tmap272 = psRaw + 0x700;

    for (int ri = 0; ri < nA; ++ri) {
        uint64_t fnRaw = *(uint64_t*)&roleFNs[ri];
        auto cit = roleCfgMap.find(fnRaw);
        if (cit == roleCfgMap.end()) continue;
        auto& rc = cit->second;

        // Find 272B entry
        int idx = -1;
        g.F272(tmap272, &idx, fnRaw);
        if (idx < 0) continue;
        void* ed = *(void**)tmap272;
        if (!ed) continue;
        unsigned char* re = (unsigned char*)ed + 272 * idx;

        // Build v43 to get WeaponMap
        unsigned char v43[0x100] = {};
        g.BldV43(v43, (void*)&roleFNs[ri]);
        void* wmData = *(void**)(v43 + 0xB0);
        int   wmNum  = *(int*)(v43 + 0xB8);
        if (!wmData || wmNum <= 0) continue;

        // Weapons are matched by FName ComparisonIndex, so intern each config
        // weapon's FName once per role instead of once per world weapon.
        std::vector<std::pair<uint64_t, const WeaponCfg*>> cfgKeys;
        cfgKeys.reserve(rc.weapons.size());
        for (auto& wc : rc.weapons)
            cfgKeys.emplace_back(RawOf(wc.weaponId), &wc);

        for (int wi = 0; wi < wmNum; ++wi) {
            unsigned char* wepEntry = (unsigned char*)wmData + 192 * wi;
            uint64_t wepFN = *(uint64_t*)wepEntry;
            if (!wepFN) continue;

            for (auto& [wkeyRaw, pwc] : cfgKeys) {
                if ((wepFN & 0xFFFFFFFF) != (wkeyRaw & 0xFFFFFFFF)) continue;
                const WeaponCfg& wc = *pwc;

                int hash = HashFName(wepFN);
                void* existing = g.F192(re + 0xB8, hash, (void*)&wepFN);
                if (!existing) continue;
                unsigned char* val = (unsigned char*)existing;

                void* c2Data = *(void**)(val + 0x50); int c2Num = *(int*)(val + 0x58);
                void* c1Data = *(void**)val;           int c1Num = *(int*)(val + 8);
                if (!c2Data || !c1Data) continue;

                for (auto& pc : wc.parts) {
                    uint64_t c1Key = 0;
                    for (int e = 0; e < c2Num; ++e) {
                        unsigned char* p = (unsigned char*)c2Data + 20 * e;
                        if (*p == (uint8_t)pc.slot) { c1Key = *(uint64_t*)(p + 4); break; }
                    }
                    if (!c1Key) continue;

                    // B.5: cache the attachment (part id) — replaces the default part id.
                    if (!pc.partId.empty()) {
                        FName partFn = MkN(pc.partId);
                        B5WeaponConfig::CachePartAttachment(sid, wepFN, c1Key, *(uint64_t*)&partFn);
                    }

                    // B.5: cache the ornament — routed by its `type` (see Docs/Loadout-Persistence.md).
                    //   PartOri / absent -> painting:  C1 input + output WeaponPartSkinPaintingID (+0x18), value = id
                    //   anything else    -> part skin: output WeaponPartSkinID (+0x08), value = TYPE
                    if (!pc.ornId.empty()) {
                        FName ornFn = MkN(pc.ornId);
                        const bool isPainting = pc.ornType.empty() || pc.ornType == "PartOri";
                        if (isPainting) {
                            for (int ce = 0; ce < c1Num; ++ce) {
                                unsigned char* cp = (unsigned char*)c1Data + 48 * ce;
                                if (*(uint64_t*)(cp + 0) == c1Key) {
                                    *(uint64_t*)(cp + 8) = *(uint64_t*)&ornFn; break;
                                }
                            }
                            B5WeaponConfig::CachePartOverride(sid, wepFN, c1Key, 0, *(uint64_t*)&ornFn);
                        } else {
                            // The ornament `type` is the part-skin token: DT_ItemType.json
                            // classifies SMGTigerMZL / SMGTigerGRP (and PartOri) as
                            // EPBItemType::WeaponPartSkin — exactly the value WeaponPartSkinID
                            // expects. The `id` (e.g. GSW-SMG_PTTiger) has no DT_ItemType entry
                            // at all, so writing it there can never resolve; that was the bug
                            // the [SKIN-DT] "NOT IN ANY SKIN TABLE" log line exposed.
                            // Leave the C1 default ("PartOri") in the input untouched.
                            FName skinFn = MkN(pc.ornType);
                            B5WeaponConfig::CachePartOverride(sid, wepFN, c1Key, *(uint64_t*)&skinFn, 0);
                        }
                    }
                }

                // Weapon suit (receiver/body) — expand {WeaponSuit, painting} into
                // per-part skins. `WO-*` types are the badge slot, not a suit.
                if (!wc.ornType.empty() && wc.ornType.rfind("WO-", 0) != 0)
                    ApplySuitExpansion(sid, wepFN, wc, (unsigned char*)c1Data, c1Num,
                                       (unsigned char*)c2Data, c2Num);

                break;
            }
        }
    }
}

static bool ParseAndPush(const std::string& json, APBPlayerState* PS) {
    std::string sid = PS->GetDefaultIDStr().ToString();
    OutputDebugStringA(("[LSYNC] ParseAndPush " + sid + " " + std::to_string(json.size()) + "B\n").c_str());

    nlohmann::json root;
    try { root = nlohmann::json::parse(json); }
    catch (...) { WriteLog("[LSYNC] JSON FAIL"); return false; }
    auto& roles = root["roles"];

    // Parse JSON roles into a lookup map (raw FName → 6 items)
    std::unordered_map<uint64_t, std::vector<FName>> roleMap;
    std::unordered_map<uint64_t, RoleCfg> roleCfgMap; // extracted C++ configs, no JSON on game thread

    for (auto& [rid, rd] : roles.items()) {
        if (!rd.is_object()) continue;
        FName rf = MkN(rid);
        std::vector<FName> items(S);
        for (int s=0;s<S;++s) {
            std::string item="None";
            if (rd.contains(SK[s])) { auto& v=rd[SK[s]]; if(v.is_string())item=v.get<std::string>(); }
            if (item.empty()||item=="null"||item=="None") item="None";
            items[s] = (item!="None"?MkN(item):FName());
        }
        uint64_t raw = *(uint64_t*)&rf;
        roleMap[raw] = std::move(items);

        // Character cosmetics for this role (operator skin, head ornament, arm
        // badge, suit paint). Independent of _weaponConfigs — every role has them.
        // Mapping: _skinToken -> EPBSkinClass::Skin, cosmetic10 -> Ornaments,
        // cosmetic9 -> SpecialSlot, _ornamentId -> the SkinPaintingID scalar.
        // See Docs/Loadout-Persistence.md.
        {
            FCharCosmeticCfg cc;
            cc.SkinFName  = RawOf(JsonStr(rd, "_skinToken"));
            cc.PaintFName = RawOf(JsonStr(rd, "_ornamentId"));
            cc.BadgeFName = RawOf(JsonStr(rd, "cosmetic9"));  // ArmBadge
            cc.HeadFName  = RawOf(JsonStr(rd, "cosmetic10")); // HeadAccessory
            CharCosmetics::CacheRoleCosmetics(sid, raw, cc);
        }

        // Extract _weaponConfigs — safe, no operator[], no value(), handles null
        {
            auto& jd = rd;
            auto it = jd.find("_weaponConfigs");
            if (it != jd.end() && it->is_object()) {
                RoleCfg rc;
                for (auto& wp : it->items()) {
                    const auto& wv = wp.value();
                    if (!wv.is_object()) continue;
                    WeaponCfg wc;
                    wc.weaponId = wp.key();
                    // Weapon ornament: {Type, Id} = {WeaponSuit, WeaponSuitePainting}
                    // (e.g. SkinSMGTiger / SkinSMGTiger_PTTiger) or a WO-* badge.
                    auto oit = wv.find("ornament");
                    if (oit != wv.end() && oit->is_object()) {
                        auto idit = oit->find("id");
                        if (idit != oit->end() && idit->is_string())
                            wc.ornId = idit->get<std::string>();
                        auto tyit = oit->find("type");
                        if (tyit != oit->end() && tyit->is_string())
                            wc.ornType = tyit->get<std::string>();
                    }
                    // Parts
                    auto pit = wv.find("parts");
                    if (pit != wv.end() && pit->is_array()) {
                        for (auto& pn : *pit) {
                            if (!pn.is_object()) continue;
                            PartCfg pc;
                            auto sit = pn.find("slot");
                            if (sit != pn.end() && sit->is_number()) pc.slot = sit->get<int>();
                            auto piit = pn.find("partId");
                            if (piit != pn.end() && piit->is_string()) pc.partId = piit->get<std::string>();
                            auto poit = pn.find("ornament");
                            if (poit != pn.end() && poit->is_object()) {
                                auto poid = poit->find("id");
                                if (poid != poit->end() && poid->is_string())
                                    pc.ornId = poid->get<std::string>();
                                // `type` is often null (PTOriginal) — treat null as "painting".
                                auto poty = poit->find("type");
                                if (poty != poit->end() && poty->is_string())
                                    pc.ornType = poty->get<std::string>();
                            }
                            if (pc.slot >= 0 && !pc.partId.empty())
                                wc.parts.push_back(std::move(pc));
                        }
                    }
                    if (!wc.weaponId.empty())
                        rc.weapons.push_back(std::move(wc));
                }
                roleCfgMap[raw] = std::move(rc);
            }
        }
    }

    // Sort by canonical authoritative order (hardcoded from runtime probe: sub_14171CE40 comparison)
    // Order: 0x3D709C, 0x3D70A0, 0x108258, 0x3D70A4, 0x3D70A7, 0x3D70AB
    static const uint32_t AUTH_ORDER[] = {0x3D709C, 0x3D70A0, 0x108258, 0x3D70A4, 0x3D70A7, 0x3D70AB};
    std::vector<FName> roleFNs;
    std::vector<FName> allItems;
    for (uint32_t ci : AUTH_ORDER) {
        uint64_t raw = ci; // Num=0 for all known roles
        auto it = roleMap.find(raw);
        if (it != roleMap.end()) {
            roleFNs.push_back(*(FName*)&raw);
            for (int s = 0; s < S; ++s) allItems.push_back(it->second[s]);
            roleMap.erase(it);
        } else {
            roleFNs.push_back(*(FName*)&raw);
            for (int s = 0; s < S; ++s) allItems.push_back(FName());
        }
    }
    // Append any remaining JSON roles not in canonical list
    for (auto& [raw, items] : roleMap) {
        roleFNs.push_back(*(FName*)&raw);
        for (int s = 0; s < S; ++s) allItems.push_back(items[s]);
    }
    int nA=(int)roleFNs.size();
    LogF("[LSYNC] accumulated %d roles %zu items (authoritative order)", nA, allItems.size());

    // standalone arrays
    TArray<FName> rIDs, cfgRA; TArray<int32> quotas;
    for(int i=0;i<nA;++i){rIDs.Add(roleFNs[i]);quotas.Add(8);cfgRA.Add(roleFNs[i]);}

    // config
    FPBFieldModRoleGameSavedNetworkConfig config;
    config.RoleArray = cfgRA;
    auto& outer = config.RoleInventoryNetworkConfigArray;
    outer.Reserve(nA);

    int ii=0;
    for(int i=0;i<nA;++i) {
        FPBInventoryNetworkConfig* inv = (FPBInventoryNetworkConfig*)((char*)_D(outer)+_N(outer)*32);
        memset(inv,0,32);
        _N(outer)++;
        for(int s=0;s<S;++s) inv->CharacterSlots.Add(SE[s]);
        for(int s=0;s<S;++s) inv->InventoryItems.Add(allItems[ii++]);
    }

    // Send bulk init first (roles + quotas)
    PS->ClientInitFieldMod(config, rIDs, quotas);

    // Then per-role refresh — may write to UPBFieldModManager
    int itemOff = 0;
    for (int i = 0; i < nA; ++i) {
        FPBInventoryNetworkConfig perRoleCfg;
        for (int s = 0; s < S; ++s) {
            perRoleCfg.CharacterSlots.Add(SE[s]);
            perRoleCfg.InventoryItems.Add(allItems[itemOff++]);
        }
        PS->ClientRefreshRolePreOrderingInventory(roleFNs[i], perRoleCfg);
    }

    // Ensure 192B entries exist (skip-existing + insert)
    RunPhase2Injection(PS, roleFNs);
    g_InNativeCall = true;
    ApplyCustomWeaponConfigs(PS, roleFNs, roleCfgMap);
    g_InNativeCall = false;

    // B.5: reapply cached configs to weapons that spawn after cache population
    B5WeaponConfig::ReapplyToExistingWeapons();

    // B.5: same for character cosmetics (operator skin / head ornament / badge)
    CharCosmetics::ReapplyToExistingCharacters();

    return true;
}

static bool IsSID(const std::string& s) {
    if (s.length() != 17) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// Resolve a live PlayerState from a Steam ID. Called on the game thread when a
// queued loadout is drained: the controller captured at PostLogin cannot be
// reused, because the HTTP fetch runs on a detached thread and the player may be
// long gone (a raw actor pointer would dangle — UE does not null it).
static APBPlayerState* FindPlayerStateById(const std::string& sid)
{
    UWorld* world = UWorld::GetWorld();
    if (!world) return nullptr;

    for (int li = 0; li < world->Levels.Num(); ++li)
    {
        ULevel* level = world->Levels[li];
        if (!level) continue;

        for (int ai = 0; ai < level->Actors.Num(); ++ai)
        {
            AActor* actor = level->Actors[ai];
            if (!actor || !actor->IsA(APBPlayerController::StaticClass()))
                continue;

            APBPlayerState* ps = static_cast<APBPlayerState*>(
                static_cast<APBPlayerController*>(actor)->PlayerState);
            if (ps && ps->GetDefaultIDStr().ToString() == sid)
                return ps;
        }
    }
    return nullptr;
}

void LoadoutSync::OnPlayerJoined(AGameMode*, APBPlayerController* PC){
    if(!PC||!PC->PlayerState)return;
    auto*PS=static_cast<APBPlayerState*>(PC->PlayerState);
    std::string sid=PS->GetDefaultIDStr().ToString();
    LogF("[LSYNC] OnPlayerJoined %s", sid.c_str());
    if(!IsSID(sid)){WriteLog("[LSYNC] SKIP not SteamID");return;}
    std::thread([sid](){
        // Retry: the metaserver may be restarting exactly when someone joins, and
        // dropping the loadout costs that player the whole match for a blip.
        std::string json;
        for (int attempt = 1; attempt <= 3 && json.empty(); ++attempt)
        {
            if (attempt > 1) Sleep(2000);
            LogF("[LSYNC][W] HTTP %s (attempt %d/3)", sid.c_str(), attempt);
            json = HttpGet(sid);
        }
        if(json.empty()){LogF("[LSYNC][W] HTTP failed after 3 attempts for %s", sid.c_str());return;}
        LogF("[LSYNC][W] queued %zuB", json.size());
        std::lock_guard<std::mutex> lk(LoadoutSync::g_Mutex);
        LoadoutSync::g_Pending.push_back({sid,std::move(json)});
    }).detach();
}

// Every Steam ID with a live PlayerController right now.
static void CollectLivePlayerIds(std::unordered_set<std::string>& out)
{
    UWorld* world = UWorld::GetWorld();
    if (!world) return;

    for (int li = 0; li < world->Levels.Num(); ++li)
    {
        ULevel* level = world->Levels[li];
        if (!level) continue;

        for (int ai = 0; ai < level->Actors.Num(); ++ai)
        {
            AActor* actor = level->Actors[ai];
            if (!actor || !actor->IsA(APBPlayerController::StaticClass()))
                continue;

            APBPlayerState* ps = static_cast<APBPlayerState*>(
                static_cast<APBPlayerController*>(actor)->PlayerState);
            if (ps) out.insert(ps->GetDefaultIDStr().ToString());
        }
    }
}

void LoadoutSync::PruneStaleCaches(){
    // The per-player caches are keyed by Steam ID and nothing ever removed an
    // entry, so a server left up for days kept one set per player who had ever
    // joined. Sweep every ~2 s; an entry must be missing twice in a row before it
    // is dropped, because a player caught mid-connect has no PlayerController
    // actor yet and losing their cache would leave their cosmetics unapplied.
    static int s_Ticks = 0;
    if (++s_Ticks % 120 != 0) return;

    std::unordered_set<std::string> live;
    CollectLivePlayerIds(live);

    std::unordered_set<std::string> ids;
    std::vector<std::string> buf;
    B5WeaponConfig::CachedPlayerIds(buf);
    CharCosmetics::CachedPlayerIds(buf);
    for (auto& id : buf) ids.insert(std::move(id));

    static std::unordered_set<std::string> s_MissingLastSweep;
    std::unordered_set<std::string> missingNow;

    for (const std::string& id : ids)
    {
        if (live.count(id)) continue;
        missingNow.insert(id);
        if (s_MissingLastSweep.count(id))
        {
            B5WeaponConfig::PrunePlayer(id);
            CharCosmetics::PrunePlayer(id);
            LogF("[LSYNC] pruned caches for departed player %s", id.c_str());
        }
    }
    s_MissingLastSweep.swap(missingNow);
}

void LoadoutSync::DrainPending(){
    if(LoadoutSync::g_Pending.empty())return;
    std::vector<LoadoutSync::PendingLoadout> batch;
    {std::lock_guard<std::mutex> lk(LoadoutSync::g_Mutex);batch.swap(LoadoutSync::g_Pending);}
    LogF("[LSYNC] DrainPending %zu entries", batch.size());
    for(auto&p:batch){
        APBPlayerState* PS = FindPlayerStateById(p.playerId);
        if(!PS){
            LogF("[LSYNC] %s left before the loadout arrived; dropped", p.playerId.c_str());
            continue;
        }
        ParseAndPush(p.json,PS);
    }
}
