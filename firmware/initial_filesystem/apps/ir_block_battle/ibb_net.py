"""Small repeated IR event protocol for IR Block Battle."""

import random
import time

from badge import *

MAGIC = 0xB
TYPE_HELLO = 1
TYPE_STATE = 2
TYPE_ATTACK = 3
TYPE_OVER = 4
TYPE_NAME = 5

SEND_GAP_MS = 230
HELLO_MS = 850
STATE_MS = 1500
NAME_MS = 6000
NAME_MAX = 8
DUP_MS = 3000


def _kind_name(kind):
    if kind == TYPE_HELLO:
        return "hello"
    if kind == TYPE_STATE:
        return "state"
    if kind == TYPE_ATTACK:
        return "attack"
    if kind == TYPE_OVER:
        return "over"
    if kind == TYPE_NAME:
        return "name"
    return "kind" + str(kind)


def _sender_id():
    try:
        uid = my_uuid()
        compact = "".join(ch for ch in uid if ch in "0123456789abcdefABCDEF")
        if compact:
            return int(compact[-3:], 16) & 0x0FFF
    except Exception:
        pass
    return random.randint(1, 0x0FFE)


def _clean_name(value):
    allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_"
    out = ""
    for ch in str(value).upper():
        if ch in allowed:
            out += ch
        elif ch == ".":
            out += " "
        if len(out) >= NAME_MAX:
            break
    return out.strip()


def _local_name():
    try:
        with open("/badgeInfo.txt", "r") as info:
            for line in info.read().split("\n"):
                if line.strip().startswith("name"):
                    eq = line.find("=")
                    if eq >= 0:
                        return _clean_name(line[eq + 1:])
    except Exception:
        pass
    return ""


def _ticks_add(now, delta):
    try:
        return time.ticks_add(now, delta)
    except AttributeError:
        return now + delta


