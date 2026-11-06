#!/usr/bin/env python3
"""The STM32 node's cycle rows -> a CSV: one row per uplink, the library's LoRaWAN crypto in CPU cycles and in us.

    stm32/run_node.sh log                                         # writes results/stm32_<device>_cyc.csv as rows come
    python3 stm32/cyc_csv.py results/stm32_<device>.log > c.csv   # the same from a log already kept
    python3 stm32/cyc_csv.py --summary results/stm32_<device>.log # the medians per column, on stderr
    python3 stm32/cyc_csv.py --selftest

Input: the node's serial log (stm32/lorawan_node: "cyc,uplink,..." once, then a "cyc,<n>,..." row per uplink), with or
without the timestamp arduino-cli monitor puts in front of each line; every other line is skipped. Output columns:

    time            when the row reached the computer (the monitor's timestamp, else the time it was read)
    uplink, fcnt    the node's count since its start, and the uplink's LoRaWAN frame counter
    data_rate       DR0-DR5 (AS923: DR0 = SF12, DR2 = SF10)
    status          ok, or failed (the send itself)
    uplink_ms       the send to the end of its receive windows (RX1, RX2 if RX1 was empty): mostly airtime and waiting
    reply           yes / no: an answer came in RX1 or RX2
    cpu_mhz         the core clock, for cycles -> us
    up_mic_*        the uplink's MIC computed (AES-128-CMAC): cycles, calls, us
    up_aes_*        the uplink's payload encrypted (AES-128, one call per 16-byte block): cycles, calls, us
    up_total_us     both
    down_mic_*      the reply's MIC checked; down_aes_*: the reply decrypted; down_total_us: both. Empty without a reply
"""
import argparse, csv, datetime, io, re, statistics, sys

IN = ("uplink", "fcnt", "dr", "status", "up_ms", "reply", "cpu_mhz", "up_mic_cyc", "up_mic_calls", "up_aes_cyc",
      "up_aes_calls", "down_mic_cyc", "down_mic_calls", "down_aes_cyc", "down_aes_calls")  # the sketch's CYC_HDR
OUT = ("time", "uplink", "fcnt", "data_rate", "status", "uplink_ms", "reply", "cpu_mhz",
       "up_mic_cycles", "up_mic_calls", "up_mic_us", "up_aes_cycles", "up_aes_calls", "up_aes_us", "up_total_us",
       "down_mic_cycles", "down_mic_calls", "down_mic_us", "down_aes_cycles", "down_aes_calls", "down_aes_us",
       "down_total_us")
ROW = re.compile(r"(?:^|[\s\]>])(cyc,.*)$")  # "cyc,..." at the start, or after the monitor's timestamp


def convert(line: str, names: list) -> dict | None:
    """One log line -> an output row, or None (another line, the header, a broken row). `names`: the header's columns,
    updated when the line is the header."""
    m = ROW.search(line.rstrip("\r\n"))
    if not m:
        return None
    fields = m[1].split(",")[1:]
    if fields and fields[0] == "uplink":  # the header: the column names this build prints
        names[:] = fields
        return None
    if len(fields) != len(names) or not all(f.lstrip("-").isdigit() for i, f in enumerate(fields) if names[i] != "status"):
        return None  # cut off or garbled on the serial line
    r = dict(zip(names, fields))
    stamp = line[:m.start(1)].strip(" \t[]->")
    mhz = int(r["cpu_mhz"]) or 48
    out = {"time": stamp or datetime.datetime.now().isoformat(sep=" ", timespec="milliseconds"),
           "uplink": r["uplink"], "fcnt": r["fcnt"], "data_rate": f"DR{r['dr']}", "status": r["status"],
           "uplink_ms": r["up_ms"], "reply": "yes" if r["reply"] == "1" else "no", "cpu_mhz": mhz}
    for side in ("up", "down"):
        total, seen = 0.0, False
        for op in ("mic", "aes"):
            cyc, calls = int(r[f"{side}_{op}_cyc"]), int(r[f"{side}_{op}_calls"])
            if calls:
                seen, total = True, total + cyc / mhz
                out.update({f"{side}_{op}_cycles": cyc, f"{side}_{op}_calls": calls, f"{side}_{op}_us": f"{cyc / mhz:.2f}"})
            else:  # none: empty, not 0 (a 0 would count as a measurement)
                out.update({f"{side}_{op}_cycles": "", f"{side}_{op}_calls": "", f"{side}_{op}_us": ""})
        out[f"{side}_total_us"] = f"{total:.2f}" if seen else ""
    return out


