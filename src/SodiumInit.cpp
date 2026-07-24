#include "SodiumInit.h"

#include "Logger.h"

#include <sodium.h>

bool InitializeSodium() {
    if (sodium_init() < 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Fatal: libsodium failed to initialize!");
        return false;
    }
    g_logger.log(__FUNCTION__, Logger::Level::Debug, "libsodium initialized successfully.");
    return true;
}
