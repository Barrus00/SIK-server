import socket
import sys
import re
import os
import time
import traceback
import random
from dataclasses import dataclass

if len(sys.argv) != 2:
    print("test_runner.py <port>")
    exit(0)

port = int(sys.argv[1])
s = None
current_test = None
passed = 0
failed = 0
warnings = 0
warned_conn_close = False


class Test(object):
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        global current_test
        current_test = self.name
        # print(f"RUN  [{self.name}]")
        return None

    def __exit__(self, etype, eval, tb):
        global passed
        global failed
        if etype is None:
            print(f"PASS [{self.name}]")
            passed += 1
        else:
            print("")
            print(f"FAIL [{self.name}]")
            failed += 1
            traceback.print_exception(etype, eval, tb)
            print("")
        return True

@dataclass
class Response:
    code: int
    content_length: int
    body: bytes
    conn_closed: bool
    headers: set


def request(data, body='', reuse_connection=False, validate_connection=True):
    global s
    global warned_conn_close
    if not reuse_connection:
        if s is not None:
            s.close()
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("localhost", port))

    if isinstance(body, str):
        body = body.encode()
    client_conn_closed = 'connection:close' in [x[0] + ':' + x[2].strip() for x in [x.lower().partition(':') for x in data]]
    s.send(('\r\n'.join(data) + '\r\n\r\n').encode() + body)

    buf = s.recv(4096)
    while b'\r\n\r\n' not in buf:
        rx = s.recv(4096)
        assert(rx != b'') # eof
        buf += rx
    http_res, _, rx_body = buf.partition(b'\r\n\r\n')
    http_res = http_res.split(b'\r\n')
    status_line = http_res[0]
    assert(status_line.startswith(b'HTTP/1.1 '))
    status_line = status_line[len(b'HTTP/1.1 '):]
    status_code, _, status_reason = status_line.partition(b' ')
    status_code = int(status_code)

    received_headers = {}
    conn_closed = False
    content_length = 0
    for h in http_res[1:]:
        name, _, val = h.partition(b':')
        name = name.lower()
        assert(name in [b"connection", b'content-type', b'content-length', b'server', b'location'])
        assert(name not in received_headers)
        val = val.strip()
        received_headers[name] = val
        if name == b'connection' and val.lower() == b'close':
            conn_closed = True
        if name == b'content-length':
            content_length = int(val)

    if not data[0].startswith("HEAD "):
        while len(rx_body) < content_length:
            rx = s.recv(1024*1024)
            assert(rx != b'') # eof
            rx_body += rx
        assert(len(rx_body) == content_length)
    else:
        assert(rx_body == b'')

    assert(not ((status_code == 200 or status_code == 404) and conn_closed and not client_conn_closed))
    if not warned_conn_close and client_conn_closed and not conn_closed:
        warn_if(True, "server probably should reply with connection: close if connection was closed")
        warned_conn_close = True

    if not conn_closed and not client_conn_closed and validate_connection: # validate that the connection was not closed
        res = request(["GET / HTTP/1.1", "Connection: close"], reuse_connection=True, validate_connection=False)
        assert(res.code == 400)

    return Response(status_code, content_length, rx_body, conn_closed or client_conn_closed, received_headers)

def warn_if(cond, msg):
    global warnings
    if cond:
        print(f"WARN [{current_test}] Warning from test: " + msg)
        warnings += 1


asset_a = open("test_static/a", "rb").read()
asset_chars = open("test_static/dir/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-0123456789.txt", "rb").read()

with Test("simple head request works"):
    res = request(["HEAD /a HTTP/1.1"])
    assert(res.code == 200 and res.content_length == len(asset_a))

with Test("simple get request works"):
    res = request(["GET /a HTTP/1.1"])
    assert(res.code == 200 and res.body == asset_a)

with Test("path with all allowed character returns 200"):
    res = request(["GET /dir/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-0123456789.txt HTTP/1.1"])
    assert(res.code == 200 and res.body == asset_chars)

with Test("invalid path in head returns 404"):
    res = request(["HEAD /ThisFileShouldNotExit HTTP/1.1"])
    assert(res.code == 404)
    head_error_page_length = res.content_length

with Test("invalid path in get returns 404 and same content length"):
    res = request(["GET /ThisFileShouldNotExit HTTP/1.1"])
    assert(res.code == 404)
    assert(len(res.body) == head_error_page_length)

with Test("unsupported http version returns 400"):
    res = request(["GET /a HTTP/1.0"])
    assert(res.code == 400 and res.conn_closed)

with Test("invalid method returns 405 or 501"):
    res = request(["TEST /a HTTP/1.1"])
    assert(res.code == 405 or res.code == 501)
    warn_if(not res.conn_closed, "invalid methods probably should close the connection")

with Test("invalid characters in path return 404"):
    for c in range(0, 256):
        if not re.match('[a-zA-Z0-9.\-/ \n\r]', chr(c)):
            res = request(["HEAD /" + chr(c) + " HTTP/1.1"])
            assert(res.code == 404 or res.code == 400)
            res = request(["GET /" + chr(c) + " HTTP/1.1"])
            assert(res.code == 404 or res.code == 400)

