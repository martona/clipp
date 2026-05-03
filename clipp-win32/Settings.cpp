#include "Settings.h"

std::string Settings::multicastIp() {
    return "239.255.10.10";
}

int Settings::mdnsPort() {
    return 15353;
}

int Settings::tcpPort() {
    return 15353;
}
