"""Unit tests for the plain-C++ logic of the linc-robot-v1 board and the radio code.

The sources under test have no ESP-IDF dependency, so each test compiles a small driver with the
host C++ compiler. The tests are skipped when no compiler is installed.
"""
import os
import shutil
import subprocess
import tempfile
import textwrap
import unicodedata
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RADIO = ROOT / "main" / "radio"
BOARD = ROOT / "main" / "boards" / "linc-robot-v1"
COMPILER = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")


def nfc(text):
    return unicodedata.normalize("NFC", text)


@unittest.skipUnless(COMPILER, "no host C++ compiler available")
class LincRobotLogicTest(unittest.TestCase):
    def run_driver(self, source, files, includes):
        with tempfile.TemporaryDirectory() as tmp:
            driver = Path(tmp) / "driver.cc"
            driver.write_text(nfc(textwrap.dedent(source)), encoding="utf-8")
            exe = Path(tmp) / "driver"
            command = [COMPILER, "-std=c++20", "-Wall", "-Werror", "-UNDEBUG", str(driver)]
            command += [str(path) for path in files]
            for include in includes:
                command += ["-I", str(include)]
            command += ["-o", str(exe)]
            build = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    # ---------------------------------------------------------------- url_util
    def test_url_encode_decode(self):
        self.run_driver(
            """
            #include <cassert>
            #include <string>
            #include "url_util.h"

            int main() {
                assert(url::Encode("Bài ca") == "B%C3%A0i%20ca");
                assert(url::Encode("a-b_c.d~e") == "a-b_c.d~e");
                assert(url::Encode("x y&z=1/é") == "x%20y%26z%3D1%2F%C3%A9");
                assert(url::Decode("B%C3%A0i+ca") == "Bài ca");
                assert(url::Decode("100%") == "100%");        // dangling percent kept
                assert(url::Decode("%zz%4") == "%zz%4");      // invalid sequences kept
                assert(url::Decode("%41%62") == "Ab");
                const std::string text = "tên bài: Hà Nội & Sài Gòn?";
                assert(url::Decode(url::Encode(text)) == text);
                return 0;
            }
            """,
            [RADIO / "url_util.cc"],
            [RADIO],
        )

    # ---------------------------------------------------------------- hls_playlist
    def test_hls_playlists(self):
        self.run_driver(
            r"""
            #include <cassert>
            #include <string>
            #include "hls_playlist.h"

            int main() {
                // master playlist: the first variant is followed, relative to the master's address
                auto master = ParseHlsPlaylist(
                    "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=64000\nlow/index.m3u8\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=128000\nhigh/index.m3u8\n",
                    "https://example.com/live/master.m3u8");
                assert(master.valid && master.is_master);
                assert(master.variant_url == "https://example.com/live/low/index.m3u8");

                // media playlist with CRLF line ends and the three kinds of segment address
                auto media = ParseHlsPlaylist(
                    "#EXTM3U\r\n#EXT-X-TARGETDURATION:8\r\n#EXT-X-MEDIA-SEQUENCE:100\r\n"
                    "#EXTINF:6,\r\nseg100.aac\r\n#EXTINF:6,\r\n/abs/seg101.aac\r\n"
                    "#EXTINF:6,\r\nhttps://cdn.example.net/seg102.aac?token=1\r\n",
                    "https://example.com/live/index.m3u8?session=9");
                assert(media.valid && !media.is_master && !media.ended && !media.has_fmp4);
                assert(media.media_sequence == 100 && media.target_duration == 8);
                assert(media.segments.size() == 3);
                assert(media.segments[0] == "https://example.com/live/seg100.aac");
                assert(media.segments[1] == "https://example.com/abs/seg101.aac");
                assert(media.segments[2] == "https://cdn.example.net/seg102.aac?token=1");

                auto vod = ParseHlsPlaylist("#EXTM3U\n#EXTINF:6,\na.ts\n#EXT-X-ENDLIST\n", "http://h/x.m3u8");
                assert(vod.ended && vod.segments.size() == 1 && vod.target_duration == 6);

                auto fmp4 = ParseHlsPlaylist("#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:6,\na.m4s\n", "http://h/x.m3u8");
                assert(fmp4.has_fmp4);

                assert(!ParseHlsPlaylist("<html>login</html>", "http://h/x.m3u8").valid);
                assert(!ParseHlsPlaylist("", "http://h/x.m3u8").valid);

                assert(ResolveUrl("https://h/a/b.m3u8?x=1", "c.ts") == "https://h/a/c.ts");
                assert(ResolveUrl("https://h/a/b.m3u8", "//cdn/x.ts") == "https://cdn/x.ts");
                assert(ResolveUrl("https://h", "x.ts") == "https://h/x.ts");
                return 0;
            }
            """,
            [RADIO / "hls_playlist.cc"],
            [RADIO],
        )

    # ---------------------------------------------------------------- radio_channels
    def test_radio_channel_matching(self):
        self.run_driver(
            """
            #include <cassert>
            #include <cstring>
            #include <string>
            #include "radio_channels.h"

            static std::string Id(const char* query) {
                const RadioChannel* channel = RadioChannels::Find(query);
                return channel ? channel->id : "";
            }

            int main() {
                assert(Id("vov1") == "vov1");
                assert(Id("VOV5") == "vov5");
                assert(Id("VOV giao thong Ha Noi") == "vovgt_hn");
                assert(Id("vov giao thông tp.hcm") == "vovgt_hcm");
                assert(Id("giao thông Sài Gòn") == "vovgt_hcm");
                assert(Id("đài Tây Nguyên") == "vov4_taynguyen");
                assert(Id("tiếng Anh") == "vov5_english");
                assert(Id("mê kông") == "vov_mekong");
                assert(Id("VOV") == "vov1");                  // generic name: the news channel
                assert(Id("xyz không có đài này") == "");
                for (const auto& channel : RadioChannels::List()) {
                    assert(std::strncmp(channel.url, "http", 4) == 0);
                    assert(Id(channel.id) == channel.id);       // every id finds itself
                }
                return 0;
            }
            """,
            [RADIO / "radio_channels.cc"],
            [RADIO],
        )

    # ---------------------------------------------------------------- ascii_fold
    def test_ascii_fold(self):
        self.run_driver(
            """
            #include <cassert>
            #include <string>
            #include "ascii_fold.h"

            int main() {
                assert(AsciiFold("Bài ca Đường") == "Bai ca Duong");
                assert(AsciiFold("Nguyễn Huệ") == "Nguyen Hue");
                assert(AsciiFold("Ươm Mầm Ổi") == "Uom Mam Oi");
                assert(AsciiFold("Việt Nam – 2026") == "Viet Nam ? 2026");  // en dash is not ASCII
                assert(AsciiFold("plain ASCII 123") == "plain ASCII 123");
                assert(AsciiFold("日本") == "??");
                assert(AsciiFold("") == "");
                // a truncated UTF-8 sequence must not read past the end
                assert(AsciiFold(std::string("ab\\xE1\\xBB")) == "ab");
                return 0;
            }
            """,
            [BOARD / "ascii_fold.cc"],
            [BOARD],
        )

    # ---------------------------------------------------------------- signal_pattern
    def test_signal_patterns(self):
        self.run_driver(
            """
            #include <cassert>
            #include "signal_pattern.h"

            static bool Is(SignalState s, bool left, bool right) { return s.left == left && s.right == right; }

            int main() {
                // normal blink: 5 ticks (250 ms) on, 5 off, starts on
                assert(Is(SignalAt(SignalPattern::kBoth, 0), true, true));
                assert(Is(SignalAt(SignalPattern::kBoth, 4), true, true));
                assert(Is(SignalAt(SignalPattern::kBoth, 5), false, false));
                assert(Is(SignalAt(SignalPattern::kBoth, 9), false, false));
                assert(Is(SignalAt(SignalPattern::kBoth, 10), true, true));
                assert(Is(SignalAt(SignalPattern::kBoth, -3), true, true));   // negative counts as 0
                // only the chosen side blinks
                assert(Is(SignalAt(SignalPattern::kLeft, 0), true, false));
                assert(Is(SignalAt(SignalPattern::kLeft, 5), false, false));
                assert(Is(SignalAt(SignalPattern::kRight, 0), false, true));
                assert(Is(SignalAt(SignalPattern::kRight, 5), false, false));
                // the two sides take turns
                assert(Is(SignalAt(SignalPattern::kAlternate, 0), true, false));
                assert(Is(SignalAt(SignalPattern::kAlternate, 5), false, true));
                assert(Is(SignalAt(SignalPattern::kAlternate, 10), true, false));
                // fast = 100 ms, slow = 500 ms
                assert(Is(SignalAt(SignalPattern::kFastBoth, 1), true, true));
                assert(Is(SignalAt(SignalPattern::kFastBoth, 2), false, false));
                assert(Is(SignalAt(SignalPattern::kFastBoth, 4), true, true));
                assert(Is(SignalAt(SignalPattern::kSlowBoth, 9), true, true));
                assert(Is(SignalAt(SignalPattern::kSlowBoth, 10), false, false));
                assert(Is(SignalAt(SignalPattern::kSlowBoth, 20), true, true));
                assert(Is(SignalAt(SignalPattern::kOff, 0), false, false));

                // wheel speeds -> signals
                assert(MovementPattern(0, 0) == SignalPattern::kOff);
                assert(MovementPattern(70, 70) == SignalPattern::kBoth);      // forward
                assert(MovementPattern(-70, -70) == SignalPattern::kBoth);    // backward
                assert(MovementPattern(-70, 70) == SignalPattern::kLeft);     // spin left
                assert(MovementPattern(70, -70) == SignalPattern::kRight);    // spin right
                assert(MovementPattern(40, 70) == SignalPattern::kLeft);      // curve left
                assert(MovementPattern(70, 40) == SignalPattern::kRight);     // curve right

                SignalPattern pattern = SignalPattern::kOff;
                assert(ParseSignalMode("LEFT", &pattern) && pattern == SignalPattern::kLeft);
                assert(ParseSignalMode("phai", &pattern) && pattern == SignalPattern::kRight);
                assert(ParseSignalMode("hazard", &pattern) && pattern == SignalPattern::kBoth);
                assert(ParseSignalMode("off", &pattern) && pattern == SignalPattern::kOff);
                assert(!ParseSignalMode("blue", &pattern));

                // light shows that go with the faces
                assert(EffectForEmotion("angry").pattern == SignalPattern::kFastBoth);
                assert(EffectForEmotion("angry").duration_ms == 3000);
                assert(EffectForEmotion("surprised").duration_ms == 600);
                assert(EffectForEmotion("happy").pattern == SignalPattern::kAlternate);
                assert(EffectForEmotion("loving").pattern == SignalPattern::kSlowBoth);
                assert(EffectForEmotion("neutral").pattern == SignalPattern::kOff);
                assert(EffectForEmotion("no_such_face").pattern == SignalPattern::kOff);
                return 0;
            }
            """,
            [BOARD / "signal_pattern.cc"],
            [BOARD],
        )


if __name__ == "__main__":
    unittest.main()
