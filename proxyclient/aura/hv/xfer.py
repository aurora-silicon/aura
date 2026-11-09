# SPDX-License-Identifier: MIT
"""Host side of the HvXfer bulk host<->guest channel (src/hv/windows/hv_xfer.c).

The guest writes one command word to a hooked doorbell page; EL2 raises one
HV_XFER proxy event; this module moves the whole buffer with one
REQ_MEMWRITE/REQ_MEMREAD over the proxy pipe and answers. The cost is one VM
exit per buffer instead of one per byte.

:class:`XferHostDevice` only talks to a small ``transport`` object with
``readmem``/``writemem``, so the protocol runs off-hardware unchanged
(tests/hv_xfer/).

Safety posture. The guest names streams, so it could otherwise name a host
file. It cannot:

* GET streams are served read-only from ONE directory (``serve_dir``), by a
  plain basename resolved at OPEN time against a directory descriptor pinned
  when the engine starts. The final component is opened with O_NOFOLLOW, so a
  symlink is refused by the kernel rather than by a racy pre-check, and
  anything that is not a regular file is refused. The file is read once at
  OPEN; what the guest receives is that snapshot, and its size and SHA-256 are
  recorded per OPEN.
* PUT streams land create-only in a separate ``inbox``: bytes go to a hidden
  ``.<name>.<pid>.<n>.part`` created with O_EXCL, and CLOSE publishes it with
  link(2), which fails rather than replace an existing name. A guest that dies
  mid-transfer leaves a visible ``.part``, never a truncated file that looks
  complete. No inbox means PUT is disabled (NODEV).
* Every command is logged; every stream records its byte count and SHA-256.
"""

import gzip
import hashlib
import json
import os
import re
import stat
import time
from pathlib import Path

from construct import Struct, Int64ul, Int32ul, Int32sl

__all__ = [
    "XferExcInfo", "XferHostDevice", "ProxyTransport", "sanitise_name", "HV_XFER_ID", "HV_XFER_VERSION", "HV_XFER_REGS_SIZE",
    "HV_XFER_PAGE_SIZE", "HV_XFER_NAME_SIZE", "HV_XFER_MAX_WINDOW",
    "CMD_NOP", "CMD_PING", "CMD_OPEN", "CMD_GET", "CMD_PUT", "CMD_CLOSE",
    "CMD_SET_WINDOW", "CMD_RESET_WINDOW", "CMD_NAMES",
    "ST_OK", "ST_INVAL", "ST_NODEV", "ST_IO", "ST_RANGE", "ST_FAULT", "ST_BADCMD",
    "TAG_WRITE", "TAG_ID_MASK", "REG",
]

# Mirrors struct hv_xfer_exc_info in src/hv/windows/hv_xfer.h -- keep in step.
XferExcInfo = Struct(
    "devbase" / Int64ul,
    "win_phys" / Int64ul,
    "win_size" / Int64ul,
    "name_phys" / Int64ul,
    "cmd" / Int32ul,
    "tag" / Int32ul,
    "off" / Int32ul,
    # The C struct's `len`; renamed to keep Container access off the builtin.
    "length" / Int32ul,
    "result" / Int32ul,
    "status" / Int32sl,
)

HV_XFER_ID = 0x46585648  # 'HVXF'
HV_XFER_VERSION = 1
HV_XFER_REGS_SIZE = 0x4000
HV_XFER_PAGE_SIZE = 0x4000
HV_XFER_NAME_SIZE = 64
HV_XFER_MAX_WINDOW = 64 << 20

#: Doorbell register offsets, mirroring the HV_XFER_REG_* defines.
REG = {
    "ID": 0x000, "VERSION": 0x004, "FEATURES": 0x008, "PAGESIZE": 0x00c,
    "WIN_LO": 0x010, "WIN_HI": 0x014, "WIN_SIZE": 0x018, "MAX_XFER": 0x01c,
    "CMD": 0x020, "OFF": 0x024, "LEN": 0x028, "TAG": 0x02c,
    "STATUS": 0x030, "RESULT": 0x034, "SEQ": 0x038, "ERRORS": 0x03c,
    "UWIN_LO": 0x040, "UWIN_HI": 0x044, "UWIN_SIZE": 0x048,
    "NAME": 0x080,
}

