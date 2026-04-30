#include <stdafx.h>

#include "Components/Voting.h"
#include "Effects/Condition/EffectCondition.h"

static bool OnCondition()
{
	auto *voting = GetComponent<Voting>();
	return voting && voting->IsEnabled();
}

REGISTER_EFFECT_CONDITION(EffectConditionType::VotingEnabled, OnCondition, "Voting is not enabled");