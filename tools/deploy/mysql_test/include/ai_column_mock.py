#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Deterministic OpenAI compatible endpoint used by the ai column tests.

The background service of the AI column calls the model through the same endpoint the
ai_complete/ai_embed functions use, so the tests need a server that answers the same
request in the same way every time. This one answers two routes:

    POST .../chat/completions   -> {"choices": [{"message": {"content": "<answer>"}}]}
    POST .../embeddings         -> {"data": [{"embedding": [<float>, ...]}]}

The answer of a completion is derived from the prompt: the AI column definition carries a
prompt hint chosen from the result type at DDL time, so answering "42" for a number column
and a JSON object for a json column is enough to exercise the whole write back path.

Fault injection lives on a control route, which is what the retry and the result type
check are tested with:

    POST /__control  {"mode": "ok|error|wrong_dim|non_numeric|garbage", "fail_next": 2}
    GET  /__stats    {"completion": n, "embedding": n, "injected": n}

Usage:
    python3 ai_column_mock.py --ports 19120,19121,19122,19123,19124 --dim 4
"""

import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODE_OK = "ok"
MODE_ERROR = "error"
MODE_WRONG_DIM = "wrong_dim"
MODE_NON_NUMERIC = "non_numeric"
MODE_GARBAGE = "garbage"
MODES = (MODE_OK, MODE_ERROR, MODE_WRONG_DIM, MODE_NON_NUMERIC, MODE_GARBAGE)

STATE = {
    "mode": MODE_OK,
    "fail_next": 0,
    "dim": 4,
    "completion": 0,
    "embedding": 0,
    "injected": 0,
}
STATE_LOCK = threading.Lock()


def answer_for(prompt):
    """A deterministic answer, chosen so that it satisfies the prompt hint of the column."""
    lowered = (prompt or "").lower()
    if "json" in lowered:
        return '{"ok": true}'
    if "date literal" in lowered:
        return "2024-01-02 03:04:05"
    if "single number" in lowered:
        return "42"
    digest = hashlib.md5((prompt or "").encode("utf-8")).hexdigest()[:8]
    return "answer-" + digest


def vector_for(text, dim):
    """A deterministic vector: the same input always maps to the same embedding."""
    digest = hashlib.md5((text or "").encode("utf-8")).digest()
    values = []
    for i in range(dim):
        byte = digest[i % len(digest)]
        values.append(round(byte / 255.0, 6))
    return values


def text_inputs(payload):
    """Both the completion and the embedding route deliver their input as text."""
    if not isinstance(payload, dict):
        return []
    for key in ("input", "messages", "prompt"):
        value = payload.get(key)
        if isinstance(value, str):
            return [value]
        if isinstance(value, list):
            texts = []
            for item in value:
                if isinstance(item, str):
                    texts.append(item)
                elif isinstance(item, dict) and isinstance(item.get("content"), str):
                    texts.append(item["content"])
            if texts:
                return texts
    return []


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep the test output readable
        pass

    def _send(self, status, body):
        raw = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path.rstrip("/").endswith("/__stats"):
            with STATE_LOCK:
                self._send(200, dict((k, STATE[k]) for k in
                                     ("mode", "completion", "embedding", "injected")))
        else:
            self._send(404, {"error": "unknown route"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length > 0 else b""
        try:
            payload = json.loads(raw.decode("utf-8")) if raw else {}
        except ValueError:
            self._send(400, {"error": "invalid json"})
            return

        route = self.path.rstrip("/")
        if route.endswith("/__control"):
            self._control(payload)
        elif route.endswith("/chat/completions"):
            self._completion(payload)
        elif route.endswith("/embeddings"):
            self._embedding(payload)
        else:
            self._send(404, {"error": "unknown route"})

    def _control(self, payload):
        mode = payload.get("mode")
        if isinstance(mode, str) and mode in MODES:
            with STATE_LOCK:
                STATE["mode"] = mode
        fail_next = payload.get("fail_next")
        if isinstance(fail_next, int):
            with STATE_LOCK:
                STATE["fail_next"] = fail_next
        dim = payload.get("dim")
        if isinstance(dim, int) and dim > 0:
            with STATE_LOCK:
                STATE["dim"] = dim
        with STATE_LOCK:
            self._send(200, dict((k, STATE[k]) for k in ("mode", "fail_next", "dim")))

    def _should_fail(self):
        """Returns True when this request is the one that has to fail on purpose."""
        with STATE_LOCK:
            injected = STATE["mode"] == MODE_ERROR or STATE["fail_next"] > 0
            if injected:
                if STATE["fail_next"] > 0:
                    STATE["fail_next"] -= 1
                STATE["injected"] += 1
            return injected

    def _completion(self, payload):
        if self._should_fail():
            self._send(500, {"error": {"message": "injected model failure"}})
            return
        texts = text_inputs(payload)
        with STATE_LOCK:
            STATE["completion"] += 1
            garbage = STATE["mode"] == MODE_GARBAGE
        content = "" if garbage else answer_for(texts[0] if texts else "")
        self._send(200, {"choices": [{"index": 0, "message": {"role": "assistant",
                                                              "content": content}}]})

    def _embedding(self, payload):
        if self._should_fail():
            self._send(500, {"error": {"message": "injected model failure"}})
            return
        texts = text_inputs(payload)
        with STATE_LOCK:
            STATE["embedding"] += 1
            mode = STATE["mode"]
            dim = STATE["dim"]
        if mode == MODE_WRONG_DIM:
            dim += 1
        data = []
        for i, text in enumerate(texts):
            if mode == MODE_NON_NUMERIC:
                embedding = ["not-a-number"] + vector_for(text, max(dim - 1, 0))
            elif mode == MODE_GARBAGE:
                embedding = "not a vector"
            else:
                embedding = vector_for(text, dim)
            data.append({"index": i, "embedding": embedding})
        self._send(200, {"data": data})


def serve(port, ready):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    ready.set()
    server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", default="19120",
                        help="comma separated list of ports, 19120-19124 by convention")
    parser.add_argument("--dim", type=int, default=4, help="dimension of the embeddings")
    parser.add_argument("--mode", default=MODE_OK, choices=MODES,
                        help="start in an injected failure mode")
    parser.add_argument("--fail-next", type=int, default=0,
                        help="fail this many of the next requests")
    args = parser.parse_args()

    STATE["dim"] = args.dim
    STATE["mode"] = args.mode
    STATE["fail_next"] = args.fail_next

    servers = []
    for port in [int(p) for p in args.ports.split(",") if p.strip()]:
        ready = threading.Event()
        thread = threading.Thread(target=serve, args=(port, ready), daemon=True)
        thread.start()
        ready.wait()
        servers.append(port)
    print("ai column mock listening on %s" % ",".join(str(p) for p in servers), flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
