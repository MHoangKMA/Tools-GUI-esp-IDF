#ifndef RADIO_CHANNELS_H
#define RADIO_CHANNELS_H

// Built-in radio channel table and the name matcher that maps what a speech recogniser returns
// ("VOV giao thong Ha Noi", "đài Tây Nguyên", "tieng Anh") to a channel. Plain C++, no ESP-IDF
// headers, so it can be unit-tested on a PC.

#include <string>
#include <vector>

struct RadioChannel {
    const char* id;    // short id used by MCP tools and the web page, e.g. "vov1"
    const char* name;  // display name
    const char* url;   // HLS playlist or direct stream
};

class RadioChannels {
public:
    static const std::vector<RadioChannel>& List();

    // Matches an id ("vov1") or free text. nullptr when nothing matches.
    static const RadioChannel* Find(const std::string& query);
};

#endif  // RADIO_CHANNELS_H
