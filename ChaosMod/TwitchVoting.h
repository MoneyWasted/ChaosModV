#pragma once
#include "EffectDispatcher.h"
#include <map>
#include <array>
#include <memory>

class TwitchVoting
{
public:
	TwitchVoting(bool enableTwitchVoting, bool twitchVotingNoVoteChance, std::shared_ptr<EffectDispatcher> effectDispatcher,
		std::map<EffectType, std::array<int, 3>> enabledEffects);

	inline bool IsEnabled() const
	{
		return m_enableTwitchVoting;
	}
	void Tick();
	
private:
	const bool m_enableTwitchVoting;
	const bool m_twitchVotingNoVoteChance;
	const std::shared_ptr<EffectDispatcher> m_effectDispatcher;
	const std::map<EffectType, std::array<int, 3>> m_enabledEffects;

	bool m_isVotingRunning = false;
	std::array<EffectType, 3> m_effectChoices;
};