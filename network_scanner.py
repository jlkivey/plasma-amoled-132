#!/usr/bin/env python3
"""
Network Device Scanner with Threat Detection
Scans the local network for active devices and flags potential security threats.
"""

import argparse
import ipaddress
import socket
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime


# ---------------------------------------------------------------------------
# Threat definitions
# ---------------------------------------------------------------------------

# Ports that are inherently risky or commonly exploited
RISKY_PORTS: dict[int, str] = {
    21:   "FTP (plaintext credentials)",
    22:   "SSH (brute-force target)",
    23:   "Telnet (plaintext, no encryption)",
    25:   "SMTP (open relay risk)",
    53:   "DNS (open resolver / amplification risk)",
    80:   "HTTP (unencrypted web service)",
    110:  "POP3 (plaintext email)",
    135:  "MS-RPC (Windows attack surface)",
    137:  "NetBIOS (information disclosure)",
    138:  "NetBIOS Datagram",
    139:  "NetBIOS Session (SMB)",
    143:  "IMAP (plaintext email)",
    161:  "SNMP (community string exposure)",
    389:  "LDAP (cleartext directory access)",
    443:  "HTTPS",
    445:  "SMB (EternalBlue / ransomware vector)",
    512:  "rexec (remote exec, plaintext)",
    513:  "rlogin (plaintext remote login)",
    514:  "rsh / syslog (plaintext remote shell)",
    554:  "RTSP (IP camera stream)",
    1080: "SOCKS proxy",
    1433: "MS SQL Server",
    1521: "Oracle DB",
    2049: "NFS (file share exposure)",
    3306: "MySQL/MariaDB",
    3389: "RDP (brute-force / BlueKeep target)",
    4444: "Common RAT/backdoor port",
    5432: "PostgreSQL",
    5555: "Android ADB (device takeover)",
    5900: "VNC (remote desktop, often unauth)",
    6379: "Redis (often unauthenticated)",
    8080: "HTTP-alt (dev server / proxy)",
    8443: "HTTPS-alt",
    8888: "Jupyter / dev servers",
    9200: "Elasticsearch (often unauthenticated)",
    27017:"MongoDB (often unauthenticated)",
}

# Ports where open access is almost always a critical misconfiguration
CRITICAL_PORTS: set[int] = {
    23, 512, 513, 514,          # Plaintext remote-access protocols
    445, 135, 137, 138, 139,    # SMB / NetBIOS
    3389,                        # RDP
    4444,                        # RAT backdoor
    5555,                        # ADB
    6379,                        # Redis
    9200,                        # Elasticsearch
    27017,                       # MongoDB
}

SEVERITY_CRITICAL = "CRITICAL"
SEVERITY_HIGH     = "HIGH"
SEVERITY_INFO     = "INFO"


# ---------------------------------------------------------------------------
# Network helpers
# ---------------------------------------------------------------------------

