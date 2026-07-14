#include "jb_vip_kits.h"
#include <random>
#include <cstdio>
#include <algorithm>

#define MAX_PLAYERS 64

#define CS_TEAM_NONE 0
#define CS_TEAM_SPECTATOR 1
#define CS_TEAM_T 2
#define CS_TEAM_CT 3

jb_vip_kits g_jb_vip_kits;
PLUGIN_EXPOSE(jb_vip_kits, g_jb_vip_kits);

// SYSTEM API`s
IVEngineServer2* engine = nullptr;
CGlobalVars* gpGlobals = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

// API
IUtilsApi* utils;
IJailbreakApi* jailbreak_api;
IVIPApi* vip_api;
IMenusApi* menus_api;
IPlayersApi* players_api;
// =========================================
//  VARS
// =========================================
std::map<std::string, std::string> phrases;

int g_iKitCooldown[MAX_PLAYERS+1] = {0};
std::unordered_map<int,float> g_mGrenadeDamage;

// =========================================
// CONFIG VARS
// =========================================
bool g_bFreedayBlock;
bool g_bGameDayBlock;


struct KitWeapon{
    std::string sWeaponClassname;

    // Base Weapon
    int iClip1;
    int iClip2;

    // Grenade
    float flGrenadeDamage;
};
struct KitInfo{
    std::string sDisplayName;
    int iCooldown;

    std::vector<KitWeapon> vWeapons;
};

std::unordered_map<std::string,KitInfo> g_mKits;

//==========================================
// HELPERS
//==========================================
std::vector<std::string> ParseStringList(const char* str) {
    std::vector<std::string> result;
    
    if (!str || str[0] == '\0') return result;

    const char* current = str;

    while (*current != '\0') {
        
        while (*current == ' ' || *current == '\t') {
            current++;
        }
        if (*current == '\0') break;
        const char* end = current;
        while (*end != '\0' && *end != ',') {
            end++;
        }

        const char* tail = end - 1;
        while (tail > current && (*tail == ' ' || *tail == '\t')) {
            tail--;
        }

        if (tail >= current) {
            size_t length = tail - current + 1;
            result.push_back(std::string(current, length));
        }

        current = end;
        if (*current == ',') {
            current++;
        }
    }

    return result;
}

// =========================================
// CONFIGS 
// =========================================
void LoadConfig() {
    g_mKits.clear();
    KeyValues* config = new KeyValues("Config");
    const char* path = "addons/configs/Jailbreak/vip_kits.ini";
    if (!config->LoadFromFile(g_pFullFileSystem, path)) {
        utils->ErrorLog("%s Failed to load: %s",g_PLAPI->GetLogTag(), path);
        delete config;
        return;
    }

    g_bFreedayBlock = config->GetBool("freeday_block",true);
    g_bGameDayBlock = config->GetBool("gameday_block",true);


    auto kitskey = config->FindKey("Kits");
    if (kitskey) {
        for (auto kv = kitskey->GetFirstTrueSubKey();kv;kv = kv->GetNextTrueSubKey()) {
            std::string kitName = kv->GetName();

            std::string display = kv->GetString("display",kitName.c_str());
            int cooldown = kv->GetInt("cooldown",0);


            KitInfo data;
            data.iCooldown = cooldown;
            data.sDisplayName = display;

            auto weapons = kv->FindKey("weapons");
            if (weapons) {
                for (auto kv2 = weapons->GetFirstTrueSubKey();kv2;kv2 = kv2->GetNextTrueSubKey()) {
                    std::string weaponClassname = kv2->GetName();

                    int clip1 = kv2->GetInt("clip1",-1);
                    int clip2 = kv2->GetInt("clip2",-1);
                    float flDamage = kv2->GetFloat("damage",-1.0f);

                    KitWeapon weaponData;
                    weaponData.sWeaponClassname = weaponClassname;
                    weaponData.iClip1 = clip1;
                    weaponData.iClip2 = clip2;
                    weaponData.flGrenadeDamage = flDamage;
                    data.vWeapons.push_back(weaponData);
                }
            }
            g_mKits[kitName] = data;
        }



    } else {
        META_CONPRINTF("%s Failed to find 'Kits' key in config.\n",g_PLAPI->GetLogTag());
    }


    delete config;
}

