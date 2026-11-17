#!/usr/bin/env python3
"""SB Card Builder - prepare an SD card for the ESP32-P4 SB Player from an xLights show folder.

  * reads xlights_networks.xml and writes outputs.json (E1.31 / DDP outputs)
  * converts xLights .fseq files (zstd / zlib / sparse, v1 or v2) to the plain
    uncompressed FSEQ v2 the player reads, and copies the matching audio
  * builds playlists/*.json and schedule.json
  * can instead export sequences for an SB Pixel 16 controller
    (https://github.com/computergeek1507/sb_pixel_16): only that controller's channels

Run with no arguments for the GUI, or see --help for the command line.
zstd-compressed sequences need:  python -m pip install zstandard   (built in from Python 3.14)
"""
import argparse
import json
import ntpath
import os
import queue
import re
import shutil
import struct
import sys
import threading
import xml.etree.ElementTree as ET
import zlib
from dataclasses import dataclass, field

APP = "SB Card Builder"
MAX_E131 = 8          # MAX_E131_OUTPUTS in firmware config.h
MAX_DDP = 8           # MAX_DDP_OUTPUTS
MAX_PLAYLIST = 64     # MAX_PLAYLIST_ENTRIES in scheduler.h
MAX_SCHEDULE = 32     # MAX_SCHEDULE_ENTRIES
MAX_NAME = 100        # firmware path buffers are 128-140 bytes including "/sequences/"
MAX_FILE = 0xFFFFFFFF  # FAT32 limit, and the player seeks with 32-bit offsets
NO_BOOT = "(none)"
PLAYER = "SB Player"
PIXEL16 = "SB Pixel 16: "
PIXEL16_MAX_FRAME = 262144   # FSEQ_MAX_FRAME in sb_pixel_16 fseq_player.h
PIXEL16_MAX_FILES = 64       # FSEQ_MAX_FILES
DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]  # bit0 = Sunday
IPV4 = re.compile(r"^(\d{1,3}\.){3}\d{1,3}$")


# ── xLights networks ──────────────────────────────────────────────────────────

@dataclass
class Controller:
    name: str
    protocol: str
    ip: str
    state: str                    # Active / Inactive / xLights Only
    start: int = 0                # 0-based absolute start channel
    channels: int = 0
    universes: list = field(default_factory=list)   # [(universe, size)] for E1.31
    keep_numbers: bool = True     # DDP "Keep Channel Numbers"

    @property
    def usable(self):
        return self.protocol in ("E131", "DDP")

    def pixel16_layout(self):
        """convert_fseq options that give an SB Pixel 16 just this controller's channels.

        Its ports are set up by start channel. Over E1.31 (and DDP without 'Keep Channel
        Numbers') channel 1 is the controller's first channel, so the file is renumbered
        to match. DDP with 'Keep Channel Numbers' uses xLights' absolute numbers, which a
        sparse range keeps.
        """
        return dict(max_channels=self.channels, first=self.start,
                    sparse=self.protocol == "DDP" and self.keep_numbers, max_frame=PIXEL16_MAX_FRAME)


def _int(el, attr, default=0):
    try:
        return int(el.get(attr, default))
    except (TypeError, ValueError):
        return default


def _add_network(c, net):
    size = _int(net, "MaxChannels", 512)
    count = max(1, _int(net, "NumUniverses", 1)) if net.get("NetworkType") == "E131" else 1
    for i in range(count):
        c.universes.append((_int(net, "BaudRate", 1) + i, size))
        c.channels += size
    if net.get("KeepChannelNumbers", "1") == "0":
        c.keep_numbers = False


def parse_networks(path):
    """Return the controllers of an xlights_networks.xml in channel order."""
    root = ET.parse(path).getroot()
    out, prev_key = [], None
    for el in root:
        if el.tag == "Controller":
            nets = el.findall("network")
            proto = el.get("Protocol") or (nets[0].get("NetworkType") if nets else "") or el.get("Type", "?")
            ip = el.get("IP") or (nets[0].get("ComPort") if nets else "") or ""
            c = Controller(el.get("Name", "?"), proto, ip, el.get("ActiveState", "Active"))
            for n in nets:
                _add_network(c, n)
            out.append(c)
            prev_key = None
        elif el.tag == "network":      # pre-2020 files: bare outputs, group runs to the same device
            key = (el.get("NetworkType"), el.get("ComPort"), el.get("Enabled", "Yes"))
            if key != prev_key:
                state = "Active" if el.get("Enabled", "Yes") != "No" else "Inactive"
                out.append(Controller(el.get("Description") or el.get("ComPort", "?"),
                                      el.get("NetworkType", "?"), el.get("ComPort", ""), state))
                prev_key = key
            _add_network(out[-1], el)
    ch = 0
    for c in out:                      # every controller takes channel space, active or not
        c.start = ch
        ch += c.channels
    return out


