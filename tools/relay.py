#!/usr/bin/env python3
"""Stream the Pocket Voice device's output to this Mac over USB.

`arduino-cli monitor` reads nothing from this board: the ESP32-S3's
USB-Serial-JTAG peripheral only counts itself connected while the host asserts
DTR, and the monitor leaves the modem lines alone to avoid tripping the chip's
auto-reset. So assert DTR and RTS by hand, then just read.

    ./tools/relay.py                 # follow everything
    ./tools/relay.py --only "[say]"  # transcripts only
    ./tools/relay.py --copy          # each transcript onto the clipboard
    ./tools/relay.py --type          # paste it into whatever app is in front
    ./tools/relay.py --send          # hand it to Blurt, which inserts it

--type needs Accessibility permission for the program running this script
(System Settings > Privacy & Security > Accessibility). Without it the paste
silently does nothing, which is how macOS reports a missing grant.

--send is the better one: Blurt does the inserting, so the text inherits its
focus pinning and its editable-target check instead of a blind Cmd-V. Turn the
switch on in Blurt's Settings > Advanced > Local insertion, copy the token, and
pass it as --token or in BLURT_INSERT_TOKEN.
"""
import argparse
import fcntl
import os
import pathlib
import socket
import struct
import subprocess
import sys
import termios
import time

TIOCM_DTR = 0x0002
TIOCM_RTS = 0x0004
TIOCMBIS = 0x8004746C  # _IOW('t', 108, int) on Darwin

MARKER = "[say] "


def open_port(dev, baud=115200):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
    iflag = oflag = lflag = 0
    cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
    cc = list(cc)
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, baud, baud, cc])
    # The whole point: without DTR the board's CDC stays "disconnected" and
    # drops every byte it writes.
    fcntl.ioctl(fd, TIOCMBIS, struct.pack("I", TIOCM_DTR | TIOCM_RTS))
    return fd


def unescape(line):
    """The device's wire form back into real text.

    `\\n` is a newline and `\\\\` a backslash; any other backslash pair passes
    through untouched. Same rule Blurt's listener applies, so one convention
    runs from the microphone to the text field.
    """
    out = []
    i = 0
    while i < len(line):
        c = line[i]
        if c == "\\" and i + 1 < len(line):
            nxt = line[i + 1]
            if nxt == "n":
                out.append("\n")
                i += 2
                continue
            if nxt == "\\":
                out.append("\\")
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def find_socket():
    """Blurt's socket, without hardcoding a product name a dev build changes."""
    base = pathlib.Path.home() / "Library" / "Application Support"
    preferred = base / "Blurt" / "insert.sock"
    if preferred.exists():
        return preferred
    found = sorted(base.glob("*/insert.sock"))
    return found[0] if found else preferred


