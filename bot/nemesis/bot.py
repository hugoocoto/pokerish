#!/usr/bin/env python3
"""
Nemesis -- a 9-max tournament bot with fundamentally sound, position-aware
push/fold, deep-stack, and board-texture-aware postflop play, plus an
adaptive layer that fingerprints and exploits specific behavioral leaks
(e.g. position-blind shove frequency) in whichever opponent shows them.
See README.md in this directory for the full writeup.

Run (from this directory, after creating the venv):
    python3 -m venv venv
    venv/bin/pip install -r requirements.txt
    venv/bin/python bot.py --host 127.0.0.1 --port 9000 --name Nemesis [--token SECRET] [-v]
"""
import argparse
import asyncio
import logging

from nemesis.protocol import NemesisBot


def main():
    ap = argparse.ArgumentParser(description="Nemesis poker bot")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--name", default="Nemesis")
    ap.add_argument("--token", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    bot = NemesisBot(args.host, args.port, args.name, args.token)
    asyncio.run(bot.run())


if __name__ == "__main__":
    main()