void LoadTranslations() {
    phrases.clear();
    KeyValues* g_kvPhrases = new KeyValues("Phrases");
    const char *pszPath = "addons/translations/jailbreak_kits.phrases.txt";

    if (!g_kvPhrases->LoadFromFile(g_pFullFileSystem, pszPath))
    {
        utils->ErrorLog("%s Failed to load %s", g_PLAPI->GetLogTag(), pszPath);
        delete g_kvPhrases;
        return;
    }

    const char* language = utils->GetLanguage();

    for (KeyValues *pKey = g_kvPhrases->GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey()) {
        phrases[std::string(pKey->GetName())] = std::string(pKey->GetString(language));
    }
    delete g_kvPhrases;
}

const char* GetTranslation(const char* key) {
    auto it = phrases.find(key);
    if (it == phrases.end()) return key;
    else return it->second.c_str();
}

void PrintSlotPrefixed(int iSlot, const char* content) {
    if (!content || content[0] == '\0') return;
    char buf[512];
    g_SMAPI->Format(buf, sizeof(buf), "%s %s", GetTranslation("Prefix"), content);
    utils->PrintToChat(iSlot, "%s",buf);
}

void PrintAllPrefixed(const char* content) {
    if (!content || content[0] == '\0') return;
    char buf[512];
    g_SMAPI->Format(buf, sizeof(buf), "%s %s", GetTranslation("Prefix"), content);
    utils->PrintToChatAll("%s",buf);
}

// =========================================
// OTHER
// =========================================

class JB_EntityListener : public IEntityListener {
public:
    void OnEntitySpawned(CEntityInstance* pEntity) override;
};

JB_EntityListener g_pEntityListener;

void JB_EntityListener::OnEntitySpawned(CEntityInstance* pEntity)
{
    if (!pEntity) return;

    if (strcmp(pEntity->GetClassname(), "hegrenade_projectile") == 0) {
        
        CBaseEntity* pBaseEntity = dynamic_cast<CBaseEntity*>(pEntity);
        if (!pBaseEntity) return;

        CHandle<CBaseEntity> hEnt = pBaseEntity->GetHandle();

        utils->NextFrame([hEnt]() {
            CBaseGrenade* pGrenade = dynamic_cast<CBaseGrenade*>(hEnt.Get());
            if (!pGrenade) return;

            auto pPawn = pGrenade->m_hThrower.Get();
            if (!pPawn) return;


            auto pController = pPawn->m_hController.Get();
            if (!pController) return;


            int iSlot = pController->GetPlayerSlot(); 
            if (iSlot < 0 || iSlot > 64) return;

            if (g_mGrenadeDamage.count(iSlot) && g_mGrenadeDamage[iSlot] != -1.0f) {

                pGrenade->m_flDamage = g_mGrenadeDamage[iSlot];
                
                g_mGrenadeDamage[iSlot] = -1.0f; 
            }
        });
    }
}

bool HasFirearms(CCSPlayerController* pController) {
    if (!pController) return false;

    auto pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) return false;

    auto WeaponService = pPawn->m_pWeaponServices.Get();
    if (!WeaponService) return false;

    auto myWeapons = WeaponService->m_hMyWeapons.Get();
    if (!myWeapons) return false;

    for (int i = 0; i < myWeapons->Count(); i++) {
        auto WeaponHandle = myWeapons->Element(i);
        if (!WeaponHandle.IsValid()) continue;

        auto pWeapon = WeaponHandle.Get();
        if (!pWeapon) continue;

        std::string classname = pWeapon->GetClassname();

        if (classname.find("knife") != std::string::npos) {
            continue;
        }

        if (classname.find("grenade") != std::string::npos || 
            classname == "weapon_flashbang" || 
            classname == "weapon_decoy" || 
            classname == "weapon_molotov") {
            continue;
        }

        return true; 
    }

    return false;
}

