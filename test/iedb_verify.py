#!/usr/bin/env python3
"""End-to-end verification for TSRANGEQUERY and aggregation timestamp alignment.

Runs against a built mosquitto tree. Typical usage inside the local docker
build environment (see CLAUDE.md):

    docker run --platform linux/amd64 --rm -v /tmp/mosq-src:/src \
        -v $PWD/test/iedb_verify.py:/verify.py -w /src \
        mosq-build:20.04 python3 /verify.py

The script:
1. Starts the broker with the iedb plugin.
2. Publishes 8 samples via MQTT (/edge/property/gw001/post) using the
   plugin's HMAC-SHA256 auth scheme.
3. TSAGGQUERY: asserts output Timestamps are aligned to the bucket boundary
   (multiples of 300s) and the averages are exact.
4. TSRANGEQUERY: asserts accumulated in-range time is exactly 320s.
"""
import subprocess, time, json, hashlib, hmac, urllib.request, os, sys

SRC = os.environ.get("SRC", "/src")
BASE = 1756800000  # epoch seconds, divisible by 300 (5-min boundary)

def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)

def ok(msg):
    print("OK:", msg)

# ---------- 1. broker ----------
conf = "/tmp/verify.conf"
with open(conf, "w") as f:
    f.write("user root\n"
            "listener 1883\n"
            "allow_anonymous true\n"
            "plugin %s/plugins/iedb/iedb.so\n"
            "plugin_opt_iedb_dir /tmp/iedbdata\n"
            "listener 8080\n"
            "protocol websockets\n"
            "http_dir /tmp/www\n" % SRC)
os.makedirs("/tmp/iedbdata", exist_ok=True)
os.makedirs("/tmp/www", exist_ok=True)
os.chmod("/tmp/iedbdata", 0o777)

env = dict(os.environ)
env["LD_LIBRARY_PATH"] = SRC + "/lib"
broker = subprocess.Popen([SRC + "/src/mosquitto", "-c", conf],
                          env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(3)
if broker.poll() is not None:
    fail("broker exited at startup")
ok("broker started with iedb plugin")

# ---------- 2. HMAC credentials (mirror of auth.c check_hmac_sha256) ----------
uname_part = "gw001"       # gateway sn part of the MQTT username
un = "abcde"               # username after '|' - must be same length as the sn part
clientid = uname_part + "|" + "x" * 45 + "9876543210"
username = uname_part + "|" + un
id_len = len(uname_part)
ts_len = len(clientid) - id_len - 47
if ts_len < 0:
    ts_part = clientid[id_len + 46:]
else:
    ts_part = clientid[id_len + 46: id_len + 46 + ts_len]
msg = ("clientId" + uname_part + "devicename" + un +
       "productkey" + un + "timestamp" + ts_part)
secret = hashlib.md5((un + "Luomi@4321").encode()).hexdigest().upper()
password = hmac.new(secret.encode(), msg.encode(), hashlib.sha256).hexdigest()
ok("hmac password computed (len=%d)" % len(password))

# ---------- 3. ingest 8 samples over two 5-minute buckets ----------
# bucket1 = [BASE, BASE+300): offsets 60..240 (4 samples, avg 25)
# bucket2 = [BASE+300, BASE+600): offsets 320..440 (3 samples, avg 60)
# offset 620 is the first sample of bucket3, triggers finalize of bucket2
samples = [(60, 10), (120, 20), (180, 30), (240, 40),
           (320, 50), (380, 60), (440, 70), (620, 80)]
pub = SRC + "/client/mosquitto_pub"
for off, val in samples:
    payload = json.dumps([{"dn": "sensor1", "time": BASE + off,
                           "properties": {"temp": val}}])
    r = subprocess.run([pub, "-h", "127.0.0.1", "-i", clientid,
                        "-u", username, "-P", password,
                        "-t", "/edge/property/gw001/post", "-m", payload],
                       env=env, capture_output=True, timeout=10)
    if r.returncode != 0:
        fail("publish offset %d failed: %s" % (off, r.stderr.decode(errors="replace")))
    time.sleep(0.1)
ok("8 samples published via MQTT")

# ---------- 4. TSAGGQUERY: boundary-aligned timestamps + exact averages ----------
url = "http://127.0.0.1:8080/TSAGGQUERY/%d/%d/sensor1/avg(temp,300,300)" % (BASE, BASE + 700)
body = json.loads(urllib.request.urlopen(url, timeout=10).read())
rows = body.get("sensor1")
if not rows:
    fail("TSAGGQUERY returned no rows: %s" % body)
timestamps = [row["Timestamp"] for row in rows]
for ts in timestamps:
    if ts % 300 != 0:
        fail("timestamp %d not aligned to 300s boundary (old anchor-at-first-sample bug?)" % ts)
if timestamps != [BASE, BASE + 300]:
    fail("expected bucket timestamps [%d, %d], got %s" % (BASE, BASE + 300, timestamps))
avgs = [row["Fields"]["avg(temp,300,300)"] for row in rows]
if abs(avgs[0] - 25.0) > 0.001 or abs(avgs[1] - 60.0) > 0.001:
    fail("expected avgs [25, 60], got %s" % avgs)
ok("TSAGGQUERY aligned: timestamps=%s avgs=%s" % (timestamps, avgs))

# ---------- 5. TSRANGEQUERY: in-range duration ----------
# values: 10,20,30,40,50,60,70,80 at +60..+620; range [15,65]
# in-range from +120 (20) through +380 (60); leaves range at +440 (70)
# expected accumulated time = 440-120 = 320 s
url = "http://127.0.0.1:8080/TSRANGEQUERY/%d/%d/15/65/sensor1/" % (BASE, BASE + 700)
body = json.loads(urllib.request.urlopen(url, timeout=10).read())
dev = body.get("sensor1")
if not dev:
    fail("TSRANGEQUERY returned no device data: %s" % body)
got = dev.get("temp")
if got != 320:
    fail("expected in-range time 320s, got %s" % got)
ok("TSRANGEQUERY in-range time: %ss" % got)

# ---------- 6. cleanup ----------
broker.terminate()
broker.wait(timeout=5)
print("ALL_VERIFICATIONS_PASSED")
