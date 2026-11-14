#!/usr/bin/env python3

import argparse
import hashlib
import http.client
import json
import os
import pathlib
import random
import socket
import statistics
import subprocess
import tempfile
import time

CLIENT_SOURCE = pathlib.Path(__file__).with_name("client.go")
CLOCK_HZ = os.sysconf("SC_CLK_TCK")


def make_html(size):
    rng = random.Random(29384)
    lines = []
    length = 0
    while length < size:
        line = (
            f'<section class="post" id="item-{len(lines)}">'
            f"<h2>Example article {rng.randrange(100000)}</h2>"
            f"<p>Sample content for compression benchmarking: "
            f"{rng.getrandbits(64):016x}.</p></section>\n"
        ).encode()
        lines.append(line)
        length += len(line)
    return b"".join(lines)[:size]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start(variant, binary, base, html_dir, cpu):
    root = base / variant
    root.mkdir()
    port = free_port()
    conf = root / "nginx.conf"
    conf.write_text(f"""
daemon off;
master_process off;
pid {root}/nginx.pid;
error_log {root}/error.log notice;
events {{ worker_connections 128; }}
http {{
    access_log off;
    sendfile off;
    default_type text/html;
    server {{
        listen 127.0.0.1:{port};
        root {html_dir};
        zstd on;
        zstd_comp_level 3;
        zstd_min_length 20;
        zstd_buffers 2 128k;
    }}
}}
""")
    proc = subprocess.Popen(
        ["taskset", "-c", str(cpu), str(binary), "-p", str(root), "-c", str(conf)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"{variant} nginx failed: {proc.stderr.read().decode()}")
        if (root / "nginx.pid").exists():
            return proc, root, port
        time.sleep(0.02)
    proc.terminate()
    proc.wait(timeout=5)
    raise TimeoutError(f"{variant} nginx startup timed out")


def encoded(port, path):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request("GET", path, headers={"Accept-Encoding": "zstd"})
        response = conn.getresponse()
        body = response.read()
        if response.status != 200 or response.getheader("Content-Encoding") != "zstd":
            raise RuntimeError(f"unexpected response {response.status}, {response.getheaders()}")
        return body
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser(description="Compare two nginx builds with the same workload.")
    parser.add_argument("--baseline", required=True, type=pathlib.Path)
    parser.add_argument("--candidate", required=True, type=pathlib.Path)
    parser.add_argument("--server-cpu", type=int, default=0)
    parser.add_argument("--client-cpu", type=int, default=1)
    parser.add_argument("--zstd", default="zstd", help="zstd command or absolute path")
    args = parser.parse_args()
    if args.server_cpu == args.client_cpu:
        parser.error("server and client must use different CPUs")
    binaries = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}

    rng = random.Random(931)
    with tempfile.TemporaryDirectory(prefix="nginx-zstd-isolated-") as tmp:
        base = pathlib.Path(tmp)
        client = base / "client"
        subprocess.run(["go", "build", "-o", str(client), str(CLIENT_SOURCE)], check=True)
        html_dir = base / "html"
        html_dir.mkdir()
        bodies = {size: make_html(size * 1024) for size in (8, 64, 256)}
        for size, data in bodies.items():
            (html_dir / f"{size}.html").write_bytes(data)
        servers = {}
        try:
            for variant, binary in binaries.items():
                servers[variant] = start(variant, binary, base, html_dir, args.server_cpu)

            for size, body in bodies.items():
                frames = {}
                for variant, (_, root, port) in servers.items():
                    # Check output again after the worker context has been reused.
                    encoded(port, f"/{size}.html")
                    frame = encoded(port, f"/{size}.html")
                    compressed = root / "verify.zst"
                    decoded = root / "verify.html"
                    compressed.write_bytes(frame)
                    subprocess.run(
                        [args.zstd, "-q", "-d", "-f", str(compressed), "-o", str(decoded)],
                        check=True,
                    )
                    if decoded.read_bytes() != body:
                        raise RuntimeError(f"{variant} response {size} KiB is corrupted")
                    frames[variant] = frame
                if frames["baseline"] != frames["candidate"]:
                    raise RuntimeError(f"compressed bytes differ for {size} KiB")
                print(json.dumps({
                    "preflight": size, "payload_sha256": hashlib.sha256(body).hexdigest(),
                    "compressed_sha256": hashlib.sha256(frames["candidate"]).hexdigest(),
                    "compressed_bytes": len(frames["candidate"]), "bytes_identical": True,
                }), flush=True)

            results = {(variant, size): [] for variant in binaries for size in bodies}
            for size in bodies:
                first = rng.choice(("baseline", "candidate"))
                for round_number in range(3):
                    order = (first, "candidate" if first == "baseline" else "baseline")
                    if round_number % 2:
                        order = order[::-1]
                    for variant in order:
                        proc, _, port = servers[variant]
                        output = subprocess.run(
                            ["taskset", "-c", str(args.client_cpu), str(client),
                             f"http://127.0.0.1:{port}/{size}.html",
                             str(proc.pid), str(CLOCK_HZ)],
                            check=True, capture_output=True, text=True,
                            env={**os.environ, "GOMAXPROCS": "1"},
                            timeout=20,
                        )
                        result = json.loads(output.stdout)
                        results[(variant, size)].append(result)
                        print(json.dumps({
                            "variant": variant, "size_kib": size,
                            "round": round_number + 1, **result,
                        }), flush=True)
            for size in bodies:
                for variant in binaries:
                    runs = results[(variant, size)]
                    print(json.dumps({
                        "summary": True, "variant": variant, "size_kib": size,
                        **{metric: statistics.median(r[metric] for r in runs)
                           for metric in ("rps", "cpu_sec", "cpu_us_per_request")},
                    }), flush=True)
        finally:
            for proc, _, _ in servers.values():
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()