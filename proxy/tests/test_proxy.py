import subprocess
import time
import urllib.request
import json
import sys
import socket
import os
import urllib.error

TOKEN = "test-token"


def send_command(*args):
    encoded = [str(arg).encode() for arg in args]
    request = f"*{len(encoded)}\r\n".encode()
    for arg in encoded:
        request += f"${len(arg)}\r\n".encode() + arg + b"\r\n"
    with socket.create_connection(("127.0.0.1", 6379)) as sock:
        sock.sendall(request)
        return sock.recv(1024)

def wait_for_port(port, timeout=10):
    start_time = time.time()
    while time.time() - start_time < timeout:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            if sock.connect_ex(('127.0.0.1', port)) == 0:
                return True
        time.sleep(0.1)
    return False

def run_test():
    dicedb_proc = None
    proxy_proc = None
    try:
        # Start DiceDB server
        print("Starting DiceDB server...")
        dicedb_proc = subprocess.Popen(["../../src/dicedb-server", "--port", "6379"], cwd=os.path.dirname(__file__), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_for_port(6379):
            print("DiceDB server failed to start")
            sys.exit(1)

        assert send_command("SET", f"proxy:auth:tokens:{TOKEN}", "active").startswith(b"+OK")

        # Start Proxy
        print("Starting Proxy server...")
        proxy_proc = subprocess.Popen(["../dicedb-proxy", "8080"], cwd=os.path.dirname(__file__), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_for_port(8080):
            print("Proxy server failed to start")
            sys.exit(1)

        import uuid
        test_key = "testkey_" + str(uuid.uuid4())

        # Give it a tiny bit of time to connect to upstream
        time.sleep(0.5)

        # Helper to make requests with auth header
        def make_req(url):
            req = urllib.request.Request(url)
            req.add_header("Authorization", f"Bearer {TOKEN}")
            return req

        print("Testing authentication is required...")
        try:
            urllib.request.urlopen(f"http://127.0.0.1:8080/GET/{test_key}")
            raise AssertionError("Expected a request without a token to fail")
        except urllib.error.HTTPError as error:
            assert error.code == 401, f"Expected 401, got {error.code}"

        # Test 1: GET non-existent key
        print("Testing GET non-existent key...")
        with urllib.request.urlopen(make_req(f"http://127.0.0.1:8080/GET/{test_key}")) as response:
            data = json.loads(response.read().decode())
            assert data["result"] is None, f"Expected null, got {data}"

        # Test 2: SET a key
        print("Testing SET key...")
        with urllib.request.urlopen(make_req(f"http://127.0.0.1:8080/SET/{test_key}/helloworld")) as response:
            data = json.loads(response.read().decode())
            assert data["result"] == "OK", f"Expected OK, got {data}"

        # Test 3: GET the key
        print("Testing GET existing key...")
        with urllib.request.urlopen(make_req(f"http://127.0.0.1:8080/GET/{test_key}")) as response:
            data = json.loads(response.read().decode())
            assert data["result"] == "helloworld", f"Expected helloworld, got {data}"

        print("All tests passed successfully!")

    except Exception as e:
        print(f"Test failed: {e}")
        sys.exit(1)
    finally:
        if proxy_proc:
            proxy_proc.terminate()
            proxy_proc.wait()
        if dicedb_proc:
            dicedb_proc.terminate()
            dicedb_proc.wait()

if __name__ == "__main__":
    run_test()
