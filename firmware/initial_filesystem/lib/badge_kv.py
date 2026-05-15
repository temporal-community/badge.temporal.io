"""badge_kv — friendly wrapper around badge.kv_put/get/delete/keys.

The native primitives are flat (badge.kv_put, badge.kv_get, ...) — this
module re-exposes them under a `kv` namespace so callers can write
``from badge_kv import kv`` and use ``kv.get`` / ``kv.put`` like a
normal dict-ish API.

Why use this instead of ``open('save.json', 'w')``?
---------------------------------------------------
NVS state survives **every** flash type (firmware OTA, factory
fatfs.bin reflash, Community Apps install). Files on FATFS get wiped
by a ``fatfs.bin`` reflash. Use ``kv`` for game saves, scores, user
prefs that must persist across firmware updates.

Limits:
  * 15 chars per key (NVS hard limit; ASCII printable, no ``"`` or backslash)
  * 1 KB per value
  * 64 keys per badge
  * Supported value types: ``str``, ``int``, ``float``, ``bytes``

Example::

    from badge_kv import kv

    score = kv.get("hi_breaksnake", 0)
    score += 1
    kv.put("hi_breaksnake", score)
"""

import badge as _badge


class _KV:
    def get(self, key, default=None):
        """Return the stored value (typed) or ``default`` if absent."""
        return _badge.kv_get(key, default)

    def put(self, key, value):
        """Store ``value`` under ``key``. Raises TypeError on unsupported
        value types and OSError if NVS is full."""
        _badge.kv_put(key, value)

    def set(self, key, value):
        """Alias for :meth:`put` to feel more dict-like."""
        _badge.kv_put(key, value)

    def delete(self, key):
        """Remove ``key`` if present. Returns True on delete, False on miss."""
        return _badge.kv_delete(key)

    def keys(self):
        """Return a list of stored key names."""
        return _badge.kv_keys()

    def __contains__(self, key):
        return key in _badge.kv_keys()


kv = _KV()
