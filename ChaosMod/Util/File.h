#pragma once

#include <sys/stat.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

inline bool DoesFileExist(std::string_view fileName)
{
	struct stat temp;
	return stat(fileName.data(), &temp) == 0;
}

inline bool DoesFeatureFlagExist(const std::string &featureFlagName)
{
	return DoesFileExist("chaosmod\\." + featureFlagName);
}

inline std::vector<std::filesystem::directory_entry> GetFiles(const std::string &path, std::string_view extension,
                                                              bool recursive,
                                                              const std::vector<std::string> &blacklistedFiles = {})
{
	std::vector<std::filesystem::directory_entry> entries;
	const auto expectedExtension = std::filesystem::path(extension);

	std::error_code error;
	if (!std::filesystem::exists(path, error) || error)
		return entries;

	auto handleEntry = [&](const std::filesystem::directory_entry &entry)
	{
		const auto entryPath = entry.path();
		if (entry.is_regular_file() && entryPath.has_extension() && entryPath.extension() == expectedExtension
		    && entry.file_size() > 0)
		{
			bool addFile = true;
			for (const auto &blacklistedFile : blacklistedFiles)
			{
				if (entryPath.string().ends_with(blacklistedFile))
				{
					addFile = false;
					break;
				}
			}

			if (addFile)
				entries.push_back(entry);
		}
	};

	if (recursive)
		for (const auto &entry : std::filesystem::recursive_directory_iterator(path))
			handleEntry(entry);
	else
		for (const auto &entry : std::filesystem::directory_iterator(path))
			handleEntry(entry);

	return entries;
}