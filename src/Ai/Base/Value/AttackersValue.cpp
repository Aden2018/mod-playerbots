/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AttackersValue.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "ServerFacade.h"

GuidVector AttackersValue::Calculate()
{
    std::unordered_set<Unit*> targets;

    GuidVector result;
    if (!botAI->AllowActivity(ALL_ACTIVITY))
        return result;

    AddAttackersOf(bot, targets);

    if (Group* group = bot->GetGroup())
        AddAttackersOf(group, targets);

    // ============ 新增：收集当前Bot主人Master自身攻击目标 + Master的NPCBots攻击目标 ============
    Player* master = botAI->GetMaster();
    if (master)
    {
        // 1. 主人自身攻击目标
        Unit* masterVictim = master->GetVictim();
        if (masterVictim && masterVictim->IsAlive() && masterVictim->GetMapId() == bot->GetMapId() &&
            !masterVictim->IsFriendlyTo(master))
        {
            targets.insert(masterVictim);
        }

        // 2. master 的 NPCBots 目标也加入攻击列表（你提供的逻辑+安全校验）
        if (master->HaveBot())
        {
            for (auto const& [_, pbot] : *master->GetBotMgr()->GetBotMap())
            {
                Unit* botVictim = pbot->GetVictim();
                if (!botVictim)
                    continue;
                // 存活、同地图、敌对过滤
                if (!botVictim->IsAlive() || botVictim->GetMapId() != master->GetMapId())
                    continue;
                if (botVictim->IsFriendlyTo(master))
                    continue;

                targets.insert(botVictim);
            }
        }
    }
    // ======================================================================================

    RemoveNonThreating(targets);

    // prioritized target
    GuidVector prioritizedTargets = AI_VALUE(GuidVector, "prioritized targets");
    for (ObjectGuid target : prioritizedTargets)
    {
        Unit* unit = botAI->GetUnit(target);
        if (unit && IsValidTarget(unit, bot))
            targets.insert(unit);
    }
    if (Group* group = bot->GetGroup())
    {
        ObjectGuid skullGuid = group->GetTargetIcon(7);
        Unit* skullTarget = botAI->GetUnit(skullGuid);
        if (skullTarget && IsValidTarget(skullTarget, bot))
            targets.insert(skullTarget);
    }

    for (Unit* unit : targets)
        result.push_back(unit->GetGUID());

    if (bot->duel && bot->duel->Opponent)
        result.push_back(bot->duel->Opponent->GetGUID());

    // workaround for bots of same faction not fighting in arena
    if (bot->InArena())
    {
        GuidVector possibleTargets = AI_VALUE(GuidVector, "possible targets");
        for (ObjectGuid const guid : possibleTargets)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (unit && unit->IsPlayer() && IsValidTarget(unit, bot))
                result.push_back(unit->GetGUID());
        }
    }

    return result;
}

void AttackersValue::AddAttackersOf(Group* group, std::unordered_set<Unit*>& targets)
{
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot || member->GetMapId() != bot->GetMapId() ||
            ServerFacade::instance().GetDistance2d(bot, member) > sPlayerbotAIConfig.sightDistance)
            continue;

        AddAttackersOf(member, targets);

        // ========== 新增：队员自身攻击目标 ==========
        Unit* memberVictim = member->GetVictim();
        if (memberVictim && memberVictim->IsAlive() && memberVictim->GetMapId() == bot->GetMapId() &&
            !memberVictim->IsFriendlyTo(member))
        {
            targets.insert(memberVictim);
        }

        // ========== 新增：该队员名下所有NPCBots攻击目标 ==========
        if (member->HaveBot())
        {
            for (auto const& [_, pbot] : *member->GetBotMgr()->GetBotMap())
            {
                Unit* botVictim = pbot->GetVictim();
                if (!botVictim)
                    continue;
                if (!botVictim->IsAlive() || botVictim->GetMapId() != member->GetMapId())
                    continue;
                if (botVictim->IsFriendlyTo(member))
                    continue;

                targets.insert(botVictim);
            }
        }
        // ===========================================

        // 原有收集队员Playerbots攻击者逻辑保留（用于支援挨打队友）
        PlayerbotAI* memberBotAI = GET_PLAYERBOT_AI(member);
        if (memberBotAI)
        {
            GuidVector memberAttackers = memberBotAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
            for (ObjectGuid atkGuid : memberAttackers)
            {
                Unit* atkUnit = botAI->GetUnit(atkGuid);
                if (atkUnit && IsValidTarget(atkUnit, bot))
                    targets.insert(atkUnit);
            }
        }
    }
}

