#pragma once

#include <vector>
#include "DefaultResourcePack.h"

inline const char* RESOURCE_PACK_FOLDER = "assets/resourcepacks";

class ResourcePackList
{
public:
	ResourcePackList() = default;
	void RefreshList();
	const std::vector<std::shared_ptr<ResourcePack>>& GetPackList() const { return resourcePacks; }
	const std::shared_ptr<ResourcePack> GetActivePack() const { return resourcePacks[currPack]; }
	void SwitchPack(size_t newPack) { currPack = newPack; }
private:
	std::vector<std::shared_ptr<ResourcePack>> resourcePacks{};
	size_t currPack = 0;

	void AddDefaultPack();
};