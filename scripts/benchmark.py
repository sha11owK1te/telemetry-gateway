"""Local HTTP ingestion benchmark. Reports observed values, not MCU performance."""
import argparse
import concurrent.futures
import json
import statistics
import time
import urllib.request
from protocol import frame
from simulator import send


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    parser.add_argument("--producers", type=int, default=4)
    parser.add_argument("--requests", type=int, default=50, help="requests per producer")
    parser.add_argument("--batch", type=int, default=10, help="frames per request")
    args = parser.parse_args()
    if not 1 <= args.producers <= 16 or args.requests < 1 or not 1 <= args.batch <= 256:
        parser.error("producers 1..16, requests positive, batch 1..256")
    def stats():
        with urllib.request.urlopen(args.url + "/api/stats", timeout=10) as response: return json.load(response)
    before = stats()
    def produce(device):
        latencies, accepted, dropped = [], 0, 0
        for batch in range(args.requests):
            payload = b"".join(frame(device, batch * args.batch + n) for n in range(args.batch))
            start = time.perf_counter(); result = send(args.url, "bench-" + str(device), payload)
            latencies.append((time.perf_counter() - start) * 1000)
            accepted += result["accepted"]; dropped += result["dropped"]
        return latencies, accepted, dropped
    start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.producers) as pool:
        results = list(pool.map(produce, range(1, args.producers + 1)))
    ingestion_seconds = time.perf_counter() - start
    accepted, dropped = sum(r[1] for r in results), sum(r[2] for r in results)
    latencies = sorted(value for r in results for value in r[0])
    # Server is asynchronous: separately wait for accepted rows to be stored or fail.
    deadline = time.monotonic() + 30
    while True:
        after = stats()
        completed = after["stored"] - before["stored"] + after["storage_errors"] - before["storage_errors"]
        if completed >= accepted or time.monotonic() >= deadline: break
        time.sleep(0.05)
    report = {"scope": "localhost HTTP ingestion; run on an otherwise idle gateway",
        "producers": args.producers, "requests_per_producer": args.requests, "frames_per_request": args.batch,
        "accepted": accepted, "dropped": dropped, "ingestion_seconds": round(ingestion_seconds, 4),
        "accepted_frames_per_second": round(accepted / ingestion_seconds, 2),
        "request_latency_ms_p50": round(statistics.median(latencies), 3),
        "request_latency_ms_p95": round(latencies[max(0, int(len(latencies) * .95) - 1)], 3),
        "stored_delta": after["stored"] - before["stored"],
        "storage_errors_delta": after["storage_errors"] - before["storage_errors"],
        "total_drain_seconds": round(time.perf_counter() - start, 4)}
    print(json.dumps(report, indent=2))


if __name__ == "__main__": main()
