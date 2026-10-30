#!/usr/bin/env python3
"""Commit and push the current branch through the local spv accelerator.

The spv wrapper, its logs and any credentials stay outside the repository.
This script never reads or copies them; it only executes the local binary
when a SOCKS endpoint is not already running. Every path is checked before
staging, and credential-like paths abort the run instead of being ignored.
"""
import argparse
import base64
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path, PurePosixPath
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROXY = "socks5h://127.0.0.1:7890"
SENSITIVE_DIRS = {".cache", ".tools", ".keys", "build"}
SENSITIVE_NAMES = ("credential", "password", "secret")
START_TIMEOUT = 30
PUSH_TIMEOUT = 180


def git(*args, env=None, check=True, timeout=60):
    return subprocess.run(
        ["git", *args],
        cwd=ROOT,
        env=env,
        check=check,
        text=True,
        capture_output=True,
        timeout=timeout,
    )


def git_text(*args, env=None):
    return git(*args, env=env).stdout.strip()


def sensitive_reason(path):
    parts = PurePosixPath(path).parts
    if not parts:
        return None
    if parts[0] in SENSITIVE_DIRS:
        return f"inside protected directory {parts[0]}/"
    name = parts[-1].lower()
    if "spv" in name:
        return "looks like an spv accelerator file"
    if name.endswith((".seed", ".token")):
        return "looks like key material"
    if any(word in name for word in SENSITIVE_NAMES):
        return "looks like a credential file"
    return None


def changed_paths():
    raw = git("status", "--porcelain=v1", "-z", "-uall").stdout
    tokens = raw.split("\0")
    paths = []
    index = 0
    while index < len(tokens):
        record = tokens[index]
        index += 1
        if not record:
            continue
        status, path = record[:2], record[3:]
        paths.append(path)
        # Rename/copy records carry the original path as the next token.
        if status[0] in "RC" or status[1] in "RC":
            index += 1
    return paths


def parse_proxy(proxy):
    url = urlparse(proxy)
    return url.hostname or "127.0.0.1", url.port or 7890


def port_open(host, port, timeout=0.3):
    try:
        with socket.create_connection((host, port), timeout):
            return True
    except OSError:
        return False


def spv_binary():
    configured = os.environ.get("SPV_BIN")
    candidates = [configured] if configured else []
    candidates += [str(Path.home() / ".local" / "bin" / "spv"), shutil.which("spv")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise SystemExit("spv not found; set SPV_BIN or put the accelerator on PATH")


def redact(text):
    text = re.sub(r"(password:\s*)[^\s,]+", r"\1***", text, flags=re.I)
    text = re.sub(r"(Authorization:\s*Basic\s*)[A-Za-z0-9+/=]+", r"\1***", text, flags=re.I)
    return text[-1200:]


def start_proxy(proxy):
    host, port = parse_proxy(proxy)
    if port_open(host, port):
        return None

    binary = spv_binary()
    env = dict(os.environ)
    env["PATH"] = str(Path.home() / ".local" / "bin") + ":" + env.get("PATH", "")
    process = subprocess.Popen(
        [binary],
        cwd=ROOT,
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        start_new_session=True,
    )

    lines = []
    sent = False

    def reader():
        for line in process.stdout:
            lines.append(line)

    threading.Thread(target=reader, daemon=True).start()
    deadline = time.monotonic() + START_TIMEOUT
    try:
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise SystemExit("spv exited before opening the proxy:\n" + redact("".join(lines)))
            if not sent:
                text = "".join(lines)
                nodes = [
                    (match.group(1), match.group(2))
                    for match in re.finditer(r"节点名称:\s*(.*?),\s*节点编号:\s*(\d+)", text)
                ]
                if nodes:
                    pick = next((node for node in nodes if "香港" in node[0]), nodes[0])
                    process.stdin.write(pick[1] + "\n")
                    process.stdin.flush()
                    sent = True
            if port_open(host, port):
                return process
            time.sleep(0.4)
        raise SystemExit(f"spv did not open {host}:{port} within {START_TIMEOUT}s")
    except Exception:
        stop_proxy(process)
        raise


def stop_proxy(process):
    if not process or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()


def push_env(proxy, token):
    env = dict(os.environ)
    env["GIT_TERMINAL_PROMPT"] = "0"
    if proxy:
        env["ALL_PROXY"] = env["HTTPS_PROXY"] = env["HTTP_PROXY"] = proxy
    if token:
        basic = base64.b64encode(f"x-access-token:{token}".encode()).decode()
        env["GIT_CONFIG_COUNT"] = "2"
        env["GIT_CONFIG_KEY_0"] = "credential.helper"
        env["GIT_CONFIG_VALUE_0"] = ""
        env["GIT_CONFIG_KEY_1"] = "http.extraHeader"
        env["GIT_CONFIG_VALUE_1"] = f"Authorization: Basic {basic}"
    return env


def reject_embedded_credentials(remote):
    url = git_text("config", "--get", f"remote.{remote}.url")
    parsed = urlparse(url)
    if parsed.username or parsed.password:
        raise SystemExit(f"remote {remote} embeds credentials in its URL; refusing to push")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-m", "--message", help="commit message; required when there are changes")
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--branch", help="branch to push; defaults to the current branch")
    parser.add_argument("--proxy", default=DEFAULT_PROXY, help=f"local SOCKS proxy ({DEFAULT_PROXY})")
    parser.add_argument("--no-proxy", action="store_true", help="push without starting the accelerator")
    parser.add_argument("--dry-run", action="store_true", help="show what would be staged and exit")
    parser.add_argument("--test", action="store_true", help="run host-only tests before committing")
    args = parser.parse_args()

    branch = args.branch or git_text("rev-parse", "--abbrev-ref", "HEAD")
    if branch == "HEAD":
        raise SystemExit("detached HEAD; pass --branch explicitly")

    paths = changed_paths()
    blocked = [(path, sensitive_reason(path)) for path in paths if sensitive_reason(path)]
    if blocked:
        print("refusing to stage sensitive paths:", file=sys.stderr)
        for path, reason in blocked:
            print(f"  {path}: {reason}", file=sys.stderr)
        raise SystemExit(1)

    if args.dry_run:
        for path in paths:
            print(path)
        return 0

    if args.test:
        subprocess.run([sys.executable, str(ROOT / "tools" / "test.py"), "--host-only"], cwd=ROOT, check=True)

    if paths:
        if not args.message:
            raise SystemExit("changes are present; pass -m/--message")
        git("add", "-A", "--", *paths)
        git("commit", "-m", args.message)
        print(f"committed: {git_text('rev-parse', '--short', 'HEAD')}")
    else:
        print("nothing to commit")

    reject_embedded_credentials(args.remote)
    process = None
    proxy = None if args.no_proxy else args.proxy
    try:
        if proxy:
            process = start_proxy(proxy)
        token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
        env = push_env(proxy, token)
        git("push", args.remote, branch, env=env, timeout=PUSH_TIMEOUT)
        local = git_text("rev-parse", "HEAD")
        remote_line = git_text("ls-remote", args.remote, f"refs/heads/{branch}", env=env, timeout=PUSH_TIMEOUT)
        remote = remote_line.split()[0] if remote_line else ""
        if remote != local:
            raise SystemExit(f"remote {branch} is {remote or 'missing'}, local HEAD is {local}")
        print(f"pushed {args.remote}/{branch} at {local[:12]}")
    finally:
        stop_proxy(process)
    return 0


if __name__ == "__main__":
    sys.exit(main())
