#include "ZipResourcePack.h"

std::shared_ptr<std::istream> ZipResourcePack::GetStream(const std::string& name)
{
    int index = mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, 0);
    if (index < 0)
    {
        return nullptr;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, (mz_uint)index, &stat))
        return nullptr;

    std::vector<uint8_t> data(stat.m_uncomp_size);

    if (!mz_zip_reader_extract_to_mem(
        &zip,
        (mz_uint)index,
        data.data(),
        data.size(),
        0))
    {
        return nullptr;
    }

    return std::make_shared<MemoryBufferStream>(std::move(data));
}

std::vector<std::string> ZipResourcePack::GetEntries()
{
    std::vector<std::string> result;

    int count = (int)mz_zip_reader_get_num_files(&zip);

    for (int i = 0; i < count; i++)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            continue;

        std::string name = stat.m_filename;

        if (name == "pack.yml" || name == "pack.yaml")
            continue;

        result.push_back(name);
    }

    return result;
}