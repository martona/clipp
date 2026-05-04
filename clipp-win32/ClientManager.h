#pragma once

#include <memory>
#include <mutex>
#include <vector>

class Client;

class ClientManager {
public:
    void AddClient(std::unique_ptr<Client> client);
    void Cleanup();
    void Terminate();

private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<Client>> clients_;
};
