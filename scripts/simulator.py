"""Standard-library-only simulated sensor fleet; no UART hardware required."""
import argparse
import json
import math
import random
import time
import urllib.error
import urllib.request

from protocol import frame


def send(base, source, data):
    request = urllib.request.Request(base + "/api/ingest/" + source, data=data,
                                     headers={"Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        # A 429 can mean partial acceptance; never blindly resend this chunk.
        if error.code == 429:
            return json.load(error)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    parser.add_argument("--devices", type=int, default=4)
    parser.add_argument("--interval", type=float, default=0.8)
    parser.add_argument("--rounds", type=int, default=0, help="0 runs until Ctrl+C")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--fault-every", type=int, default=0, help="inject CRC and truncated frames every N rounds")
    args = parser.parse_args()
    if not 1 <= args.devices <= 64 or args.interval <= 0 or args.rounds < 0 or args.fault_every < 0:
        parser.error("devices must be 1..64; interval positive; rounds/fault-every nonnegative")
    rng = random.Random(args.seed)
    print(f"Simulator -> {args.url}; devices={args.devices}; seed={args.seed}", flush=True)
    sequence = 0
    try:
        while not args.rounds or sequence < args.rounds:
            sequence += 1
            stream = bytearray()
            for device in range(1, args.devices + 1):
                temperature = 22 + device * 1.1 + 1.8 * math.sin(sequence / 10 + device) + rng.uniform(-0.15, 0.15)
                humidity = 48 + device * 3 + 4 * math.cos(sequence / 15 + device)
                stream.extend(frame(device, sequence, temperature, humidity))
            # One logical stream is serialized. Chunk boundaries intentionally ignore frame boundaries.
            offset = 0
            accepted = dropped = 0
            while offset < len(stream):
                size = rng.randint(1, 32)
                result = send(args.url, "sim-fleet", bytes(stream[offset:offset + size]))
                accepted += result["accepted"]
                dropped += result["dropped"]
                offset += size
            if args.fault_every and sequence % args.fault_every == 0:
                bad = bytearray(frame()); bad[-1] ^= 1
                send(args.url, "sim-crc", bytes(bad))
                send(args.url, "sim-timeout", frame()[:4])
            if sequence == 1 or sequence % 10 == 0:
                print(f"round={sequence} accepted={accepted} dropped={dropped}", flush=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("Simulator stopped.")
    except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
        raise SystemExit(f"Gateway unavailable: {error}. Start the gateway first.")


if __name__ == "__main__":
    main()
