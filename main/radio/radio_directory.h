#ifndef RADIO_DIRECTORY_H
#define RADIO_DIRECTORY_H

// Finds internet radio stations in the community directory radio-browser.info: free, no API
// key, thousands of stations including many Vietnamese ones. The results feed
// RadioPlayer::PlayList(), which falls back to the next station when one does not work.
//
// The directory is run by volunteers: be gentle (a handful of requests per search, small result
// lists) and expect an occasional dead stream. Only MP3 / AAC / HLS stations are returned
// because those are the formats RadioPlayer can decode.

#include <string>
#include <vector>

#include "radio_player.h"

class RadioDirectory {
public:
    // name: part of the station name ("" = any); country: ISO 3166 code like "VN" ("" = any);
    // tag: genre such as "pop" or "news" ("" = any). Most-listened stations first.
    static bool Search(const std::string& name, const std::string& country, const std::string& tag,
                       int limit, std::vector<RadioSource>* stations, std::string* error);
};

#endif  // RADIO_DIRECTORY_H
