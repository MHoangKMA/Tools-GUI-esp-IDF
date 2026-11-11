#ifndef URL_UTIL_H
#define URL_UTIL_H

// Percent-encoding helpers shared by the radio / music code and the robot web page.
// Plain C++, no ESP-IDF headers, so they can be unit-tested on a PC.

#include <string>

namespace url {

// Encodes everything except letters, digits and - _ . ~
std::string Encode(const std::string& text);

// Reverses Encode(); also turns '+' into a space. Invalid %XX sequences are kept as they are.
std::string Decode(const std::string& text);

}  // namespace url

#endif  // URL_UTIL_H
