#include "MappedFile.h"

#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

bool MappedFile::Open(const std::filesystem::path& path, std::string& error)
{
	Close();

#ifdef _WIN32
	HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		error = "Could not open the file.";
		return false;
	}

	LARGE_INTEGER fileSize{};
	if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0)
	{
		CloseHandle(file);
		error = "The file is empty.";
		return false;
	}

	if (static_cast<uint64_t>(fileSize.QuadPart) > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
	{
		CloseHandle(file);
		error = "The file is too large to map.";
		return false;
	}

	HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
	if (mapping == nullptr)
	{
		CloseHandle(file);
		error = "Could not map the file.";
		return false;
	}

	const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	if (view == nullptr)
	{
		CloseHandle(mapping);
		CloseHandle(file);
		error = "Could not map a view of the file (out of address space?).";
		return false;
	}

	fileHandle = file;
	mappingHandle = mapping;
	data = static_cast<const uint8_t*>(view);
	size = static_cast<size_t>(fileSize.QuadPart);
	return true;
#else
	fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0)
	{
		error = "Could not open the file.";
		return false;
	}

	struct stat st{};
	if (fstat(fd, &st) != 0 || st.st_size <= 0)
	{
		::close(fd);
		fd = -1;
		error = "The file is empty.";
		return false;
	}

	void* view = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
	if (view == MAP_FAILED)
	{
		::close(fd);
		fd = -1;
		error = "Could not map the file.";
		return false;
	}

	data = static_cast<const uint8_t*>(view);
	size = static_cast<size_t>(st.st_size);
	return true;
#endif
}

void MappedFile::Close()
{
#ifdef _WIN32
	if (data != nullptr)
	{
		UnmapViewOfFile(data);
	}
	if (mappingHandle != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(mappingHandle));
		mappingHandle = nullptr;
	}
	if (fileHandle != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(fileHandle));
		fileHandle = nullptr;
	}
#else
	if (data != nullptr)
	{
		munmap(const_cast<uint8_t*>(data), size);
	}
	if (fd >= 0)
	{
		::close(fd);
		fd = -1;
	}
#endif
	data = nullptr;
	size = 0;
}

void MappedFile::Prefetch() const
{
	if (data == nullptr || size == 0) return;

#ifdef _WIN32
	WIN32_MEMORY_RANGE_ENTRY range{};
	range.VirtualAddress = const_cast<uint8_t*>(data);
	range.NumberOfBytes = size;
	PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
#else
	madvise(const_cast<uint8_t*>(data), size, MADV_WILLNEED);
#endif
}
