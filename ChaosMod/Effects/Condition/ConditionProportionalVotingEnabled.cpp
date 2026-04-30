#include <stdafx.h>

#include "Components/Voting.h"
#include "Effects/Condition/EffectCondition.h"

static bool OnCondition()
{
	auto *voting = GetComponent<Voting>();
	return voting && voting->IsEnabled() && voting->GetVotingMode() == VotingMode::Percentage;
}

REGISTER_EFFECT_CONDITION(EffectConditionType::ProportionalVotingEnabled, OnCondition,
                          "Proportional voting is not enabled");