def build_outputs(controllers):
    """Map controllers to the player's outputs.json. Returns (doc, warnings, channels_needed)."""
    e131, ddp, warn = [], [], []
    for c in controllers:
        if not c.usable:
            warn.append(f"{c.name}: {c.protocol} is not supported by the player - skipped")
            continue
        multicast = c.ip.upper() == "MULTICAST"
        if not multicast and not IPV4.match(c.ip):
            warn.append(f"{c.name}: address '{c.ip}' is not an IPv4 address (host names are not supported) - skipped")
            continue
        if c.protocol == "DDP":
            if multicast:
                warn.append(f"{c.name}: DDP cannot be multicast - skipped")
                continue
            if not c.keep_numbers and c.start:
                warn.append(f"{c.name}: 'Keep Channel Numbers' is off in xLights, but the player always sends "
                            f"absolute channel offsets - this controller will show the wrong data")
            ddp.append({"enabled": True, "ip": c.ip, "channel_start": c.start, "channel_count": c.channels})
            continue
        # E1.31: one player output = consecutive universes of one size (the last may be shorter)
        runs, ch = [], c.start
        for uni, size in c.universes:
            r = runs[-1] if runs else None
            if r and uni == r["next"] and not r["short"] and size <= r["size"]:
                r["count"] += size
                r["next"] += 1
                r["short"] = size < r["size"]
            else:
                runs.append({"uni": uni, "size": size, "start": ch, "count": size, "next": uni + 1, "short": False})
            ch += size
        if len(runs) > 1:
            warn.append(f"{c.name}: universes are not consecutive / equal sized - uses {len(runs)} player outputs")
        for r in runs:
            e131.append({"enabled": True, "universe_start": r["uni"], "universe_size": r["size"],
                         "channel_start": r["start"], "channel_count": r["count"],
                         "multicast": multicast, "unicast_ip": "" if multicast else c.ip})
    for kind, lst, mx in (("E1.31", e131, MAX_E131), ("DDP", ddp, MAX_DDP)):
        if len(lst) > mx:
            warn.append(f"{len(lst)} {kind} outputs needed but the player supports {mx} - the last "
                        f"{len(lst) - mx} were dropped")
            del lst[mx:]
    needed = max([o["channel_start"] + o["channel_count"] for o in e131 + ddp], default=0)
    return {"e131": e131, "ddp": ddp}, warn, needed


# ── FSEQ ──────────────────────────────────────────────────────────────────────

@dataclass
class FseqInfo:
    major: int
    minor: int
    data_offset: int
    channels: int          # bytes stored per frame
    frames: int
    step_ms: int
    comp: int              # 0 none, 1 zstd, 2 zlib
    blocks: list           # [(first_frame, length)]
    sparse: list           # [(start, count)]
    media: str
    uid: bytes

    @property
    def full_channels(self):
        return max((s + n for s, n in self.sparse), default=self.channels)

    def describe(self):
        comp = {0: "uncompressed", 1: "zstd", 2: "zlib"}.get(self.comp, f"comp {self.comp}")
        sp = f", {len(self.sparse)} sparse ranges" if self.sparse else ""
        secs = self.frames * self.step_ms / 1000
        return (f"v{self.major}.{self.minor} {comp}{sp}, {self.full_channels} ch, {self.frames} frames "
                f"@ {self.step_ms} ms ({secs:.0f} s), media: {self.media or '-'}")


def read_fseq_info(f):
    hdr = f.read(32)
    if len(hdr) < 28 or hdr[0:4] not in (b"PSEQ", b"FSEQ"):
        raise ValueError("not an FSEQ file")
    data_offset, minor, major, var_offset, channels, frames, step = struct.unpack_from("<HBBHIIB", hdr, 4)
    comp, blocks, sparse, uid = 0, [], [], bytes(8)
    if major == 2:
        comp = hdr[20] & 0x0F
        nblocks = hdr[21] | ((hdr[20] >> 4) << 8)
        nsparse = hdr[22]
        uid = hdr[24:32]
        table = f.read(nblocks * 8)
        blocks = [struct.unpack_from("<II", table, i * 8) for i in range(nblocks)]
        sparse = _sparse(f.read(nsparse * 6))
    elif major != 1:
        raise ValueError(f"unsupported FSEQ version {major}.{minor}")
    media = ""
    f.seek(var_offset)
    var = f.read(max(0, data_offset - var_offset))
    pos = 0
    while pos + 4 <= len(var):
        length = struct.unpack_from("<H", var, pos)[0]
        if length < 4:
            break
        code, body = var[pos + 2:pos + 4], var[pos + 4:pos + length]
        if code == b"ED" and len(var) >= pos + 18:      # extended data: real code, file offset, length
            code = var[pos + 4:pos + 6]
            where, size = struct.unpack_from("<QI", var, pos + 6)
            f.seek(where)
            body = f.read(size)
            length = 18
        if code == b"mf":
            media = body.split(b"\0")[0].decode("utf-8", "replace")
        elif code == b"SR":                             # full sparse table when it exceeds 255 ranges
            sparse = _sparse(body)
        pos += length
    return FseqInfo(major, minor, data_offset, channels, frames, step or 50, comp, blocks, sparse, media, uid)


def _sparse(raw):
    return [(int.from_bytes(raw[i:i + 3], "little"), int.from_bytes(raw[i + 3:i + 6], "little"))
            for i in range(0, len(raw) - 5, 6)]


def _decompress(comp, data):
    if comp == 2:
        return zlib.decompressobj(47).decompress(data)     # zlib or gzip wrapper
    if comp != 1:
        raise ValueError(f"unknown compression type {comp}")
    try:
        from compression import zstd                       # Python 3.14+
        return zstd.decompress(data)
    except ImportError:
        pass
    try:
        import zstandard
    except ImportError:
        raise RuntimeError("this sequence is zstd-compressed; install support with:  "
                           "python -m pip install zstandard") from None
    out = bytearray()
    while data:                                            # a block can hold several zstd frames
        d = zstandard.ZstdDecompressor().decompressobj()
        out += d.decompress(data)
        if not d.eof or len(d.unused_data) == len(data):
            break
        data = d.unused_data
    return bytes(out)


def _iter_frames(f, info):
    n = info.channels
    f.seek(info.data_offset)
    if info.comp == 0:
        while True:
            b = f.read(n)
            if len(b) < n:
                return
            yield b
    buf = bytearray()
    for _, length in info.blocks:
        if not length:
            continue
        buf += _decompress(info.comp, f.read(length))
        pos = 0
        while len(buf) - pos >= n:
            yield bytes(buf[pos:pos + n])
            pos += n
        del buf[:pos]


