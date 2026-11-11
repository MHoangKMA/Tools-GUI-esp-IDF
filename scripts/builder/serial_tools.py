"""Serial helpers that run INSIDE the ESP-IDF environment (where pyserial is installed).

    python serial_tools.py ports                 # JSON list of serial ports
    python serial_tools.py monitor PORT [BAUD]   # interactive monitor

The monitor prints what the device sends. Lines written to its stdin are commands:
    T<text>   send <text> + newline to the device
    R         reset the board (DTR/RTS pulse)
    B         reset into the ROM bootloader (download mode)
    Q         quit
"""
import json
import sys
import threading
import time


def list_ports():
    from serial.tools import list_ports as lp
    ports = [{"device": p.device, "description": p.description} for p in sorted(lp.comports())]
    print(json.dumps(ports))


def _pulse_reset(ser, bootloader=False):
    # classic ESP auto-reset circuit: DTR -> GPIO0, RTS -> EN
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    if bootloader:
        ser.dtr = True
        ser.rts = False
        time.sleep(0.1)
        ser.dtr = False
    else:
        ser.rts = False


def _commands(ser, stop):
    for raw in sys.stdin:
        raw = raw.rstrip("\r\n")
        if not raw:
            continue
        kind, rest = raw[0], raw[1:]
        try:
            if kind == "T":
                ser.write(rest.encode("utf-8") + b"\n")
            elif kind == "R":
                _pulse_reset(ser)
            elif kind == "B":
                _pulse_reset(ser, bootloader=True)
            elif kind == "Q":
                break
        except Exception as error:  # port unplugged etc.
            print("[ERROR] %s" % error, flush=True)
    stop.set()


def monitor(port, baud):
    import serial
    # Opening the port may reset the board: that is wanted, you see the boot log
    with serial.Serial(port, baud, timeout=0.2) as ser:
        print("--- monitor on %s at %d baud ---" % (port, baud), flush=True)
        stop = threading.Event()
        threading.Thread(target=_commands, args=(ser, stop), daemon=True).start()
        pending = b""
        while not stop.is_set():
            try:
                chunk = ser.read(512)
            except Exception as error:
                print("[ERROR] port lost: %s" % error, flush=True)
                return 1
            if not chunk:
                if pending and b"\n" not in pending:  # flush a prompt that has no newline
                    print(pending.decode("utf-8", errors="replace").rstrip("\r"), flush=True)
                    pending = b""
                continue
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                print(line.decode("utf-8", errors="replace").rstrip("\r"), flush=True)
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "ports":
        list_ports()
    elif len(sys.argv) >= 3 and sys.argv[1] == "monitor":
        sys.exit(monitor(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 115200))
    else:
        print(__doc__)
        sys.exit(2)
