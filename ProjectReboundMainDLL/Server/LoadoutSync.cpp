// LoadoutSync.cpp
#include "LoadoutSync.h"
#include "B5WeaponConfig.h"
#include "CharCosmetics.h"
#include "../Logging/LogManager.h"
#include "../Logging/FileLog.h"
#include "../Libs/json.hpp"
#include "../Libs/safetyhook.hpp"
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

#define ODS(msg) WriteLog(msg)

template<typename T> static auto&  _D(T& a) { return *reinterpret_cast<void**>(&a); }
template<typename T> static int32& _N(T& a)  { return *reinterpret_cast<int32*>((char*)&a+8); }
template<typename T> static int32& _M(T& a)  { return *reinterpret_cast<int32*>((char*)&a+12); }

static constexpr int S = 6;
static const char* SK[S] = {"primaryWeapon","secondaryWeapon","leftPylon","rightPylon","meleeWeapon","mobilityModule"};

// === injection hooks ===
static SafetyHookInline g_HkPathSel;       // sub_14171CE40: char (void* mgr)
static SafetyHookInline g_HkConverter;     // sub_141546780: void* (void* out, void* wid, void* data)
static SafetyHookInline g_HkC1Find;        // sub_14166D8B0: __int64 (__int64* tmap, __int64* key)
static SafetyHookInline g_HkAcc0;          // sub_141665DF0: void* (__int64 a1, _BYTE* sel)
static SafetyHookInline g_HkAcc3;          // sub_1408A72D0: __int64 (__int64 a1)
static SafetyHookInline g_HkGetWeaponConfigCore; // sub_141716E50: production getter hook
static SafetyHookInline g_HkSpawnWeaponCfg;      // sub_1417316B0: spawn-time weapon config builder
thread_local bool g_ForceSimplePath = false;   // force selector → FALSE for Simple path
thread_local bool g_InConverter = false;       // set during converter execution for child hooks
thread_local int  g_ConvCallDepth = 0;         // nest depth for converter calls
struct FNameRaw { uint32_t CI; uint32_t Num; uint64_t Raw() { return (uint64_t)CI | ((uint64_t)Num << 32); } };
struct TArrayRaw { void* Data; int32_t Num; int32_t Max; };
struct FPBWeaponNetworkConfigRaw {
    TArrayRaw WeaponPartSlotTypeArray;
    TArrayRaw WeaponPartConfigs;
    FNameRaw  OrnamentID;
    FNameRaw  WeaponID;
    FNameRaw  WeaponClassID;
};
static char HkPathSelector(void* mgr) {
    if (g_ForceSimplePath) return 0;
    return g_HkPathSel.call<char>(mgr);
}

static void* HkConverter(void* output, void* weaponID, void* entryData) {
    g_InConverter = true;
    g_ConvCallDepth++;
    void* ret = g_HkConverter.call<void*>(output, weaponID, entryData);
    g_ConvCallDepth--;
    if (g_ConvCallDepth == 0) g_InConverter = false;
    return ret;
}

static __int64 HkC1Find(__int64* tmap, __int64* key) {
    __int64 ret = g_HkC1Find.call<__int64>(tmap, key);
#if 0 // verbose C1 diagnostic — disabled to reduce spam
    if (g_InConverter) {
        char buf[256];
        sprintf_s(buf, "[C1-FIND] tmap=%p key=%08X/%08X ret=%p",
            tmap, (uint32_t)*key, (uint32_t)(*key>>32), (void*)ret); ODS(buf);
        if (ret && ret != 8) {
            unsigned char* v = (unsigned char*)ret;
            sprintf_s(buf, "[C1-FIND]   entry: key=%08X/%08X f0=%08X/%08X f1=%08X/%08X f2=%08X/%08X f3=%08X/%08X",
                *(uint32_t*)(v-8), *(uint32_t*)(v-4), *(uint32_t*)(v+0),*(uint32_t*)(v+4),
                *(uint32_t*)(v+8),*(uint32_t*)(v+12), *(uint32_t*)(v+16),*(uint32_t*)(v+20),
                *(uint32_t*)(v+24),*(uint32_t*)(v+28)); ODS(buf);
        }
    }
#endif
    return ret;
}

static void* HkAcc0(void* a1, uint8_t* sel) {
    void* ret = g_HkAcc0.call<void*>(a1, sel);
#if 0 // verbose ACC0 diagnostic — disabled to reduce spam
    if (g_InConverter) {
        char buf[128];
        sprintf_s(buf, "[ACC0] a1=%p sel=%d ret=%p -> %08X/%08X",
            a1, *sel, ret, ret ? *(uint32_t*)ret : 0, ret ? *((uint32_t*)ret+1) : 0); ODS(buf);
    }
#endif
    return ret;
}

static __int64 HkAcc3(__int64 a1) {
    __int64 ret = g_HkAcc3.call<__int64>(a1);
#if 0 // verbose ACC3 diagnostic — disabled to reduce spam
    if (g_InConverter) {
        char buf[128];
        uint64_t deref = ret ? *(uint64_t*)ret : 0;
        sprintf_s(buf, "[ACC3] a1=%p ret=%p *ret=%08X/%08X",
            (void*)a1, (void*)ret, (uint32_t)deref, (uint32_t)(deref>>32)); ODS(buf);
    }
#endif
    return ret;
}

static __int64 HkGetWeaponConfigCore(void* mgr, void* out, __int64 roleID, __int64 weaponID);
static void* HkSpawnWeaponCfgFn(void* a1, __int64 a2, __int64 a3);

static int s_SpawnCallCount = 0;
static void* HkSpawnWeaponCfgFn(void* a1, __int64 a2, __int64 a3) {
    s_SpawnCallCount++;
    void* ret = g_HkSpawnWeaponCfg.call<void*>(a1, a2, a3);
    if (s_SpawnCallCount <= 5 && ret) {
        char buf[256];
        // Dump returned object — is it a Weapon Actor? Get its class name
        SDK::UObject* obj = (SDK::UObject*)ret;
        uint32_t widCI = (uint32_t)(a2 & 0xFFFFFFFF);
        uint32_t wid2CI = (uint32_t)(a3 & 0xFFFFFFFF);
        sprintf_s(buf,"[SPAWN-RET] call#%d a1=%p ret=%p class=%s weapon1=%08X weapon2=%08X",
            s_SpawnCallCount, a1, ret, obj->Class ? obj->Class->GetName().c_str() : "?",
            widCI, wid2CI); ODS(buf);
        // If ret is a PBWeapon, dump +0x03E8
        if (obj->IsA(SDK::APBWeapon::StaticClass())) {
            unsigned char* wpn = (unsigned char*)ret;
            void* slotData = *(void**)(wpn + 0x3E8);
            int   slotNum  = *(int*)(wpn + 0x3F0);
            void* partData = *(void**)(wpn + 0x3F8);
            int   partNum  = *(int*)(wpn + 0x400);
            sprintf_s(buf,"[SPAWN-WPN] +0x3E8 slots=%d parts=%d Data=%p", slotNum, partNum, partData); ODS(buf);
            for (int s=0; s<partNum&&s<3; ++s) {
                unsigned char* p = (unsigned char*)partData + 32*s;
                sprintf_s(buf,"[SPAWN-WPN]   P[%d] %04X/%04X/%04X/%04X",
                    s, *(uint32_t*)(p+0),*(uint32_t*)(p+8),*(uint32_t*)(p+16),*(uint32_t*)(p+24)); ODS(buf);
            }
        }
    }
    return ret;
}