def convert_fseq(src, dst, max_channels=0, progress=None, cancelled=None, first=0, sparse=False, max_frame=0):
    """Write src as uncompressed FSEQ v2 holding max_channels channels (0 = all) from channel `first` (0-based).

    The kept channels are renumbered from 1 unless sparse is set, which stores them as one
    sparse range so they keep their xLights numbers. max_frame > 0 is the largest frame the
    target device can buffer.
    """
    with open(src, "rb") as f:
        info = read_fseq_info(f)
        if not info.channels or not info.frames:
            raise ValueError("sequence has no channel data")
        full = info.full_channels
        if first >= full:
            raise ValueError(f"sequence has {full} channels - nothing at channel {first + 1}")
        out_ch = min(max_channels, full - first) if max_channels > 0 else full - first
        if max_frame and out_ch > max_frame:
            raise ValueError(f"{out_ch} channels per frame, but the controller buffers at most {max_frame}")
        ranges = b""
        if sparse and first:
            if first >= 1 << 24 or out_ch >= 1 << 24:
                raise ValueError("channel numbers are too high for a sparse FSEQ range")
            ranges = struct.pack("<I", first)[:3] + struct.pack("<I", out_ch)[:3]
        var_offset = 32 + len(ranges)
        var = b""
        for code, text in ((b"mf", ntpath.basename(info.media)), (b"sp", APP)):
            if text:
                body = text.encode("utf-8") + b"\0"
                var += struct.pack("<H", len(body) + 4) + code + body
        var += bytes(-(var_offset + len(var)) % 4)
        data_offset = var_offset + len(var)
        size = data_offset + out_ch * info.frames
        if size > MAX_FILE:
            raise ValueError(f"result would be {size / 2**30:.1f} GB; the SD card (FAT32) and player are "
                             f"limited to 4 GB - shorten the sequence or trim channels")
        header = b"PSEQ" + struct.pack("<HBBHIIBBBBBB", data_offset, 0, 2, var_offset, out_ch, info.frames,
                                       info.step_ms, 0, 0, 0, 1 if ranges else 0, 0) + info.uid + ranges
        tmp = dst + ".part"
        done = 0
        try:
            with open(tmp, "wb") as o:
                o.write(header + var)
                for frame in _iter_frames(f, info):
                    if cancelled and cancelled():
                        raise InterruptedError("cancelled")
                    if info.sparse:
                        wide, pos = bytearray(full), 0
                        for start, count in info.sparse:
                            wide[start:start + count] = frame[pos:pos + count]
                            pos += count
                        frame = wide
                    o.write(frame[first:first + out_ch])
                    done += 1
                    if progress and done % 50 == 0:
                        progress(done, info.frames)
                    if done >= info.frames:
                        break
            if done != info.frames:
                raise ValueError(f"file is truncated: {done} of {info.frames} frames found")
            os.replace(tmp, dst)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
    return info, out_ch, size


def find_media(media, search_dirs):
    """Locate the audio file named in an FSEQ header. Returns a path or None."""
    if not media:
        return None
    if os.path.isfile(media):
        return media
    name = ntpath.basename(media)
    for d in search_dirs:
        for sub in ("", "Audio", "audio", "Music", "music", "Media", "media"):
            p = os.path.join(d, sub, name)
            if os.path.isfile(p):
                return p
    return None


def export_sequence(src, out_dir, max_channels=0, copy_audio=True, search_dirs=(), log=print,
                    progress=None, cancelled=None, first=0, sparse=False, max_frame=0):
    seq_dir = os.path.join(out_dir, "sequences")
    os.makedirs(seq_dir, exist_ok=True)
    name = os.path.basename(src)
    if len(name) > MAX_NAME:
        raise ValueError(f"file name is longer than {MAX_NAME} characters - rename it")
    info, out_ch, size = convert_fseq(src, os.path.join(seq_dir, name), max_channels, progress, cancelled,
                                      first, sparse, max_frame)
    log(f"{name}: {info.describe()}")
    if first:
        log(f"   xLights channels {first + 1}-{first + out_ch}, "
            + ("kept as a sparse range" if sparse else "renumbered from 1"))
    log(f"   wrote {out_ch} ch, {size / 2**20:.1f} MB ({out_ch * 1000 / info.step_ms / 2**20:.2f} MB/s read rate)")
    if copy_audio and info.media:
        audio = find_media(info.media, [os.path.dirname(src), *search_dirs])
        if not audio:
            log(f"   WARNING audio not found: {info.media}")
        elif os.path.splitext(audio)[1].lower() not in (".mp3", ".wav"):
            log(f"   WARNING the player only plays .mp3 / .wav - not copied: {os.path.basename(audio)}")
        else:
            dst = os.path.join(seq_dir, os.path.basename(audio))
            if not (os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(audio)):
                shutil.copyfile(audio, dst)
            log(f"   audio: {os.path.basename(audio)}")
    return info


def pixel16_notes(controller, out_dir):
    """Lines to show after exporting for an SB Pixel 16."""
    notes = []
    count = len(list_card_sequences(out_dir))
    if count > PIXEL16_MAX_FILES:
        notes.append(f"WARNING the card has {count} sequences; the SB Pixel 16 plays only the first "
                     f"{PIXEL16_MAX_FILES}")
    if controller.protocol == "DDP" and controller.keep_numbers and controller.start:
        notes.append(f"Port start channels on the SB Pixel 16 are xLights channel numbers "
                     f"(the controller starts at {controller.start + 1}).")
    else:
        notes.append("Port start channels on the SB Pixel 16 count from 1 at the controller's first channel, "
                     "the same as for E1.31.")
    notes.append("Set its input protocol to FSEQ. It loops every file in /sequences; it has no audio, "
                 "playlists or schedule.")
    return notes


# ── Card files ────────────────────────────────────────────────────────────────

def write_json(path, doc):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)


def read_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def list_card_sequences(out_dir):
    d = os.path.join(out_dir, "sequences")
    return sorted(n for n in os.listdir(d) if n.lower().endswith(".fseq")) if os.path.isdir(d) else []


def load_playlists(out_dir):
    d = os.path.join(out_dir, "playlists")
    names = sorted(n for n in os.listdir(d) if n.lower().endswith(".json")) if os.path.isdir(d) else []
    return {n: read_json(os.path.join(d, n), {}) for n in names}


