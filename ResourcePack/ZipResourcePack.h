#pragma once

#include "ResourcePack.h"
#include <miniz.h>
#include <filesystem>

namespace fs = std::filesystem;

// TODO: move this to IO folder
class MemoryBufferStream : public std::istream
{
public:
	MemoryBufferStream(std::vector<uint8_t>&& data)
		: buffer(std::move(data)),
		buf(buffer.data(), buffer.size()),
		std::istream(&buf)
	{
	}

private:
	std::vector<uint8_t> buffer;

	struct Buffer : std::streambuf
	{
		Buffer(uint8_t* data, size_t size)
		{
			setg(reinterpret_cast<char*>(data),
				reinterpret_cast<char*>(data),
				reinterpret_cast<char*>(data + size));
		}
	} buf;
};

class ZipResourcePack : public ResourcePack
{
public:
	ZipResourcePack(const fs::path& path) : ResourcePack(path.string().c_str())
	{
		memset(&zip, 0, sizeof(zip));
		open = mz_zip_reader_init_file(&zip, path.string().c_str(), 0);
		if (!open)
			throw std::runtime_error("Failed to open ZIP: " + path.string());
	}

	~ZipResourcePack()
	{
		if (!open) return;
		mz_zip_reader_end(&zip);
	}

	std::shared_ptr<std::istream> GetStream(const std::string& name) override;
	std::vector<std::string> GetEntries() override;
private:
	mz_zip_archive zip{};
	bool open = false;
};