static void InstallInjectionHooks() {
    static bool done = false;
    if (done) return;
    done = true;
    g_HkPathSel   = safetyhook::create_inline((void*)(BaseAddress + 0x171CE40), (void*)HkPathSelector);
    g_HkConverter = safetyhook::create_inline((void*)(BaseAddress + 0x1546780), (void*)HkConverter);
    g_HkC1Find    = safetyhook::create_inline((void*)(BaseAddress + 0x166D8B0), (void*)HkC1Find);
    g_HkAcc0      = safetyhook::create_inline((void*)(BaseAddress + 0x1665DF0), (void*)HkAcc0);
    g_HkAcc3      = safetyhook::create_inline((void*)(BaseAddress + 0x8A72D0),  (void*)HkAcc3);
    g_HkGetWeaponConfigCore = safetyhook::create_inline((void*)(BaseAddress + 0x1716E50), (void*)HkGetWeaponConfigCore);
    g_HkSpawnWeaponCfg      = safetyhook::create_inline((void*)(BaseAddress + 0x17316B0), (void*)HkSpawnWeaponCfgFn);
    char hkOk[128];
    sprintf_s(hkOk, "[LSYNC] Getter hook installed at %p valid=%d",
        (void*)(BaseAddress + 0x1716E50), (bool)g_HkGetWeaponConfigCore); ODS(hkOk);
}

// ---- extracted from ParseAndPush to reduce nesting ----

// DO NOT READ, ONLY IF U WANT PAIN
static void DumpV43Data(const std::vector<FName>& roleFNs) {
    static bool s_V43Done = false;
    if (s_V43Done || roleFNs.empty()) return;
    s_V43Done = true;
    typedef void (__fastcall *FnBuildV43)(void* out, void* roleFName);
    FnBuildV43 BuildV43 = (FnBuildV43)(BaseAddress + 0x1544D00);
    unsigned char v43[0x100] = {};
    BuildV43(v43, (void*)&roleFNs[0]);

    auto FnStr = [](uint64_t raw) {
        uint32_t ci = (uint32_t)(raw & 0xFFFFFFFF);
        uint32_t num = (uint32_t)(raw >> 32);
        char b[64]; sprintf_s(b,"idx=%u num=%u", ci, num); return std::string(b);
    };
    auto ArrStr = [](unsigned char* p) {
        void* d = *(void**)(p); int32_t n = *(int32*)(p + 8); int32_t m = *(int32*)(p + 12);
        char b[128]; sprintf_s(b,"Data=%p Num=%d Max=%d", d, n, m); return std::string(b);
    };
    auto MapStr = [](unsigned char* p) {
        void* d = *(void**)(p); int32_t n = *(int32*)(p + 8); int32_t m = *(int32*)(p + 12);
        void* h = *(void**)(p + 0x38); int32_t hs = *(int32*)(p + 0x48);
        char b[128]; sprintf_s(b,"Data=%p Num=%d Max=%d Hash=%p HashSize=%d", d, n, m, h, hs); return std::string(b);
    };

    ODS("[LSYNC] ========== V43 DUMP ==========");
    ODS(("[LSYNC] +0x00 CharacterID: " + FnStr(*(uint64_t*)(v43+0x00))).c_str());
    ODS(("[LSYNC] +0x08 Skin/Paint:  " + FnStr(*(uint64_t*)(v43+0x08))).c_str());
    ODS(("[LSYNC] +0x10 SkinClassAr: " + ArrStr(v43+0x10)).c_str());
    ODS(("[LSYNC] +0x20 SkinIDArray: " + ArrStr(v43+0x20)).c_str());
    ODS(("[LSYNC] +0x30 InventoryMap:" + MapStr(v43+0x30)).c_str());
    ODS(("[LSYNC] +0x80 WeaponID0:   " + FnStr(*(uint64_t*)(v43+0x80))).c_str());
    ODS(("[LSYNC] +0x88 WeaponID1:   " + FnStr(*(uint64_t*)(v43+0x88))).c_str());
    ODS(("[LSYNC] +0x90 WeaponCfg0:  " + ArrStr(v43+0x90)).c_str());
    ODS(("[LSYNC] +0xA0 WeaponCfg1:  " + ArrStr(v43+0xA0)).c_str());
    ODS(("[LSYNC] +0xB0 WeaponMap:   " + MapStr(v43+0xB0)).c_str());
    ODS("[LSYNC] ======== V43 DUMP END ========");

    // sub_141654C80 dump
    {
        unsigned char* wm = v43 + 0xB0;
        void* _wd = *(void**)wm; int32_t _wn = *(int32*)(wm + 8);
        if (_wd && _wn > 0) {
            typedef void (__fastcall *FnBuildWeaponCfg)(void* out, void* weaponFName);
            FnBuildWeaponCfg BuildWC = (FnBuildWeaponCfg)(BaseAddress + 0x1654C80);
            unsigned char wcBuf[0xC0] = {};
            BuildWC(wcBuf, _wd);
            char hb2[2048]; int off = 0;
            off += sprintf_s(hb2 + off, 2048 - off, "[LSYNC] sub_141654C80 output (0xB0 bytes):");
            for (int i = 0; i < 0xB0; i += 16) {
                off += sprintf_s(hb2 + off, 2048 - off, "\n  +%02X:", i);
                for (int j = 0; j < 16 && (i + j) < 0xB0; j++)
                    off += sprintf_s(hb2 + off, 2048 - off, " %02X", wcBuf[i + j]);
            }
            ODS(hb2);
            void* p50 = *(void**)(wcBuf + 0x50); int n50 = *(int*)(wcBuf + 0x58);
            char b3[128]; sprintf_s(b3, "[LSYNC] Container#2 at src+0x50: Data=%p Num=%d", p50, n50); ODS(b3);
            if (p50 && n50 > 0) {
                off = 0; off += sprintf_s(hb2 + off, 2048 - off, "[LSYNC] Container#2 elements:");
                for (int i = 0; i < n50 * 32 && i < 512; i += 16) {
                    off += sprintf_s(hb2 + off, 2048 - off, "\n  +%02X:", i);
                    for (int j = 0; j < 16 && (i + j) < n50 * 32; j++)
                        off += sprintf_s(hb2 + off, 2048 - off, " %02X", ((unsigned char*)p50)[i + j]);
                }
                ODS(hb2);
            }
        }
    }

    // Dump first 192B WeaponMap entry
    {
        unsigned char* wp = v43 + 0xB0;
        void* wd = *(void**)wp; int32_t wn = *(int32*)(wp + 8);
        if (wd && wn > 0) {
            char hb[2048]; int off = 0;
            off += sprintf_s(hb + off, 2048 - off, "[LSYNC] WeaponMap[0] 192B entry at %p:", wd);
            for (int i = 0; i < 192; i += 16) {
                off += sprintf_s(hb + off, 2048 - off, "\n  +%02X:", i);
                for (int j = 0; j < 16 && (i + j) < 192; j++)
                    off += sprintf_s(hb + off, 2048 - off, " %02X", ((unsigned char*)wd)[i + j]);
            }
            ODS(hb);
            unsigned char* e = (unsigned char*)wd;
            void* p08 = *(void**)(e + 0x08); int n08 = *(int*)(e + 0x10); int m08 = *(int*)(e + 0x14);
            char b2[128]; sprintf_s(b2, "[LSYNC] +08 TArray: Data=%p Num=%d Max=%d", p08, n08, m08); ODS(b2);
            if (p08 && n08 > 0) {
                char dbg[2048]; off = 0;
                off += sprintf_s(dbg + off, 2048 - off, "[LSYNC] +08 elements at %p:", p08);
                for (int i = 0; i < n08 * 4 && i < 256; i += 16) {
                    off += sprintf_s(dbg + off, 2048 - off, "\n  +%02X:", i);
                    for (int j = 0; j < 16 && (i + j) < n08 * 4; j++)
                        off += sprintf_s(dbg + off, 2048 - off, " %02X", ((unsigned char*)p08)[i + j]);
                }
                ODS(dbg);
            }
            void* p20 = *(void**)(e + 0x20); int n20 = *(int*)(e + 0x30); int m20 = *(int*)(e + 0x34);
            sprintf_s(b2, "[LSYNC] +20 TArray: Data=%p Num=%d Max=%d", p20, n20, m20); ODS(b2);
            if (p20 && n20 > 0) {
                char dbg[2048]; off = 0;
                off += sprintf_s(dbg + off, 2048 - off, "[LSYNC] +20 elements at %p:", p20);
                int bytes = n20 > 1 ? 64 : n20 * 4;
                for (int i = 0; i < bytes && i < 256; i += 16) {
                    off += sprintf_s(dbg + off, 2048 - off, "\n  +%02X:", i);
                    for (int j = 0; j < 16 && (i + j) < bytes; j++)
                        off += sprintf_s(dbg + off, 2048 - off, " %02X", ((unsigned char*)p20)[i + j]);
                }
                ODS(dbg);
            }
        } else {
            char b[99]; sprintf_s(b, "[LSYNC] WeaponMap empty: Data=%p Num=%d", wd, wn); ODS(b);
        }
    }
}