def summary(rows: list) -> str:
    """The median of each cycle / us column, and the counts"""
    lines = [f"{len(rows)} uplinks, {sum(r['status'] == 'ok' for r in rows)} sent, "
             f"{sum(r['reply'] == 'yes' for r in rows)} with a reply"]
    for k in OUT[8:]:
        v = [float(r[k]) for r in rows if r[k] != ""]
        if v and not k.endswith("_calls"):
            lines.append(f"  {k:18} median {statistics.median(v):12.2f}   min {min(v):12.2f}   max {max(v):12.2f}   n={len(v)}")
    return "\n".join(lines)


def run(src, out, want_summary=False, echo=False) -> list:
    w = csv.DictWriter(out, OUT)
    if out.seekable() and out.tell() == 0 or not out.seekable():
        w.writeheader()
        out.flush()
    names, rows = list(IN), []
    for line in src:
        if echo:  # the live log on the terminal too (run_node.sh log)
            print(line, end="" if line.endswith("\n") else "\n", flush=True)
        if (row := convert(line, names)) is not None:
            w.writerow(row)
            out.flush()  # live: a row is in the file as soon as it came
            rows.append(row)
    if want_summary:
        print(summary(rows), file=sys.stderr)
    return rows


def selftest():
    log = ("#device ABP ...\n#cpu 48 MHz, cycles from DWT->CYCCNT\n"
           "[2026-11-06 10:42:00] cyc," + ",".join(IN) + "\n"
           "[2026-11-06 10:43:01] #up 1 FCnt 0 DR2: 2093 ms\n"
           "[2026-11-06 10:43:01] cyc,1,0,2,ok,2093,1,48,41232,1,9840,2,38112,1,4920,1\n"
           "cyc,2,1,2,ok,3104,0,48,41200,1,9810,2,0,0,0,0\n"
           "[2026-11-06 10:45:01] cyc,3,2,2,ok,2093,1,48,41\n"                     # cut off: skipped
           "10:46:01.123 -> cyc,4,3,2,failed,12,0,48,0,0,0,0,0,0,0,0\n")
    out = io.StringIO()
    run(io.StringIO(log), out)
    rows = list(csv.DictReader(io.StringIO(out.getvalue())))
    assert [r["uplink"] for r in rows] == ["1", "2", "4"], rows
    a, b, c = rows
    assert (a["time"], a["data_rate"], a["reply"], a["up_mic_us"], a["up_aes_us"]) == \
           ("2026-11-06 10:43:01", "DR2", "yes", "859.00", "205.00"), a
    assert (a["up_total_us"], a["down_mic_us"], a["down_aes_us"], a["down_total_us"]) == \
           ("1064.00", "794.00", "102.50", "896.50"), a
    assert (b["reply"], b["down_mic_cycles"], b["down_total_us"], b["up_mic_cycles"]) == ("no", "", "", "41200"), b
    assert b["time"][:4].isdigit(), b  # no timestamp: the time it was read
    assert (c["time"], c["status"], c["up_total_us"]) == ("10:46:01.123", "failed", ""), c
    assert list(rows[0]) == list(OUT)
    s = summary([dict(r) for r in rows])
    assert s.startswith("3 uplinks, 2 sent, 1 with a reply") and "up_mic_us" in s and "_calls" not in s, s
    print("cyc_csv selftest OK")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", nargs="*", help="the node's serial log(s); none: stdin")
    ap.add_argument("-o", "--out", help="append to this CSV (its header written if it is new); default: stdout")
    ap.add_argument("--summary", action="store_true", help="the medians per column on stderr, at the end")
    ap.add_argument("--echo", action="store_true", help="every input line to stdout too (with -o: the live log)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        sys.exit()
    if a.echo and not a.out:
        ap.error("--echo needs -o (stdout carries the log, so the CSV goes to a file)")
    src = (ln for f in a.log for ln in open(f, encoding="utf-8", errors="replace")) if a.log else sys.stdin
    out = open(a.out, "a", newline="", encoding="utf-8") if a.out else sys.stdout
    try:
        run(src, out, a.summary, a.echo)
    except KeyboardInterrupt:  # Ctrl-C on the live log: the rows so far are written already
        pass