def schedule_entries(days, start, end, playlist):
    """Player windows cannot cross midnight, so split one that does. start/end are minutes."""
    def entry(d, s, e):
        return {"days": d, "start_hour": s // 60, "start_min": s % 60,
                "end_hour": e // 60, "end_min": e % 60, "playlist": playlist}
    if end > start:
        return [entry(days, start, end)]
    out = [entry(days, start, 24 * 60)]
    if end > 0:
        out.append(entry(((days << 1) | (days >> 6)) & 0x7F, 0, end))   # same nights, next morning
    return out


def days_text(mask):
    return "Every day" if mask & 0x7F == 0x7F else " ".join(d for i, d in enumerate(DAYS) if mask >> i & 1)


# ── Player update ─────────────────────────────────────────────────────────────

FIRMWARE_FILE = "firmware.bin"   # SD_FIRMWARE_FILE in firmware sdupdate.h
LITTLEFS_FILE = "littlefs.bin"   # SD_LITTLEFS_FILE
UPDATE_NAMES = {"firmware": FIRMWARE_FILE, "littlefs": LITTLEFS_FILE}
ESP_IMAGE_MAGIC = 0xE9           # first byte of an ESP application image
ESP32P4_CHIP_ID = 0x0012         # esp_image_header_t.chip_id, at byte 12


def check_update_image(head, size, kind):
    """Why the first 8 KB of a file (head) is not a player update of this kind, or None if it looks right."""
    if kind == "firmware":
        if len(head) < 24 or head[0] != ESP_IMAGE_MAGIC:
            return "not an ESP32 application image - use SBPlayer.ino.bin, not the merged .bin or littlefs.bin"
        chip = struct.unpack_from("<H", head, 12)[0]
        if chip != ESP32P4_CHIP_ID:
            return f"built for a different chip (chip id {chip}), not the ESP32-P4"
        return None
    if not head or head[0] == ESP_IMAGE_MAGIC or b"littlefs" not in head or size % 4096:
        return "not a LittleFS image - use the littlefs.bin built from firmware/SBPlayer/data"
    return None


def copy_update(path, out_dir, kind):
    """Copies a firmware or LittleFS image to the card root; the player installs it at its next boot."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        why = check_update_image(f.read(8192), size, kind)
    if why:
        raise ValueError(f"{os.path.basename(path)}: {why}")
    name = UPDATE_NAMES[kind]
    os.makedirs(out_dir, exist_ok=True)
    tmp = os.path.join(out_dir, name + ".part")
    shutil.copyfile(path, tmp)
    os.replace(tmp, os.path.join(out_dir, name))
    return f"Copied {os.path.basename(path)} to {name} ({size} bytes); the player installs it at its next boot"


# ── GUI ───────────────────────────────────────────────────────────────────────

def run_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    class App(tk.Tk):
        def __init__(self):
            super().__init__()
            self.title(APP)
            try:                                   # icon drawn by tools/icon/make_icon.py
                self._icon = tk.PhotoImage(file=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                             "sb_card_builder.png"))
                self.iconphoto(True, self._icon)
            except tk.TclError:
                pass
            self.geometry("940x680")
            self.controllers, self.use = [], []
            self.playlists, self.schedule = {}, []
            self.events = queue.Queue()
            self.cancel = threading.Event()
            self.show_dir, self.out_dir = tk.StringVar(), tk.StringVar()

            top = ttk.Frame(self, padding=8)
            top.pack(fill="x")
            for row, (label, var, cmd) in enumerate((("xLights show folder", self.show_dir, self.pick_show),
                                                     ("SD card folder", self.out_dir, self.pick_out))):
                ttk.Label(top, text=label).grid(row=row, column=0, sticky="w", pady=2)
                ttk.Entry(top, textvariable=var, state="readonly").grid(row=row, column=1, sticky="ew", padx=6)
                ttk.Button(top, text="Browse...", command=cmd).grid(row=row, column=2)
            top.columnconfigure(1, weight=1)

            panes = ttk.Panedwindow(self, orient="vertical")
            panes.pack(fill="both", expand=True, padx=8, pady=(0, 8))
            nb = ttk.Notebook(panes)
            panes.add(nb, weight=4)
            for title, build in (("Outputs", self.tab_outputs), ("Sequences", self.tab_sequences),
                                 ("Playlists", self.tab_playlists), ("Schedule", self.tab_schedule),
                                 ("Player update", self.tab_update)):
                frame = ttk.Frame(nb, padding=8)
                nb.add(frame, text=title)
                build(frame)
            logf = ttk.Frame(panes)
            panes.add(logf, weight=1)
            self.logbox = tk.Text(logf, height=7, state="disabled", wrap="word")
            sb = ttk.Scrollbar(logf, command=self.logbox.yview)
            self.logbox.configure(yscrollcommand=sb.set)
            sb.pack(side="right", fill="y")
            self.logbox.pack(fill="both", expand=True)
            self.after(100, self.pump)

        # -- helpers
        def log(self, msg):
            self.logbox.configure(state="normal")
            self.logbox.insert("end", msg + "\n")
            self.logbox.see("end")
            self.logbox.configure(state="disabled")

        def need_out(self):
            if not self.out_dir.get():
                messagebox.showinfo(APP, "Choose the SD card folder first.")
                return None
            return self.out_dir.get()

        def pick_show(self):
            d = filedialog.askdirectory(title="xLights show folder")
            if not d:
                return
            self.show_dir.set(d)
            net = os.path.join(d, "xlights_networks.xml")
            try:
                self.controllers = parse_networks(net)
            except (OSError, ET.ParseError) as e:
                self.controllers = []
                self.log(f"Could not read {net}: {e}")
            self.use = [c.usable and c.state == "Active" for c in self.controllers]
            self.target["values"] = [PLAYER] + [PIXEL16 + c.name for c in self.controllers if c.channels]
            self.target.set(PLAYER)
            self.refresh_outputs()
            self.rescan()

        def pick_out(self):
            d = filedialog.askdirectory(title="SD card (or a folder to copy to the card)")
            if not d:
                return
            self.out_dir.set(d)
            self.playlists = load_playlists(d)
            doc = read_json(os.path.join(d, "schedule.json"), {})
            self.schedule = doc.get("entries", [])
            self.sch_boot.set(doc.get("boot_playlist") or NO_BOOT)
            self.log(f"Card folder: {len(list_card_sequences(d))} sequences, {len(self.playlists)} playlists, "
                     f"{len(self.schedule)} schedule entries")
            self.refresh_card()

        def refresh_card(self):
            self.avail.delete(0, "end")
            for n in list_card_sequences(self.out_dir.get()) if self.out_dir.get() else []:
                self.avail.insert("end", n)
            self.pl_list.delete(0, "end")
            for n in sorted(self.playlists):
                self.pl_list.insert("end", n)
            self.sch_playlist["values"] = sorted(self.playlists)
            self.sch_boot["values"] = [NO_BOOT] + sorted(self.playlists)
            self.sch_tree.delete(*self.sch_tree.get_children())
            for i, e in enumerate(self.schedule):
                self.sch_tree.insert("", "end", iid=str(i), values=(
                    days_text(e.get("days", 127)), f"{e.get('start_hour', 0):02d}:{e.get('start_min', 0):02d}",
                    f"{e.get('end_hour', 0):02d}:{e.get('end_min', 0):02d}", e.get("playlist", "")))

        # -- outputs
        def tab_outputs(self, f):
            cols = ("use", "name", "proto", "ip", "state", "start", "channels", "universes")
            heads = ("Use", "Controller", "Protocol", "Address", "xLights state", "Start ch", "Channels", "Universes")
            self.out_tree = ttk.Treeview(f, columns=cols, show="headings", height=9)
            for c, h, w in zip(cols, heads, (40, 170, 70, 120, 100, 80, 80, 150)):
                self.out_tree.heading(c, text=h)
                self.out_tree.column(c, width=w, anchor="w")
            self.out_tree.pack(fill="both", expand=True)
            self.out_tree.bind("<ButtonRelease-1>", self.toggle_output)
            ttk.Label(f, text="Click a row to include or exclude it. Only 'Active' controllers are selected "
                              "by default.").pack(anchor="w", pady=(4, 0))
            self.warnbox = tk.Text(f, height=5, state="disabled", wrap="word", foreground="#a04000")
            self.warnbox.pack(fill="x", pady=6)
            ttk.Button(f, text="Write outputs.json to card", command=self.write_outputs).pack(anchor="e")

        def selected_outputs(self):
            return build_outputs([c for c, u in zip(self.controllers, self.use) if u])

        def refresh_outputs(self):
            self.out_tree.delete(*self.out_tree.get_children())
            for i, c in enumerate(self.controllers):
                unis = [u for u, _ in c.universes]
                uni = "" if c.protocol != "E131" or not unis else (
                    f"{unis[0]}-{unis[-1]}" if len(unis) > 1 else str(unis[0]))
                self.out_tree.insert("", "end", iid=str(i), values=(
                    "[x]" if self.use[i] else "[ ]", c.name, c.protocol, c.ip, c.state,
                    c.start + 1, c.channels, uni))
            doc, warn, needed = self.selected_outputs()
            text = "\n".join(warn) or (f"{len(doc['e131'])} E1.31 and {len(doc['ddp'])} DDP outputs, "
                                       f"{needed} channels." if self.controllers else "")
            self.warnbox.configure(state="normal")
            self.warnbox.delete("1.0", "end")
            self.warnbox.insert("end", text)
            self.warnbox.configure(state="disabled")

        def toggle_output(self, event):
            row = self.out_tree.identify_row(event.y)
            if row:
                self.use[int(row)] = not self.use[int(row)]
                self.refresh_outputs()
                self.out_tree.selection_set(row)

        def write_outputs(self):
            out = self.need_out()
            if not out:
                return
            doc, warn, _ = self.selected_outputs()
            if not doc["e131"] and not doc["ddp"]:
                messagebox.showinfo(APP, "No usable controllers are selected.")
                return
            write_json(os.path.join(out, "outputs.json"), doc)
            self.log(f"Wrote outputs.json: {len(doc['e131'])} E1.31, {len(doc['ddp'])} DDP outputs")
            for w in warn:
                self.log("   WARNING " + w)

        # -- sequences
        def tab_sequences(self, f):
            self.seq_list = tk.Listbox(f, selectmode="extended", exportselection=False)
            self.seq_list.pack(fill="both", expand=True)
            bar = ttk.Frame(f)
            bar.pack(fill="x", pady=6)
            ttk.Button(bar, text="Add files...", command=self.add_seqs).pack(side="left")
            ttk.Button(bar, text="Rescan show folder", command=self.rescan).pack(side="left", padx=6)
            ttk.Button(bar, text="Remove from list", command=lambda: self.remove_selected(self.seq_list)).pack(side="left")
            row = ttk.Frame(f)
            row.pack(fill="x", pady=(0, 4))
            ttk.Label(row, text="Export for").pack(side="left")
            self.target = ttk.Combobox(row, state="readonly", width=40, values=[PLAYER])
            self.target.set(PLAYER)
            self.target.pack(side="left", padx=6)
            ttk.Label(row, text="an SB Pixel 16 gets only its own channels, and no audio").pack(side="left")
            self.trim, self.audio = tk.BooleanVar(value=True), tk.BooleanVar(value=True)
            ttk.Checkbutton(f, text="Drop channels above the last selected output (smaller files, faster reads)",
                            variable=self.trim).pack(anchor="w")
            ttk.Checkbutton(f, text="Copy matching audio (.mp3 / .wav)", variable=self.audio).pack(anchor="w")
            run = ttk.Frame(f)
            run.pack(fill="x", pady=(6, 0))
            self.progress = ttk.Progressbar(run, maximum=1000)
            self.progress.pack(side="left", fill="x", expand=True, padx=(0, 6))
            self.cancel_btn = ttk.Button(run, text="Cancel", command=self.cancel.set, state="disabled")
            self.cancel_btn.pack(side="right")
            self.convert_btn = ttk.Button(run, text="Convert selected to card", command=self.convert)
            self.convert_btn.pack(side="right", padx=6)

        def rescan(self):
            d = self.show_dir.get()
            if not d:
                return
            self.seq_list.delete(0, "end")
            for n in sorted(os.listdir(d)):
                if n.lower().endswith(".fseq"):
                    self.seq_list.insert("end", os.path.join(d, n))
            self.seq_list.select_set(0, "end")

        def add_seqs(self):
            have = set(self.seq_list.get(0, "end"))
            for p in filedialog.askopenfilenames(title="FSEQ files", initialdir=self.show_dir.get() or None,
                                                 filetypes=[("FSEQ sequences", "*.fseq"), ("All files", "*.*")]):
                if p not in have:
                    self.seq_list.insert("end", p)
                    self.seq_list.select_set("end")

        @staticmethod
        def remove_selected(listbox):
            for i in reversed(listbox.curselection()):
                listbox.delete(i)

        def convert(self):
            out = self.need_out()
            files = [self.seq_list.get(i) for i in self.seq_list.curselection()]
            if not out or not files:
                if out:
                    messagebox.showinfo(APP, "Select one or more sequences.")
                return
            layout, audio, pixel16 = {}, self.audio.get(), None
            if self.target.get().startswith(PIXEL16):
                pixel16 = next(c for c in self.controllers if c.channels and PIXEL16 + c.name == self.target.get())
                layout, audio = pixel16.pixel16_layout(), False
            else:
                layout["max_channels"] = self.selected_outputs()[2] if self.trim.get() else 0
                if self.trim.get() and not layout["max_channels"]:
                    self.log("No outputs selected, so channels are not trimmed.")
            self.cancel.clear()
            self.convert_btn.configure(state="disabled")
            self.cancel_btn.configure(state="normal")
            args = (files, out, layout, audio, [self.show_dir.get()] if self.show_dir.get() else [], pixel16)
            threading.Thread(target=self.worker, args=args, daemon=True).start()

        def worker(self, files, out, layout, audio, dirs, pixel16):
            put = self.events.put
            ok = 0
            for n, src in enumerate(files):
                try:
                    export_sequence(src, out, copy_audio=audio, search_dirs=dirs, log=lambda m: put(("log", m)),
                                    progress=lambda d, t, n=n: put(("bar", (n + d / t) / len(files))),
                                    cancelled=self.cancel.is_set, **layout)
                    ok += 1
                except InterruptedError:
                    put(("log", "Cancelled."))
                    break
                except Exception as e:      # report and carry on with the next file
                    put(("log", f"{os.path.basename(src)}: FAILED - {e}"))
                put(("bar", (n + 1) / len(files)))
            for note in pixel16_notes(pixel16, out) if pixel16 and ok else []:
                put(("log", note))
            put(("done", f"Converted {ok} of {len(files)} sequences."))

        def pump(self):
            try:
                while True:
                    kind, val = self.events.get_nowait()
                    if kind == "log":
                        self.log(val)
                    elif kind == "bar":
                        self.progress["value"] = val * 1000
                    else:
                        self.log(val)
                        self.progress["value"] = 0
                        self.convert_btn.configure(state="normal")
                        self.cancel_btn.configure(state="disabled")
                        self.refresh_card()
            except queue.Empty:
                pass
            self.after(100, self.pump)

        # -- playlists
        def tab_playlists(self, f):
            left = ttk.Frame(f)
            left.pack(side="left", fill="y")
            ttk.Label(left, text="Playlists on card").pack(anchor="w")
            self.pl_list = tk.Listbox(left, width=24, exportselection=False)
            self.pl_list.pack(fill="y", expand=True)
            self.pl_list.bind("<<ListboxSelect>>", self.load_playlist)
            ttk.Button(left, text="Delete", command=self.delete_playlist).pack(fill="x", pady=(6, 0))

            mid = ttk.Frame(f)
            mid.pack(side="left", fill="both", expand=True, padx=8)
            ttk.Label(mid, text="Sequences on card").pack(anchor="w")
            self.avail = tk.Listbox(mid, selectmode="extended", exportselection=False)
            self.avail.pack(fill="both", expand=True)
            ttk.Button(mid, text="Add to playlist >>", command=self.pl_add).pack(fill="x", pady=(6, 0))

            right = ttk.Frame(f)
            right.pack(side="left", fill="both", expand=True)
            row = ttk.Frame(right)
            row.pack(fill="x")
            ttk.Label(row, text="Name").pack(side="left")
            self.pl_name = tk.StringVar(value="default")
            ttk.Entry(row, textvariable=self.pl_name).pack(side="left", fill="x", expand=True, padx=6)
            self.pl_items = tk.Listbox(right, selectmode="extended", exportselection=False)
            self.pl_items.pack(fill="both", expand=True, pady=6)
            bar = ttk.Frame(right)
            bar.pack(fill="x")
            ttk.Button(bar, text="Up", width=6, command=lambda: self.pl_move(-1)).pack(side="left")
            ttk.Button(bar, text="Down", width=6, command=lambda: self.pl_move(1)).pack(side="left", padx=4)
            ttk.Button(bar, text="Remove", command=lambda: self.remove_selected(self.pl_items)).pack(side="left")
            self.pl_repeat, self.pl_shuffle = tk.BooleanVar(value=True), tk.BooleanVar()
            opts = ttk.Frame(right)
            opts.pack(fill="x", pady=6)
            ttk.Checkbutton(opts, text="Repeat", variable=self.pl_repeat).pack(side="left")
            ttk.Checkbutton(opts, text="Shuffle", variable=self.pl_shuffle).pack(side="left", padx=8)
            ttk.Button(opts, text="Save playlist", command=self.save_playlist).pack(side="right")

        def load_playlist(self, _=None):
            sel = self.pl_list.curselection()
            if not sel:
                return
            name = self.pl_list.get(sel[0])
            doc = self.playlists.get(name, {})
            self.pl_name.set(name[:-5])
            self.pl_repeat.set(bool(doc.get("repeat")))
            self.pl_shuffle.set(bool(doc.get("shuffle")))
            self.pl_items.delete(0, "end")
            for e in doc.get("entries", []):
                self.pl_items.insert("end", e)

        def pl_add(self):
            for i in self.avail.curselection():
                self.pl_items.insert("end", self.avail.get(i))

        def pl_move(self, step):
            sel = list(self.pl_items.curselection())
            for i in (sel if step < 0 else reversed(sel)):
                j = i + step
                if 0 <= j < self.pl_items.size() and j not in self.pl_items.curselection():
                    text = self.pl_items.get(i)
                    self.pl_items.delete(i)
                    self.pl_items.insert(j, text)
                    self.pl_items.select_set(j)

        def save_playlist(self):
            out = self.need_out()
            if not out:
                return
            name = re.sub(r'[\\/:*?"<>|]', "_", self.pl_name.get().strip())
            entries = list(self.pl_items.get(0, "end"))
            if not name or not entries or len(name) > 50:
                messagebox.showinfo(APP, "A playlist needs a name (up to 50 characters) and at least one sequence.")
                return
            if len(entries) > MAX_PLAYLIST:
                messagebox.showinfo(APP, f"The player reads at most {MAX_PLAYLIST} entries per playlist.")
                return
            doc = {"repeat": self.pl_repeat.get(), "shuffle": self.pl_shuffle.get(), "entries": entries}
            write_json(os.path.join(out, "playlists", name + ".json"), doc)
            self.playlists[name + ".json"] = doc
            self.log(f"Wrote playlists/{name}.json ({len(entries)} sequences)")
            self.refresh_card()

        def delete_playlist(self):
            sel = self.pl_list.curselection()
            if not sel:
                return
            name = self.pl_list.get(sel[0])
            if messagebox.askyesno(APP, f"Delete playlist {name} from the card folder?"):
                os.remove(os.path.join(self.out_dir.get(), "playlists", name))
                del self.playlists[name]
                self.log(f"Deleted playlists/{name}")
                self.refresh_card()

        # -- schedule
        def tab_schedule(self, f):
            cols = ("days", "start", "end", "playlist")
            self.sch_tree = ttk.Treeview(f, columns=cols, show="headings", height=8)
            for c, h, w in zip(cols, ("Days", "Start", "End", "Playlist"), (260, 70, 70, 220)):
                self.sch_tree.heading(c, text=h)
                self.sch_tree.column(c, width=w, anchor="w")
            self.sch_tree.pack(fill="both", expand=True)
            form = ttk.Frame(f)
            form.pack(fill="x", pady=8)
            self.sch_days = [tk.BooleanVar(value=True) for _ in DAYS]
            for d, v in zip(DAYS, self.sch_days):
                ttk.Checkbutton(form, text=d, variable=v).pack(side="left")
            self.sch_start, self.sch_end = tk.StringVar(value="18:00"), tk.StringVar(value="23:00")
            for label, var in (("  Start", self.sch_start), ("End", self.sch_end)):
                ttk.Label(form, text=label).pack(side="left", padx=(8, 2))
                ttk.Entry(form, textvariable=var, width=6).pack(side="left")
            ttk.Label(form, text="Playlist").pack(side="left", padx=(8, 2))
            self.sch_playlist = ttk.Combobox(form, state="readonly", width=22)
            self.sch_playlist.pack(side="left")
            boot = ttk.Frame(f)
            boot.pack(fill="x", pady=(0, 8))
            ttk.Label(boot, text="Play at boot").pack(side="left")
            self.sch_boot = ttk.Combobox(boot, state="readonly", width=22, values=[NO_BOOT])
            self.sch_boot.set(NO_BOOT)
            self.sch_boot.pack(side="left", padx=6)
            ttk.Label(boot, text="starts at power-on with no clock needed, and plays whenever no entry "
                                 "above is active").pack(side="left")
            bar = ttk.Frame(f)
            bar.pack(fill="x")
            ttk.Button(bar, text="Add entry", command=self.sch_add).pack(side="left")
            ttk.Button(bar, text="Remove selected", command=self.sch_remove).pack(side="left", padx=6)
            ttk.Button(bar, text="Write schedule.json to card", command=self.sch_save).pack(side="right")
            ttk.Label(f, text="Times are 24-hour HH:MM. A window that runs past midnight is split into two "
                              "entries, because the player cannot cross midnight in one.").pack(anchor="w", pady=(6, 0))

        @staticmethod
        def minutes(text):
            m = re.fullmatch(r"\s*(\d{1,2}):(\d{2})\s*", text)
            if not m or int(m[1]) > 23 or int(m[2]) > 59:
                raise ValueError(text)
            return int(m[1]) * 60 + int(m[2])

        def sch_add(self):
            days = sum(1 << i for i, v in enumerate(self.sch_days) if v.get())
            try:
                start, end = self.minutes(self.sch_start.get()), self.minutes(self.sch_end.get())
            except ValueError:
                messagebox.showinfo(APP, "Enter times as HH:MM, for example 17:30.")
                return
            if not days or not self.sch_playlist.get():
                messagebox.showinfo(APP, "Pick at least one day and a playlist (save a playlist first).")
                return
            new = schedule_entries(days, start, end, self.sch_playlist.get())
            if len(self.schedule) + len(new) > MAX_SCHEDULE:
                messagebox.showinfo(APP, f"The player reads at most {MAX_SCHEDULE} schedule entries.")
                return
            self.schedule += new
            self.refresh_card()

        def sch_remove(self):
            for iid in sorted(self.sch_tree.selection(), key=int, reverse=True):
                del self.schedule[int(iid)]
            self.refresh_card()

        def sch_save(self):
            out = self.need_out()
            if out:
                boot = self.sch_boot.get()
                boot = "" if boot == NO_BOOT else boot
                if boot and not self.playlists.get(boot, {}).get("repeat"):
                    self.log(f"   NOTE {boot} does not have Repeat ticked, so it will play once and stop")
                write_json(os.path.join(out, "schedule.json"), {"boot_playlist": boot, "entries": self.schedule})
                self.log(f"Wrote schedule.json ({len(self.schedule)} entries, play at boot: {boot or 'none'})")

        # -- player update
        def tab_update(self, f):
            self.upd_paths = {"firmware": tk.StringVar(), "littlefs": tk.StringVar()}
            grid = ttk.Frame(f)
            grid.pack(fill="x")
            for row, (kind, label) in enumerate((("firmware", "Firmware (SBPlayer.ino.bin)"),
                                                 ("littlefs", "Web pages (littlefs.bin)"))):
                ttk.Label(grid, text=label).grid(row=row, column=0, sticky="w", pady=2)
                ttk.Entry(grid, textvariable=self.upd_paths[kind], state="readonly").grid(
                    row=row, column=1, sticky="ew", padx=6)
                ttk.Button(grid, text="Browse...", command=lambda k=kind: self.pick_update(k)).grid(row=row, column=2)
                ttk.Button(grid, text="Clear", command=lambda k=kind: self.upd_paths[k].set("")).grid(
                    row=row, column=3, padx=(6, 0))
            grid.columnconfigure(1, weight=1)
            ttk.Label(f, wraplength=860, justify="left", text=(
                "Copies the files to the root of the card as firmware.bin and littlefs.bin. The player installs "
                "them at its next boot, renames them to *.installed.bin (or *.failed.bin), writes the result to "
                "update.log on the card and restarts. Device settings are kept. Both files come from the "
                "Firmware build on GitHub Actions; use SBPlayer.ino.bin, not the merged .bin.")).pack(
                anchor="w", pady=10)
            ttk.Button(f, text="Copy update to card", command=self.copy_updates).pack(anchor="e")

        def pick_update(self, kind):
            p = filedialog.askopenfilename(title="Player " + kind + " image",
                                           filetypes=[("Firmware images", "*.bin"), ("All files", "*.*")])
            if p:
                self.upd_paths[kind].set(p)

        def copy_updates(self):
            out = self.need_out()
            picked = [(k, v.get()) for k, v in self.upd_paths.items() if v.get()]
            if not out or not picked:
                if out:
                    messagebox.showinfo(APP, "Choose a firmware and/or web pages image first.")
                return
            for kind, path in picked:
                try:
                    self.log(copy_update(path, out, kind))
                except (OSError, ValueError) as e:
                    self.log(f"FAILED - {e}")

    App().mainloop()


# ── Command line ──────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--show", help="xLights show folder (reads xlights_networks.xml and *.fseq)")
    ap.add_argument("--out", help="SD card folder to write")
    ap.add_argument("--fseq", nargs="*", help="sequences to convert (default: every .fseq in --show)")
    ap.add_argument("--all-controllers", action="store_true", help="include Inactive / xLights Only controllers")
    ap.add_argument("--no-trim", action="store_true", help="keep channels above the last output")
    ap.add_argument("--no-audio", action="store_true", help="do not copy audio files")
    ap.add_argument("--pixel16", metavar="CONTROLLER",
                    help="export for an SB Pixel 16: keep only this xLights controller's channels "
                         "(no outputs.json, no audio)")
    ap.add_argument("--firmware", metavar="BIN",
                    help="copy a player firmware image (SBPlayer.ino.bin) to the card; installed at next boot")
    ap.add_argument("--littlefs", metavar="BIN",
                    help="copy a player web pages image (littlefs.bin) to the card; installed at next boot")
    ap.add_argument("--info", metavar="FSEQ", help="print an FSEQ file's header and exit")
    a = ap.parse_args()
    if a.info:
        with open(a.info, "rb") as f:
            print(read_fseq_info(f).describe())
        return 0
    if not a.out:
        if a.show or a.fseq or a.firmware or a.littlefs:
            ap.error("--out is required")
        run_gui()
        return 0
    update_failed = 0
    for kind, path in (("littlefs", a.littlefs), ("firmware", a.firmware)):
        if path:
            try:
                print(copy_update(path, a.out, kind))
            except (OSError, ValueError) as e:
                update_failed += 1
                print(f"FAILED - {e}")
    layout, pixel16 = {}, None
    if a.pixel16:
        if not a.show:
            ap.error("--pixel16 needs --show")
        found = [c for c in parse_networks(os.path.join(a.show, "xlights_networks.xml"))
                 if c.name.lower() == a.pixel16.lower() and c.channels]
        if not found:
            print(f"ERROR no controller named '{a.pixel16}' in xlights_networks.xml")
            return 1
        pixel16, layout = found[0], found[0].pixel16_layout()
    elif a.show and os.path.isfile(os.path.join(a.show, "xlights_networks.xml")):
        ctrl = [c for c in parse_networks(os.path.join(a.show, "xlights_networks.xml"))
                if a.all_controllers or c.state == "Active"]
        doc, warn, needed = build_outputs(ctrl)
        for w in warn:
            print("WARNING", w)
        write_json(os.path.join(a.out, "outputs.json"), doc)
        print(f"outputs.json: {len(doc['e131'])} E1.31, {len(doc['ddp'])} DDP outputs, {needed} channels")
        layout["max_channels"] = 0 if a.no_trim else needed
    files = a.fseq if a.fseq is not None else (
        sorted(os.path.join(a.show, n) for n in os.listdir(a.show) if n.lower().endswith(".fseq")) if a.show else [])
    failed = 0
    for src in files:
        try:
            export_sequence(src, a.out, copy_audio=not a.no_audio and not pixel16,
                            search_dirs=[a.show] if a.show else [], **layout)
        except Exception as e:
            failed += 1
            print(f"{os.path.basename(src)}: FAILED - {e}")
    for note in pixel16_notes(pixel16, a.out) if pixel16 and len(files) > failed else []:
        print(note)
    return 1 if failed or update_failed else 0


if __name__ == "__main__":
    sys.exit(main())
