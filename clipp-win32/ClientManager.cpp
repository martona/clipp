#include "Logger.h"
#include "ClientManager.h"

#include <algorithm>
#include <iostream>

#include "Client.h"

void ClientManager::AddClient(std::unique_ptr<Client> client) {
    if (!client) {
        return;
    }
    client->Start();
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.push_back(std::move(client));
}

void ClientManager::Cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto before = clients_.size();
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [](const std::unique_ptr<Client>& c) {
        return !c || !c->IsRunning();
    }), clients_.end());

    if (clients_.size() != before) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, L"Client cleanup removed %zu client(s).", (before - clients_.size()));
    }
}

void ClientManager::Terminate() {
    std::vector<std::unique_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients.swap(clients_);
    }

    for (auto& client : clients) {
        if (client) {
            client->Terminate();
        }
    }
}
