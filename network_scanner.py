#!/usr/bin/env python3
"""
Network Device Scanner
Scans the local network for active devices and displays their IP, MAC, and hostname.
"""

import argparse
import ipaddress
import socket
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime


def get_local_network() -> str:
    """Detect the local network CIDR automatically."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
        # Assume /24 subnet
        parts = local_ip.rsplit(".", 1)
        return f"{parts[0]}.0/24"
    except OSError:
        return "192.168.1.0/24"


def ping(ip: str, timeout: int = 1) -> bool:
    """Ping a single IP address. Returns True if host is up."""
    try:
        result = subprocess.run(
            ["ping", "-c", "1", "-W", str(timeout), str(ip)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0
    except FileNotFoundError:
        # Windows fallback
        result = subprocess.run(
            ["ping", "-n", "1", "-w", str(timeout * 1000), str(ip)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0


def get_hostname(ip: str) -> str:
    """Reverse DNS lookup for an IP address."""
    try:
        return socket.gethostbyaddr(str(ip))[0]
    except (socket.herror, socket.gaierror):
        return ""


def get_mac(ip: str) -> str:
    """Read MAC address from the ARP cache after a ping."""
    try:
        result = subprocess.run(
            ["arp", "-n", str(ip)],
            capture_output=True,
            text=True,
        )
        for line in result.stdout.splitlines():
            parts = line.split()
            # arp output columns: Address / HWtype / HWaddress / Flags / Iface
            if len(parts) >= 3 and parts[0] == str(ip):
                mac = parts[2]
                return mac if mac != "(incomplete)" else ""
    except FileNotFoundError:
        pass
    return ""


def scan_host(ip: str, timeout: int) -> dict | None:
    """Ping a host; if alive, collect MAC and hostname."""
    if not ping(ip, timeout):
        return None
    hostname = get_hostname(ip)
    mac = get_mac(ip)
    return {"ip": ip, "mac": mac or "N/A", "hostname": hostname or "N/A"}


def print_table(devices: list[dict]) -> None:
    """Pretty-print the list of discovered devices."""
    if not devices:
        print("\nNo devices found.")
        return

    col_ip = max(len("IP Address"), *(len(d["ip"]) for d in devices))
    col_mac = max(len("MAC Address"), *(len(d["mac"]) for d in devices))
    col_host = max(len("Hostname"), *(len(d["hostname"]) for d in devices))

    sep = f"+{'-'*(col_ip+2)}+{'-'*(col_mac+2)}+{'-'*(col_host+2)}+"
    header = f"| {'IP Address':<{col_ip}} | {'MAC Address':<{col_mac}} | {'Hostname':<{col_host}} |"

    print(f"\n{sep}")
    print(header)
    print(sep)
    for d in sorted(devices, key=lambda x: ipaddress.ip_address(x["ip"])):
        print(f"| {d['ip']:<{col_ip}} | {d['mac']:<{col_mac}} | {d['hostname']:<{col_host}} |")
    print(sep)
    print(f"\n{len(devices)} device(s) found.")


def scan_network(network: str, workers: int, timeout: int, verbose: bool) -> list[dict]:
    """Scan all hosts in the given CIDR range concurrently."""
    try:
        net = ipaddress.ip_network(network, strict=False)
    except ValueError as e:
        print(f"Invalid network: {e}", file=sys.stderr)
        sys.exit(1)

    hosts = list(net.hosts())
    print(f"Scanning {network}  ({len(hosts)} hosts)  …")

    found: list[dict] = []
    lock = threading.Lock()
    done = 0

    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {executor.submit(scan_host, str(ip), timeout): str(ip) for ip in hosts}
        for future in as_completed(futures):
            done += 1
            if verbose:
                ip = futures[future]
                print(f"\r  {done}/{len(hosts)}  checked …", end="", flush=True)
            result = future.result()
            if result:
                with lock:
                    found.append(result)
                if verbose:
                    print(f"\r  [+] Found {result['ip']:<16} {result['mac']:<18} {result['hostname']}")

    if verbose:
        print()  # newline after progress
    return found


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Scan the local network for active devices.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Examples:\n"
               "  python network_scanner.py\n"
               "  python network_scanner.py -n 10.0.0.0/24\n"
               "  python network_scanner.py -n 192.168.0.0/24 -w 100 -t 2 -v",
    )
    parser.add_argument(
        "-n", "--network",
        help="Network CIDR to scan (default: auto-detect local /24)",
    )
    parser.add_argument(
        "-w", "--workers",
        type=int, default=50,
        help="Number of concurrent threads (default: 50)",
    )
    parser.add_argument(
        "-t", "--timeout",
        type=int, default=1,
        help="Ping timeout in seconds (default: 1)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print each discovered device as it is found",
    )
    args = parser.parse_args()

    network = args.network or get_local_network()
    print(f"Network Device Scanner  —  {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Target network : {network}")
    print(f"Threads        : {args.workers}")
    print(f"Ping timeout   : {args.timeout}s")

    devices = scan_network(network, args.workers, args.timeout, args.verbose)
    print_table(devices)


if __name__ == "__main__":
    main()
