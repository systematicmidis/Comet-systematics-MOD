#pragma once

#include "ResourcePack.h"

class DefaultResourcePack : public ResourcePack
{
public:
	static std::shared_ptr<DefaultResourcePack> Instance()
	{
		static std::shared_ptr<DefaultResourcePack> instance(new DefaultResourcePack());
		return instance;
	}

	DefaultResourcePack(const DefaultResourcePack&) = delete;
	DefaultResourcePack& operator=(const DefaultResourcePack&) = delete;

	void Init() override;
	std::shared_ptr<std::istream> GetStream(const std::string& name) override;
	std::vector<std::string> GetEntries() override;
	std::shared_ptr<SignatureState> GetSignature() const override
	{
		return std::make_shared<SignatureState>(::SIGNED);
	}
private:
	DefaultResourcePack() : ResourcePack("") {}
};