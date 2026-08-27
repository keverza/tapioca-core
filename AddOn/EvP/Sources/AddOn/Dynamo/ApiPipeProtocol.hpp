#ifndef EVP_APIPIPEPROTOCOL_HPP
#define EVP_APIPIPEPROTOCOL_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace evp::dynamo::protocol {

constexpr uint32_t HeaderSize = 8;
constexpr uint32_t MaxCommandBytes = 1024;
constexpr uint32_t MaxParamsBytes = 8 * 1024 * 1024;
constexpr uint32_t MaxResponseBytes = 16 * 1024 * 1024;
constexpr uint8_t ResponseAck = 0x06;

struct RequestSizes {
    uint32_t commandBytes = 0;
    uint32_t paramsBytes = 0;
};

bool DecodeRequestHeader (const uint8_t* bytes, size_t size, RequestSizes& sizes, std::string& error);
bool IsAllowedCommand (std::string_view command);
std::vector<uint8_t> EncodeRequest (const std::string& command, const std::string& paramsJson);
std::vector<uint8_t> EncodeResponse (const std::string& envelopeJson);
bool DecodeResponseHeader (const uint8_t* bytes, size_t size, uint32_t& envelopeBytes, std::string& error);

} // namespace evp::dynamo::protocol

#endif
