#!/usr/bin/env python3
"""Runs the clean-room njserver from a ninjamsrv-style config file, for the old test suite."""
import os, shlex, sys
NJSERVER = os.environ["NJSERVER"]
args = []
port = "2049"
for line in open(sys.argv[1]):
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    t = shlex.split(line)
    k = t[0].lower()
    if k == "port": port = t[1]
    elif k == "anonymoususers" and t[1].lower() == "yes": args.append("--anon")
    elif k == "anonymousmaskip" and t[1].lower() == "yes": args.append("--mask-ip")
    elif k == "maxchannels": args += ["--max-channels", t[1]]
    elif k == "maxusers": args += ["--max-users", t[1]]
    elif k == "maxpendingconnections": args += ["--max-pending", t[1], t[2] if len(t) > 2 else "8"]
    elif k == "anonymoususers" and t[1] == "2": args += ["--anon", "--anon-multi"]
    elif k == "defaultbpm": args += ["--bpm", t[1]]
    elif k == "defaultbpi": args += ["--bpi", t[1]]
    elif k == "allowcodecs": args += ["--allow-codecs", ",".join(t[1:])]
    elif k == "requirecodec": args += ["--require-codec", t[1]]
    elif k == "opusbitrate": args += ["--opus-kbps", t[1]]
    elif k == "setvotingthreshold": args += ["--vote-threshold", t[1]]
    elif k == "setkeepalive": args += ["--keepalive", t[1]]
    elif k == "defaulttopic": args += ["--topic", t[1]]
    elif k == "serverlicense": args += ["--licence", t[1]]
    elif k == "user":
        privs = t[3].lower() if len(t) > 3 else "cv"
        args += ["--user", "%s:%s:%s" % (t[1], t[2], privs)]
os.execv(NJSERVER, [NJSERVER, port] + args)
