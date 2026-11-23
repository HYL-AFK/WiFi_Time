"""Decode WiTime NVS runtime event log dumps (full NVS partition images).

Layout mirrors main/common/runtime_event_log.c:
  header: magic(4) version(2) record_size(2) capacity(2) write_index(2)
          read_index(2) count(2) reserved(8) next_sequence(4) header_crc32(4)
  record: magic(2) version(1) event_type(1) timestamp(8) value1(4) value2(4)
          flags(4) sequence(4) record_crc32(4)

Both store layouts are supported: legacy 16-record (544 bytes) and the current
32-record (1056 bytes) store. Every copy of the NVS blob found in the dump is
CRC-validated, then all records are merged into one timeline ordered by the
monotonic `sequence` field, so the whole history preserved in NVS is visible.

Usage: python parse_nvs_runtime_log.py <nvs_dump.bin> [...]
"""

import struct
import sys
import zlib
from datetime import datetime, timezone

STORE_MAGIC = b"GLTR"
RECORD_MAGIC = 0x524C
HEADER_SIZE = 32
RECORD_SIZE = 32
TIME_VALID_FLAG = 1 << 0

EVENT_NAMES = {
    1: "BOOT",
    2: "WIFI_CONNECTED",
    3: "WIFI_FAILURE",
    4: "SNTP_SYNC",
    5: "WEATHER_SUCCESS",
    6: "WEATHER_FAILURE",
    7: "RADAR_STATE",
    8: "BRIGHTNESS_CHANGE",
    9: "DISPLAY_ERROR",
    10: "DISPLAY_ANIMATION",
}

RESET_REASONS = {
    0: "UNKNOWN", 1: "POWERON", 2: "EXT", 3: "SW", 4: "PANIC",
    5: "INT_WDT", 6: "TASK_WDT", 7: "WDT", 8: "DEEPSLEEP",
    9: "BROWNOUT", 10: "SDIO", 11: "USB", 12: "JTAG", 13: "EFUSE",
    14: "PWR_GLITCH", 15: "CPU_LOCKUP",
}

DISPLAY_FLAGS = [
    (1 << 5, "ANIMATION_HANDOFF_FAILED"),
    (1 << 8, "REFRESH_TIMEOUT"),
    (1 << 9, "REFRESH_RETRY"),
    (1 << 10, "RMT_RECOVERY_SUCCESS"),
    (1 << 11, "RMT_RECOVERY_FAILURE"),
    (1 << 12, "HEALTH_STALLED"),
    (1 << 13, "SUMMARY"),
    (1 << 14, "FRAME_STALLED"),
]

# 2026-10-15 修复后的编码：动画阶段使用互斥位（bit2/3/4），不再与 TIME_VALID(bit0) 冲突。
ANIMATION_PHASE_BITS = [(1 << 2, "BEGIN"), (1 << 3, "COMPLETE"), (1 << 4, "ABORT")]
# 旧编码（修复前固件）：低 4 位 1/2/3；0x3 与 COMPLETE|time_valid 数值相同，无法区分 ABORT。
ANIMATION_PHASE_LEGACY = {
    1: "BEGIN(legacy)",
    2: "COMPLETE(legacy)",
    3: "COMPLETE|time_valid(legacy; ambiguous with ABORT)",
}

WEATHER_REASON = {
    1: "body_overflow", 2: "http_status", 3: "json_parse",
    4: "missing_current", 5: "fetch_failed",
}


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def decode_flags(event_type, flags):
    parts = []
    if event_type == 10:
        for bit, name in ANIMATION_PHASE_BITS:
            if flags & bit:
                parts.append("phase=" + name)
        if not any(flags & bit for bit, _ in ANIMATION_PHASE_BITS):
            legacy = ANIMATION_PHASE_LEGACY.get(flags & 0x0F)
            if legacy:
                parts.append("phase=" + legacy)
    for bit, name in DISPLAY_FLAGS:
        if flags & bit:
            parts.append(name)
    if flags & TIME_VALID_FLAG:
        parts.append("time_valid")
    return ",".join(parts) if parts else "-"


def decode_values(event_type, value1, value2, flags):
    if event_type == 1:
        return ("reset_reason=%s heap_free=%d" %
                (RESET_REASONS.get(value1, "?"), value2))
    if event_type == 2:
        return "rssi=%d channel=%d" % (value1, value2)
    if event_type == 3:
        return "failure_count=%d reason=%d" % (value1, value2)
    if event_type == 4:
        return "elapsed_ms=%d epoch=%d" % (value1, value2)
    if event_type == 5:
        return "weather_code=%d temp_c_x10=%d" % (value1, value2)
    if event_type == 6:
        return "status=%d reason=%s" % (value1, WEATHER_REASON.get(value2, value2))
    if event_type == 7:
        return "detected=%d distance_mm=%d" % (value1, value2)
    if event_type == 8:
        return "brightness=%d" % value1
    if event_type == 9:
        if flags & (1 << 5):
            return ("attempted_frames=%d max_refresh_ms=%d  <animation handoff failed>" %
                    (value1, value2))
        if flags & (1 << 13):
            return ("frame=%d last_refresh_ms=%d refresh_errors=%d recovery=%d" %
                    (value1, value2, (flags >> 16) & 0xFF, (flags >> 24) & 0xFF))
        if flags & ((1 << 12) | (1 << 14)):
            return ("frame=%d last_refresh_ms=%d anim_id=%d" %
                    (value1, value2, (flags >> 16) & 0xFF))
        return "frame=%d err=0x%X flags=0x%X" % (value1, value2, flags)
    if event_type == 10:
        return ("attempted=%d elapsed_ms=%d anim_ok=%d max_refresh_ms=%d" %
                (value1, value2, (flags >> 16) & 0xFF, (flags >> 24) & 0xFF))
    return "value1=%d value2=%d" % (value1, value2)