// DO NOT READ, ONLY IF U WANT PAIN
static void RunDiagnostics(void* existing, uint64_t wepFN, const std::vector<FName>& roleFNs) {
    static bool done = false;
    if (done) return;
    done = true;

    unsigned char* val = (unsigned char*)existing;
    char bb[256];

    // --- C1/C2 dump ---
    void* c2Data = *(void**)(val + 0x50); int c2Num = *(int*)(val + 0x58);
    void* c1Data = *(void**)val;           int c1Num = *(int*)(val + 8);
    sprintf_s(bb,"[LSYNC] Container#2 (20B): Data=%p Num=%d", c2Data, c2Num); ODS(bb);
    if (c2Data && c2Num > 0) {
        for (int e = 0; e < c2Num && e < 20; ++e) {
            unsigned char* p = (unsigned char*)c2Data + 20 * e;
            uint8_t slot = *p;
            uint64_t cfgKey = *(uint64_t*)(p + 4);
            int next = *(int*)(p + 12); int hv = *(int*)(p + 16);
            sprintf_s(bb,"[LSYNC]   C2[%d] slot=%d key=%04X/%04X next=%d hash=%d",
                e, slot, (uint32_t)(cfgKey>>32), (uint32_t)cfgKey, next, hv); ODS(bb);
        }
    }
    sprintf_s(bb,"[LSYNC] Container#1 (48B): Data=%p Num=%d", c1Data, c1Num); ODS(bb);
    if (c1Data && c1Num > 0) {
        for (int e = 0; e < c1Num && e < 30; ++e) {
            unsigned char* p = (unsigned char*)c1Data + 48 * e;
            uint64_t keyFN = *(uint64_t*)(p + 0);
            uint64_t f0 = *(uint64_t*)(p + 8); uint64_t f1 = *(uint64_t*)(p + 16);
            uint64_t f2 = *(uint64_t*)(p + 24); uint64_t f3 = *(uint64_t*)(p + 32);
            int next = *(int*)(p + 40); int hv = *(int*)(p + 44);
            sprintf_s(bb,"[LSYNC]   C1[%d] key=%04X/%04X v0=%04X/%04X v1=%04X/%04X v2=%04X/%04X v3=%04X/%04X",
                e, (uint32_t)(keyFN>>32),(uint32_t)keyFN,
                (uint32_t)(f0>>32),(uint32_t)f0, (uint32_t)(f1>>32),(uint32_t)f1,
                (uint32_t)(f2>>32),(uint32_t)f2, (uint32_t)(f3>>32),(uint32_t)f3); ODS(bb);
        }
    }

    // --- Find Manager ---
    UPBFieldModManager* Mgr = nullptr;
    for (int i = SDK::UObject::GObjects->Num() - 1; i >= 0; --i) {
        auto* Obj = SDK::UObject::GObjects->GetByIndex(i);
        if (Obj && Obj->IsA(UPBFieldModManager::StaticClass())) {
            Mgr = static_cast<UPBFieldModManager*>(Obj); break;
        }
    }
    if (!Mgr) { ODS("[LSYNC] DIAG: Manager not found"); return; }

    // --- ROLE-CMP probe ---
    ODS("[LSYNC] === GetWeaponNetworkConfig READBACK ===");
    {
        typedef char (__fastcall *FnPathSelector)(void* manager);
        typedef void* (__fastcall *FnGetAuth)(void* globalPtr);
        FnPathSelector PathSel = (FnPathSelector)(BaseAddress + 0x171CE40);
        FnGetAuth GetAuth = (FnGetAuth)(BaseAddress + 0x1599830);
        void* globalPtr = *(void**)(BaseAddress + 0x5EA1C60);

        char sel = PathSel(Mgr);
        struct FNameRaw { uint32_t CI; uint32_t Num; uint64_t Raw() { return (uint64_t)CI | ((uint64_t)Num << 32); } };
        FNameRaw* mData = *(FNameRaw**)((char*)Mgr + 0x140);
        int       mNum  = *(int*)((char*)Mgr + 0x148);
        void* authObj = globalPtr ? GetAuth(globalPtr) : nullptr;
        FNameRaw* aData = authObj ? *(FNameRaw**)((char*)authObj + 0xC38) : nullptr;
        int       aNum  = authObj ? *(int*)((char*)authObj + 0xC40) : 0;

        sprintf_s(bb, "[ROLE-CMP] Manager=%p Mgr.Num=%d  Auth=%p Auth.Num=%d  Selector=%s",
            Mgr, mNum, authObj, aNum, sel ? "TRUE" : "FALSE"); ODS(bb);

        int common = mNum < aNum ? mNum : aNum;
        bool orderedEqual = (mNum == aNum);
        for (int i = 0; i < common; ++i) {
            FNameRaw m = mData[i], a = aData[i];
            bool match = (m.Raw() == a.Raw());
            if (!match) orderedEqual = false;
            sprintf_s(bb, "[ROLE-CMP] [%d] M=%08X/%08X A=%08X/%08X %s",
                i, m.Num, m.CI, a.Num, a.CI, match ? "MATCH" : "MISMATCH"); ODS(bb);
        }
        if (mNum > common) {
            for (int i = common; i < mNum; ++i) {
                FNameRaw m = mData[i]; orderedEqual = false;
                sprintf_s(bb, "[ROLE-CMP] [%d] M=%08X/%08X A=--- EXTRA", i, m.Num, m.CI); ODS(bb);
            }
        }
        if (aNum > common) {
            for (int i = common; i < aNum; ++i) {
                FNameRaw a = aData[i]; orderedEqual = false;
                sprintf_s(bb, "[ROLE-CMP] [%d] M=--- A=%08X/%08X MISSING", i, a.Num, a.CI); ODS(bb);
            }
        }
        sprintf_s(bb, "[ROLE-CMP] orderedEqual=%s", orderedEqual ? "TRUE" : "FALSE"); ODS(bb);
    }

#if 0
    // --- TEST0 trigger: force Simple path for passive Converter probe ---
    ODS("[LSYNC] === TEST0: forcing Simple path for passive probe ===");
    FName testRole = roleFNs[0];
    FName testWeap; *(uint64_t*)&testWeap = wepFN;
    g_ForceSimplePath = true;
    g_InNativeCall = true;
    SDK::FPBWeaponNetworkConfig cfg = Mgr->GetWeaponNetworkConfig(testRole, testWeap);
    g_InNativeCall = false;
    g_ForceSimplePath = false;
    char tmp[256];
    sprintf_s(tmp, "[TEST0] GWN returned: Parts.Num=%d", cfg.WeaponPartConfigs.Num()); ODS(tmp);

    // === TEST 2: feed live 192B entry+0x08 directly to converter ===
    {
        static bool s_Test2Done = false;
        if (!s_Test2Done) {
            s_Test2Done = true;
            ODS("[TEST2] === live 192B value → converter ===");
            typedef FPBWeaponNetworkConfigRaw* (__fastcall *FnConvert)(FPBWeaponNetworkConfigRaw*, const void*, const void*);
            FnConvert Convert = (FnConvert)(BaseAddress + 0x1546780);
            FName wf; *(uint64_t*)&wf = wepFN;
            FPBWeaponNetworkConfigRaw out{};
            Convert(&out, &wf, val);
            sprintf_s(tmp, "[TEST2] Ornament=%04X Weapon=%04X Class=%04X Slots=%d Parts=%d",
                out.OrnamentID.CI, out.WeaponID.CI, out.WeaponClassID.CI,
                out.WeaponPartSlotTypeArray.Num, out.WeaponPartConfigs.Num); ODS(tmp);
            for (int s = 0; s < out.WeaponPartConfigs.Num && s < 10; ++s) {
                uint32_t* v = (uint32_t*)((uint8_t*)out.WeaponPartConfigs.Data + 32*s);
                int sl = *(uint8_t*)((uint8_t*)out.WeaponPartSlotTypeArray.Data + s);
                sprintf_s(tmp, "[TEST2]   Part[%d] slot=%d partID=%04X skinID=%04X spec=%04X paint=%04X",
                    s, sl, v[0], v[2], v[4], v[6]); ODS(tmp);
            }
            // TEST 2b: convert via sub_1416BCBB0 first, then feed to converter
            {
                ODS("[TEST2b] === 192B entry → sub_1416BCBB0 → converter ===");
                typedef void* (__fastcall *FnCopyBuf)(void* dst, void* src);
                typedef FPBWeaponNetworkConfigRaw* (__fastcall *FnConvert)(FPBWeaponNetworkConfigRaw*, const void*, const void*);
                FnCopyBuf CopyBuf = (FnCopyBuf)(BaseAddress + 0x16BCBB0);
                FnConvert  Convert = (FnConvert)(BaseAddress + 0x1546780);
                unsigned char tmpBuf[0xC0] = {};
                CopyBuf(tmpBuf, val);
                FName wf2; *(uint64_t*)&wf2 = wepFN;
                FPBWeaponNetworkConfigRaw out2{};
                Convert(&out2, &wf2, tmpBuf);
                sprintf_s(tmp, "[TEST2b] Ornament=%04X Weapon=%04X Class=%04X Slots=%d Parts=%d",
                    out2.OrnamentID.CI, out2.WeaponID.CI, out2.WeaponClassID.CI,
                    out2.WeaponPartSlotTypeArray.Num, out2.WeaponPartConfigs.Num); ODS(tmp);
                for (int s = 0; s < out2.WeaponPartConfigs.Num && s < 10; ++s) {
                    uint32_t* v = (uint32_t*)((uint8_t*)out2.WeaponPartConfigs.Data + 32*s);
                    int sl = *(uint8_t*)((uint8_t*)out2.WeaponPartSlotTypeArray.Data + s);
                    sprintf_s(tmp, "[TEST2b]   Part[%d] slot=%d partID=%04X skinID=%04X spec=%04X paint=%04X",
                        s, sl, v[0], v[2], v[4], v[6]); ODS(tmp);
                }
                ODS("[TEST2b] === DONE ===");
            }
            // TEST 2c: compare C1 entry field layout — same PartID in canonical vs live
            {
                ODS("[TEST2c] === C1 field layout comparison ===");
                // Build canonical source buffer via sub_141654C80
                typedef void (__fastcall *FnBuildBuf)(void* out, void* weaponFName);
                FnBuildBuf BuildBuf = (FnBuildBuf)(BaseAddress + 0x1654C80);
                unsigned char canon[0xC0] = {};
                FName wf3; *(uint64_t*)&wf3 = wepFN;
                BuildBuf(canon, &wf3);

                // Read canonical C1
                void* canonC1Data = *(void**)canon;  // Container#1 Data at buffer+0x00
                int   canonC1Num  = *(int*)(canon + 8);

                // Read live C1
                void* liveC1Data = *(void**)val;      // Container#1 Data at entry+0x00
                int   liveC1Num  = *(int*)(val + 8);

                sprintf_s(tmp, "[TEST2c] Canon C1=%p Num=%d  Live C1=%p Num=%d",
                    canonC1Data, canonC1Num, liveC1Data, liveC1Num); ODS(tmp);

                // For each active slot in C2, look up the PartID in both C1s
                void* c2D = *(void**)(val + 0x50); int c2N = *(int*)(val + 0x58);
                for (int si = 0; si < c2N && si < 20; ++si) {
                    unsigned char* c2e = (unsigned char*)c2D + 20*si;
                    uint32_t slotId = c2e[0];
                    uint64_t partKey = *(uint64_t*)(c2e + 4);
                    if (slotId == 0 || partKey == 0) continue;

                    // Find in canonical C1
                    uint32_t cv[4] = {};
                    for (int e = 0; e < canonC1Num; ++e) {
                        unsigned char* p = (unsigned char*)canonC1Data + 48*e;
                        if (*(uint64_t*)(p+0) == partKey) {
                            cv[0]=*(uint32_t*)(p+8); cv[1]=*(uint32_t*)(p+16);
                            cv[2]=*(uint32_t*)(p+24); cv[3]=*(uint32_t*)(p+32); break;
                        }
                    }
                    // Find in live C1
                    uint32_t lv[4] = {};
                    for (int e = 0; e < liveC1Num; ++e) {
                        unsigned char* p = (unsigned char*)liveC1Data + 48*e;
                        if (*(uint64_t*)(p+0) == partKey) {
                            lv[0]=*(uint32_t*)(p+8); lv[1]=*(uint32_t*)(p+16);
                            lv[2]=*(uint32_t*)(p+24); lv[3]=*(uint32_t*)(p+32); break;
                        }
                    }
                    sprintf_s(tmp, "[TEST2c] PartKey=%04X slot=%d  Canon=[%04X,%04X,%04X,%04X]  Live=[%04X,%04X,%04X,%04X]",
                        (uint32_t)partKey, slotId,
                        cv[0],cv[1],cv[2],cv[3], lv[0],lv[1],lv[2],lv[3]); ODS(tmp);

                    // Check permutation hypothesis: Live→Canon = [1,0,3,2]
                    bool permOk = (cv[0]==lv[1] && cv[1]==lv[0] && cv[2]==lv[3] && cv[3]==lv[2]);
                    if (!permOk) {
                        // Also try identity
                        bool ident = (cv[0]==lv[0] && cv[1]==lv[1] && cv[2]==lv[2] && cv[3]==lv[3]);
                        sprintf_s(tmp, "[TEST2c]   perm=[1,0,3,2] %s  identity=%s",
                            permOk?"MATCH":"no", ident?"MATCH":"no"); ODS(tmp);
                    } else {
                        ODS("[TEST2c]   perm=[1,0,3,2] MATCH");
                    }
                }
                ODS("[TEST2c] === DONE ===");

                // dump TSparseArray header fields that converter reads
                ODS("[TEST2d] === buffer header comparison ===");
                unsigned char* cb = canon;       // canonical buffer (0xB0)
                unsigned char* lb = val;          // live 192B entry+0x08
                sprintf_s(tmp, "[TEST2d] Canon: C2.Data=%p C2.Num=%d  Live: C2.Data=%p C2.Num=%d",
                    *(void**)(cb+0x50), *(int*)(cb+0x58),
                    *(void**)(lb+0x50), *(int*)(lb+0x58)); ODS(tmp);
                // Dump raw bytes at +0x50..+0x70 for both
                auto HexDump = [](const char* label, unsigned char* p) {
                    char b[512]; int off=0;
                    off+=sprintf_s(b+off,512-off,"%s +50..+6F:",label);
                    for(int i=0;i<32;i++) off+=sprintf_s(b+off,512-off," %02X",p[0x50+i]);
                    ODS(b);
                };
                HexDump("[TEST2d] Canon", cb);
                HexDump("[TEST2d] Live ", lb);
                sprintf_s(tmp, "[TEST2d] Canon +60=%08X +70=%p +78=%d  Live +60=%08X +70=%p +78=%d",
                    *(uint32_t*)(cb+0x60), *(void**)(cb+0x70), *(int*)(cb+0x78),
                    *(uint32_t*)(lb+0x60), *(void**)(lb+0x70), *(int*)(lb+0x78)); ODS(tmp);
                // TEST 3: compare C1 entry pointers returned by 48B TMap::Find
                ODS("[TEST3] === 48B TMap::Find pointer comparison ===");
                {
                    typedef int* (__fastcall *FnFind48)(void* tmap, int* outIdx, uint64_t key);
                    FnFind48 Find48 = (FnFind48)(BaseAddress + 0x8E3B00);
                    int idxC = -1, idxL = -1;
                    uint64_t testKey = *(uint64_t*)((unsigned char*)c2D + 4); // first slot's PartID
                    Find48(canon, &idxC, testKey);
                    Find48(lb,    &idxL, testKey);
                    unsigned char* ptrC = idxC>=0 ? ((unsigned char*)*(void**)canon + 48*idxC + 8) : nullptr;
                    unsigned char* ptrL = idxL>=0 ? ((unsigned char*)*(void**)lb    + 48*idxL + 8) : nullptr;
                    sprintf_s(tmp, "[TEST3] PartKey=%04X Canon: idx=%d entry+8=%p  Live: idx=%d entry+8=%p",
                        (uint32_t)testKey, idxC, ptrC, idxL, ptrL); ODS(tmp);
                    if (ptrC && ptrL) {
                        // Dump the 32-byte value region at both pointers
                        sprintf_s(tmp, "[TEST3] Canon C1[0..31]: %02X%02X%02X%02X...  Live C1[0..31]: %02X%02X%02X%02X...",
                            ptrC[0],ptrC[1],ptrC[2],ptrC[3], ptrL[0],ptrL[1],ptrL[2],ptrL[3]); ODS(tmp);
                        // Compare raw 32 bytes
                        bool same = (memcmp(ptrC, ptrL, 32) == 0);
                        sprintf_s(tmp, "[TEST3] C1 value 32B raw comparison: %s", same ? "IDENTICAL" : "DIFFERENT"); ODS(tmp);
                    }
                }
                ODS("[TEST3] === DONE ===");
                ODS("[TEST2d] === DONE ===");
            }
            ODS("[TEST2] === DONE ===");
        }
    }
#endif
}

