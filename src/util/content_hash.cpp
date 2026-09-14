#include "util/content_hash.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <fstream>
#include <vector>

namespace nexus::util {

std::optional<std::string> ComputeFileSha256(
    const std::filesystem::path& path)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
    {
        return std::nullopt;
    }

    DWORD objectLength = 0;
    DWORD propertySize = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
            &propertySize, 0)))
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    std::vector<UCHAR> object(objectLength);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            algorithm, &hash, object.data(), objectLength, nullptr, 0, 0)))
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    std::vector<UCHAR> buffer(1024 * 1024);
    while (input)
    {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0 &&
            !BCRYPT_SUCCESS(BCryptHashData(
                hash, buffer.data(), static_cast<ULONG>(count), 0)))
        {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return std::nullopt;
        }
    }
    if (input.bad())
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    std::array<UCHAR, 32> digest = {};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (UCHAR byte : digest)
    {
        result.push_back(kHex[(byte >> 4) & 0x0f]);
        result.push_back(kHex[byte & 0x0f]);
    }
    return result;
}

}  // namespace nexus::util
