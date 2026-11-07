#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <pthread.h>

#include "cli.h"
#include "kv_rpc/service.h"

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: kv_server DB_PATH [HOST [PORT]]\n";
    return 2;
  }
  minirpc::Endpoint endpoint{argc >= 3 ? argv[2] : "127.0.0.1", 9000};
  if (argc == 4 && !ParsePort(argv[3], &endpoint.port)) {
    std::cerr << "PORT must be an integer in 1..65535\n";
    return 2;
  }
  // Block before any DB or reactor workers exist; the main thread owns shutdown.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  const int mask_error = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  if (mask_error != 0) {
    std::cerr << "pthread_sigmask: " << std::strerror(mask_error) << '\n';
    return 1;
  }
  try {
    kv::Options options;
    options.db_path = argv[1];
    kv::DB db(options);
    auto status = db.Open();
    if (!status.ok()) {
      std::cerr << status.ToString() << '\n';
      return 1;
    }
    minirpc::RpcServer server;
    kv::rpc::RegisterService(server, db);
    const auto started = server.Start(endpoint);
    if (!started.ok()) {
      std::cerr << started.ToString() << '\n';
      return 1;
    }
    std::cout << "KVService listening on " << endpoint.ToString()
              << "; data=" << options.db_path << std::endl;
    int received = 0;
    const int wait_error = sigwait(&signals, &received);
    server.Stop(std::chrono::seconds(5));
    status = db.Close();
    if (wait_error != 0)
      std::cerr << "sigwait: " << std::strerror(wait_error) << '\n';
    if (!status.ok()) std::cerr << status.ToString() << '\n';
    return wait_error == 0 && status.ok() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
