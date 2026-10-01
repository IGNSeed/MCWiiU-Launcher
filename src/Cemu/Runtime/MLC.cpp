#include "MLC.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "util/helpers/helpers.h"

namespace Cemu::Runtime
{
bool CreateDefaultMLCFiles(const std::filesystem::path& mlc) noexcept
{
    try
    {
        const fs::path directories[] = {
            mlc, mlc / "sys", mlc / "usr",
            mlc / "usr/title/00050000", // Base titles.
            mlc / "usr/title/0005000c", // DLC.
            mlc / "usr/title/0005000e", // Updates.
            mlc / "usr/save/00050010/1004a000/user/common/db", // Mii Maker.
            mlc / "usr/save/00050010/1004a100/user/common/db",
            mlc / "usr/save/00050010/1004a200/user/common/db",
            mlc / "sys/title/0005001b/1005c000/content"
        };
        for (const auto& path : directories)
        {
            fs::create_directories(path);
            if (!fs::is_directory(path))
                return false;
        }

        const auto languageDirectory = mlc / "sys/title/0005001b/1005c000/content";
        const auto languageFile = languageDirectory / "language.txt";
        if (fs::exists(languageFile) && !fs::is_regular_file(languageFile))
            return false;
        if (!fs::exists(languageFile))
        {
            std::ofstream file(languageFile);
            const char* languages[] = {"ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh"};
            for (const auto* language : languages)
                file << '"' << language << "\",\n";
            file.flush();
            if (!file)
                return false;
        }

        const auto countryFile = languageDirectory / "country.txt";
        if (fs::exists(countryFile) && !fs::is_regular_file(countryFile))
            return false;
        if (!fs::exists(countryFile))
        {
            std::ofstream file(countryFile);
            for (sint32 i = 0; i < NCrypto::GetCountryCount(); ++i)
            {
                const char* country = NCrypto::GetCountryAsString(i);
                if (boost::iequals(country, "NN"))
                    file << "NULL,\n";
                else
                    file << '"' << country << "\",\n";
            }
            file.flush();
            if (!file)
                return false;
        }
        // Use the existing random-name probe; never overwrite a user's fixed-name file.
        return TestWriteAccess(mlc);
    }
    catch (...)
    {
        return false;
    }
}
}