// DO NOT READ, ONLY IF U WANT PAIN
static void RunPhase2Injection(APBPlayerState* PS, const std::vector<FName>& roleFNs) {
    int nA = (int)roleFNs.size();
    static bool s_Injected = false;
    if (s_Injected || nA <= 0) return;
    s_Injected = true;
    ODS("[LSYNC] === 272B WEAPON CONFIG INJECTION v2 ===");

    typedef void   (__fastcall *FnBuildV43)(void* out, void* roleFName);
    typedef int*   (__fastcall *FnFind272)(void* tmap, int* outIdx, uint64_t fname);
    typedef void   (__fastcall *FnBuildSrc)(void* out, void* weaponFName);
    typedef void*  (__fastcall *FnFind192)(void* tmap, int hash, void* keyFName);
    typedef void*  (__fastcall *FnInsert192)(void* tmap, int hash, void* keyFName);

    FnBuildV43  BldV43  = (FnBuildV43)(BaseAddress + 0x1544D00);
    FnFind272   F272    = (FnFind272) (BaseAddress + 0x15AEB20);
    FnBuildSrc  BSrc    = (FnBuildSrc)(BaseAddress + 0x1654C80);
    FnFind192   F192    = (FnFind192) (BaseAddress + 0x164F740);
    FnInsert192 I192    = (FnInsert192)(BaseAddress + 0x164ED60);

    unsigned char* psRaw = (unsigned char*)PS;
    void* tmap272 = psRaw + 0x700;

    auto HashFn = [](uint64_t fnameRaw) -> int {
        uint32_t ci = (uint32_t)(fnameRaw & 0xFFFFFFFF);
        return ((uint16_t)ci << 16) + (uint16_t)ci + 524289 * (ci >> 16) + ((uint16_t)ci >> 4);
    };

    std::unordered_set<unsigned __int64> seenRoles;
    int wcInserted = 0, wcSkipped = 0;

    for (int ri = 0; ri < nA; ++ri) {
        uint64_t fnRaw = *(uint64_t*)&roleFNs[ri];
        if (seenRoles.count(fnRaw)) {
            char bb[128]; sprintf_s(bb,"[LSYNC] SKIP duplicate role[%d] idx=%u", ri, (uint32_t)fnRaw); ODS(bb);
            continue;
        }
        seenRoles.insert(fnRaw);

        int idx = -1;
        F272(tmap272, &idx, fnRaw);
        char bb[128];
        if (idx == -1) {
            sprintf_s(bb, "[LSYNC] 272B MISSING role[%d]", ri); ODS(bb);
            continue;
        }
        sprintf_s(bb, "[LSYNC] 272B role[%d] idx=%d", ri, idx); ODS(bb);

        void* entryData = *(void**)tmap272;
        if (!entryData) continue;
        unsigned char* roleEntry = (unsigned char*)entryData + 272 * idx;
        void* tmap192 = roleEntry + 0xB8;

        unsigned char v43[0x100] = {};
        BldV43(v43, (void*)&roleFNs[ri]);
        void* wmData = *(void**)(v43 + 0xB0);
        int   wmNum  = *(int*)(v43 + 0xB8);

        if (!wmData || wmNum <= 0) continue;
        sprintf_s(bb, "[LSYNC] WeaponMap has %d weapons", wmNum); ODS(bb);

        for (int wi = 0; wi < wmNum && wi < 10; ++wi) {
            void* wepKey = (unsigned char*)wmData + 192 * wi;
            uint64_t wepFN = *(uint64_t*)wepKey;
            if (wepFN == 0) continue;

            int hash = (int)(((wepFN >> 32) & 0xFFFFFFFF) + HashFn(wepFN));
            void* existing = F192(tmap192, hash, wepKey);
            if (existing) {
                wcSkipped++;
                // RunDiagnostics disabled — DO NOT READ, ONLY IF U WANT PAIN
                // RunDiagnostics(existing, wepFN, roleFNs);
                continue;
            }

            unsigned char srcBuf[0xC0] = {};
            BSrc(srcBuf, wepKey);
            I192(tmap192, hash, wepKey);
            wcInserted++;
        }
    }
    {char bbb[128];sprintf_s(bbb,"[LSYNC] 192B: %d inserted, %d skipped", wcInserted, wcSkipped);ODS(bbb);}
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
    HINTERNET hC=WinHttpConnect(hS,L"132.226.204.81",8000,0); if(!hC){WinHttpCloseHandle(hS);return{};}
    HINTERNET hR=WinHttpOpenRequest(hC,L"GET",p.c_str(),0,0,0,0); if(!hR){WinHttpCloseHandle(hC);WinHttpCloseHandle(hS);return{};}
    std::string r; BOOL ok=WinHttpSendRequest(hR,0,0,0,0,0,0);
    if(ok)ok=WinHttpReceiveResponse(hR,0);
    if(ok){DWORD br;char b[4096];while(WinHttpReadData(hR,b,4095,&br)&&br>0){b[br]=0;r.append(b,br);if(r.size()>256*1024)break;}}
    WinHttpCloseHandle(hR);WinHttpCloseHandle(hC);WinHttpCloseHandle(hS); return r;
}

