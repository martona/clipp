#include "platform.h"

#ifdef __APPLE__

#include "Logger.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr char kMacOSControlCommandOpenMainWindow[] = "open-main-window\n";

std::atomic_bool g_controlServerRunning{ false };
std::thread g_controlServerThread;
std::atomic_int g_controlServerSocket{ -1 };
}

static std::string MacOSControlSocketPath() {
    return "/tmp/net.clipp.ios." + std::to_string(static_cast<long long>(geteuid())) + ".sock";
}

static bool FillMacOSControlSocketAddress(sockaddr_un& address, const std::string& socketPath) {
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "macOS control socket path is too long: %s", socketPath.c_str());
        return false;
    }
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    return true;
}

static bool SendMacOSControlCommand(const char* command) {
    const std::string socketPath = MacOSControlSocketPath();

    sockaddr_un address{};
    if (!FillMacOSControlSocketAddress(address, socketPath)) {
        return false;
    }

    const int clientSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (clientSocket == -1) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, "Failed to create macOS control socket client (errno=%d).", errno);
        return false;
    }

    int noSigPipe = 1;
    setsockopt(clientSocket, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));

    const bool connected = connect(clientSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    if (!connected) {
        close(clientSocket);
        return false;
    }

    const size_t commandLength = std::strlen(command);
    size_t sentTotal = 0;
    while (sentTotal < commandLength) {
        const ssize_t sent = send(clientSocket, command + sentTotal, commandLength - sentTotal, 0);
        if (sent <= 0) {
            close(clientSocket);
            return false;
        }
        sentTotal += static_cast<size_t>(sent);
    }

    close(clientSocket);
    return true;
}

static void MacOSControlServerThreadProc() {
    while (g_controlServerRunning.load()) {
        const int serverSocket = g_controlServerSocket.load();
        if (serverSocket == -1) {
            break;
        }

        const int clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket == -1) {
            if (g_controlServerRunning.load() && errno == EINTR) {
                continue;
            }
            break;
        }

        char buffer[128]{};
        const ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        close(clientSocket);

        if (bytesRead <= 0) {
            continue;
        }

        if (std::strcmp(buffer, kMacOSControlCommandOpenMainWindow) == 0) {
            RequestMacOSShowMainWindow();
        }
    }
}

static bool BindMacOSControlSocket(const std::string& socketPath, int& serverSocket) {
    sockaddr_un address{};
    if (!FillMacOSControlSocketAddress(address, socketPath)) {
        return false;
    }

    serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to create macOS control socket server (errno=%d).", errno);
        return false;
    }

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        return true;
    }

    const int bindError = errno;
    close(serverSocket);
    serverSocket = -1;
    errno = bindError;
    return false;
}

SingleInstanceResult EnsureSingleInstance() {
    if (g_controlServerRunning.load()) {
        return SingleInstanceResult::Continue;
    }

    const std::string socketPath = MacOSControlSocketPath();
    int serverSocket = -1;
    if (!BindMacOSControlSocket(socketPath, serverSocket)) {
        const int bindError = errno;
        if (bindError != EADDRINUSE) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to bind macOS control socket at %s (errno=%d).", socketPath.c_str(), bindError);
            return SingleInstanceResult::ExitFailure;
        }

        if (SendMacOSControlCommand(kMacOSControlCommandOpenMainWindow)) {
            return SingleInstanceResult::ExitSuccess;
        }

        // A pathname Unix socket can be unlinked even while a live process still owns the bound
        // socket. If we removed it before probing, a second instance could create a fresh socket at
        // the same path and both processes would think they are primary. Only unlink after connect
        // fails, which is the stale-crash-leftover case we actually want to repair.
        if (unlink(socketPath.c_str()) != 0 && errno != ENOENT) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to remove stale macOS control socket at %s (errno=%d).", socketPath.c_str(), errno);
            return SingleInstanceResult::ExitFailure;
        }

        if (!BindMacOSControlSocket(socketPath, serverSocket)) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to bind macOS control socket after stale cleanup at %s (errno=%d).", socketPath.c_str(), errno);
            return SingleInstanceResult::ExitFailure;
        }
    }

    if (listen(serverSocket, 4) != 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to listen on macOS control socket at %s (errno=%d).", socketPath.c_str(), errno);
        close(serverSocket);
        unlink(socketPath.c_str());
        return SingleInstanceResult::ExitFailure;
    }

    g_controlServerSocket.store(serverSocket);
    g_controlServerRunning.store(true);
    try {
        g_controlServerThread = std::thread(MacOSControlServerThreadProc);
    } catch (...) {
        g_controlServerRunning.store(false);
        close(g_controlServerSocket.load());
        g_controlServerSocket.store(-1);
        unlink(socketPath.c_str());
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start macOS control socket thread.");
        return SingleInstanceResult::ExitFailure;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, "macOS control socket listening at %s.", socketPath.c_str());
    return SingleInstanceResult::Continue;
}

void StopSingleInstanceServer() {
    const bool wasRunning = g_controlServerRunning.exchange(false);
    if (!wasRunning && g_controlServerSocket.load() == -1) {
        return;
    }

    SendMacOSControlCommand("stop\n");

    const int serverSocket = g_controlServerSocket.exchange(-1);
    if (serverSocket != -1) {
        shutdown(serverSocket, SHUT_RDWR);
        close(serverSocket);
    }

    if (g_controlServerThread.joinable()) {
        g_controlServerThread.join();
    }

    unlink(MacOSControlSocketPath().c_str());
}

#endif