CMD_NOP = 0
CMD_PING = 1
CMD_OPEN = 2
CMD_GET = 3
CMD_PUT = 4
CMD_CLOSE = 5
CMD_SET_WINDOW = 6
CMD_RESET_WINDOW = 7
CMD_NAMES = {CMD_NOP: "NOP", CMD_PING: "PING", CMD_OPEN: "OPEN", CMD_GET: "GET",
             CMD_PUT: "PUT", CMD_CLOSE: "CLOSE", CMD_SET_WINDOW: "SET_WINDOW",
             CMD_RESET_WINDOW: "RESET_WINDOW"}

ST_OK = 0
ST_INVAL = -1
ST_NODEV = -2
ST_IO = -3
ST_RANGE = -4
ST_FAULT = -5
ST_BADCMD = -6

#: TAG bit 31 selects the direction at OPEN time.
TAG_WRITE = 1 << 31
TAG_ID_MASK = 0x7fffffff

#: OPEN reports the stream size in a 32-bit RESULT, so nothing larger can ever
#: be described to the guest. The configurable cap is lower still.
_ABSOLUTE_MAX_STREAM = 0xffffffff
DEFAULT_MAX_STREAM = 256 << 20

_SAFE_NAME = re.compile(r"\A[A-Za-z0-9][A-Za-z0-9._+-]{0,62}\Z")
_CLOEXEC = getattr(os, "O_CLOEXEC", 0)


def sanitise_name(raw):
    """The guest's 64 raw NAME bytes as a plain basename, or None.

    Accepts exactly what the guest driver's apple_hvxfer_name_ok() accepts
    (``^[A-Za-z0-9][A-Za-z0-9._+-]{0,62}$``), minus any name containing
    ``..``. There is no stripping or normalisation: a name either is a plain
    basename or it is refused.
    """
    if isinstance(raw, (bytes, bytearray)):
        raw = bytes(raw).split(b"\0", 1)[0]
        try:
            raw = raw.decode("ascii")
        except UnicodeDecodeError:
            return None
    if not isinstance(raw, str) or not _SAFE_NAME.match(raw):
        return None
    if ".." in raw or "/" in raw or "\\" in raw or os.path.isabs(raw):
        return None
    return raw


class ProxyTransport:
    """The real transport: Aura's proxy, with optional in-target gunzip.

    ``gzdec`` is the resident's P_GZDEC op, the one compressed_writemem() uses
    to land the Mu image. The decompression runs in EL2 while the guest is
    already stopped, so a compressible payload costs less wire time.
    """

    #: Below this, framing and the gzip header cost more than they save.
    COMPRESS_MIN = 4096
    #: Only bother if compression actually wins by a clear margin.
    COMPRESS_RATIO = 0.9

    def __init__(self, iface, proxy, heap, compress=True):
        self.iface = iface
        self.p = proxy
        self.heap = heap
        self.compress = compress
        self.wire_bytes = 0

    def readmem(self, addr, size):
        data = self.iface.readmem(addr, size)
        self.wire_bytes += len(data)
        return data

    def writemem(self, addr, data):
        if self.compress and len(data) >= self.COMPRESS_MIN:
            payload = gzip.compress(data, compresslevel=1)
            if len(payload) < len(data) * self.COMPRESS_RATIO:
                with self.heap.guarded_malloc(len(payload)) as scratch:
                    self.iface.writemem(scratch, payload)
                    self.wire_bytes += len(payload)
                    out = self.p.gzdec(scratch, len(payload), addr, len(data))
                    if out != len(data):
                        raise IOError(f"gzdec produced {out} bytes, expected {len(data)}")
                return
        self.iface.writemem(addr, data)
        self.wire_bytes += len(data)


def _open_dir(path, label):
    path = Path(path)
    if not path.is_absolute():
        raise ValueError(f"{label} must be an absolute path: {path}")
    try:
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | _CLOEXEC)
    except OSError as error:
        raise ValueError(f"{label} is not a real directory (symlinks refused): {path}: "
                         f"{error.strerror}") from error
    return fd