// === B.5 production: getter hook with PlayerState fallback ===
thread_local APBPlayerState* g_CurrentSpawnPS = nullptr;

void LoadoutSync::SetSpawnPlayerState(APBPlayerState* PS) {
    g_CurrentSpawnPS = PS;
}

static __int64 HkGetWeaponConfigCore(void* mgr, void* out, __int64 roleID, __int64 weaponID) {
    static int callCount = 0;
    callCount++;
    if (callCount <= 3) {
        char buf[128];
        sprintf_s(buf, "[GWN-HOOK] call#%d mgr=%p PS=%p weapon=%08X",
            callCount, mgr, g_CurrentSpawnPS, (uint32_t)(weaponID & 0xFFFFFFFF)); ODS(buf);
    }
    if (g_CurrentSpawnPS && weaponID) {
        typedef int* (__fastcall *FnFind272)(void* tmap, int* outIdx, uint64_t fname);
        typedef void* (__fastcall *FnFind192)(void* tmap, int hash, void* keyFName);
        typedef void* (__fastcall *FnConvert)(void* output, void* wid, void* data);
        FnFind272 Find272 = (FnFind272)(BaseAddress + 0x15AEB20);
        FnFind192 Find192 = (FnFind192)(BaseAddress + 0x164F740);
        FnConvert  Convert  = (FnConvert)(BaseAddress + 0x1546780);

        unsigned char* psRaw = (unsigned char*)g_CurrentSpawnPS;
        int idx = -1;
        Find272(psRaw + 0x700, &idx, roleID);
        if (idx >= 0) {
            void* ed = *(void**)(psRaw + 0x700);
            if (ed) {
                unsigned char* re = (unsigned char*)ed + 272 * idx;
                uint32_t ci = (uint32_t)(weaponID & 0xFFFFFFFF);
                int hash = (int)(((weaponID >> 32) & 0xFFFFFFFF) + ((uint16_t)ci << 16) + (uint16_t)ci + 524289 * (ci >> 16) + ((uint16_t)ci >> 4));
                void* existing = Find192(re + 0xB8, hash, (void*)&weaponID);
                if (existing) {
                    Convert(out, &weaponID, existing);
                    // Checkpoint A: log natural getter output
                    {
                        uint32_t* v = (uint32_t*)((uint8_t*)*(void**)((uint8_t*)out + 0x10) + 0);
                        char buf[256];
                        sprintf_s(buf, "[GWN-NATURAL] weapon=%08X skin=%04X/%04X spec=%04X/%04X paint=%04X/%04X",
                            (uint32_t)(weaponID & 0xFFFFFFFF), v[2], v[3], v[4], v[5], v[6], v[7]); ODS(buf);
                    }
                    g_CurrentSpawnPS = nullptr;
                    return (__int64)out;
                }
            }
        }
        g_CurrentSpawnPS = nullptr;
    }
    return g_HkGetWeaponConfigCore.call<__int64>(mgr, out, roleID, weaponID);
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
    char b[320];

    auto* mgr = static_cast<UPBDataTableManager*>(
        USubsystemBlueprintLibrary::GetEngineSubsystem(
            TSubclassOf<UEngineSubsystem>(UPBDataTableManager::StaticClass())));
    UDataTable* dt = mgr ? mgr->WeaponSuitDefinitionDataTable.Get() : nullptr;
    if (!dt) { ODS("[SUIT] WeaponSuitDefinitionDataTable unavailable"); return; }

    FName suitFn = MkN(wc.ornType);
    FPBWeaponSuiteDefinitionRow* row = nullptr;
    int rowNum = dt->RowMap.Num();
    for (int i = 0; i < rowNum; ++i) {
        auto& pr = dt->RowMap[i];
        if (pr.Key() == suitFn) { row = (FPBWeaponSuiteDefinitionRow*)pr.Value(); break; }
    }
    if (!row) {
        sprintf_s(b, "[SUIT] %s -> NO ROW in WeaponSuitDefinitionDataTable (%d rows)",
                  wc.ornType.c_str(), rowNum);
        ODS(b);
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
        sprintf_s(b, "[SUIT] %s slot=%d skin=%s -> +0x08 (part=0x%04X)",
                  wc.ornType.c_str(), slot, skinStr.c_str(), (unsigned)(kit->second & 0xFFFF));
        ODS(b);
        ++applied;
    }
    sprintf_s(b, "[SUIT] %s row=%p map=%d applied=%d", wc.ornType.c_str(),
              (void*)row, n, applied);
    ODS(b);

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
        if (!pdt) { ODS("[SUIT] WeaponSuitPaintingDefinitionDataTable unavailable"); return; }

        FName paintFn = MkN(wc.ornId);
        FPBWeaponSuitePaintingDefinitionRow* prow = nullptr;
        int pNum = pdt->RowMap.Num();
        for (int i = 0; i < pNum; ++i) {
            auto& pr = pdt->RowMap[i];
            if (pr.Key() == paintFn) { prow = (FPBWeaponSuitePaintingDefinitionRow*)pr.Value(); break; }
        }
        if (!prow) {
            sprintf_s(b, "[SUIT] %s -> NO ROW in WeaponSuitPaintingDefinitionDataTable (%d rows)",
                      wc.ornId.c_str(), pNum);
            ODS(b);
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
            sprintf_s(b, "[SUIT] %s paint slot=%d %s -> +0x18 (part=0x%04X)",
                      wc.ornId.c_str(), slot, paintStr.c_str(), (unsigned)(kit->second & 0xFFFF));
            ODS(b);
            ++pApplied;
        }
        sprintf_s(b, "[SUIT] %s paint row=%p map=%d applied=%d", wc.ornId.c_str(),
                  (void*)prow, pn, pApplied);
        ODS(b);
    }
}

