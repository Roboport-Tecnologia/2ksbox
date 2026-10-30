#!/usr/bin/env python3
"""A tiny upload server for getting files off the rig.

    python3 tools/upload-server.py [dir] [port]

Serves a plain HTML form (no JavaScript, fine for IE6 / old Firefox) on
every interface; every file posted to it lands in `dir` (default
build/uploads/). An existing name gets a numeric suffix, nothing is ever
overwritten. Stdlib only.
"""
import os
import re
import sys
import socket
from email.parser import BytesParser
from email.policy import default as email_policy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FORM = b"""<html><head><title>upload</title></head><body>
<h2>Upload to %s</h2>
<form method="post" enctype="multipart/form-data">
<input type="file" name="f" size="60"><br><br>
<input type="submit" value="Upload">
</form>
<hr><pre>%s</pre></body></html>"""


def lan_addresses():
    """This machine's IPv4 addresses other than loopback. The hostname
    lookup this used to do answers 127.0.0.1 on a Linux box whose
    /etc/hosts names it that way, which the rig cannot reach."""
    ips = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))     # no packet is sent; it picks the route's source
        ips.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127.") and ip not in ips:
                ips.append(ip)
    except OSError:
        pass
    return ips or ["<this machine's IP>"]


def safe_name(name):
    name = os.path.basename(name.replace("\\", "/")) or "upload"
    name = re.sub(r"[^A-Za-z0-9._-]", "_", name)
    return name


def unique_path(d, name):
    path = os.path.join(d, name)
    stem, ext = os.path.splitext(name)
    n = 1
    while os.path.exists(path):
        path = os.path.join(d, "%s.%d%s" % (stem, n, ext))
        n += 1
    return path


class Handler(BaseHTTPRequestHandler):
    outdir = "."

    def listing(self):
        rows = []
        for n in sorted(os.listdir(self.outdir)):
            p = os.path.join(self.outdir, n)
            if os.path.isfile(p):
                rows.append("%10d  %s" % (os.path.getsize(p), n))
        return "\n".join(rows).encode() or b"(nothing uploaded yet)"

    def page(self, code=200):
        body = FORM % (os.path.abspath(self.outdir).encode(), self.listing())
        self.send_response(code)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.page()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)
        head = ("Content-Type: %s\r\n\r\n" % self.headers["Content-Type"]).encode()
        msg = BytesParser(policy=email_policy).parsebytes(head + raw)
        saved = 0
        for part in msg.iter_parts():
            name = part.get_filename()
            if not name:
                continue
            path = unique_path(self.outdir, safe_name(name))
            with open(path, "wb") as f:
                f.write(part.get_payload(decode=True))
            print("saved %s (%d bytes) from %s" % (path, os.path.getsize(path), self.client_address[0]), flush=True)
            saved += 1
        self.page(200 if saved else 400)

    def log_message(self, fmt, *args):
        pass


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/uploads"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    os.makedirs(outdir, exist_ok=True)
    Handler.outdir = outdir
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print("saving to %s; open one of these on the rig:" % os.path.abspath(outdir), flush=True)
    for ip in lan_addresses():
        print("  http://%s:%d/" % (ip, port), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
