#include <stdafx.h>

#include "HandleToEntityStructHook.h"

#include "Memory/Hooks/Hook.h"

static std::unordered_map<Entity, Entity> ms_VehicleMap;

__int64 (*_OG_HandleToEntityStruct)(Entity entity);
__int64 _HK_HandleToEntityStruct(Entity entity)
{
	if (entity <= 0)
		return 0;
	Entity vehToContinue = entity;
	for (auto it = ms_VehicleMap.find(vehToContinue); it != ms_VehicleMap.end(); it = ms_VehicleMap.find(vehToContinue))
	{
		vehToContinue = it->second;
		if (vehToContinue <= 0)
			return 0;
	}
	return _OG_HandleToEntityStruct(vehToContinue);
}

static bool OnHook()
{
	Handle handle = Memory::FindPattern("83 F9 FF 74 31 4C 8B 0D", "83 F9 FF 74 64 41");
	if (!handle.IsValid())
		return false;

	Memory::AddHook(handle.Get<void>(), _HK_HandleToEntityStruct, &_OG_HandleToEntityStruct);

	return true;
}

static RegisterHook registerHook(OnHook, nullptr, "_HandleToEntityStruct", true);

namespace Hooks
{
	void ProxyEntityHandle(Entity origHandle, Entity newHandle)
	{
		ms_VehicleMap.emplace(origHandle, newHandle);

		for (auto it = ms_VehicleMap.begin(); it != ms_VehicleMap.end();)
		{
			if (!DOES_ENTITY_EXIST(it->second))
				it = ms_VehicleMap.erase(it);
			else
				it++;
		}
	}
}