class DuelLink:
    def __init__(self):
        self.sender = _sender_id()
        self.local_name = _local_name()
        self.peer = -1
        self.peer_name = ""
        self.linked = False
        self.ready = False
        self.seq = 0
        self.last_rx = 0
        self.last_send = 0
        self.last_hello = 0
        self.last_state = 0
        self.last_name = 0
        self.peer_pending = 0
        self.peer_over = False
        self._name_tx_index = NAME_MAX + 1
        self._name_rx = [""] * NAME_MAX
        self._name_rx_started = False
        self._name_rx_seen = 0
        self._seen = {}
        self._queue = []

    def start(self):
        try:
            ir_start()
            try:
                ir_tx_power(50)
            except Exception:
                pass
            ir_flush()
            self.ready = True
            print("[IBB] ir ready sender={:03x}".format(self.sender))
            if self.local_name:
                print("[IBB] local name {}".format(self.local_name))
        except Exception:
            self.ready = False
            print("[IBB] ir unavailable")

    def stop(self):
        if not self.ready:
            return
        try:
            ir_stop()
        except Exception:
            pass
        self.ready = False

    def _pack(self, kind, seq, payload):
        return (
            (MAGIC << 28)
            | ((self.sender & 0x0FFF) << 16)
            | ((kind & 0x0F) << 12)
            | ((seq & 0x0F) << 8)
            | (payload & 0xFF)
        )

    def _send_now(self, kind, seq, payload, now):
        if not self.ready:
            return False
        try:
            ir_send_words([self._pack(kind, seq, payload)])
            self.last_send = now
            if kind != TYPE_HELLO and kind != TYPE_NAME:
                print(
                    "[IBB] tx {} seq={} payload={}".format(
                        _kind_name(kind), seq, payload
                    )
                )
            return True
        except Exception as exc:
            print("[IBB] tx failed {}: {}".format(_kind_name(kind), exc))
            return False

    def queue(self, kind, payload, repeats=3, seq=None):
        if seq is None:
            self.seq = (self.seq + 1) & 0x0F
            seq = self.seq
        self._queue.append([kind, seq & 0x0F, payload & 0xFF, repeats, 0])
        if len(self._queue) > 6:
            self._queue.pop(0)
        if kind != TYPE_HELLO and kind != TYPE_NAME:
            print(
                "[IBB] queue {} seq={} payload={} repeats={}".format(
                    _kind_name(kind), seq & 0x0F, payload & 0xFF, repeats
                )
            )

    def send_attack(self, lines):
        if lines > 0:
            self.queue(TYPE_ATTACK, lines, 3)

    def send_over(self):
        self.queue(TYPE_OVER, 0, 4)

    def _service_name_tx(self, now):
        if not self.linked or not self.local_name:
            return False

        name_len = len(self.local_name)
        if self._name_tx_index > name_len:
            if time.ticks_diff(now, self.last_name) < NAME_MS:
                return False
            self._name_tx_index = 0

        payload = 0
        if self._name_tx_index < name_len:
            payload = ord(self.local_name[self._name_tx_index])

        self.queue(TYPE_NAME, payload, 2, self._name_tx_index & 0x0F)
        self._name_tx_index += 1
        if payload == 0:
            self.last_name = now
        return True

    def _accept_name(self, seq, payload):
        if seq == 0 and payload:
            self._name_rx = [""] * NAME_MAX
            self._name_rx_started = True
            self._name_rx_seen = 0
        if payload == 0:
            if not self._name_rx_started or seq > NAME_MAX:
                return
            expected = (1 << seq) - 1
            if (self._name_rx_seen & expected) != expected:
                return
            name = _clean_name("".join(self._name_rx))
            if name and name != self.peer_name:
                self.peer_name = name
                print("[IBB] peer name {}".format(name))
            self._name_rx_started = False
            return
        if not self._name_rx_started:
            return
        if seq < NAME_MAX and 32 <= payload <= 126:
            self._name_rx[seq] = chr(payload)
            self._name_rx_seen |= 1 << seq

    def service_tx(self, now, pending):
        if not self.ready:
            return
        if time.ticks_diff(now, self.last_send) < SEND_GAP_MS:
            return

        if self._queue:
            item = self._queue[0]
            if not item[4] or time.ticks_diff(now, item[4]) >= 0:
                if self._send_now(item[0], item[1], item[2], now):
                    item[3] -= 1
                    item[4] = _ticks_add(now, SEND_GAP_MS)
                    if item[3] <= 0:
                        self._queue.pop(0)
                return

        if time.ticks_diff(now, self.last_hello) >= HELLO_MS:
            if self._send_now(TYPE_HELLO, 0, 0, now):
                self.last_hello = now
            return

        if self.linked and time.ticks_diff(now, self.last_state) >= STATE_MS:
            if self._send_now(TYPE_STATE, 0, pending, now):
                self.last_state = now
            return

        if self._service_name_tx(now):
            return

    def service_rx(self, now):
        if not self.ready:
            return 0
        attacks = 0
        for _i in range(5):
            try:
                words = ir_read_words()
            except Exception:
                words = None
            if words is None:
                break
            if not words:
                continue
            word = int(words[0])
            if (word >> 28) != MAGIC:
                continue
            sender = (word >> 16) & 0x0FFF
            if sender == self.sender:
                continue

            kind = (word >> 12) & 0x0F
            seq = (word >> 8) & 0x0F
            payload = word & 0xFF
            peer_changed = self.peer != sender
            was_linked = self.linked and self.peer == sender
            if peer_changed:
                self.peer_name = ""
                self._name_rx = [""] * NAME_MAX
                self._name_rx_started = False
                self._name_rx_seen = 0
            self.peer = sender
            self.linked = True
            self.last_rx = now
            if not was_linked:
                self._name_tx_index = 0
                print("[IBB] link peer={:03x}".format(sender))

            if kind == TYPE_STATE:
                self.peer_pending = payload
            elif kind == TYPE_ATTACK:
                seen = self._seen.get(sender)
                duplicate = seen and seen[0] == seq and time.ticks_diff(now, seen[1]) < DUP_MS
                if not duplicate:
                    self._seen[sender] = (seq, now)
                    attacks += payload
                    print("[IBB] rx attack seq={} lines={}".format(seq, payload))
                else:
                    print("[IBB] rx duplicate attack seq={}".format(seq))
            elif kind == TYPE_OVER:
                self.peer_over = True
                print("[IBB] rx over")
            elif kind == TYPE_NAME:
                self._accept_name(seq, payload)

        if self.linked and time.ticks_diff(now, self.last_rx) > 5000:
            self.linked = False
            print("[IBB] link timeout")
        return attacks