with Test("\\r and \\n characters in path maybe return 404"):
    res = request(["GET /" + '\r' + " HTTP/1.1"])
    warn_if(res.code != 404, "Requesting GET /\\r should probably result in 404")
    res = request(["GET /" + '\n' + " HTTP/1.1"])
    warn_if(res.code != 404, "Requesting GET /\\n should probably result in 404")

with Test("more than one space in the path returns 400"):
    res = request(["GET  /a HTTP/1.1"])
    assert(res.code == 400)
    res = request(["GET /a  HTTP/1.1"])
    assert(res.code == 400)

with Test("ignored duplicate headers are allowed"):
    res = request(["GET /a HTTP/1.1", "ignored: first", "ignored: second"])
    assert(res.code == 200)

with Test("duplicate headers specified in the task content return 400"):
    res = request(["GET /a HTTP/1.1", "connection: close", "connection: close"])
    assert(res.code == 400)

with Test("header without a name returns 400"):
    res = request(["GET /a HTTP/1.1", ": invalid"])
    assert(res.code == 400)

with Test("header without colon fails"):
    res = request(["GET /a HTTP/1.1", "invalid"])
    assert(res.code == 400)

with Test("missing slash at start of the path returns 400"):
    res = request(["GET a HTTP/1.1", "invalid"])
    assert(res.code == 400)

with Test("connection close closes the connection"):
    res = request(["GET /a HTTP/1.1", "connection: close"])
    assert(res.code == 200 and res.conn_closed)
    res = request(["GET /a HTTP/1.1", "connection:close"])
    assert(res.code == 200 and res.conn_closed)
    res = request(["GET /a HTTP/1.1", "ConNECtioN: close"])
    assert(res.code == 200 and res.conn_closed)

with Test("space before colon in header either returns 400 or the header is ignored"):
    res = request(["GET /a HTTP/1.1", "connection : close"])
    assert((res.code == 200 and not res.conn_closed) or res.code == 400)

with Test("'..' path name tests return 200"):
    res = request(["GET /dir/../a HTTP/1.1"])
    assert(res.code == 200 and res.body == asset_a)

    res = request(["GET /dir/dir2/../../a HTTP/1.1"])
    assert(res.code == 200 and res.body == asset_a)

with Test("'../test-runner.py' returns 404"):
    res = request(["GET /../test-runner.py HTTP/1.1"])
    assert(res.code == 404)

with Test("'/../a' returns 404"):
    res = request(["GET /../a HTTP/1.1"])
    assert(res.code == 404)

with Test("'/a/' returns 404"):
    res = request(["GET /a/ HTTP/1.1"])
    assert(res.code == 404 or res.code == 400)

with Test("stray \\r or \\n should do something sane"):
    res = request(["GET /a HTTP/1.1", "first: second\rthird: fourth"])
    assert((res.code == 200 and res.body == asset_a) or res.code == 400)
    res = request(["GET /a HTTP/1.1", "first: second\nthird: fourth"])
    assert((res.code == 200 and res.body == asset_a) or res.code == 400)

with Test("correlated server head request works"):
    res = request(["HEAD /redirect HTTP/1.1"])
    assert(res.code == 302)
    assert(res.headers[b'location'] == b'http://127.0.0.1:2567/redirect')

with Test("correlated server get request works"):
    res = request(["GET /redirect HTTP/1.1"])
    assert(res.code == 302)
    assert(res.headers[b'location'] == b'http://127.0.0.1:2567/redirect')

with Test("server handles long non-existing paths and headers"):
    res = request(["HEAD /" + ("a" * (8192 - 1)) + " HTTP/1.1"])
    assert(res.code == 404)

    res = request(["GET /" + ("a" * (8192 - 1)) + " HTTP/1.1"])
    assert(res.code == 404)

    res = request(["HEAD /" + ("a" * (8192 - 1)) + " HTTP/1.1"] + ["cookie: " + ("a" * 8192)] * 256)
    assert(res.code == 404)

    res = request(["GET /" + ("a" * (8192 - 1)) + " HTTP/1.1"] + ["cookie: " + ("a" * 8192)] * 256)
    assert(res.code == 404)

with Test("dynamically created 64mb file is properly loaded"):
    asset_random_test = random.getrandbits(1024 * 1024 * 64 * 8).to_bytes(1024 * 1024 * 64, 'little')
    with open("test_static/Random64M.bin", "wb") as f:
        f.write(asset_random_test)
    res = request(["GET /Random64M.bin HTTP/1.1"])
    assert(res.code == 200 and res.body == asset_random_test)
    os.remove("test_static/Random64M.bin")
    asset_random_test = None

with Test("server does not crash if remote closes socket"):
    asset_random_test = random.getrandbits(1024 * 1024 * 8 * 8).to_bytes(1024 * 1024 * 8, 'little')
    with open("test_static/Random8M.bin", "wb") as f:
        f.write(asset_random_test)

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("localhost", port))
    s.send(b"GET /Random8M.bin HTTP/1.1\r\n\r\n")
    s.shutdown(socket.SHUT_RDWR)
    s.close()

    res = request(["GET /a HTTP/1.1"]) # ensure the server is still alive
    assert(res.code == 200)

    os.remove("test_static/Random8M.bin")
    asset_random_test = None

print("")
print(f"{passed} tests passed, {failed} tests failed" + (f", {warnings} warnings" if warnings > 0 else ""))