struct AddGuardiansHelper
{
    explicit AddGuardiansHelper(std::vector<Unit*>& units) : units(units) {}

    void operator()(Unit* target) const { units.push_back(target); }

    std::vector<Unit*>& units;
};

void AttackersValue::AddAttackersOf(Player* player, std::unordered_set<Unit*>& targets)
{
    if (!player || !player->IsInWorld() || player->IsBeingTeleported())
        return;

    for (auto const& [guid, ref] : player->GetThreatMgr().GetThreatenedByMeList())
    {
        Unit* attacker = ref->GetOwner();
        if (!attacker)
            continue;

        if (player->IsValidAttackTarget(attacker) &&
            player->GetDistance2d(attacker) < sPlayerbotAIConfig.sightDistance)
            targets.insert(attacker);
    }

    if (player->HaveBot())
    {
        for (auto const& [_, npcBot] : *player->GetBotMgr()->GetBotMap())

        {
            if (!npcBot || !npcBot->IsInWorld() || !npcBot->IsAlive() || npcBot->IsDuringRemoveFromWorld())

                continue;

            if (npcBot->GetMapId() != bot->GetMapId() ||

                ServerFacade::instance().GetDistance2d(bot, npcBot) > sPlayerbotAIConfig.sightDistance)

                continue;

            for (auto const& [guid, ref] : npcBot->GetThreatMgr().GetThreatenedByMeList())

            {
                Unit* attacker = ref->GetOwner();

                if (!attacker)

                    continue;

                if (bot->IsValidAttackTarget(attacker) &&

                    bot->GetDistance2d(attacker) < sPlayerbotAIConfig.sightDistance)

                    targets.insert(attacker);
            }

            if (Unit* botTarget = npcBot->GetVictim())

            {
                if (bot->IsValidAttackTarget(botTarget) &&

                    bot->GetDistance2d(botTarget) < sPlayerbotAIConfig.sightDistance)

                    targets.insert(botTarget);
            }
        }
    }
}

