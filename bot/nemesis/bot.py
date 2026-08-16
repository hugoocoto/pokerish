#!/usr/bin/env python3
"""
Nemesis -- a 9-max tournament bot built to beat Prometheus, by directly
targeting confirmed leaks in bot/prometheus/bot.cpp: position-blind push/fold
ranges, an under-calling gap in the 15-20bb band, no real ICM despite the
README claiming it, and coarse 3-tier position bucketing facing a raise.
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