def parse_copies(data):
    copies = []
    search = 0
    while True:
        offset = data.find(STORE_MAGIC, search)
        if offset < 0:
            break
        search = offset + 1
        for capacity in (32, 16):
            store_size = HEADER_SIZE + RECORD_SIZE * capacity
            blob = data[offset:offset + store_size]
            if len(blob) < store_size:
                continue
            (magic, version, record_size, cap, write_index, read_index,
             count) = struct.unpack_from("<IHHHHHH", blob, 0)
            next_sequence, header_crc = struct.unpack_from("<II", blob, 24)
            if (magic != 0x52544C47 or version != 1 or record_size != RECORD_SIZE
                    or cap != capacity or count > capacity
                    or write_index >= capacity or read_index >= capacity):
                continue
            if header_crc != crc32(blob[:28]):
                continue

            records = []
            valid = True
            for i in range(count):
                index = (read_index + i) % capacity
                raw = blob[HEADER_SIZE + index * RECORD_SIZE:
                           HEADER_SIZE + (index + 1) * RECORD_SIZE]
                (rec_magic, rec_version, event_type) = struct.unpack_from("<HBB", raw, 0)
                (timestamp,) = struct.unpack_from("<Q", raw, 4)
                (value1, value2, flags, sequence, rec_crc) = struct.unpack_from(
                    "<iiIII", raw, 12)
                if (rec_magic != RECORD_MAGIC or rec_version != 1
                        or rec_crc != crc32(raw[:28])):
                    valid = False
                    break
                records.append((event_type, timestamp, value1, value2, flags,
                                sequence))
            if not valid:
                continue

            page_offset = offset - (offset % 4096)
            page_seq = struct.unpack_from("<I", data, page_offset + 4)[0]
            copies.append({
                "offset": offset,
                "page": page_offset // 4096,
                "page_seq": page_seq,
                "capacity": capacity,
                "count": count,
                "write_index": write_index,
                "read_index": read_index,
                "next_sequence": next_sequence,
                "records": records,
            })
            break
    return copies


def report(path):
    data = open(path, "rb").read()
    print("=" * 100)
    print("%s   size=%d bytes" % (path, len(data)))
    print("=" * 100)

    copies = parse_copies(data)
    if not copies:
        print("no CRC-valid runtime event store found in this dump\n")
        return

    copies.sort(key=lambda c: c["next_sequence"])
    print("CRC-valid blob copies found: %d" % len(copies))
    print("  oldest copy: page=%d page_seq=%d nvs_offset=0x%X records=%d next_seq=%d cap=%d"
          % (copies[0]["page"], copies[0]["page_seq"], copies[0]["offset"],
             copies[0]["count"], copies[0]["next_sequence"], copies[0]["capacity"]))
    newest = copies[-1]
    print("  newest copy: page=%d page_seq=%d nvs_offset=0x%X records=%d next_seq=%d cap=%d"
          % (newest["page"], newest["page_seq"], newest["offset"],
             newest["count"], newest["next_sequence"], newest["capacity"]))
    print()

    merged = {}
    for copy in copies:
        for event_type, timestamp, value1, value2, flags, sequence in copy["records"]:
            merged[sequence] = (event_type, timestamp, value1, value2, flags)
    print("--- MERGED EVENT TIMELINE (%d unique events, ordered by sequence) ---"
          % len(merged))
    for sequence in sorted(merged):
        event_type, timestamp, value1, value2, flags = merged[sequence]
        name = EVENT_NAMES.get(event_type, "TYPE_%u" % event_type)
        if flags & TIME_VALID_FLAG and timestamp:
            stamp = datetime.fromtimestamp(timestamp, timezone.utc).strftime(
                "%Y-%m-%d %H:%M:%S")
        else:
            stamp = "-"
        print("seq=%-5d %-11s %-19s %-58s flags=0x%08X [%s]"
              % (sequence, name, stamp, decode_values(event_type, value1, value2, flags),
                 flags, decode_flags(event_type, flags)))
    print()

    print("--- NEWEST COPY RAW (%d records, oldest first) ---" % newest["count"])
    for event_type, timestamp, value1, value2, flags, sequence in newest["records"]:
        name = EVENT_NAMES.get(event_type, "TYPE_%u" % event_type)
        print("seq=%-5d %-11s value1=%-12d value2=%-12d flags=0x%08X [%s]"
              % (sequence, name, value1, value2, flags,
                 decode_flags(event_type, flags)))
    print()


def main():
    paths = sys.argv[1:]
    if not paths:
        print(__doc__)
        return 1
    for path in paths:
        report(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