void AttackersValue::RemoveNonThreating(std::unordered_set<Unit*>& targets)
{
    for (std::unordered_set<Unit*>::iterator tIter = targets.begin(); tIter != targets.end();)
    {
        Unit* unit = *tIter;
        if (bot->GetMapId() != unit->GetMapId() || !hasRealThreat(unit) || !IsValidTarget(unit, bot))
        {
            std::unordered_set<Unit*>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else
            ++tIter;
    }
}

bool AttackersValue::hasRealThreat(Unit* attacker)
{
    return attacker && attacker->IsInWorld() && attacker->IsAlive() && !attacker->IsPolymorphed() &&
           // !attacker->isInRoots() &&
           !attacker->IsFriendlyTo(bot);
}

bool AttackersValue::IsPossibleTarget(Unit* attacker, Player* bot, float /*range*/)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    // Basic check
    if (!attacker)
        return false;

    // bool inCannon = botAI->IsInVehicle(false, true);
    // bool enemy = botAI->GetAiObjectContext()->GetValue<Unit*>("enemy player target")->Get();

    // Validity checks
    if (!attacker->IsVisible() || !attacker->IsInWorld() || attacker->GetMapId() != bot->GetMapId())
        return false;

    if (attacker->isDead() || attacker->HasSpiritOfRedemptionAura())
        return false;

    // Flag checks
    if (attacker->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2))
        return false;

    if (attacker->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC) || attacker->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;

    // Skip targets that are immune to all damage (e.g., Ice Block, Divine Shield)
    if (attacker->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) &&
        attacker->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC))
        return false;

    // Relationship checks
    if (attacker->IsFriendlyTo(bot))
        return false;

    // Critter exception
    if (attacker->GetCreatureType() == CREATURE_TYPE_CRITTER && !attacker->IsInCombat())
        return false;

    // Visibility check
    if (!bot->CanSeeOrDetect(attacker))
        return false;

    // PvP prohibition checks (skip for duels)
    if ((attacker->GetGUID().IsPlayer() || attacker->GetGUID().IsPet()) &&
        (!bot->duel || bot->duel->Opponent != attacker) &&
        (sPlayerbotAIConfig.IsPvpProhibited(attacker->GetZoneId(), attacker->GetAreaId()) ||
        sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId())))
    {
        // This will stop aggresive pets from starting an attack.
        // This will stop currently attacking pets from continuing their attack.
        // This will first require the bot to change from a combat strat. It will
        // not be reached if the bot only switches targets, including NPC targets.
        for (Unit::ControlSet::const_iterator itr = bot->m_Controlled.begin();
            itr != bot->m_Controlled.end(); ++itr)
        {
            Creature* creature = dynamic_cast<Creature*>(*itr);
            if (creature && creature->GetVictim() == attacker)
            {
                creature->AttackStop();
                if (CharmInfo* charmInfo = creature->GetCharmInfo())
                    charmInfo->SetIsCommandAttack(false);
            }
        }

        return false;
    }

    // Unflagged player check
    if (attacker->IsPlayer() && !attacker->IsPvP() && !attacker->IsFFAPvP() &&
        (!bot->duel || bot->duel->Opponent != attacker))
        return false;

    // Creature-specific checks
    Creature* c = attacker->ToCreature();
    if (c)
    {
        if (c->IsInEvadeMode())
            return false;

        bool leaderHasThreat = false;
        if (bot->GetGroup() && botAI->GetMaster())
            leaderHasThreat = attacker->GetThreatMgr().GetThreat(botAI->GetMaster());

        bool isMemberBotGroup = false;
        if (bot->GetGroup() && botAI->GetMaster())
        {
            PlayerbotAI* masterBotAI = GET_PLAYERBOT_AI(botAI->GetMaster());
            if (masterBotAI && !masterBotAI->IsRealPlayer())
                isMemberBotGroup = true;
        }

        bool canAttack = (!isMemberBotGroup && botAI->HasStrategy("attack tagged", BOT_STATE_NON_COMBAT)) ||
            leaderHasThreat ||
            (!c->hasLootRecipient() &&
                (!c->GetVictim() ||
                    (c->GetVictim() &&
                        ((!c->GetVictim()->IsPlayer() || bot->IsInSameGroupWith(c->GetVictim()->ToPlayer())) ||
                            (botAI->GetMaster() && c->GetVictim() == botAI->GetMaster()))))) ||
            c->isTappedBy(bot);

        if (!canAttack)
            return false;
    }

    return true;
}

bool AttackersValue::IsValidTarget(Unit* attacker, Player* bot)
{
    return IsPossibleTarget(attacker, bot) && bot->IsWithinLOSInMap(attacker);
}

bool PossibleAddsValue::Calculate()
{
    GuidVector possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();
    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();

    for (ObjectGuid const guid : possible)
    {
        if (find(attackers.begin(), attackers.end(), guid) != attackers.end())
            continue;
        Unit* add = botAI->GetUnit(guid);
        if (!add || !add->IsInWorld() || add->IsDuringRemoveFromWorld())
            continue;

        if (!add->GetTarget() && !add->GetThreatMgr().GetLastVictim() && add->IsHostileTo(bot))
        {
            for (ObjectGuid const attackerGUID : attackers)
            {
                Unit* attacker = botAI->GetUnit(attackerGUID);
                if (!attacker)
                    continue;

                float dist = ServerFacade::instance().GetDistance2d(attacker, add);
                if (ServerFacade::instance().IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.aoeRadius * 1.5f))
                    continue;

                if (ServerFacade::instance().IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.aggroDistance))
                    return true;
            }
        }
    }

    return false;
}