def get_local_network() -> str:
    """Detect the local network CIDR automatically."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
        parts = local_ip.rsplit(".", 1)
        return f"{parts[0]}.0/24"
    except OSError:
        return "192.168.1.0/24"


def ping(ip: str, timeout: int = 1) -> bool:
    """Return True if the host responds to ICMP ping."""
    try:
        result = subprocess.run(
            ["ping", "-c", "1", "-W", str(timeout), str(ip)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0
    except FileNotFoundError:
        result = subprocess.run(
            ["ping", "-n", "1", "-w", str(timeout * 1000), str(ip)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0


def get_hostname(ip: str) -> str:
    """Reverse DNS lookup."""
    try:
        return socket.gethostbyaddr(str(ip))[0]
    except (socket.herror, socket.gaierror):
        return ""


def get_mac(ip: str) -> str:
    """Read MAC from ARP cache."""
    try:
        result = subprocess.run(
            ["arp", "-n", str(ip)],
            capture_output=True,
            text=True,
        )
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[0] == str(ip):
                mac = parts[2]
                return mac if mac != "(incomplete)" else ""
    except FileNotFoundError:
        pass
    return ""


# ---------------------------------------------------------------------------
# Port scanning & threat analysis
# ---------------------------------------------------------------------------

def check_port(ip: str, port: int, timeout: float = 0.5) -> bool:
    """Return True if TCP port is open."""
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except (socket.timeout, ConnectionRefusedError, OSError):
        return False


def scan_ports(ip: str, timeout: float = 0.5) -> list[int]:
    """Check all RISKY_PORTS and return the ones that are open."""
    open_ports: list[int] = []
    with ThreadPoolExecutor(max_workers=30) as ex:
        futures = {ex.submit(check_port, ip, port, timeout): port for port in RISKY_PORTS}
        for future in as_completed(futures):
            if future.result():
                open_ports.append(futures[future])
    return sorted(open_ports)


def classify_threats(open_ports: list[int]) -> list[dict]:
    """
    Return a list of threat records for every open risky port.
    Each record: {port, service, severity, reason}
    """
    threats: list[dict] = []
    for port in open_ports:
        service = RISKY_PORTS.get(port, "Unknown")
        severity = SEVERITY_CRITICAL if port in CRITICAL_PORTS else SEVERITY_HIGH
        threats.append({
            "port":     port,
            "service":  service,
            "severity": severity,
        })
    return threats


# ---------------------------------------------------------------------------
# Host scanning
# ---------------------------------------------------------------------------

def scan_host(ip: str, ping_timeout: int, port_timeout: float, do_threat: bool) -> dict | None:
    """Ping host; if alive collect MAC, hostname, and (optionally) threat info."""
    if not ping(ip, ping_timeout):
        return None
    hostname = get_hostname(ip)
    mac = get_mac(ip)
    result: dict = {
        "ip":       ip,
        "mac":      mac or "N/A",
        "hostname": hostname or "N/A",
        "threats":  [],
        "open_ports": [],
    }
    if do_threat:
        open_ports = scan_ports(ip, port_timeout)
        result["open_ports"] = open_ports
        result["threats"]    = classify_threats(open_ports)
    return result


def scan_network(
    network: str,
    workers: int,
    ping_timeout: int,
    port_timeout: float,
    do_threat: bool,
    verbose: bool,
) -> list[dict]:
    """Scan all hosts in the CIDR range concurrently."""
    try:
        net = ipaddress.ip_network(network, strict=False)
    except ValueError as e:
        print(f"Invalid network: {e}", file=sys.stderr)
        sys.exit(1)

    hosts = list(net.hosts())
    print(f"Scanning {network}  ({len(hosts)} hosts)  …")

    found: list[dict] = []
    lock  = threading.Lock()
    done  = 0

    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {
            executor.submit(scan_host, str(ip), ping_timeout, port_timeout, do_threat): str(ip)
            for ip in hosts
        }
        for future in as_completed(futures):
            done += 1
            ip = futures[future]
            if verbose:
                print(f"\r  {done}/{len(hosts)} checked …", end="", flush=True)
            result = future.result()
            if result:
                with lock:
                    found.append(result)
                if verbose:
                    threat_count = len(result["threats"])
                    flag = f"  *** {threat_count} threat(s)!" if threat_count else ""
                    print(f"\r  [+] {result['ip']:<16} {result['mac']:<18} {result['hostname']}{flag}")

    if verbose:
        print()
    return found


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

SEVERITY_COLOR = {
    SEVERITY_CRITICAL: "\033[91m",   # red
    SEVERITY_HIGH:     "\033[93m",   # yellow
    SEVERITY_INFO:     "\033[96m",   # cyan
}
RESET = "\033[0m"


def print_device_table(devices: list[dict]) -> None:
    """Print the basic device table."""
    if not devices:
        print("\nNo devices found.")
        return

    col_ip   = max(len("IP Address"),   *(len(d["ip"])       for d in devices))
    col_mac  = max(len("MAC Address"),  *(len(d["mac"])      for d in devices))
    col_host = max(len("Hostname"),     *(len(d["hostname"]) for d in devices))
    col_ports= len("Open Risky Ports")

    sep    = f"+{'-'*(col_ip+2)}+{'-'*(col_mac+2)}+{'-'*(col_host+2)}+{'-'*(col_ports+2)}+"
    header = (
        f"| {'IP Address':<{col_ip}} "
        f"| {'MAC Address':<{col_mac}} "
        f"| {'Hostname':<{col_host}} "
        f"| {'Open Risky Ports':<{col_ports}} |"
    )

    print(f"\n{sep}")
    print(header)
    print(sep)
    for d in sorted(devices, key=lambda x: ipaddress.ip_address(x["ip"])):
        ports_str = ", ".join(str(p) for p in d["open_ports"]) if d["open_ports"] else "none"
        print(
            f"| {d['ip']:<{col_ip}} "
            f"| {d['mac']:<{col_mac}} "
            f"| {d['hostname']:<{col_host}} "
            f"| {ports_str:<{col_ports}} |"
        )
    print(sep)
    print(f"\n{len(devices)} device(s) found.")


def print_threat_report(devices: list[dict]) -> None:
    """Print a per-device threat summary."""
    threatened = [d for d in devices if d["threats"]]
    if not threatened:
        print("\n\033[92m[OK] No threats detected on any discovered host.\033[0m")
        return

    print(f"\n{'='*60}")
    print(f"  THREAT REPORT  —  {len(threatened)} host(s) with findings")
    print(f"{'='*60}")

    for d in sorted(threatened, key=lambda x: ipaddress.ip_address(x["ip"])):
        critical = sum(1 for t in d["threats"] if t["severity"] == SEVERITY_CRITICAL)
        high     = sum(1 for t in d["threats"] if t["severity"] == SEVERITY_HIGH)
        print(f"\n  Host : {d['ip']}  ({d['hostname']})")
        print(f"  MAC  : {d['mac']}")
        print(f"  Risks: {critical} CRITICAL  {high} HIGH")
        print(f"  {'Port':<7} {'Severity':<10} Service / Risk")
        print(f"  {'-'*55}")
        for t in sorted(d["threats"], key=lambda x: (x["severity"] != SEVERITY_CRITICAL, x["port"])):
            color = SEVERITY_COLOR.get(t["severity"], "")
            print(f"  {t['port']:<7} {color}{t['severity']:<10}{RESET} {t['service']}")

    total_critical = sum(
        1 for d in threatened for t in d["threats"] if t["severity"] == SEVERITY_CRITICAL
    )
    total_high = sum(
        1 for d in threatened for t in d["threats"] if t["severity"] == SEVERITY_HIGH
    )
    print(f"\n{'='*60}")
    print(f"  Total findings: {total_critical} CRITICAL  {total_high} HIGH")
    print(f"{'='*60}\n")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Scan the local network for devices and detect security threats.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python network_scanner.py\n"
            "  python network_scanner.py -n 192.168.1.0/24 --no-threat\n"
            "  python network_scanner.py -n 10.0.0.0/24 -w 80 -v"
        ),
    )
    parser.add_argument("-n", "--network",   help="Network CIDR (default: auto-detect)")
    parser.add_argument("-w", "--workers",   type=int,   default=50,  help="Concurrent threads (default: 50)")
    parser.add_argument("-t", "--timeout",   type=int,   default=1,   help="Ping timeout seconds (default: 1)")
    parser.add_argument("-p", "--port-timeout", type=float, default=0.5, help="Port probe timeout seconds (default: 0.5)")
    parser.add_argument("--no-threat",       action="store_true",     help="Skip threat/port scanning")
    parser.add_argument("-v", "--verbose",   action="store_true",     help="Live output as devices are found")
    args = parser.parse_args()

    network    = args.network or get_local_network()
    do_threat  = not args.no_threat

    print(f"Network Device Scanner  —  {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Target network  : {network}")
    print(f"Threads         : {args.workers}")
    print(f"Ping timeout    : {args.timeout}s")
    print(f"Threat scanning : {'enabled' if do_threat else 'disabled'}")

    devices = scan_network(
        network, args.workers, args.timeout,
        args.port_timeout, do_threat, args.verbose,
    )

    print_device_table(devices)

    if do_threat:
        print_threat_report(devices)


if __name__ == "__main__":
    main()
