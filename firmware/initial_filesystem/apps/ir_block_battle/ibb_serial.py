"""Optional USB serial controls for IR Block Battle development."""

import sys

try:
    from badge import serial_available, serial_read
except Exception:
    serial_available = None
    serial_read = None

try:
    import select
except Exception:
    select = None


HELP = (
    "[IBB] serial controls: Enter=start, a/d=move, s=soft, w/space=hard, "
    "j/k=rotate, h=hold, g/f/r/t=setup 1/2/3/4, 2/3/4=send attack"
)


class SerialControls:
    def __init__(self):
        self.ready = False
        self._mode = ""
        self._poller = None
        if serial_available is not None and serial_read is not None:
            self.ready = True
            self._mode = "badge"
            print(HELP)
            return
        if select is None:
            return
        try:
            self._poller = select.poll()
            self._poller.register(sys.stdin, select.POLLIN)
            self.ready = True
            self._mode = "select"
            print(HELP)
        except Exception as exc:
            self.ready = False
            print("[IBB] serial controls unavailable: {}".format(exc))

    def poll(self, limit=8):
        if not self.ready:
            return ()

        chars = []
        for _i in range(limit):
            if self._mode == "badge":
                try:
                    if not serial_available():
                        break
                    ch = serial_read()
                except Exception:
                    break
            else:
                try:
                    events = self._poller.poll(0)
                except Exception:
                    return tuple(chars)
                if not events:
                    break

                try:
                    ch = sys.stdin.read(1)
                except Exception:
                    break
            if not ch:
                break
            chars.append(ch)

        return tuple(chars)


def normalize(ch):
    if isinstance(ch, bytes):
        try:
            ch = ch.decode()
        except Exception:
            return ""
    if ch in ("\r", "\n"):
        return "\n"
    if ch == " ":
        return " "
    try:
        return ch.lower()
    except Exception:
        return ch
