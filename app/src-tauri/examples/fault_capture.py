"""Captures the board's serial console to a file, for `fault_daemon --check`.

Run it with the IDF Python env (it has pyserial): `mise run app:fault-capture -- board.log`.
It does not toggle DTR or RTS when it opens the port, so it does not reset the board.
Type `r` + Enter to reset the board with an RTS pulse (the same one esptool's hard reset
sends), and `q` + Enter or Ctrl-C to stop. It refuses to overwrite an existing file.
"""

import argparse
import select
import sys
import time

import serial


def pulse_reset(port: serial.Serial) -> None:
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", help="log file to create")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0
    port.dtr = False
    port.rts = False
    port.open()
    print(f"capturing {args.port} to {args.out}; r+Enter resets the board, q+Enter stops")
    with open(args.out, "xb") as out:
        try:
            watching = [port, sys.stdin]
            while True:
                ready, _, _ = select.select(watching, [], [])
                if port in ready:
                    out.write(port.read(4096))
                    out.flush()
                if sys.stdin in ready:
                    line = sys.stdin.readline()
                    if line == "":  # EOF: keep capturing, stop reading commands
                        watching = [port]
                        continue
                    command = line.strip()
                    if command == "r":
                        pulse_reset(port)
                        print("reset pulse sent")
                    elif command == "q":
                        break
        except KeyboardInterrupt:
            pass
    port.close()


if __name__ == "__main__":
    main()