static void ApplyCustomWeaponConfigs(APBPlayerState* PS, const std::vector<FName>& roleFNs,
                                      const std::unordered_map<uint64_t, RoleCfg>& roleCfgMap) {
    int nA = (int)roleFNs.size();
    std::string sid = PS->GetDefaultIDStr().ToString(); // per-player cache key
    typedef int*  (__fastcall *FnFind272)(void* tmap, int* outIdx, uint64_t fname);
    typedef void  (__fastcall *FnBldV43)(void* out, void* roleFName);
    typedef void* (__fastcall *FnFind192)(void* tmap, int hash, void* keyFName);
    FnFind272 F272   = (FnFind272)(BaseAddress + 0x15AEB20);
    FnBldV43  BldV43 = (FnBldV43)(BaseAddress + 0x1544D00);
    FnFind192 F192   = (FnFind192)(BaseAddress + 0x164F740);

    auto HashFn = [](uint64_t raw) -> int {
        uint32_t ci = (uint32_t)(raw & 0xFFFFFFFF);
        return (int)(((raw >> 32) & 0xFFFFFFFF) + ((uint16_t)ci << 16) + (uint16_t)ci + 524289 * (ci >> 16) + ((uint16_t)ci >> 4));
    };

    ODS("[CSTM] ApplyCustomWeaponConfigs START");

    unsigned char* psRaw = (unsigned char*)PS;
    void* tmap272 = psRaw + 0x700;

    for (int ri = 0; ri < nA; ++ri) {
        uint64_t fnRaw = *(uint64_t*)&roleFNs[ri];
        auto cit = roleCfgMap.find(fnRaw);
        if (cit == roleCfgMap.end()) continue;
        auto& rc = cit->second;

        // Find 272B entry
        int idx = -1;
        F272(tmap272, &idx, fnRaw);
        if (idx < 0) continue;
        void* ed = *(void**)tmap272;
        if (!ed) continue;
        unsigned char* re = (unsigned char*)ed + 272 * idx;

        // Build v43 to get WeaponMap
        unsigned char v43[0x100] = {};
        BldV43(v43, (void*)&roleFNs[ri]);
        void* wmData = *(void**)(v43 + 0xB0);
        int   wmNum  = *(int*)(v43 + 0xB8);
        if (!wmData || wmNum <= 0) continue;

        for (int wi = 0; wi < wmNum; ++wi) {
            unsigned char* wepEntry = (unsigned char*)wmData + 192 * wi;
            uint64_t wepFN = *(uint64_t*)wepEntry;
            if (!wepFN) continue;

            for (auto& wc : rc.weapons) {
                FName wkeyFn = MkN(wc.weaponId);
                uint64_t wkeyRaw = *(uint64_t*)&wkeyFn;
                if ((wepFN & 0xFFFFFFFF) != (wkeyRaw & 0xFFFFFFFF)) continue;

                int hash = HashFn(wepFN);
                void* existing = F192(re + 0xB8, hash, (void*)&wepFN);
                if (!existing) continue;
                unsigned char* val = (unsigned char*)existing;

                void* c2Data = *(void**)(val + 0x50); int c2Num = *(int*)(val + 0x58);
                void* c1Data = *(void**)val;           int c1Num = *(int*)(val + 8);
                if (!c2Data || !c1Data) continue;

                if (!wc.ornId.empty()) {
                    FName ornFn = MkN(wc.ornId);
                    for (int e = 0; e < c2Num; ++e) {
                        unsigned char* p = (unsigned char*)c2Data + 20 * e;
                        if (*p == 12) {
                            uint64_t wclassKey = *(uint64_t*)(p + 4);
                            for (int ce = 0; ce < c1Num; ++ce) {
                                unsigned char* cp = (unsigned char*)c1Data + 48 * ce;
                                if (*(uint64_t*)(cp + 0) == wclassKey) {
                                    *(uint64_t*)(cp + 16) = *(uint64_t*)&ornFn; break;
                                }
                            }
                            break;
                        }
                    }
                    // B.5: cache weapon-level ornament for sub_141654C80 hook
                    B5WeaponConfig::CacheWeaponOrnament(sid, wepFN, *(uint64_t*)&ornFn);
                }

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

                    // B.5: cache the ornament — routed by its `type` (NEXT_SESSION_PROMPT.md §9).
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
                        char rb[320];
                        sprintf_s(rb, "[LSYNC][ORN] part=%s type=%s id=%s -> %s=%s",
                            pc.partId.c_str(), pc.ornType.empty() ? "<null>" : pc.ornType.c_str(),
                            pc.ornId.c_str(),
                            isPainting ? "paint(+0x18)" : "skin(+0x08)",
                            isPainting ? pc.ornId.c_str() : pc.ornType.c_str());
                        ODS(rb);
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
    InstallInjectionHooks();  // one-shot install of the injection hooks
    std::string sid = PS->GetDefaultIDStr().ToString();
    OutputDebugStringA(("[LSYNC] ParseAndPush " + sid + " " + std::to_string(json.size()) + "B\n").c_str());

    nlohmann::json root;
    try { root = nlohmann::json::parse(json); }
    catch (...) { ODS("[LSYNC] JSON FAIL"); return false; }
    auto& roles = root["roles"];
    int nR = (int)roles.size();
    {char b[99];sprintf_s(b,"[LSYNC] %d roles",nR);ODS(b);}

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
        // See NEXT_SESSION_PROMPT.md §16.
        {
            auto rawOf = [](const std::string& s) -> std::uint64_t {
                FName fn = MkN(s);
                return *reinterpret_cast<std::uint64_t*>(&fn);
            };
            FCharCosmeticCfg cc;
            cc.SkinFName  = rawOf(JsonStr(rd, "_skinToken"));
            cc.PaintFName = rawOf(JsonStr(rd, "_ornamentId"));
            cc.BadgeFName = rawOf(JsonStr(rd, "cosmetic9"));  // ArmBadge
            cc.HeadFName  = rawOf(JsonStr(rd, "cosmetic10")); // HeadAccessory
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
    {char b[99];sprintf_s(b,"[LSYNC] accumulated %d roles %zu items (authoritative order)",nA,allItems.size());ODS(b);}

#if 0 // DO NOT READ, ONLY IF U WANT PAIN
    DumpV43Data(roleFNs);
#endif

    // standalone arrays
    TArray<FName> rIDs, cfgRA; TArray<int32> quotas;
    for(int i=0;i<nA;++i){rIDs.Add(roleFNs[i]);quotas.Add(8);cfgRA.Add(roleFNs[i]);}
    {char b[99];sprintf_s(b,"[LSYNC] standalone arrays OK rIDs=%d",rIDs.Num());ODS(b);}

    // config
    FPBFieldModRoleGameSavedNetworkConfig config;
    config.RoleArray = cfgRA;
    auto& outer = config.RoleInventoryNetworkConfigArray;
    outer.Reserve(nA);
    {char b[99];sprintf_s(b,"[LSYNC] outer Reserve(%d) OK",nA);ODS(b);}

    int ii=0;
    for(int i=0;i<nA;++i) {
        FPBInventoryNetworkConfig* inv = (FPBInventoryNetworkConfig*)((char*)_D(outer)+_N(outer)*32);
        memset(inv,0,32);
        _N(outer)++;
        for(int s=0;s<S;++s) inv->CharacterSlots.Add(SE[s]);
        for(int s=0;s<S;++s) inv->InventoryItems.Add(allItems[ii++]);
    }
    {char b[99];sprintf_s(b,"[LSYNC] config built %d elements",_N(outer));ODS(b);}

    // ---- DUMP: raw FName hex ----
    {
        char b[256];
        void* rd = _D(cfgRA); int32 rn = _N(cfgRA);
        sprintf_s(b,"[LSYNC] DUMP cfgRA Data=%p Num=%d",rd,rn); ODS(b);
        if(rd&&rn>0){auto*f=(unsigned char*)rd;
            for(int i=0;i<rn&&i<8;i++){uint64_t fn=*(uint64_t*)(f+i*8);
                sprintf_s(b,"[LSYNC] Role[%d]=0x%016llX idx=%u num=%d",i,fn,(uint32)(fn&0xFFFFFFFF),(int32)(fn>>32));ODS(b);}}

        void* od=_D(outer); int32 on=_N(outer);
        sprintf_s(b,"[LSYNC] DUMP outer Data=%p Num=%d",od,on); ODS(b);
        if(od&&on>0){auto*e=(unsigned char*)od;
            void* csD=_D(*(FPBInventoryNetworkConfig*)e);
            int32 csN=_N(*(FPBInventoryNetworkConfig*)e);
            void* iiD=*(void**)(e+0x10);
            int32 iiN=*(int32*)(e+0x18);
            sprintf_s(b,"[LSYNC] DUMP elem0 cs.D=%p cs.N=%d ii.D=%p ii.N=%d",csD,csN,iiD,iiN);ODS(b);
            if(iiD&&iiN>0&&iiN<=10){auto*ifn=(unsigned char*)iiD;
                for(int s=0;s<iiN&&s<8;s++){uint64_t fn=*(uint64_t*)(ifn+s*8);
                    sprintf_s(b,"[LSYNC] Item[%d]=0x%016llX idx=%u num=%d",s,fn,(uint32)(fn&0xFFFFFFFF),(int32)(fn>>32));ODS(b);}}}}

    // Dump UFunction native pointer for IDA
    {
        UFunction* Func = PS->Class->GetFunction("PBPlayerState", "ClientInitFieldMod");
        char b[256];
        if (Func) {
            // SDK CoreUObject_classes.hpp:341-343
            // UFunction::FunctionFlags at +0xB0, UFunction::ExecFunction at +0xD8
            uintptr_t execFn = *(uintptr_t*)((char*)Func + 0xD8);
            uintptr_t rva = execFn - BaseAddress;
            sprintf_s(b,"[LSYNC] IDA: UFunction=%p ExecFunction=%p RVA=0x%llX flags=0x%08X",
                Func, (void*)execFn, (unsigned long long)rva, Func->FunctionFlags);
            ODS(b);
        } else {
            ODS("[LSYNC] IDA: GetFunction returned null!");
        }
    }

    // Send bulk init first (roles + quotas)
    ODS("[LSYNC] calling ClientInitFieldMod...");
    PS->ClientInitFieldMod(config, rIDs, quotas);
    ODS("[LSYNC] ClientInitFieldMod returned");

    // Then per-role refresh — may write to UPBFieldModManager
    int itemOff = 0;
    for (int i = 0; i < nA; ++i) {
        FPBInventoryNetworkConfig perRoleCfg;
        for (int s = 0; s < S; ++s) {
            perRoleCfg.CharacterSlots.Add(SE[s]);
            perRoleCfg.InventoryItems.Add(allItems[itemOff++]);
        }
        ODS(("[LSYNC] ClientRefreshRolePreOrderingInventory role[" + std::to_string(i) + "]...").c_str());
        PS->ClientRefreshRolePreOrderingInventory(roleFNs[i], perRoleCfg);
    }
    ODS("[LSYNC] all per-role refreshes done");

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

static bool IsSID(const std::string& s){if(s.length()!=17)return 0;for(char c:s)if(c<'0'||c>'9')return 0;return 1;}

void LoadoutSync::SelfTestStart(){
    static bool once=false;
    if(!once){once=true;ODS("[LSYNC] === DLL BUILD 20260808-v3 ===");}
}
void LoadoutSync::SelfTestDrain(){}

void LoadoutSync::OnPlayerJoined(AGameMode*, APBPlayerController* PC){
    if(!PC||!PC->PlayerState)return;
    auto*PS=static_cast<APBPlayerState*>(PC->PlayerState);
    std::string sid=PS->GetDefaultIDStr().ToString();
    ODS(("[LSYNC] OnPlayerJoined "+sid).c_str());
    if(!IsSID(sid)){ODS("[LSYNC] SKIP not SteamID");return;}
    std::thread([PC,sid](){
        ODS(("[LSYNC][W] HTTP "+sid).c_str());
        std::string json=HttpGet(sid);
        if(json.empty()){ODS("[LSYNC][W] HTTP empty");return;}
        {char b[99];sprintf_s(b,"[LSYNC][W] queued %zuB",json.size());ODS(b);}
        std::lock_guard<std::mutex> lk(LoadoutSync::g_Mutex);
        LoadoutSync::g_Pending.push_back({PC,std::move(json)});
    }).detach();
}

void LoadoutSync::DrainPending(){
    if(LoadoutSync::g_Pending.empty())return;
    std::vector<LoadoutSync::PendingLoadout> batch;
    {std::lock_guard<std::mutex> lk(LoadoutSync::g_Mutex);batch.swap(LoadoutSync::g_Pending);}
    {char b[99];sprintf_s(b,"[LSYNC] DrainPending %zu entries",batch.size());ODS(b);}
    for(auto&p:batch){
        if(!p.PC)continue;
        auto*PS=static_cast<APBPlayerState*>(p.PC->PlayerState);
        if(!PS)continue;
        ParseAndPush(p.json,PS);
    }
}
