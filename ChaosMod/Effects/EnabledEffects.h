#pragma once

#include "Effects/Condition/EffectCondition.h"
#include "Effects/EffectData.h"
#include "Effects/EffectIdentifier.h"

#include <functional>
#include <unordered_set>
#include <unordered_map>

class EffectData;

class EffectsIdentifierHasher
{
  public:
	std::size_t operator()(const EffectIdentifier &effectId) const noexcept
	{
		return std::hash<std::string_view>()(effectId.Id());
	}
};

inline std::unordered_map<EffectIdentifier, EffectData, EffectsIdentifierHasher> g_EnabledEffects;

inline std::unordered_set<EffectConditionType> CollectSatisfiedConditions()
{
	std::unordered_set<EffectConditionType> ensuredConditions;
	ensuredConditions.reserve(g_EffectConditions.size());

	for (const auto &[conditionType, condition] : g_EffectConditions)
		if (condition->CheckCondition())
			ensuredConditions.insert(conditionType);

	return ensuredConditions;
}

inline std::vector<EffectData *> GetFilteredEnabledEffects()
{
	std::vector<EffectData *> filteredEffects;
	filteredEffects.reserve(g_EnabledEffects.size());

	auto ensuredConditions = CollectSatisfiedConditions();

	for (auto &[effectId, effectData] : g_EnabledEffects)
		if (effectData.ConditionType == EffectConditionType::None
		    || ensuredConditions.contains(effectData.ConditionType))
			filteredEffects.push_back(&effectData);

	return filteredEffects;
}

inline bool IsEffectFilteredOut(const EffectIdentifier &id)
{
	auto ensuredConditions = CollectSatisfiedConditions();

	if (!g_EnabledEffects.contains(id))
		return false;

	const auto &effectData = g_EnabledEffects.at(id);

	return effectData.ConditionType != EffectConditionType::None
	    && !ensuredConditions.contains(effectData.ConditionType);
}

inline std::string GetFilterReason(const EffectIdentifier &id)
{
	if (!g_EnabledEffects.contains(id))
		return "";

	const auto &effectData = g_EnabledEffects.at(id);

	if (effectData.ConditionType == EffectConditionType::None)
		return "";

	return g_EffectConditions.at(effectData.ConditionType)->GetFailReason();
}