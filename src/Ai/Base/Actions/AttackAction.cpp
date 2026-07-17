/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AttackAction.h"

#include "CreatureAI.h"
#include "Event.h"
#include "LastMovementValue.h"
#include "LootObjectStack.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Unit.h"
#include "WaitForAttackStrategy.h"

bool AttackAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    if (!target->IsInWorld())
        return false;

    return Attack(target);
}

bool AttackMyTargetAction::Execute(Event /*event*/)
{
    Player* master = GetMaster();
    if (!master)
        return false;

    Unit* targetUnit = nullptr;

    // 1. 优先主人鼠标选中目标（必须是敌对且存活）
    ObjectGuid selGuid = master->GetTarget();
    if (selGuid)
    {
        Unit* unit = botAI->GetUnit(selGuid);
        if (unit && unit->IsAlive() && !unit->IsFriendlyTo(master))
            targetUnit = unit;
    }

    // 2. 主人自身攻击目标兜底
    if (!targetUnit)
    {
        Unit* victim = master->GetVictim();
        if (victim && victim->IsAlive() && !victim->IsFriendlyTo(master))
            targetUnit = victim;
    }

    // 3. 主人宠物攻击目标兜底
    if (!targetUnit)
    {
        if (Pet* pet = master->GetPet())
        {
            Unit* victim = pet->GetVictim();
            if (victim && victim->IsAlive() && !victim->IsFriendlyTo(master))
                targetUnit = victim;
        }
    }

    // 4. 遍历队伍成员（NPCBots若以Player身份加入队伍，会被包含）
    if (!targetUnit)
    {
        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || member == master || member == botAI->GetBot())
                    continue;

                Unit* memberTarget = member->GetVictim();
                if (memberTarget && memberTarget->IsAlive() && !memberTarget->IsFriendlyTo(master))
                {
                    targetUnit = memberTarget;
                    break;
                }
            }
        }
    }

    // 5. 遍历主人控制的其他单位（如NPCBots Creature），补充可能不在队伍中的情况
    if (!targetUnit)
    {
        for (Unit::ControlSet::const_iterator itr = master->m_Controlled.begin(); 
             itr != master->m_Controlled.end(); ++itr)
        {
            Unit* controlled = *itr;
            // 跳过宠物（已经检查过）和自身（若有）
            if (!controlled || controlled == master->GetPet())
                continue;

            Unit* controlledTarget = controlled->GetVictim();
            if (controlledTarget && controlledTarget->IsAlive() && !controlledTarget->IsFriendlyTo(master))
            {
                targetUnit = controlledTarget;
                break;
            }
        }
    }

    // 无有效目标提示
    if (!targetUnit || !targetUnit->IsAlive())
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "pull_no_target_error", "You have no target", {}));
        return false;
    }

    ObjectGuid guid = targetUnit->GetGUID();
    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({guid});
    bool result = Attack(targetUnit);
    if (result)
        context->GetValue<ObjectGuid>("pull target")->Set(guid);

    return result;
}

bool AttackAction::Attack(Unit* target, bool /*with_pet*/ /*true*/)
{
    if (!target)
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_no_target_error", "I have no target", {}));

        return false;
    }

    if (!target->IsInWorld())
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_target_not_in_world_error",
                "%target is no longer in the world.",
                {{"%target", target->GetName()}}));

        return false;
    }

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE ||
        bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_in_flight_error", "I cannot attack in flight", {}));

        return false;
    }

    // Check if bot OR target is in prohibited zone/area (skip for duels)
    if ((target->IsPlayer() || target->IsPet()) &&
        (!bot->duel || bot->duel->Opponent != target) &&
        (sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId()) ||
        sPlayerbotAIConfig.IsPvpProhibited(target->GetZoneId(), target->GetAreaId())))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_pvp_prohibited_error",
                "I cannot attack other players in PvP prohibited areas.",
                {}));

        return false;
    }

    if (bot->IsFriendlyTo(target))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_target_friendly_error",
                "%target is friendly to me.",
                {{"%target", target->GetName()}}));

        return false;
    }

    if (target->isDead())
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_target_dead_error",
                "%target is dead.",
                {{"%target", target->GetName()}}));

        return false;
    }

    if (!bot->IsWithinLOSInMap(target))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_target_not_in_sight_error",
                "%target is not in my sight.",
                {{"%target", target->GetName()}}));

        return false;
    }

    // Infantry attacks are not allowed from vehicles drivers.
    // Check is needed to stop some auto-attack situations.
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    Unit* oldTarget = context->GetValue<Unit*>("current target")->Get();
    bool shouldMelee = bot->IsWithinMeleeRange(target) || botAI->IsMelee(bot);

    bool sameTarget = oldTarget == target && bot->GetVictim() == target;
    bool inCombat = botAI->GetState() == BOT_STATE_COMBAT;
    bool sameAttackMode = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == shouldMelee;

    if (sameTarget && inCombat && sameAttackMode)
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_already_attacking_error",
                "I am already attacking %target.",
                {{"%target", target->GetName()}}));

        return false;
    }

    if (!bot->IsValidAttackTarget(target))
    {
        if (verbose)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "attack_invalid_target_error", "I cannot attack an invalid target.", {}));

        return false;
    }

    // if (bot->IsMounted() && bot->IsWithinLOSInMap(target))
    // {
    //     WorldPacket emptyPacket;
    //     bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    // }

    ObjectGuid guid = target->GetGUID();
    bot->SetSelection(target->GetGUID());

    context->GetValue<Unit*>("old target")->Set(oldTarget);
    context->GetValue<Unit*>("current target")->Set(target);
    context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);

    LastMovement& lastMovement = AI_VALUE(LastMovement&, "last movement");
    bool moveControlled = bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
    if (lastMovement.priority < MovementPriority::MOVEMENT_COMBAT && bot->isMoving() && !moveControlled)
    {
        AI_VALUE(LastMovement&, "last movement").clear();
        bot->GetMotionMaster()->Clear(false);
        bot->StopMoving();
    }

    if (botAI->CanMove() && !bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
        ServerFacade::instance().SetFacingTo(bot, target);

    botAI->ChangeEngine(BOT_STATE_COMBAT);

    if (!WaitForAttackStrategy::ShouldWait(botAI))
        bot->Attack(target, shouldMelee);
    /* prevent pet dead immediately in group */
    // if (bot->GetMap()->IsDungeon() && bot->GetGroup() && !target->IsInCombat())
    // {
    //     with_pet = false;
    // }
    // if (Pet* pet = bot->GetPet())
    // {
    //     if (with_pet)
    //     {
    //         pet->SetReactState(REACT_DEFENSIVE);
    //         pet->SetTarget(target->GetGUID());
    //         pet->GetCharmInfo()->SetIsCommandAttack(true);
    //         pet->AI()->AttackStart(target);
    //     }
    //     else
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->GetCharmInfo()->IsReturning();
    //     }
    // }
    return true;
}

bool AttackDuelOpponentAction::isUseful() { return AI_VALUE(Unit*, "duel target"); }

bool AttackDuelOpponentAction::Execute(Event /*event*/) { return Attack(AI_VALUE(Unit*, "duel target")); }
