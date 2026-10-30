#ifndef GEOMETRYSERVER_HTTPSERVER_HPP
#define GEOMETRYSERVER_HTTPSERVER_HPP

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace geomsrv {

// Local data-plane server: binds 127.0.0.1, serves geometry endpoints and the
// same command bus used by the Python runtime.
// All ACAPI/Modeler access is marshalled to the main thread by the dispatcher.
class HttpServer {
  public:
    HttpServer ();
    ~HttpServer ();

    bool Start (const char* host = "127.0.0.1", int port = 19191);
    void Stop ();
    bool IsRunning () const
    {
        return running.load ();
    }
    int Port () const
    {
        return boundPort;
    }

  private:
    void RegisterRoutes ();

    std::unique_ptr<httplib::Server> server;
    std::thread listenThread;
    std::atomic<bool> running { false };
    int boundPort { 0 };
};

// One process-owned data plane shared by the palette and external commands,
// so independent callers cannot bind a second port.
HttpServer& SharedHttpServer ();
void ShutdownSharedHttpServer ();

} // namespace geomsrv

#endif
