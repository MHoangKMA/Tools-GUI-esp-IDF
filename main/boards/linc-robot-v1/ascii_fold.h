#ifndef ASCII_FOLD_H
#define ASCII_FOLD_H

// Turns UTF-8 text into printable ASCII for the built-in 5x7 font of the song OLED: Vietnamese
// letters lose their accents ("Bài ca" -> "Bai ca", "Đường" -> "Duong"), everything else that is
// not ASCII becomes '?'. Plain C++, no ESP-IDF headers, so it can be unit-tested on a PC.

#include <string>

std::string AsciiFold(const std::string& utf8);

#endif  // ASCII_FOLD_H