class BlurtInsert:
    """One connection to Blurt's loopback socket.

    The token is the first line of the connection; after that it is one
    transcript per line and one reply per line. Blurt arms its next read only
    once the current line has been inserted, so a reply is also the signal
    that it is ready for the next one.
    """

    def __init__(self, path, token):
        self.path = str(path)
        self.token = token
        self.sock = None
        self.buf = b""
        self.dead = False      # set when retrying cannot help

    def _connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(15)
        s.connect(self.path)
        self.sock = s
        self.buf = b""
        reply = self._exchange(self.token)
        if reply != "OK":
            self.close()
            self.dead = True
            raise ConnectionError(
                f"Blurt refused the token ({reply or 'no reply'}). "
                "Copy it again from Settings > Advanced > Local insertion.")

    def _readline(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("Blurt closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").strip()

    def _exchange(self, line):
        self.sock.sendall(line.encode("utf-8") + b"\n")
        return self._readline()

    def send(self, escaped):
        """`escaped` is the device's wire form, passed through as-is."""
        if self.dead:
            return None
        for attempt in (1, 2):             # one silent reconnect
            try:
                if self.sock is None:
                    self._connect()
                return self._exchange(escaped)
            except ConnectionError:
                raise
            except OSError:
                self.close()
                if attempt == 2:
                    return None
        return None

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


def clipboard_get():
    try:
        return subprocess.run(["pbpaste"], capture_output=True,
                              timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return None


def clipboard_set(data):
    try:
        subprocess.run(["pbcopy"], input=data, timeout=5, check=False)
    except (OSError, subprocess.SubprocessError):
        pass


def paste_front():
    """Cmd-V into the frontmost app. Needs Accessibility permission."""
    script = 'tell application "System Events" to keystroke "v" using command down'
    try:
        r = subprocess.run(["osascript", "-e", script],
                           capture_output=True, timeout=10)
        if r.returncode != 0:
            sys.stderr.write(
                "# paste failed - grant Accessibility permission to this "
                "program, then try again\n")
            return False
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def deliver(text, mode):
    """Put one transcript where the user asked for it."""
    if mode == "print":
        return
    previous = clipboard_get()
    clipboard_set(text.encode())
    if mode == "type":
        paste_front()
        # Give the paste time to land before handing the clipboard back.
        time.sleep(0.5)
        if previous is not None:
            clipboard_set(previous)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem1101")
    ap.add_argument("--only", default=None,
                    help="only print lines containing this substring")
    ap.add_argument("--copy", action="store_true",
                    help="put each transcript on the clipboard")
    ap.add_argument("--type", action="store_true", dest="type_it",
                    help="paste each transcript into the frontmost app")
    ap.add_argument("--send", action="store_true",
                    help="hand each transcript to Blurt, which inserts it")
    ap.add_argument("--socket", default=None,
                    help="Blurt's insert socket (default: found under "
                         "~/Library/Application Support)")
    ap.add_argument("--token", default=os.environ.get("BLURT_INSERT_TOKEN"),
                    help="Blurt's local-insertion token, or set "
                         "BLURT_INSERT_TOKEN")
    args = ap.parse_args()

    if args.send:
        mode = "send"
    elif args.type_it:
        mode = "type"
    elif args.copy:
        mode = "copy"
    else:
        mode = "print"

    sender = None
    if mode == "send":
        if not args.token:
            sys.exit("--send needs Blurt's token: pass --token, or set "
                     "BLURT_INSERT_TOKEN.\nBlurt > Settings > Advanced > "
                     "Local insertion > Copy token.")
        sock_path = pathlib.Path(args.socket) if args.socket else find_socket()
        if not sock_path.exists():
            sys.exit(f"no socket at {sock_path}\nTurn on Blurt > Settings > "
                     "Advanced > Local insertion, and make sure Blurt is "
                     "running.")
        sender = BlurtInsert(sock_path, args.token)

    try:
        fd = open_port(args.port)
    except OSError as e:
        sys.exit(f"cannot open {args.port}: {e}")

    note = {"print": "",
            "copy": " - copying transcripts",
            "type": " - pasting transcripts into the front app",
            "send": " - handing transcripts to Blurt"}[mode]
    print(f"# relay on {args.port}{note} (ctrl-c to stop)", file=sys.stderr)

    buf = b""
    try:
        while True:
            try:
                chunk = os.read(fd, 512)
            except BlockingIOError:
                chunk = b""
            if not chunk:
                time.sleep(0.01)
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")

                if text.startswith(MARKER):
                    escaped = text[len(MARKER):]
                    if mode == "send":
                        try:
                            reply = sender.send(escaped)
                        except ConnectionError as e:
                            sys.exit(f"# {e}")
                        # COPIED is not a failure: nothing editable had focus,
                        # so Blurt put it on the clipboard instead.
                        flag = {"OK": "", "COPIED": "  [clipboard]"}.get(
                            reply, f"  [{reply or 'no reply'}]")
                        print(unescape(escaped) + flag, flush=True)
                    else:
                        said = unescape(escaped)
                        deliver(said, mode)
                        print(said, flush=True)
                    continue

                if args.only and args.only not in text:
                    continue
                print(text, flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        if sender is not None:
            sender.close()
        os.close(fd)


if __name__ == "__main__":
    main()
