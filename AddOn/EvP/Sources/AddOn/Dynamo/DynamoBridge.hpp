#ifndef EVP_DYNAMOBRIDGE_HPP
#define EVP_DYNAMOBRIDGE_HPP

#include "UniString.hpp"

#include <atomic>
#include <thread>

namespace evp::dynamo {

class DynamoBridge {
  public:
    static DynamoBridge& Get ();

    bool Start (GS::UniString& error);
    void Stop ();
    GS::UniString PipeName () const;

  private:
    DynamoBridge () = default;
    ~DynamoBridge ();
    DynamoBridge (const DynamoBridge&) = delete;
    DynamoBridge& operator= (const DynamoBridge&) = delete;

    void Run (void* firstPipe);

    std::atomic<bool> stopping { false };
    std::atomic<bool> running { false };
    std::thread worker;
    GS::UniString pipeName;
};

} // namespace evp::dynamo

#endif
