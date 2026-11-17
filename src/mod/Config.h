#pragma once

#include <string>

namespace custom_skull {

struct Config {
    int         version             = 1;
    bool        enableJavaFetch     = true;
    bool        debug               = false;
    std::string skullNameFormat     = "\u00a7e{}\u7684\u5934\u9885";
    int         maxEntriesPerPacket = 8;
    bool        keepSkinListed      = true;
};

} // namespace custom_skull

