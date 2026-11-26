#ifndef EVP_SUNSTUDY_ZIPARCHIVE_HPP
#define EVP_SUNSTUDY_ZIPARCHIVE_HPP

// SunStudy/ZipArchive -- read one member out of a .zip held in memory, so a
// weather file can be opened as downloaded (the climate.onebuilding.org and
// EnergyPlus archives ship each .epw zipped with its .ddy and .stat).
//
// Reads the central directory and inflates "stored" (0) and "deflate" (8)
// members (RFC 1951), checking each against its CRC-32. ⚠️ READ-ONLY AND
// BOUNDED: no encryption, no ZIP64, no multi-disk archives, and a member
// larger than kZipMemberLimit is refused before anything is inflated, so a
// crafted archive cannot exhaust memory.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace evp::sunstudy {

constexpr size_t kZipMemberLimit = size_t (64) << 20; // 64 MB: an hourly .epw is about 1.6 MB

struct ZipMember {
    std::string name; // as stored, '/' separated
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t size = 0;
    uint32_t localHeaderOffset = 0;
};

// The archive's members from its central directory.
bool ListZipMembers (const std::vector<uint8_t>& zip, std::vector<ZipMember>& members, std::string& error);

// One member's bytes, inflated and checked.
bool ExtractZipMember (const std::vector<uint8_t>& zip, const ZipMember& member, std::vector<uint8_t>& out,
                       std::string& error);

// Inflate a raw deflate stream (no zlib or gzip header). `expected` reserves
// the output and caps it: more output than that is an error.
bool Inflate (const uint8_t* data, size_t size, size_t expected, std::vector<uint8_t>& out, std::string& error);

uint32_t Crc32 (const uint8_t* data, size_t size);

} // namespace evp::sunstudy

#endif