class _ReadStream:
    def __init__(self, sid, name, data, record):
        self.sid = sid
        self.name = name
        self.data = data
        self.pos = 0
        self.hash = hashlib.sha256()
        self.record = record


class _WriteStream:
    def __init__(self, sid, name, part, fd, record):
        self.sid = sid
        self.name = name
        self.part = part
        self.fd = fd
        self.written = 0
        self.hash = hashlib.sha256()
        self.record = record

    def abandon(self):
        try:
            os.close(self.fd)
        except OSError:
            pass
        self.record["abandoned"] = True


class XferHostDevice:
    """Serves HV_XFER commands for one mapped channel."""

    def __init__(self, serve_dir=None, inbox=None, *, max_stream_bytes=DEFAULT_MAX_STREAM,
                 log_path=None, verbose=True):
        if not 0 < max_stream_bytes <= _ABSOLUTE_MAX_STREAM:
            raise ValueError("max_stream_bytes must be in 1..0xffffffff")
        self.max_stream_bytes = max_stream_bytes
        self.verbose = verbose
        self.serve_dir = Path(serve_dir) if serve_dir is not None else None
        self.inbox = Path(inbox) if inbox is not None else None
        self._serve_fd = _open_dir(self.serve_dir, "serve_dir") if self.serve_dir else None
        self._inbox_fd = None
        if self.inbox is not None:
            self._inbox_fd = _open_dir(self.inbox, "inbox")
            if self._serve_fd is not None:
                a, b = os.fstat(self._serve_fd), os.fstat(self._inbox_fd)
                real_s, real_i = os.path.realpath(self.serve_dir), os.path.realpath(self.inbox)
                if ((a.st_dev, a.st_ino) == (b.st_dev, b.st_ino) or
                        os.path.commonpath([real_s, real_i]) in (real_s, real_i)):
                    self.close()
                    raise ValueError("inbox must be separate from serve_dir (neither inside the other)")
        self.streams = {}
        self.records = []
        self.commands = 0
        self.failures = 0
        self.bytes_out = 0
        self.bytes_in = 0
        self.seconds = 0.0
        self._part_seq = 0
        self.log_path = Path(log_path) if log_path is not None else None
        self._log = open(self.log_path, "a", buffering=1) if self.log_path is not None else None

    # -- bookkeeping ---------------------------------------------------------

    def _emit(self, entry):
        if self._log is not None:
            self._log.write(json.dumps(entry, sort_keys=True) + "\n")
        if self.verbose:
            note = f" ({entry['note']})" if entry.get("note") else ""
            print(f"hv_xfer: #{entry['n']} {entry['cmd']} tag={entry['tag']:#x} "
                  f"off={entry['off']:#x} len={entry['len']:#x} -> status={entry['status']} "
                  f"result={entry['result']:#x}{note}", flush=True)

    def summary(self):
        return {
            "serve_dir": str(self.serve_dir) if self.serve_dir else None,
            "inbox": str(self.inbox) if self.inbox else None,
            "put_enabled": self._inbox_fd is not None,
            "max_stream_bytes": self.max_stream_bytes,
            "log": str(self.log_path) if self.log_path else None,
            "commands": self.commands,
            "failures": self.failures,
            "bytes_out": self.bytes_out,
            "bytes_in": self.bytes_in,
            "host_seconds": round(self.seconds, 6),
            "streams": [dict(r) for r in self.records],
        }

    def close(self):
        self.close_all()
        for attr in ("_serve_fd", "_inbox_fd"):
            fd = getattr(self, attr, None)
            if fd is not None:
                os.close(fd)
                setattr(self, attr, None)
        log = getattr(self, "_log", None)
        if log is not None:
            log.close()
            self._log = None

    def close_all(self):
        """Abandon every open stream; committed files are left alone."""
        for stream in getattr(self, "streams", {}).values():
            if isinstance(stream, _WriteStream):
                stream.abandon()
            else:
                stream.record["served_sha256"] = stream.hash.hexdigest()
        if hasattr(self, "streams"):
            self.streams.clear()

    # -- protocol ------------------------------------------------------------

    def dispatch(self, transport, cmd, tag, off, length, win_phys, win_size, name_phys=0):
        """Execute one doorbell command; returns ``(status, result)``.

        Never raises: the guest's vCPU is parked waiting for this answer, and
        an exception here would strand it inside the proxy.
        """
        start = time.monotonic()
        self.commands += 1
        entry = {"n": self.commands, "t": round(time.time(), 6),
                 "cmd": CMD_NAMES.get(cmd, f"#{cmd}"), "tag": tag, "off": off, "len": length,
                 "win_phys": win_phys, "win_size": win_size}
        try:
            status, result, note, stream = self._dispatch(transport, cmd, tag, off, length,
                                                          win_phys, win_size, name_phys)
        except Exception as exc:  # noqa: BLE001 - see docstring
            status, result, note, stream = ST_IO, 0, f"engine exception {exc!r}", None
        elapsed = time.monotonic() - start
        self.seconds += elapsed
        if status != ST_OK:
            self.failures += 1
        entry.update(status=status, result=result & 0xffffffff, stream=stream,
                     ms=round(elapsed * 1000, 3))
        if note:
            entry["note"] = note
        try:
            self._emit(entry)
        except Exception:  # noqa: BLE001 - logging must never strand the guest
            pass
        return status, result & 0xffffffff

    def _dispatch(self, transport, cmd, tag, off, length, win_phys, win_size, name_phys):
        if cmd == CMD_NOP:
            return ST_OK, 0, None, None
        if cmd == CMD_PING:
            return ST_OK, HV_XFER_VERSION, None, None
        if cmd == CMD_OPEN:
            return self._open(transport, tag, name_phys)

        sid = tag & TAG_ID_MASK
        stream = self.streams.get(sid)
        name = stream.name if stream is not None else None

        if cmd == CMD_CLOSE:
            if stream is None:
                return ST_INVAL, 0, f"CLOSE of unknown stream {sid}", None
            del self.streams[sid]
            return self._close(stream)

        if cmd in (CMD_GET, CMD_PUT):
            if stream is None:
                return ST_INVAL, 0, f"no open stream {sid}", None
            if off >= win_size or length == 0 or length > win_size - off:
                return ST_RANGE, 0, "outside the window", name
            if cmd == CMD_GET:
                if not isinstance(stream, _ReadStream):
                    return ST_INVAL, 0, "GET on a PUT stream", name
                chunk = stream.data[stream.pos:stream.pos + length]
                if not chunk:
                    return ST_OK, 0, "EOF", name
                transport.writemem(win_phys + off, chunk)
                stream.pos += len(chunk)
                stream.hash.update(chunk)
                self.bytes_out += len(chunk)
                record = stream.record
                record["served"] = stream.pos
                if stream.pos == len(stream.data):
                    record["served_sha256"] = stream.hash.hexdigest()
                    record["complete"] = True
                return ST_OK, len(chunk), None, name
            if not isinstance(stream, _WriteStream):
                return ST_INVAL, 0, "PUT on a GET stream", name
            if stream.written + length > self.max_stream_bytes:
                return ST_IO, 0, "PUT exceeds max_stream_bytes", name
            data = transport.readmem(win_phys + off, length)
            if len(data) != length:
                return ST_IO, 0, "short window read", name
            view = memoryview(data)
            while view:
                view = view[os.write(stream.fd, view):]
            stream.written += length
            stream.hash.update(data)
            stream.record["received"] = stream.written
            self.bytes_in += length
            return ST_OK, length, None, name

        return ST_BADCMD, 0, None, None

    def _open(self, transport, tag, name_phys):
        if not name_phys:
            return ST_INVAL, 0, "no name field", None
        raw = bytes(transport.readmem(name_phys, HV_XFER_NAME_SIZE))
        name = sanitise_name(raw)
        if name is None:
            return ST_INVAL, 0, f"unsafe stream name {raw.rstrip(bytes(1))!r}", None

        sid = tag & TAG_ID_MASK
        stale = self.streams.pop(sid, None)
        note = None
        if stale is not None:
            # A guest that crashed mid-transfer never sent CLOSE. Dropping the
            # stale stream is what makes a retry work; a PUT leaves its .part.
            if isinstance(stale, _WriteStream):
                stale.abandon()
            else:
                stale.record["served_sha256"] = stale.hash.hexdigest()
            note = f"reopened, dropped stale {stale.name!r}"

        if tag & TAG_WRITE:
            status, result, why = self._open_put(sid, name)
        else:
            status, result, why = self._open_get(sid, name)
        if why:
            note = f"{note}; {why}" if note else why
        return status, result, note, name

    def _open_get(self, sid, name):
        if self._serve_fd is None:
            return ST_NODEV, 0, "GET disabled: no serve_dir"
        flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | _CLOEXEC
        try:
            fd = os.open(name, flags, dir_fd=self._serve_fd)
        except FileNotFoundError:
            return ST_NODEV, 0, "not in serve_dir"
        except OSError as error:
            # O_NOFOLLOW on a symlink is ELOOP (EMLINK on some BSDs).
            return ST_INVAL, 0, f"refused: {error.strerror}"
        try:
            st = os.fstat(fd)
            if not stat.S_ISREG(st.st_mode):
                return ST_INVAL, 0, "refused: not a regular file"
            if st.st_size > self.max_stream_bytes:
                return ST_IO, 0, f"refused: {st.st_size} bytes exceeds max_stream_bytes"
            parts, total = [], 0
            while True:
                block = os.read(fd, 1 << 20)
                if not block:
                    break
                total += len(block)
                if total > self.max_stream_bytes:
                    return ST_IO, 0, "refused: grew past max_stream_bytes while reading"
                parts.append(block)
        finally:
            os.close(fd)
        data = b"".join(parts)
        record = {"sid": sid, "name": name, "direction": "get",
                  "path": str(self.serve_dir / name), "opened": round(time.time(), 6),
                  "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                  "served": 0, "served_sha256": None, "complete": len(data) == 0,
                  "closed": False}
        if not data:
            record["served_sha256"] = record["sha256"]
        self.records.append(record)
        self.streams[sid] = _ReadStream(sid, name, data, record)
        return ST_OK, len(data), f"snapshot {len(data)} bytes sha256={record['sha256']}"

    def _open_put(self, sid, name):
        if self._inbox_fd is None:
            return ST_NODEV, 0, "PUT disabled: no inbox"
        try:
            os.stat(name, dir_fd=self._inbox_fd, follow_symlinks=False)
            return ST_IO, 0, "refused: inbox name exists (create-only)"
        except FileNotFoundError:
            pass
        self._part_seq += 1
        part = f".{name}.{os.getpid()}.{self._part_seq}.part"
        try:
            fd = os.open(part, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | _CLOEXEC,
                         0o644, dir_fd=self._inbox_fd)
        except OSError as error:
            return ST_IO, 0, f"refused: {error.strerror}"
        record = {"sid": sid, "name": name, "direction": "put",
                  "path": str(self.inbox / name), "part": str(self.inbox / part),
                  "opened": round(time.time(), 6), "received": 0, "sha256": None,
                  "committed": False, "closed": False}
        self.records.append(record)
        self.streams[sid] = _WriteStream(sid, name, part, fd, record)
        return ST_OK, 0, None

    def _close(self, stream):
        record = stream.record
        record["closed"] = True
        if isinstance(stream, _ReadStream):
            record["served_sha256"] = stream.hash.hexdigest()
            return ST_OK, stream.pos, None, stream.name
        try:
            os.fsync(stream.fd)
        finally:
            os.close(stream.fd)
        record["sha256"] = stream.hash.hexdigest()
        try:
            os.link(stream.part, stream.name, src_dir_fd=self._inbox_fd,
                    dst_dir_fd=self._inbox_fd)
        except FileExistsError:
            return ST_IO, 0, "refused at commit: inbox name exists; .part kept", stream.name
        os.unlink(stream.part, dir_fd=self._inbox_fd)
        os.fsync(self._inbox_fd)
        record["committed"] = True
        record.pop("part", None)
        return (ST_OK, stream.written, f"committed {stream.written} bytes sha256={record['sha256']}",
                stream.name)
