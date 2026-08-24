#include "stdafx.h"
#include "GameLogic/Combat/SkillExecution.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>

#include "Engine/AI/ZzzAI.h"
#include "Engine/Object/ZzzCharacter.h"
#include "Engine/Object/ZzzInterface.h"
#include "Engine/Object/PlayerActionState.h"
#include "UI/NewUI/NewUISystem.h"
#include "Core/Utilities/Log/muConsoleDebug.h"
#include "Character/CharacterManager.h"
#include "GameLogic/Skills/SkillManager.h"
#include "GameLogic/Social/PartyManager.h"
#include "World/MapInfra/MapManager.h"
#include "Network/Server/WSclient.h"

#include "MuHelper.h"
#include "MuHelperComboChain.h"

bool g_MuHelperComboForceChain = false;

namespace
{
    constexpr unsigned kComboWindowMs = 2500;

    unsigned long long ComboNowMs()
    {
        using Clock = std::chrono::steady_clock;
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
    }

    // Same tile basis CheckTile / mob targeting use (Object.Position), not the
    // path-slot PositionX/Y which lag while the hero is mid-walk.
    POINT HeroTilePos()
    {
        return {
            (int)(Hero->Object.Position[0] / TERRAIN_SCALE),
            (int)(Hero->Object.Position[1] / TERRAIN_SCALE)
        };
    }
}

constexpr int MAX_ACTIONABLE_DISTANCE = 10;
constexpr int DEFAULT_DURABILITY_THRESHOLD = 50;

SpinLock _targetsLock;
SpinLock _itemsLock;

// Movement/target globals are defined in ZzzInterface.cpp.
extern MovementSkill g_MovementSkill;
extern int SelectedCharacter;
extern int TargetX;
extern int TargetY;
extern int EnableUse;

namespace MUHelper
{
	MovementSkill& g_MovementSkill = ::g_MovementSkill;
	int& SelectedCharacter = ::SelectedCharacter;
	int& TargetX = ::TargetX;
	int& TargetY = ::TargetY;

    CMuHelper g_MuHelper;

    namespace
    {
        // ~5 s at 250 ms ticks — unstick if a walk never cleared Movement.
        constexpr int kMovementWaitTicksMax = 20;
        // Only force-clear EnableUse if a ConsumeItem reply never arrives (~5 s).
        constexpr int kEnableUseStuckTicksMax = 20;
    }

    void CMuHelper::ResetRuntimeState()
    {
        DeleteAllTargets();

        _itemsLock.lock();
        m_setItems.clear();
        _itemsLock.unlock();

        m_iCurrentItem = MAX_ITEMS;
        m_iComboState = 0;
        m_bComboStepPending = false;
        m_ullLastComboSkillMs = 0;
        m_iLastComboTargetId = -1;
        m_iCurrentBuffIndex = 0;
        m_iCurrentBuffPartyIndex = 0;
        m_iCurrentHealPartyIndex = 0;
        m_iCurrentSkill = (ActionSkillType)m_config.aiSkill[0];
        m_iSecondsElapsed = 0;
        m_iSecondsAway = 0;
        m_iLoopCounter = 0;
        m_iMovementWaitTicks = 0;
        m_iEnableUseStuckTicks = 0;
        m_bTimerActivatedBuffOngoing = false;
        m_bPetActivated = false;
        m_iTotalCost = 0;
        g_MuHelperComboForceChain = false;
    }

    void CALLBACK CMuHelper::TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
    {
        g_MuHelper.WorkLoop(hwnd, uMsg, idEvent, dwTime);
    }

    void CMuHelper::Save(const ConfigData& config)
    {
        m_config = config;

        PRECEIVE_MUHELPER_DATA netData;
        ConfigDataSerDe::Serialize(m_config, netData);

        SocketClient->ToGameServer()->SendMuHelperSaveDataRequest(reinterpret_cast<BYTE*>(&netData), sizeof(netData));
    }

    void CMuHelper::Load(const ConfigData& config)
    {
        m_config = config;
    }

    ConfigData CMuHelper::GetConfig() const {
        return m_config;
    }

    void CMuHelper::Toggle()
    {
        if (m_bActive)
        {
            TriggerStop();

            // Stop the client-driven bot immediately instead of waiting for the
            // server's status reply. After an auto-reconnect the server's new
            // session doesn't have the helper marked active, so it never replies
            // and the bot would otherwise keep running with no way to stop it.
            Stop();
        }
        else
        {
            TriggerStart();
        }
    }

    void CMuHelper::TriggerStart()
    {
        if (!Hero->SafeZone)
            SocketClient->ToGameServer()->SendMuHelperStatusChangeRequest(0);
    }

    void CMuHelper::TriggerStop()
    {
        SocketClient->ToGameServer()->SendMuHelperStatusChangeRequest(1);
    }

    void CMuHelper::Start()
    {
        if (m_bActive)
        {
            return;
        }

        ResetRuntimeState();
        m_posOriginal = HeroTilePos();

        m_iHuntingDistance = ComputeDistanceByRange(m_config.iHuntingRange);
        m_iObtainingDistance = ComputeDistanceByRange(m_config.iObtainingRange);

        m_bActive = true;
        g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Started");
    }

    void CMuHelper::Stop()
    {
        m_bActive = false;
        ResetRuntimeState();
        g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Stopped");
    }

