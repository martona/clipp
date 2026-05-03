#pragma once
#include <string>

class Settings {
public:
    static std::string multicastIp();
    static int mdnsPort();
};
