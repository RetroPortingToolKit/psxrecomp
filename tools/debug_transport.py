"""Shared, bounded JSON transport for PSX test clients.

Socket timeouts report a failed request; this module never stops a game.
Title clients retain command schemas, replay formats and scenario assertions.
"""
from __future__ import annotations

import json
import math
import socket
import time


class DebugError(RuntimeError):
    """A transport, framing, schema or runtime command failure."""


class DebugClient:
    def __init__(self, port: int, *, timeout: float = 3.0,
                 max_response_bytes: int = 1024 * 1024):
        if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
            raise ValueError("Expected a localhost TCP port in 1..65535")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("Expected a positive finite timeout")
        if not 64 <= max_response_bytes <= 2 * 1024 * 1024:
            raise ValueError("Response bound must be between 64 bytes and 2 MiB")
        self.port = port
        self.timeout = timeout
        self.max_response_bytes = max_response_bytes
        self._sequence = 0

    def request(self, command: str, **fields) -> dict:
        if not isinstance(command, str) or not command or "id" in fields or "cmd" in fields:
            raise ValueError("Expected a command without reserved id/cmd fields")
        self._sequence += 1
        request_id = self._sequence
        payload = (json.dumps(dict(id=request_id, cmd=command, **fields),
                              allow_nan=False) + "\n").encode("utf-8")
        deadline = time.monotonic() + self.timeout
        try:
            with socket.create_connection(("127.0.0.1", self.port),
                                          timeout=self.timeout) as connection:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("Connection consumed the request deadline")
                connection.settimeout(remaining)
                connection.sendall(payload)
                data = bytearray()
                while b"\n" not in data:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("Response deadline exceeded")
                    connection.settimeout(remaining)
                    chunk = connection.recv(min(16384, self.max_response_bytes + 1 - len(data)))
                    if not chunk:
                        raise DebugError(f"{command}: peer closed before a complete response")
                    data.extend(chunk)
                    if len(data) > self.max_response_bytes:
                        raise DebugError(f"{command}: response exceeds {self.max_response_bytes} bytes")
                line, _, trailing = data.partition(b"\n")
                if trailing.strip():
                    raise DebugError(f"{command}: extra response data")
                result = json.loads(line)
        except (OSError, UnicodeError, ValueError) as error:
            raise DebugError(f"port {self.port}, {command}: {error}") from error
        if not isinstance(result, dict) or type(result.get("id")) is not int or result["id"] != request_id:
            received = result.get("id") if isinstance(result, dict) else None
            detail = str(result.get("err", result.get("error", "")))[:160] if isinstance(result, dict) else ""
            raise DebugError(f"{command}: invalid or mismatched response id (expected {request_id}, received {received!r}, runtime error={detail!r})")
        if result.get("ok") is not True:
            raise DebugError(f"{command}: runtime rejected command: {str(result)[:240]}")
        return result


def isolated_environment(root):
    """Return explicit writable paths for a test-owned profile, without globals."""
    from pathlib import Path
    root = Path(root).resolve()
    if root.drive.upper() == 'C:':
        raise ValueError('Test profiles must be on a non-C: drive')
    names = {'TEMP': 'temp', 'TMP': 'temp', 'TMPDIR': 'temp',
             'LOCALAPPDATA': 'data', 'APPDATA': 'appdata',
             'XDG_CACHE_HOME': 'cache', 'XDG_DATA_HOME': 'data',
             'XDG_CONFIG_HOME': 'config', 'XDG_STATE_HOME': 'state',
             'PSX_RESIDENT_CACHE': 'resident', 'PSX_OVERLAY_CACHE_DIR': 'cache',
             'PSX_OVERLAY_CAPTURE_ROOT': 'cache'}
    result = {}
    for key, relative in names.items():
        path = root / relative
        path.mkdir(parents=True, exist_ok=True)
        result[key] = str(path)
    result['PSX_OVERLAY_CAPTURES'] = str(root / 'cache/overlay_captures.json')
    return result