void GivePlayerKit(int iSlot, const char* szKitKey) {
    if (iSlot < 0 || iSlot > 64) return;
    
    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController) return;

    auto pPawn = pController->GetPlayerPawn();
    if (!pPawn || !pPawn->IsAlive()) return;

    auto itemService = pPawn->m_pItemServices.Get();
    if (!itemService) return;

    auto kit = g_mKits.find(szKitKey);
    if (kit == g_mKits.end()) return;

    for(auto &weapon : kit->second.vWeapons) {
        
        itemService->GiveNamedItem(weapon.sWeaponClassname.c_str());

        if (weapon.flGrenadeDamage != -1.0f) {
            g_mGrenadeDamage[iSlot] = weapon.flGrenadeDamage;
        }

        if (weapon.iClip1 == -1 && weapon.iClip2 == -1) continue;
        
        auto WeaponService = pPawn->m_pWeaponServices.Get();
        if (!WeaponService) continue;

        auto myWeapons = WeaponService->m_hMyWeapons.Get(); 
        
        for (int i = 0; i < myWeapons->Count(); i++) {
            auto WeaponHandle = myWeapons->Element(i); 
            
            if (!WeaponHandle.IsValid()) continue;

            auto pWeapon = WeaponHandle.Get();
            if (!pWeapon) continue;

            if (weapon.sWeaponClassname == pWeapon->GetClassname()) {
                
                if (weapon.iClip1 != -1) {
                    pWeapon->m_iClip1 = weapon.iClip1;
                    utils->SetStateChanged(pWeapon, "CBasePlayerWeapon", "m_iClip1");
                }

                if (weapon.iClip2 != -1) {
                    pWeapon->m_iClip2 = weapon.iClip2;
                    pWeapon->m_pReserveAmmo = weapon.iClip2; 
                    utils->SetStateChanged(pWeapon, "CBasePlayerWeapon", "m_iClip2");
                }

                break;
            }
        }
    }
}


bool OpenKitsMenu(int iSlot, const char* szFeature){
    if (iSlot < 0 || iSlot > 64) return true;
    if (g_iKitCooldown[iSlot] >= 1) {
        PrintSlotPrefixed(iSlot,GetTranslation("Kits_CooldownActive"));
        return false;
    }
    if (g_bFreedayBlock && jailbreak_api->IsGlobalFreedayToday()) {
        PrintSlotPrefixed(iSlot, GetTranslation("Kits_FreedayBlock"));
        return false;
    }
    if (g_bGameDayBlock && jailbreak_api->IsGameDayToday()) {
        PrintSlotPrefixed(iSlot, GetTranslation("Kits_GameDayBlock"));
        return false;
    }

    auto pController = CCSPlayerController::FromSlot(iSlot);
    if (!pController) return false;
    if (pController->GetTeam() != CS_TEAM_T) {
        PrintSlotPrefixed(iSlot,"KitsMenu_OnlyTAvailable");
        return false;
    }
    Menu hMenu;

    menus_api->SetTitleMenu(hMenu,GetTranslation("KitsMenu_Title"));

    std::vector<std::string> vPlayerKits = ParseStringList(vip_api->VIP_GetClientFeatureString(iSlot,"jb_kits"));

    if (vPlayerKits.empty()) {
        menus_api->AddItemMenu(hMenu,"",GetTranslation("KitsMenu_NoAvailableForYou"),ITEM_DISABLED);
    } else {
        for (const auto& kitName : vPlayerKits) {
            auto it = g_mKits.find(kitName);
            
            if (it != g_mKits.end()) {
                menus_api->AddItemMenu(hMenu, kitName.c_str(), it->second.sDisplayName.c_str(), ITEM_DEFAULT);
            }
        }
    }

    

    menus_api->SetExitMenu(hMenu,true);

    menus_api->SetCallback(hMenu,[](const char* szBack, const char* szFront, int iItem, int iSlot){
        if (!szBack || szBack[0] == '\0') return;
        if (strcmp(szBack,"exit") == 0) {
            vip_api->VIP_OpenMenu(iSlot);
            return;
        }
        if (iItem < 7) {
            auto pController = CCSPlayerController::FromSlot(iSlot);
            if (!pController) return;
            auto pPawn = pController->GetPlayerPawn();
            if (!pPawn) return; 
            if (!pPawn->IsAlive()) {
                PrintSlotPrefixed(iSlot,GetTranslation("KitsMenu_CantTakeCuzDead"));
                return;
            }
            if (g_iKitCooldown[iSlot] >= 1) {
                PrintSlotPrefixed(iSlot,GetTranslation("Kits_CooldownActive"));
                menus_api->ClosePlayerMenu(iSlot);
                return;
            }
            if (HasFirearms(pController)) {
                PrintSlotPrefixed(iSlot,GetTranslation("Kits_DropAllFirearms"));
                menus_api->ClosePlayerMenu(iSlot);
                return;
            }
            auto it = g_mKits.find(szBack);
            if (it != g_mKits.end()) {
                g_iKitCooldown[iSlot] = it->second.iCooldown;
                GivePlayerKit(iSlot,it->first.c_str());
                menus_api->ClosePlayerMenu(iSlot);
            }
        }
    });

    menus_api->DisplayPlayerMenu(hMenu,iSlot,true,true);
    return false;
}


