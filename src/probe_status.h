// HTTP status policy shared by the probe and directed tests.
#pragma once

namespace pulse {

inline bool ProbeHttpStatusOk(unsigned long code, bool expect204=false) {
    return expect204 ? code==204 : code >= 200 && code < 300;
}

}  // namespace pulse
