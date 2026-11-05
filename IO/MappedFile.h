#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// A whole file mapped read-only into the address space. The loader parses every track straight out
// of this view, so nothing is copied through a stream and worker threads never share a file
// position. Pages are faulted in on demand; Prefetch() asks the OS to start reading them up front.
class MappedFile
{
public:
	MappedFile() = default;
	~MappedFile() { Close(); }

	MappedFile(const MappedFile&) = delete;
	MappedFile& operator=(const MappedFile&) = delete;

	bool Open(const std::filesystem::path& path, std::string& error);
	void Close();

	const uint8_t* Data() const { return data; }
	size_t Size() const { return size; }
	bool IsOpen() const { return data != nullptr; }

	// Best effort: a hint to the OS, not a guarantee that the pages are resident.
	void Prefetch() const;

private:
#ifdef _WIN32
	void* fileHandle = nullptr;
	void* mappingHandle = nullptr;
#else
	int fd = -1;
#endif
	const uint8_t* data = nullptr;
	size_t size = 0;
};
