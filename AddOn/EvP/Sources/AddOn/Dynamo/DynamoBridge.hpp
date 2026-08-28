#ifndef EVP_DYNAMOBRIDGE_HPP
#define EVP_DYNAMOBRIDGE_HPP

#include "UniString.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace evp::dynamo {

enum class DynamoClient { Editor, Headless };

class DynamoBridge {
  public:
    static DynamoBridge& Get ();

    bool Start (GS::UniString& error);
    void Stop ();
    void SetClientProcess (DynamoClient client, uint32_t processId, void* processHandle);
    GS::UniString PipeName () const;

  private:
    DynamoBridge () = default;
    ~DynamoBridge ();
    DynamoBridge (const DynamoBridge&) = delete;
    DynamoBridge& operator= (const DynamoBridge&) = delete;

    void Run (void* firstPipe);

    std::atomic<bool> stopping { false };
    std::atomic<bool> running { false };
    std::atomic<uint32_t> editorProcessId { 0 };
    std::atomic<void*> editorProcessHandle { nullptr };
    std::atomic<uint32_t> headlessProcessId { 0 };
    std::atomic<void*> headlessProcessHandle { nullptr };
    std::thread worker;
    GS::UniString pipeName;
};

} // namespace evp::dynamo

#endif
