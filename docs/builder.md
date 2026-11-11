# Xiaozhi Builder (công cụ build và nạp có giao diện)

Một cửa sổ giống Arduino IDE: chọn board, chọn ngôn ngữ, bấm **Build**, **Flash**, **Monitor**. Công cụ không tự làm lại gì cả,
nó gọi đúng `scripts/build.py` của dự án và `esptool` trong môi trường ESP-IDF.

## Chuẩn bị

- Python 3.9+ có tkinter (bản cài từ python.org đã có sẵn).
- ESP-IDF 6.0.1 trở lên đã cài (có `export.bat` / `export.sh`). Công cụ tự dò `IDF_PATH`, `C:\Espressif\frameworks`,
  `~/esp`; không thấy thì bấm **Browse...** chọn thư mục `esp-idf`.
- Cắm robot qua USB (cổng USB-UART hoặc USB native của ESP32-S3).

## Mở

- Windows: bấm đúp `scripts\builder_gui.bat`
- Mọi hệ điều hành: `python scripts/builder_gui.py`

## Cách dùng

1. Ô **Board**: gõ để lọc (ví dụ `linc`), chọn `linc-robot-v1`.
2. **Build options**: ngôn ngữ (mặc định theo board), wake word, và các tuỳ chọn riêng của board.
   Chỉ giá trị bạn đổi mới được gửi đi, phần còn lại giữ mặc định của board.
3. **Flash**: chọn cổng (bấm Refresh sau khi cắm), tốc độ nạp (460800 mặc định; hạ 115200 nếu lỗi), tuỳ chọn
   "Erase all flash first" (xoá sạch, mất cấu hình Wi-Fi và admin key).
4. Bấm **Build + Flash**. Thanh tiến trình chạy theo `[n/N]` của ninja, dòng lỗi màu đỏ, cảnh báo màu cam.
5. **Monitor** hiện log từ robot (115200). Bấm **Stop** để dừng bất kỳ tác vụ nào.

Menu **Tools**: Menuconfig (mở cửa sổ mới), Clean (fullclean), tải lại danh sách board.
Menu **File**: nạp một file `.bin` có sẵn (ví dụ bản build của người khác), mở thư mục `build`.

Cài đặt (đường dẫn IDF, board, cổng, tuỳ chọn) được nhớ trong `~/.xiaozhi_builder.json`.

## Kết quả build

Ảnh nạp là `build/merged-binary.bin`, ghi vào địa chỉ `0x0`. Cùng file này dùng cho web flasher
(`docs/web-flasher`) nếu bạn muốn nạp bằng trình duyệt.

## Giới hạn

- Chưa được chạy với ESP-IDF thật trong môi trường phát triển (chỉ test logic, danh sách board, và việc dựng cửa sổ).
  Nếu build lỗi, xem dòng đỏ đầu tiên trong log; lệnh đầy đủ được in ở dòng bắt đầu bằng `$`.
- Menuconfig cần terminal riêng nên mở ra cửa sổ khác, không chạy trong khung log.
- Test: `python scripts/tests/test_builder_core.py`