CGameEntitySystem* GameEntitySystem() {
    return utils ? utils->GetCGameEntitySystem() : nullptr;
}



void StartupServer() {
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = utils->GetCEntitySystem();
    gpGlobals = utils->GetCGlobalVars();
    g_pGameEntitySystem->AddListenerEntity(&g_pEntityListener);

    for (int i = 0; i < MAX_PLAYERS;i++) {
        g_iKitCooldown[i] = 0;
    }
}

bool jb_vip_kits::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) {
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);

    ConVar_Register(FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);
    g_SMAPI->AddListener(this, this);

    return true;
}



void jb_vip_kits::AllPluginsLoaded() {
    int ret;
    utils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    jailbreak_api =(IJailbreakApi*)g_SMAPI->MetaFactory(JAILBREAK_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Jailbreak Core plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    vip_api = (IVIPApi*)g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Vip Core plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    menus_api = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Utils plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    players_api = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Utils plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    LoadConfig();
    LoadTranslations();

    vip_api->VIP_RegisterFeature("jb_kits",VIP_STRING,SELECTABLE,OpenKitsMenu,nullptr,nullptr);

    utils->HookEvent(g_PLID,"round_start",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        g_mGrenadeDamage.clear();
        if (g_bFreedayBlock && jailbreak_api->IsGlobalFreedayToday()) {
            return false;
        }
        if (g_bGameDayBlock && jailbreak_api->IsGameDayToday()) {
            return false;
        }
        for (int i = 0; i < MAX_PLAYERS; i++) {
            auto pController = CCSPlayerController::FromSlot(i);
            if (!pController || !pController->IsConnected()) continue;
            auto pPawn = pController->GetPlayerPawn();
            if (!pPawn) continue;
            if (g_iKitCooldown[i] <= 0) {
                g_iKitCooldown[i] = 0;
                if (vip_api->VIP_IsClientVIP(i) && vip_api->VIP_GetClientFeatureString(i,"jb_kits")[0] != '\0'){
                    if (pController->GetTeam() == CS_TEAM_T){
                        PrintSlotPrefixed(i,GetTranslation("Kits_KitAvailable"));
                    }
                }
                continue;
            } else --g_iKitCooldown[i];
        }
    });

    utils->HookEvent(g_PLID,"player_disconnect",[](const char* szName, IGameEvent* pEvent, bool bDontBroadcast){
        int iSlot = pEvent->GetInt("userid");
        g_iKitCooldown[iSlot] = 0;
        auto it = g_mGrenadeDamage.find(iSlot);
        if (it != g_mGrenadeDamage.end()){
            g_mGrenadeDamage.erase(it);
        }
    });

    


    utils->StartupServer(g_PLID, StartupServer);

}

bool jb_vip_kits::Unload(char* error, size_t maxlen) {
    g_pGameEntitySystem->RemoveListenerEntity(&g_pEntityListener);
    jailbreak_api->ClearAllPluginHooks(g_PLID);
    utils->ClearAllHooks(g_PLID);
    ConVar_Unregister();

   
    return true;
}

const char* jb_vip_kits::GetAuthor() { return "niffox"; }
const char* jb_vip_kits::GetDate() { return __DATE__; }
const char* jb_vip_kits::GetDescription() { return "[JB] Vip Kits"; }
const char* jb_vip_kits::GetLicense() { return "Private"; }
const char* jb_vip_kits::GetLogTag() { return "[JB] Vip Kits"; }
const char* jb_vip_kits::GetName() { return "[JB] Vip Kits"; }
const char* jb_vip_kits::GetURL() { return "https://t.me/niffox_2q"; }
const char* jb_vip_kits::GetVersion() { return "1.0.2"; }