    void CMuHelper::WorkLoop(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
    {
        if (!m_bActive)
        {
            return;
        }

        if (Hero->SafeZone)
        {
            g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Entered safezone. Stopping.");
            TriggerStop();
            Stop();
            return;
        }

        // EnableUse is meant to stay set until the server ack. Only clear it if
        // that ack never arrives (known stuck-client bug), not every helper tick.
        if (EnableUse > 0)
        {
            if (++m_iEnableUseStuckTicks > kEnableUseStuckTicksMax)
            {
                EnableUse = 0;
                m_iEnableUseStuckTicks = 0;
            }
        }
        else
        {
            m_iEnableUseStuckTicks = 0;
        }

        if (Hero->Movement)
        {
            // Long regroup paths are fine; only unstick when the path is empty /
            // finished client-side but Movement never cleared.
            const bool bPathLooksStuck =
                Hero->Path.PathNum <= 1
                || Hero->Path.CurrentPath >= Hero->Path.PathNum;

            if (bPathLooksStuck && ++m_iMovementWaitTicks > kMovementWaitTicksMax)
            {
                LetHeroStop(Hero, TRUE);
                Hero->Movement = false;
                m_iMovementWaitTicks = 0;
            }
            else if (!bPathLooksStuck)
            {
                m_iMovementWaitTicks = 0;
            }
        }
        else
        {
            m_iMovementWaitTicks = 0;
        }

        Work();

        if (m_iLoopCounter++ == 4)
        {
            m_iSecondsElapsed++;

            // Periodic hygiene so 24/7 farm cannot grow stale target/item sets.
            CleanupTargets();
            CleanupItems();

            if (ComputeDistanceBetween(HeroTilePos(), m_posOriginal) > 1)
            {
                m_iSecondsAway++;
            }
            else
            {
                m_iSecondsAway = 0;
            }

            m_iLoopCounter = 0;
        }
    }

    void CMuHelper::Work()
    {
        try
        {
            if (!ActivatePet())
            {
                return;
            }

            if (!Buff())
            {
                return;
            }

            if (!RecoverHealth())
            {
                return;
            }

            if (!ObtainItem())
            {
                return;
            }

            if (!Regroup())
            {
                return;
            }

            Attack();

            RepairEquipments();
        }
        catch (...)
        {
            g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Exception occurred. Ignoring...");
        }
    }

    void CMuHelper::AddTarget(int iTargetId, bool bIsAttacking)
    {
        if (!m_bActive)
        {
            return;
        }

        CHARACTER* pTarget = FindCharacterByKey(iTargetId);
        if (!pTarget || pTarget == Hero)
        {
            return;
        }

        int iDistance = ComputeDistanceFromTarget(pTarget);

        if ((iDistance <= m_iHuntingDistance)
            || (bIsAttacking && m_config.bLongRangeCounterAttack))
        {
            _targetsLock.lock();

            m_setTargets.insert(iTargetId);

            if (bIsAttacking)
            {
                m_setTargetsAttacking.insert(iTargetId);
            }

            _targetsLock.unlock();
        }

        if (m_config.bUseSelfDefense && IsMonster(pTarget))
        {
            m_iCurrentTarget = iTargetId;
        }
    }

    void CMuHelper::DeleteTarget(int iTargetId)
    {
        _targetsLock.lock();

        m_setTargets.erase(iTargetId);
        m_setTargetsAttacking.erase(iTargetId);

        _targetsLock.unlock();

        if (iTargetId == m_iCurrentTarget)
        {
            m_iCurrentTarget = -1;
            // Restart combo on the next mob; otherwise Twisting/Death Stab fire
            // without a prior Cyclone and OpenMU never awards the combo.
            m_iComboState = 0;
            m_bComboStepPending = false;
        }
    }

    void CMuHelper::DeleteAllTargets()
    {
        _targetsLock.lock();

        m_setTargets.clear();
        m_setTargetsAttacking.clear();

        _targetsLock.unlock();

        m_iCurrentTarget = -1;
        m_iComboState = 0;
        m_bComboStepPending = false;
        m_ullLastComboSkillMs = 0;
    }

    int CMuHelper::ComputeDistanceByRange(int iRange)
    {
        return ComputeDistanceBetween({ 0, 0 }, { iRange, iRange });
    }

    int CMuHelper::ComputeDistanceFromTarget(CHARACTER* pTarget)
    {
        const POINT posHero = HeroTilePos();

        const POINT posCurrent = { pTarget->PositionX, pTarget->PositionY };
        const POINT posNext    = { pTarget->TargetX,   pTarget->TargetY };

        return std::min(
            ComputeDistanceBetween(posHero, posCurrent),
            ComputeDistanceBetween(posHero, posNext)
        );
    }

    int CMuHelper::ComputeDistanceBetween(POINT posA, POINT posB)
    {
        int iDx = posA.x - posB.x;
        int iDy = posA.y - posB.y;

        return static_cast<int>(std::ceil(std::sqrt(iDx * iDx + iDy * iDy)));
    }

    int CMuHelper::GetNearestTarget()
    {
        int iClosestMonsterId = -1;
        int iMinDistance = m_iHuntingDistance;
        std::set<int> setTargets;
        {
            _targetsLock.lock();
            setTargets = m_setTargets;
            _targetsLock.unlock();
        }

        for (const int& iMonsterId : setTargets)
        {
            int iIndex = FindCharacterIndex(iMonsterId);
            if (iIndex == MAX_CHARACTERS_CLIENT)
            {
                continue;
            }

            CHARACTER* pTarget = &CharactersClient[iIndex];

            if (!IsMonster(pTarget))
            {
                continue;
            }

            int iDistance = ComputeDistanceFromTarget(pTarget);
            if (iDistance <= iMinDistance)
            {
                iMinDistance = iDistance;
                iClosestMonsterId = iMonsterId;
            }
        }

        return iClosestMonsterId;
    }

    int CMuHelper::GetFarthestAttackingTarget()
    {
        int iFarthestMonsterId = -1;
        int iMaxDistance = -1;

        std::set<int> setTargets;
        {
            _targetsLock.lock();
            setTargets = m_setTargetsAttacking;
            _targetsLock.unlock();
        }

        for (const int& iMonsterId : setTargets)
        {
            int iIndex = FindCharacterIndex(iMonsterId);
            if (iIndex == MAX_CHARACTERS_CLIENT)
            {
                continue;
            }

            CHARACTER* pTarget = &CharactersClient[iIndex];

            if (!IsMonster(pTarget))
            {
                continue;
            }

            int iDistance = ComputeDistanceFromTarget(pTarget);
            if (iDistance > iMaxDistance)
            {
                iMaxDistance = iDistance;
                iFarthestMonsterId = iMonsterId;
            }
        }

        return iFarthestMonsterId;
    }

    void CMuHelper::CleanupTargets()
    {
        std::set<int> setTargets;
        {
            _targetsLock.lock();
            setTargets = m_setTargets;
            _targetsLock.unlock();
        }

        for (const int& iMonsterId : setTargets)
        {
            int iIndex = FindCharacterIndex(iMonsterId);
            if (iIndex == MAX_CHARACTERS_CLIENT)
            {
                DeleteTarget(iMonsterId);
                continue;
            }

            CHARACTER* pTarget = &CharactersClient[iIndex];
            if (pTarget->Dead > 0 || !pTarget->Object.Live)
            {
                DeleteTarget(iMonsterId);
                continue;
            }

            // Drop targets that left the hunt radius so the set cannot grow 24/7.
            // Keep: current target, and long-range attackers (counter-attack mode).
            if (iMonsterId == m_iCurrentTarget)
            {
                continue;
            }

            bool bIsAttackingTarget = false;
            {
                _targetsLock.lock();
                bIsAttackingTarget = m_setTargetsAttacking.count(iMonsterId) > 0;
                _targetsLock.unlock();
            }

            if (bIsAttackingTarget && m_config.bLongRangeCounterAttack)
            {
                continue;
            }

            if (ComputeDistanceFromTarget(pTarget) > m_iHuntingDistance + 2)
            {
                DeleteTarget(iMonsterId);
            }
        }
    }

    void CMuHelper::CleanupItems()
    {
        std::set<int> setItems;
        {
            _itemsLock.lock();
            setItems = m_setItems;
            _itemsLock.unlock();
        }

        for (const int& iItemId : setItems)
        {
            if (iItemId < 0 || iItemId >= MAX_ITEMS || !Items[iItemId].Object.Live)
            {
                DeleteItem(iItemId);
            }
        }
    }

    int CMuHelper::ActivatePet()
    {
        if (!m_config.bUseDarkRaven)
        {
            return 1;
        }

        if (m_bPetActivated)
        {
            return 1;
        }

        if (m_config.iDarkRavenMode == PET_ATTACK_CEASE)
        {
            SocketClient->ToGameServer()->SendPetCommandRequest(PetType::DarkRaven, PetCommandMode::Normal, 0xFFFF);
        }
        else if (m_config.iDarkRavenMode == PET_ATTACK_AUTO)
        {
            SocketClient->ToGameServer()->SendPetCommandRequest(PetType::DarkRaven, PetCommandMode::AttackRandom, 0xFFFF);
        }
        else if (m_config.iDarkRavenMode == PET_ATTACK_TOGETHER)
        {
            SocketClient->ToGameServer()->SendPetCommandRequest(PetType::DarkRaven, PetCommandMode::AttackWithOwner, 0xFFFF);
        }

        m_bPetActivated = true;
        return 1;
    }

    int CMuHelper::Buff()
    {
        if (!HasAssignedBuffSkill())
        {
            return 1;
        }

        if (m_config.bSupportParty && g_pPartyManager->IsPartyActive())
        {
            m_iCurrentBuffPartyIndex %= PartyNumber;

            PARTY_t* pMember = &Party[m_iCurrentBuffPartyIndex];
            CHARACTER* pChar = g_pPartyManager->GetPartyMemberChar(pMember);
            int iBuffResult = 1;

            if (pChar != NULL
                && ComputeDistanceFromTarget(pChar) <= MAX_ACTIONABLE_DISTANCE)
            {
                if (!m_config.bBuffDurationParty
                    && m_config.iBuffCastInterval != 0
                    && m_iSecondsElapsed % m_config.iBuffCastInterval == 0)
                {
                    m_bTimerActivatedBuffOngoing = true;
                }

                iBuffResult = BuffTarget(pChar, (ActionSkillType)m_config.aiBuff[m_iCurrentBuffIndex]);
            }

            m_iCurrentBuffPartyIndex = (m_iCurrentBuffPartyIndex + 1) % PartyNumber;

            if (m_iCurrentBuffPartyIndex == 0)
            {
                m_iCurrentBuffIndex = (m_iCurrentBuffIndex + 1) % m_config.aiBuff.size();

                // Reaching this branch means everyone's been buffed, 
                // so we're resetting the timer activated buff flag
                if (m_iCurrentBuffIndex == 0)
                {
                    m_bTimerActivatedBuffOngoing = false;
                }
            }

            return iBuffResult;
        }
        else
        {
            m_iCurrentBuffPartyIndex = 0;

            if (!m_config.bBuffDuration
                && m_config.iBuffCastInterval != 0
                && m_iSecondsElapsed % m_config.iBuffCastInterval == 0)
            {
                m_bTimerActivatedBuffOngoing = true;
            }

            if (!BuffTarget(Hero, (ActionSkillType)m_config.aiBuff[m_iCurrentBuffIndex]))
            {
                return 0;
            }
        }

        if (m_iCurrentBuffPartyIndex == 0)
        {
            m_iCurrentBuffIndex = (m_iCurrentBuffIndex + 1) % m_config.aiBuff.size();

            // Reaching this branch means everyone's been buffed, 
            // so we're resetting the timer activated buff flag
            if (m_iCurrentBuffIndex == 0)
            {
                m_bTimerActivatedBuffOngoing = false;
            }
        }

        return 1;
    }

    int CMuHelper::BuffTarget(CHARACTER* pTargetChar, ActionSkillType iBuffSkill)
    {
        if (iBuffSkill == 0 || iBuffSkill == AT_SKILL_UNDEFINED)
        {
            return 1;
        }

        OBJECT* obj = &pTargetChar->Object;

        auto CastIfMissing = [&](bool bBuffActive, bool bTimerRespected, bool bNeedsTarget) -> int
        {
            if (!bBuffActive || (bTimerRespected && m_bTimerActivatedBuffOngoing))
                return SimulateSkill(iBuffSkill, bNeedsTarget, pTargetChar->Key);
            return 1;
        };

        switch (iBuffSkill)
        {
        case AT_SKILL_ATTACK:
        case AT_SKILL_ATTACK_STR:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Attack), true, true);

        case AT_SKILL_DEFENSE:
        case AT_SKILL_DEFENSE_STR:
        case AT_SKILL_DEFENSE_MASTERY:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Defense), true, true);

        case AT_SKILL_INFINITY_ARROW:
        case AT_SKILL_INFINITY_ARROW_STR:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_InfinityArrow), false, false);

        case AT_SKILL_SOUL_BARRIER:
        case AT_SKILL_SOUL_BARRIER_STR:
        case AT_SKILL_SOUL_BARRIER_PROFICIENCY:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_WizDefense), true, true);

        case AT_SKILL_SWELL_LIFE:
        case AT_SKILL_SWELL_LIFE_STR:
        case AT_SKILL_SWELL_LIFE_PROFICIENCY:
            if (m_iComboState == 2)
            {
                return 1;
            }
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Life), true, false);

        case AT_SKILL_EXPANSION_OF_WIZARDRY:
        case AT_SKILL_EXPANSION_OF_WIZARDRY_STR:
        case AT_SKILL_EXPANSION_OF_WIZARDRY_MASTERY:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_SwellOfMagicPower), false, false);

        case AT_SKILL_ADD_CRITICAL:
        case AT_SKILL_ADD_CRITICAL_STR1:
        case AT_SKILL_ADD_CRITICAL_STR2:
        case AT_SKILL_ADD_CRITICAL_STR3:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_AddCriticalDamage), false, false);

        case AT_SKILL_ALICE_BERSERKER:
        case AT_SKILL_ALICE_BERSERKER_STR:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Berserker), false, false);

        case AT_SKILL_ALICE_THORNS:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Thorns), false, false);

        // Rage Fighter party buffs — self/party AoE, no explicit target needed.
        case AT_SKILL_ATT_UP_OURFORCES:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Att_up_Ourforces), true, false);

        case AT_SKILL_HP_UP_OURFORCES:
        case AT_SKILL_HP_UP_OURFORCES_STR:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Hp_up_Ourforces), true, false);

        case AT_SKILL_DEF_UP_OURFORCES:
        case AT_SKILL_DEF_UP_OURFORCES_STR:
        case AT_SKILL_DEF_UP_OURFORCES_MASTERY:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Def_up_Ourforces), true, false);

        default:
            return 1;
        }
    }


    int CMuHelper::ConsumePotion()
    {
        int64_t iLife = CharacterAttribute->Life;
        int64_t iLifeMax = CharacterAttribute->LifeMax;

        if (m_config.bUseHealPotion && iLifeMax > 0 && iLife > 0)
        {
            int64_t iRemaining = (iLife * 100 + iLifeMax - 1) / iLifeMax;
            if (iRemaining <= m_config.iPotionThreshold)
            {
                int iPotionIndex = g_pMyInventory->FindHealingItemIndex();
                if (iPotionIndex != -1)
                {
                    SendRequestUse(iPotionIndex, 0);
                }
            }
        }

        return 1;
    }

    int CMuHelper::RecoverHealth()
    {
        if (!Heal())
        {
            return 0;
        }
        
        if (!DrainLife())
        {
            return 0;
        }

        if (!ConsumePotion())
        {
            return 0;
        }

        return 1;
    }

    int CMuHelper::Heal()
    {
        if (!m_config.bAutoHeal)
        {
            return 1;
        }

        auto iHealingSkill = GetHealingSkill();
        if (iHealingSkill == AT_SKILL_UNDEFINED)
        {
            return 1;
        }

        if (m_config.bAutoHealParty && g_pPartyManager->IsPartyActive())
        {
            m_iCurrentHealPartyIndex %= PartyNumber;

            PARTY_t* pMember = &Party[m_iCurrentHealPartyIndex];
            CHARACTER* pChar = g_pPartyManager->GetPartyMemberChar(pMember);
            int iHealResult = 1;

            if (pChar != NULL)
            {
                if (pChar == Hero)
                {
                    iHealResult = HealSelf(iHealingSkill);
                }
                else if (pMember->stepHP * 10 <= m_config.iHealPartyThreshold
                    && ComputeDistanceFromTarget(pChar) <= MAX_ACTIONABLE_DISTANCE)
                {
                    iHealResult = SimulateSkill(iHealingSkill, true, pChar->Key);
                }
            }

            m_iCurrentHealPartyIndex = (m_iCurrentHealPartyIndex + 1) % PartyNumber;

            return iHealResult;
        }
        else
        {
            m_iCurrentHealPartyIndex = 0;
            return HealSelf(iHealingSkill);
        }

        return 1;
    }

    int CMuHelper::HealSelf(ActionSkillType iHealingSkill)
    {
        int64_t iLife = CharacterAttribute->Life;
        int64_t iLifeMax = CharacterAttribute->LifeMax;
        if (iLifeMax <= 0)
        {
            return 1;
        }

        int64_t iRemaining = (iLife * 100 + iLifeMax - 1) / iLifeMax;

        if (iRemaining <= m_config.iHealThreshold)
        {
            return SimulateSkill(iHealingSkill, true, HeroKey);
        }

        return 1;
    }

    int CMuHelper::DrainLife()
    {
        if (!m_config.bUseDrainLife)
        {
            return 1;
        }

        auto iDrainLife = GetDrainLifeSkill();
        if (iDrainLife == AT_SKILL_UNDEFINED)
        {
            return 1;
        }

        int64_t iLife = CharacterAttribute->Life;
        int64_t iLifeMax = CharacterAttribute->LifeMax;
        if (iLifeMax <= 0)
        {
            return 1;
        }

        int64_t iRemaining = (iLife * 100 + iLifeMax - 1) / iLifeMax;

        if (iRemaining <= m_config.iHealThreshold)
        {
            m_iCurrentTarget = GetNearestTarget();
            if (m_iCurrentTarget != -1)
            {
                return SimulateSkill(iDrainLife, true, m_iCurrentTarget);
            }
        }

        return 1;
    }

    int CMuHelper::RepairEquipments()
    {
        if (m_config.bRepairItem)
        {
            for (int i = 0; i < MAX_EQUIPMENT; i++)
            {
                ITEM* pItem = &CharacterMachine->Equipment[i];
                if (!pItem || pItem->Type == -1)
                {
                    continue;
                }

                ITEM_ATTRIBUTE* pAttr = &ItemAttribute[pItem->Type];
                if (!pAttr)
                {
                    continue;
                }

                int iLevel = pItem->Level;
                int iDurability = pItem->Durability;
                int iMaxDurability = CalcMaxDurability(pItem, pAttr, iLevel);

                int64_t iHealth = (iDurability * 100 + iMaxDurability - 1) / iMaxDurability;

                if (iHealth <= DEFAULT_DURABILITY_THRESHOLD)
                {
                    int64_t iGoldCost = CalcSelfRepairCost(ItemValue(pItem, 2), iDurability, iMaxDurability, pItem->Type);
                    if (iGoldCost <= CharacterMachine->Gold)
                    {
                        SocketClient->ToGameServer()->SendRepairItemRequest(i, 1);
                    }
                }
            }
        }

        return 1;
    }

    int CMuHelper::Attack()
    {
        if (m_iCurrentTarget == -1)
        {
            if (!m_setTargets.empty())
            {
                CleanupTargets();

                if (m_config.bLongRangeCounterAttack)
                {
                    m_iCurrentTarget = GetFarthestAttackingTarget();
                }

                if (m_iCurrentTarget == -1)
                {
                    m_iCurrentTarget = GetNearestTarget();
                }
            }
            else
            {
                m_iComboState = 0;
                m_bComboStepPending = false;
                return 0;
            }

            // New target only when the mob id actually changes.
            if (m_config.bUseCombo
                && m_iCurrentTarget != -1
                && m_iCurrentTarget != m_iLastComboTargetId)
            {
                m_iComboState = 0;
                m_bComboStepPending = false;
                m_ullLastComboSkillMs = 0;
            }

            if (m_iCurrentTarget != -1)
            {
                m_iLastComboTargetId = m_iCurrentTarget;
            }
        }

        if (m_config.bUseCombo)
        {
            return SimulateComboAttack();
        }

        m_iCurrentSkill = SelectAttackSkill();
        if (m_iCurrentSkill > AT_SKILL_UNDEFINED)
        {
            const float fSkillDistance = gSkillManager.GetSkillDistance(m_iCurrentSkill, Hero);
            if (GameLogic::Combat::CanExecuteSkill(Hero, m_iCurrentSkill, fSkillDistance))
            {
                return SimulateAttack(m_iCurrentSkill);
            }
        }

        if (m_config.bFallbackBasicAttack)
        {
            if (!Hero->Movement)
            {
                return SimulateBasicAttack(m_iCurrentTarget);
            }
        }

        return 1;
    }

    ActionSkillType CMuHelper::SelectAttackSkill()
    {
        const size_t safeSize = std::min({m_config.aiSkill.size(), m_config.aiSkillCondition.size(), m_config.aiSkillInterval.size()});
        for (int i = 1; i < (int)safeSize; i++)
        {
            const int iSkillId = m_config.aiSkill[i];
            if (iSkillId <= 0 || iSkillId >= MAX_SKILLS)
            {
                continue;
            }

            if ((m_config.aiSkillCondition[i] & ON_TIMER)
                && m_config.aiSkillInterval[i] != 0
                && m_iSecondsElapsed > 0
                && m_iSecondsElapsed % m_config.aiSkillInterval[i] == 0)
            {
                return (ActionSkillType)iSkillId;
            }

            if (m_config.aiSkillCondition[i] & ON_CONDITION)
            {
                int iCount = 0;
                if (m_config.aiSkillCondition[i] & ON_MOBS_NEARBY)
                {
                    iCount = (int)m_setTargets.size();
                }
                else if (m_config.aiSkillCondition[i] & ON_MOBS_ATTACKING)
                {
                    iCount = (int)m_setTargetsAttacking.size();
                }
                else
                {
                    continue;
                }

                if (((m_config.aiSkillCondition[i] & ON_MORE_THAN_TWO_MOBS)   && iCount >= 2)
                    || ((m_config.aiSkillCondition[i] & ON_MORE_THAN_THREE_MOBS) && iCount >= 3)
                    || ((m_config.aiSkillCondition[i] & ON_MORE_THAN_FOUR_MOBS)  && iCount >= 4)
                    || ((m_config.aiSkillCondition[i] & ON_MORE_THAN_FIVE_MOBS)  && iCount >= 5))
                {
                    return (ActionSkillType)iSkillId;
                }
            }
        }

        if (m_config.aiSkill[0] > 0)
        {
            return (ActionSkillType)m_config.aiSkill[0];
        }

        return AT_SKILL_UNDEFINED;
    }

    // True while the hero is mid swing; gating helper actions on it makes the
    // bot's cadence follow AttackSpeed instead of the fixed helper timer, the
    // same way the manual click path gates in MoveHero (ZzzInterface.cpp).
    static bool IsHeroSwingInProgress()
    {
        const int iAction = Hero->Object.CurrentAction;

        // Outside the swing enum range entirely -> not a swing.
        if (!Engine::Object::IsAttackAction(iAction))
            return false;

        // Several non-swing *stance* animations (mounted idle/walk/run, two-hand-
        // sword stance, ride-horse, rage-fenrir) share the [PLAYER_ATTACK_FIST ..
        // PLAYER_RIDE_SKILL] enum range that IsAttackAction() spans. MoveHero
        // (ZzzInterface.cpp) OR-excludes exactly these four ranges when deciding
        // whether the hero may move; mirror that here. Otherwise a Fenrir-mounted
        // idle character (CurrentAction == PLAYER_FENRIR_STAND, inside the range)
        // reads as a perpetual swing, IsHeroSwingInProgress() never clears, and
        // SimulateSkill()/SimulateAttack() never fire -- the auto-helper is dead
        // for the whole session while Horn of Fenrir (or any mount) is equipped.
        if ((iAction >= PLAYER_STOP_TWO_HAND_SWORD_TWO && iAction <= PLAYER_RUN_TWO_HAND_SWORD_TWO)
            || (iAction >= PLAYER_DARKLORD_STAND && iAction <= PLAYER_RUN_RIDE_HORSE)
            || (iAction >= PLAYER_FENRIR_RUN && iAction <= PLAYER_FENRIR_WALK_ONE_LEFT)
            || (iAction >= PLAYER_RAGE_FENRIR_WALK && iAction <= PLAYER_RAGE_FENRIR_STAND_ONE_LEFT))
            return false;

        // Genuine attack/skill swing -> Fenrir attack/skill actions sit below
        // PLAYER_FENRIR_RUN, so they stay gated and cadence still tracks
        // AttackSpeed when mounted.
        return true;
    }

    int CMuHelper::SimulateComboAttack()
    {
        for (int i = 0; i < (int)m_config.aiSkill.size(); i++)
        {
            if (m_config.aiSkill[i] == 0)
            {
                return 0;
            }
        }

        g_MuHelperComboForceChain = false;

        if (IsHeroSwingInProgress())
        {
            if (m_bComboStepPending
                && m_ullLastComboSkillMs != 0
                && (ComboNowMs() - m_ullLastComboSkillMs) >= kComboWindowMs)
            {
                LetHeroStop(Hero, TRUE);
                m_iComboState = (m_iComboState + 1) % 3;
                m_bComboStepPending = false;
                g_MuHelperComboForceChain = true;
            }
            else if (m_bComboStepPending || !g_MuHelperComboForceChain)
            {
                return 1;
            }
        }

        if (m_bComboStepPending && !IsHeroSwingInProgress())
        {
            m_iComboState = (m_iComboState + 1) % 3;
            m_bComboStepPending = false;
        }

        const int iComboSlot = m_iComboState;
        const ActionSkillType iComboSkill = (ActionSkillType)m_config.aiSkill[iComboSlot];

        if (!m_bComboStepPending)
        {
            if (SimulateAttack(iComboSkill))
            {
                m_bComboStepPending = true;
                m_ullLastComboSkillMs = ComboNowMs();
            }
        }

        g_MuHelperComboForceChain = false;
        return 1;
    }

    int CMuHelper::SimulateAttack(ActionSkillType iSkill)
    {
        return SimulateSkill(iSkill, true, m_iCurrentTarget);
    }

    int CMuHelper::SimulateSkill(ActionSkillType iSkill, bool bTargetRequired, int iTarget)
    {
        // Respect attack animation cadence unless helper is force-chaining combo.
        if (IsHeroSwingInProgress()
            && !(g_MuHelperComboForceChain && gSkillManager.IsKnightComboSkill(iSkill)))
        {
            return 0;
        }

        g_MovementSkill.m_iSkill = iSkill;
        g_MovementSkill.m_bMagic = true;

        const float fSkillDistance = gSkillManager.GetSkillDistance(iSkill, Hero);
        const bool bSelfPositionSkill = IsSelfPositionSkill(iSkill);

        if (bTargetRequired)
        {
            if (bSelfPositionSkill)
            {
                TargetX = Hero->PositionX;
                TargetY = Hero->PositionY;

                g_MovementSkill.m_iTarget = -1;

                // Check if current target is still valid (exists and alive)
                if (iTarget != -1)
                {
                    const int iCharIndex = FindCharacterIndex(iTarget);
                    if (iCharIndex != MAX_CHARACTERS_CLIENT)
                    {
                        CHARACTER* pCurrentTarget = &CharactersClient[iCharIndex];
                        if (pCurrentTarget->Dead > 0 || !IsMonster(pCurrentTarget))
                        {
                            DeleteTarget(iTarget);
                            return 0;
                        }
                    }
                    else
                    {
                        DeleteTarget(iTarget);
                        return 0;
                    }
                }
            }
            else
            {
                if (iTarget == -1)
                {
                    return 0;
                }

                const int iCharIndex = FindCharacterIndex(iTarget);
                if (iCharIndex == MAX_CHARACTERS_CLIENT)
                {
                    DeleteTarget(iTarget);
                    return 0;
                }

                SelectedCharacter = iCharIndex;

                CHARACTER* pTarget = &CharactersClient[iCharIndex];
                if (pTarget->Dead > 0)
                {
                    DeleteTarget(iTarget);
                    return 0;
                }

                g_MovementSkill.m_iTarget = iCharIndex;

                TargetX = (int)(pTarget->Object.Position[0] / TERRAIN_SCALE);
                TargetY = (int)(pTarget->Object.Position[1] / TERRAIN_SCALE);

                PATH_t tempPath;
                bool bHasPath = PathFinding2(Hero->PositionX, Hero->PositionY, TargetX, TargetY, &tempPath, m_iHuntingDistance + fSkillDistance);
                
                // Target not reachable, ignore it
                if (!bHasPath)
                {
                    DeleteTarget(iTarget);
                    return 0;
                }

                const bool bTargetNear = CheckTile(Hero, &Hero->Object, fSkillDistance);
                if (bTargetNear && !CheckWall(Hero->PositionX, Hero->PositionY, TargetX, TargetY))
                {
                    DeleteTarget(iTarget);
                    return 0;
                }

                // Target is not yet in range, move closer.
                if (!bTargetNear)
                {
                    if (Hero->Movement)
                    {
                        return 0;
                    }

                    Hero->Path.Lock.lock();

                    // Limit movement to 2 steps at a time
                    int pathNum = std::min<int>(tempPath.PathNum, 2);
                    for (int i = 0; i < pathNum; i++)
                    {
                        Hero->Path.PathX[i] = tempPath.PathX[i];
                        Hero->Path.PathY[i] = tempPath.PathY[i];
                    }
                    Hero->Path.PathNum = pathNum;
                    Hero->Path.CurrentPath = 0;
                    Hero->Path.CurrentPathFloat = 0;

                    Hero->Path.Lock.unlock();

                    SendMove(Hero, &Hero->Object);
                    return 0;
                }
            }
        }
        else
        {
            TargetX = Hero->PositionX;
            TargetY = Hero->PositionY;
        }

        int iSkillResult = GameLogic::Combat::ExecuteSkill(Hero, iSkill, fSkillDistance);
        if (iSkillResult == -1 && iTarget != -1)
        {
            DeleteTarget(iTarget);
        }

        return (int)(iSkillResult == 1);
    }

    int CMuHelper::SimulateBasicAttack(int iTarget)
    {
        if (iTarget == -1)
        {
            return 0;
        }

        // Let the current swing finish before attacking again, so the cadence
        // tracks AttackSpeed instead of the fixed helper timer.
        if (IsHeroSwingInProgress())
        {
            return 0;
        }

        const int iCharIndex = FindCharacterIndex(iTarget);
        if (iCharIndex == MAX_CHARACTERS_CLIENT)
        {
            DeleteTarget(iTarget);
            return 0;
        }

        CHARACTER* pTarget = &CharactersClient[iCharIndex];
        if (pTarget->Dead > 0 || !IsMonster(pTarget))
        {
            DeleteTarget(iTarget);
            return 0;
        }

        constexpr float BASIC_RANGE_DEFAULT = 1.8f;
        constexpr float BASIC_RANGE_SPEAR = 2.2f;
        constexpr float BASIC_RANGE_BOW = 6.0f;

        float fRange = BASIC_RANGE_DEFAULT;
        const int iWeaponRight = CharacterMachine->Equipment[EQUIPMENT_WEAPON_RIGHT].Type;
        if (iWeaponRight >= ITEM_SPEAR && iWeaponRight < ITEM_SPEAR + MAX_ITEM_INDEX)
        {
            fRange = BASIC_RANGE_SPEAR;
        }
        if (gCharacterManager.GetEquipedBowType() != BOWTYPE_NONE)
        {
            fRange = BASIC_RANGE_BOW;
        }

        SelectedCharacter = iCharIndex;
        TargetX = (int)(pTarget->Object.Position[0] / TERRAIN_SCALE);
        TargetY = (int)(pTarget->Object.Position[1] / TERRAIN_SCALE);

        PATH_t tempPath;
        const bool bHasPath = PathFinding2(Hero->PositionX, Hero->PositionY, TargetX, TargetY, &tempPath, m_iHuntingDistance + fRange);
        if (!bHasPath)
        {
            DeleteTarget(iTarget);
            return 0;
        }

        const bool bTargetNear = CheckTile(Hero, &Hero->Object, fRange);
        if (bTargetNear && !CheckWall(Hero->PositionX, Hero->PositionY, TargetX, TargetY))
        {
            DeleteTarget(iTarget);
            return 0;
        }

        // Target is not yet in range, move closer.
        if (!bTargetNear)
        {
            if (Hero->Movement)
            {
                return 0;
            }

            Hero->Path.Lock.lock();
            const int pathNum = std::min<int>(tempPath.PathNum, 2);
            for (int i = 0; i < pathNum; i++)
            {
                Hero->Path.PathX[i] = tempPath.PathX[i];
                Hero->Path.PathY[i] = tempPath.PathY[i];
            }
            Hero->Path.PathNum = pathNum;
            Hero->Path.CurrentPath = 0;
            Hero->Path.CurrentPathFloat = 0;
            Hero->Path.Lock.unlock();

            SendMove(Hero, &Hero->Object);
            return 0;
        }

        if (gCharacterManager.GetEquipedBowType() != BOWTYPE_NONE && !CheckArrow())
        {
            return 0;
        }

        Hero->MovementType = MOVEMENT_ATTACK;
        ActionTarget = iCharIndex;
        Attacking = 1;
        Action(Hero, &Hero->Object, true);
        return 1;
    }

    int CMuHelper::Regroup()
    {
        if (!m_config.bReturnToOriginalPosition || m_config.iMaxSecondsAway <= 0)
        {
            return 1;
        }

        if (m_iSecondsAway > m_config.iMaxSecondsAway)
        {
            if (!SimulateMove(m_posOriginal))
            {
                return 0;
            }

            m_iSecondsAway = 0;
            m_iComboState = 0;
            m_bComboStepPending = false;
            m_ullLastComboSkillMs = 0;
            m_iCurrentTarget = -1;
        }

        return 1;
    }

    int CMuHelper::SimulateMove(POINT posMove)
    {
        // Already walking: do not repath every 250 ms (causes spin / no attack).
        if (Hero->Movement)
        {
            return 0;
        }

        Hero->MovementType = MOVEMENT_MOVE;
        TargetX = (int)posMove.x;
        TargetY = (int)posMove.y;

        if (!CheckTile(Hero, &Hero->Object, 1.5f))
        {
            if (PathFinding2((Hero->PositionX), (Hero->PositionY), TargetX, TargetY, &Hero->Path))
            {
                SendMove(Hero, &Hero->Object);
                return 0;
            }

            // Unreachable anchor — drop regroup so Attack() can run again.
            m_iSecondsAway = 0;
            return 1;
        }

        return 1;
    }

    bool CMuHelper::HasAssignedBuffSkill()
    {
        for (int i = 0; i < m_config.aiBuff.size(); i++)
        {
            if (m_config.aiBuff[i] != 0)
            {
                return true;
            }
        }

        return false;
    }

    ActionSkillType CMuHelper::GetHealingSkill()
    {
        std::vector<ActionSkillType> aiHealingSkills =
        {
            AT_SKILL_HEALING,
            AT_SKILL_HEALING_STR,
        };

        for (int i = 0; i < aiHealingSkills.size(); i++)
        {
            int iSkillIndex = g_pSkillList->GetSkillIndex(aiHealingSkills[i]);
            if (iSkillIndex != -1)
            {
                return aiHealingSkills[i];
            }
        }

        return AT_SKILL_UNDEFINED;
    }

    // Matches AttackWizard() behavior in ZzzInterface.cpp for these skill IDs.
    bool CMuHelper::IsSelfPositionSkill(ActionSkillType iSkill)
    {
        return (
            iSkill == AT_SKILL_NOVA_BEGIN ||
            iSkill == AT_SKILL_NOVA ||
            iSkill == AT_SKILL_HELL_FIRE ||
            iSkill == AT_SKILL_HELL_FIRE_STR ||
            iSkill == AT_SKILL_INFERNO ||
            iSkill == AT_SKILL_INFERNO_STR ||
            iSkill == AT_SKILL_INFERNO_STR_MG
        );
    }

    ActionSkillType CMuHelper::GetDrainLifeSkill()
    {
        std::vector<ActionSkillType> aiDrainLifeSkills =
        {
            AT_SKILL_ALICE_DRAINLIFE,
            AT_SKILL_ALICE_DRAINLIFE_STR
        };

        for (int i = 0; i < aiDrainLifeSkills.size(); i++)
        {
            int iSkillIndex = g_pSkillList->GetSkillIndex(aiDrainLifeSkills[i]);
            if (iSkillIndex != -1)
            {
                return aiDrainLifeSkills[i];
            }
        }

        return AT_SKILL_UNDEFINED;
    }

    int CMuHelper::ObtainItem()
    {
        if (m_iCurrentItem == MAX_ITEMS)
        {
            m_iCurrentItem = SelectItemToObtain();
            if (m_iCurrentItem == MAX_ITEMS)
            {
                return 1;
            }
        }

        ITEM_t* pDrop = &Items[m_iCurrentItem];

        if (!pDrop->Object.Live)
        {
            DeleteItem(m_iCurrentItem);
            return 1;
        }

        TargetX = (int)(Items[m_iCurrentItem].Object.Position[0] / TERRAIN_SCALE);
        TargetY = (int)(Items[m_iCurrentItem].Object.Position[1] / TERRAIN_SCALE);

        int iDistance = ComputeDistanceBetween(HeroTilePos(), { TargetX, TargetY });
        if (iDistance > m_iObtainingDistance)
        {
            // Selected while in range, then hero walked away: go to the drop or
            // abandon it. Returning 1 here used to freeze the helper forever
            // with m_iCurrentItem stuck and Attack() never running.
            if (Hero->Movement)
            {
                return 0;
            }

            if (PathFinding2((Hero->PositionX), (Hero->PositionY), TargetX, TargetY, &Hero->Path))
            {
                SendMove(Hero, &Hero->Object);
                return 0;
            }

            DeleteItem(m_iCurrentItem);
            return 1;
        }

        if (!CheckTile(Hero, &Hero->Object, 2.0f))
        {
            if (Hero->Movement)
            {
                return 0;
            }

            if (PathFinding2((Hero->PositionX), (Hero->PositionY), TargetX, TargetY, &Hero->Path))
            {
                SendMove(Hero, &Hero->Object);
            }

            return 0;
        }

        if (SendGetItem == -1)
        {
            SendGetItem = m_iCurrentItem;
            SocketClient->ToGameServer()->SendPickupItemRequest(m_iCurrentItem);
            DeleteItem(m_iCurrentItem);
        }

        return 1;
    }

    bool CMuHelper::ShouldObtainItem(int iItemId)
    {
        ITEM_t* pDrop = &Items[iItemId];
        ITEM* pItem = &pDrop->Item;

        if ((m_config.bPickZen && IsMoneyItem(pItem))
            || (m_config.bPickJewel && IsJewelItem(pItem))
            || (m_config.bPickAncient && IsAncientItem(pItem))
            || (m_config.bPickExcellent && IsExcellentItem(pItem)))
        {
            return true;
        }

        if (m_config.bPickExtraItems)
        {
            std::wstring strDisplayName = GetItemDisplayName(pItem);

            for (const auto& str : m_config.aExtraItems)
            {
                // Check if the search keyword is in the item's display name
                if (strDisplayName.find(str) != std::wstring::npos)
                {
                    return true;
                }
            }
        }

        return m_config.bPickAllItems;
    }

    void CMuHelper::AddItem(int iItemId, POINT posWhere)
    {
        _itemsLock.lock();
        m_setItems.insert(iItemId);
        _itemsLock.unlock();
    }

    void CMuHelper::DeleteItem(int iItemId)
    {
        _itemsLock.lock();
        m_setItems.erase(iItemId);
        _itemsLock.unlock();

        if (iItemId == m_iCurrentItem)
        {
            m_iCurrentItem = MAX_ITEMS;
        }
    }

    int CMuHelper::SelectItemToObtain()
    {
        int iClosestItemId = MAX_ITEMS;
        // Same metric as ObtainItem (euclidean vs ComputeDistanceByRange).
        int iMinDistance = m_iObtainingDistance;

        std::set<int> setItems;
        {
            _itemsLock.lock();
            setItems = m_setItems;
            _itemsLock.unlock();
        }

        for (const int& iItemId : setItems)
        {
            if (iItemId < 0 || iItemId >= MAX_ITEMS)
            {
                continue;
            }

            if (!ShouldObtainItem(iItemId))
            {
                continue;
            }

            int iItemX = (int)(Items[iItemId].Object.Position[0] / TERRAIN_SCALE);
            int iItemY = (int)(Items[iItemId].Object.Position[1] / TERRAIN_SCALE);

            int iDistance = ComputeDistanceBetween(HeroTilePos(), { iItemX, iItemY });
            if (iDistance <= iMinDistance)
            {
                iMinDistance = iDistance;
                iClosestItemId = iItemId;
            }
        }

        return iClosestItemId;
    }
}
