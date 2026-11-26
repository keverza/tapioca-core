#include "SunStudy/ZipArchive.hpp"

#include <array>

namespace evp::sunstudy {

namespace {

// ---- inflate (RFC 1951), canonical Huffman decoding one bit at a time ------

struct Bits {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint32_t buffer = 0;
    int count = 0;
    bool overrun = false;

    // `need` <= 16 bits, least significant first.
    int Take (int need)
    {
        uint32_t value = buffer;
        while (count < need) {
            if (pos >= size) {
                overrun = true;
                return 0;
            }
            value |= uint32_t (data[pos++]) << count;
            count += 8;
        }
        buffer = value >> need;
        count -= need;
        return int (value & ((1u << need) - 1u));
    }
};

constexpr int kMaxBits = 15;

struct Huffman {
    std::array<int16_t, kMaxBits + 1> count {};
    std::array<int16_t, 288> symbol {};
};

// < 0 over-subscribed, 0 complete, > 0 incomplete.
int Build (Huffman& h, const int16_t* length, int n)
{
    h.count.fill (0);
    for (int s = 0; s < n; ++s)
        ++h.count[size_t (length[s])];
    if (h.count[0] == n)
        return 0; // no codes: complete, and decoding fails if one is asked for
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[size_t (len)];
        if (left < 0)
            return left;
    }
    std::array<int16_t, kMaxBits + 1> offsets {};
    for (int len = 1; len < kMaxBits; ++len)
        offsets[size_t (len + 1)] = int16_t (offsets[size_t (len)] + h.count[size_t (len)]);
    for (int s = 0; s < n; ++s)
        if (length[s] != 0)
            h.symbol[size_t (offsets[size_t (length[s])]++)] = int16_t (s);
    return left;
}

int Decode (Bits& in, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= in.Take (1);
        if (in.overrun)
            return -1;
        const int count = h.count[size_t (len)];
        if (code - count < first)
            return h.symbol[size_t (index + (code - first))];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1; // ran out of codes
}

constexpr int16_t kLengthBase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr int16_t kLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr int16_t kDistanceBase[30] = { 1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                        33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
constexpr int16_t kDistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

bool Codes (Bits& in, const Huffman& lengths, const Huffman& distances, size_t limit, std::vector<uint8_t>& out,
            std::string& error)
{
    for (;;) {
        int symbol = Decode (in, lengths);
        if (symbol < 0) {
            error = "the compressed data is damaged";
            return false;
        }
        if (symbol == 256)
            return true;
        if (symbol < 256) {
            if (out.size () >= limit) {
                error = "a member inflates beyond its stated size";
                return false;
            }
            out.push_back (uint8_t (symbol));
            continue;
        }
        symbol -= 257;
        if (symbol >= 29) {
            error = "the compressed data is damaged";
            return false;
        }
        const size_t length = size_t (kLengthBase[symbol] + in.Take (kLengthExtra[symbol]));
        symbol = Decode (in, distances);
        if (symbol < 0 || symbol >= 30) {
            error = "the compressed data is damaged";
            return false;
        }
        const size_t distance = size_t (kDistanceBase[symbol] + in.Take (kDistanceExtra[symbol]));
        if (in.overrun || distance > out.size ()) {
            error = "the compressed data is damaged";
            return false;
        }
        if (out.size () + length > limit) {
            error = "a member inflates beyond its stated size";
            return false;
        }
        for (size_t i = 0; i < length; ++i)
            out.push_back (out[out.size () - distance]);
    }
}

bool Fixed (Bits& in, size_t limit, std::vector<uint8_t>& out, std::string& error)
{
    static const auto tables = [] {
        std::pair<Huffman, Huffman> t;
        int16_t length[288];
        int s = 0;
        for (; s < 144; ++s)
            length[s] = 8;
        for (; s < 256; ++s)
            length[s] = 9;
        for (; s < 280; ++s)
            length[s] = 7;
        for (; s < 288; ++s)
            length[s] = 8;
        Build (t.first, length, 288);
        for (s = 0; s < 30; ++s)
            length[s] = 5;
        Build (t.second, length, 30);
        return t;
    }();
    return Codes (in, tables.first, tables.second, limit, out, error);
}

bool Dynamic (Bits& in, size_t limit, std::vector<uint8_t>& out, std::string& error)
{
    static constexpr int16_t kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    error = "the compressed data is damaged";
    const int nlen = in.Take (5) + 257, ndist = in.Take (5) + 1, ncode = in.Take (4) + 4;
    if (in.overrun || nlen > 286 || ndist > 30)
        return false;
    int16_t length[320] = {};
    for (int i = 0; i < ncode; ++i)
        length[kOrder[i]] = int16_t (in.Take (3));
    Huffman lengths, distances;
    if (in.overrun || Build (lengths, length, 19) != 0)
        return false;
    for (int index = 0; index < nlen + ndist;) {
        int symbol = Decode (in, lengths);
        if (symbol < 0)
            return false;
        if (symbol < 16) {
            length[index++] = int16_t (symbol);
            continue;
        }
        int16_t repeat = 0;
        if (symbol == 16) {
            if (index == 0)
                return false;
            repeat = length[index - 1];
            symbol = 3 + in.Take (2);
        }
        else if (symbol == 17)
            symbol = 3 + in.Take (3);
        else
            symbol = 11 + in.Take (7);
        if (in.overrun || index + symbol > nlen + ndist)
            return false;
        while (symbol-- > 0)
            length[index++] = repeat;
    }
    if (length[256] == 0)
        return false; // no end-of-block code
    int left = Build (lengths, length, nlen);
    if (left < 0 || (left > 0 && nlen - lengths.count[0] != 1))
        return false;
    left = Build (distances, length + nlen, ndist);
    if (left < 0 || (left > 0 && ndist - distances.count[0] != 1))
        return false;
    error.clear ();
    return Codes (in, lengths, distances, limit, out, error);
}

bool Stored (Bits& in, size_t limit, std::vector<uint8_t>& out, std::string& error)
{
    in.buffer = 0; // to a byte boundary
    in.count = 0;
    if (in.pos + 4 > in.size) {
        error = "the compressed data is damaged";
        return false;
    }
    const unsigned length = unsigned (in.data[in.pos]) | (unsigned (in.data[in.pos + 1]) << 8);
    const unsigned complement = unsigned (in.data[in.pos + 2]) | (unsigned (in.data[in.pos + 3]) << 8);
    in.pos += 4;
    if (length != (~complement & 0xffffu) || in.pos + length > in.size) {
        error = "the compressed data is damaged";
        return false;
    }
    if (out.size () + length > limit) {
        error = "a member inflates beyond its stated size";
        return false;
    }
    out.insert (out.end (), in.data + in.pos, in.data + in.pos + length);
    in.pos += length;
    return true;
}

// ---- the archive -------------------------------------------------------------

uint16_t U16 (const std::vector<uint8_t>& b, size_t at)
{
    return uint16_t (b[at] | (b[at + 1] << 8));
}

uint32_t U32 (const std::vector<uint8_t>& b, size_t at)
{
    return uint32_t (b[at]) | (uint32_t (b[at + 1]) << 8) | (uint32_t (b[at + 2]) << 16) | (uint32_t (b[at + 3]) << 24);
}

} // namespace

uint32_t Crc32 (const uint8_t* data, size_t size)
{
    static const auto table = [] {
        std::array<uint32_t, 256> t {};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool Inflate (const uint8_t* data, size_t size, size_t expected, std::vector<uint8_t>& out, std::string& error)
{
    out.clear ();
    out.reserve (expected);
    Bits in { data, size };
    int last = 0;
    do {
        last = in.Take (1);
        const int type = in.Take (2);
        if (in.overrun) {
            error = "the compressed data ends early";
            return false;
        }
        const bool ok = type == 0   ? Stored (in, expected, out, error)
                        : type == 1 ? Fixed (in, expected, out, error)
                        : type == 2 ? Dynamic (in, expected, out, error)
                                    : (error = "the compressed data is damaged", false);
        if (!ok)
            return false;
    } while (last == 0);
    return true;
}

bool ListZipMembers (const std::vector<uint8_t>& zip, std::vector<ZipMember>& members, std::string& error)
{
    members.clear ();
    // The end-of-central-directory record: 22 bytes plus a comment of up to 64 KB.
    constexpr size_t kEnd = 22;
    if (zip.size () < kEnd) {
        error = "not a zip archive";
        return false;
    }
    size_t end = SIZE_MAX;
    const size_t lowest = zip.size () > kEnd + 0xFFFF ? zip.size () - kEnd - 0xFFFF : 0;
    for (size_t at = zip.size () - kEnd + 1; at-- > lowest;)
        if (U32 (zip, at) == 0x06054b50u) {
            end = at;
            break;
        }
    if (end == SIZE_MAX) {
        error = "not a zip archive";
        return false;
    }
    if (U16 (zip, end + 4) != 0 || U16 (zip, end + 6) != 0) {
        error = "multi-part zip archives are not read";
        return false;
    }
    const size_t entries = U16 (zip, end + 10);
    const size_t directory = U32 (zip, end + 16);
    if (entries == 0xFFFF || directory == 0xFFFFFFFFu) {
        error = "ZIP64 archives are not read";
        return false;
    }
    size_t at = directory;
    for (size_t i = 0; i < entries; ++i) {
        if (at + 46 > zip.size () || U32 (zip, at) != 0x02014b50u) {
            error = "the zip directory is damaged";
            return false;
        }
        ZipMember m;
        m.flags = U16 (zip, at + 8);
        m.method = U16 (zip, at + 10);
        m.crc32 = U32 (zip, at + 16);
        m.compressedSize = U32 (zip, at + 20);
        m.size = U32 (zip, at + 24);
        const size_t nameLength = U16 (zip, at + 28), extra = U16 (zip, at + 30), comment = U16 (zip, at + 32);
        m.localHeaderOffset = U32 (zip, at + 42);
        if (at + 46 + nameLength > zip.size ()) {
            error = "the zip directory is damaged";
            return false;
        }
        m.name.assign (reinterpret_cast<const char*> (zip.data () + at + 46), nameLength);
        members.push_back (std::move (m));
        at += 46 + nameLength + extra + comment;
    }
    return true;
}

bool ExtractZipMember (const std::vector<uint8_t>& zip, const ZipMember& member, std::vector<uint8_t>& out,
                       std::string& error)
{
    out.clear ();
    if (member.flags & 1u) {
        error = member.name + " is encrypted";
        return false;
    }
    if (member.size > kZipMemberLimit) {
        error = member.name + " is larger than 64 MB";
        return false;
    }
    const size_t local = member.localHeaderOffset;
    if (local + 30 > zip.size () || U32 (zip, local) != 0x04034b50u) {
        error = "the zip entry for " + member.name + " is damaged";
        return false;
    }
    const size_t start = local + 30 + U16 (zip, local + 26) + U16 (zip, local + 28);
    if (start > zip.size () || zip.size () - start < member.compressedSize) {
        error = "the zip entry for " + member.name + " is cut short";
        return false;
    }
    const uint8_t* data = zip.data () + start;
    if (member.method == 0) {
        if (member.compressedSize != member.size) {
            error = "the zip entry for " + member.name + " is damaged";
            return false;
        }
        out.assign (data, data + member.size);
    }
    else if (member.method == 8) {
        if (!Inflate (data, member.compressedSize, member.size, out, error)) {
            error = member.name + ": " + error;
            return false;
        }
    }
    else {
        error = member.name + " uses a compression method that is not read (" + std::to_string (member.method) +
                "); unzip it first";
        return false;
    }
    if (out.size () != member.size || Crc32 (out.data (), out.size ()) != member.crc32) {
        error = member.name + " does not match its checksum";
        out.clear ();
        return false;
    }
    return true;
}

} // namespace evp::sunstudy
