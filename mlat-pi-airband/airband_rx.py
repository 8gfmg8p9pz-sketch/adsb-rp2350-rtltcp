#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
airband_rx.py - rtl_tcp (5号機 10.5.2.24:1234) を受けて AM 復調し、HDMI 音声に出す受信機
  ・画面(hdmi-radar / map4k)には一切触らない。音だけ HDMI に載せる
  ・操作はスマホのブラウザ:  http://10.5.2.207:8091/
  ・設定は /home/pi/airband_rx.json に保存 (再起動後も同じ周波数で再開)
依存: python3-numpy, alsa-utils(aplay)
"""
import json, os, socket, struct, subprocess, threading, time, math
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import numpy as np

CONF_FILE = os.environ.get("AIRBAND_CONF", "/home/pi/airband_rx.json")
DEFAULT = {
    "host": "10.5.2.24", "port": 1234,
    "rate": 250000,          # rtl_tcp サンプルレート (5号機の実績値)
    "freq": 118100000,       # Hz
    "presets": [118100000, 118800000, 119100000, 121500000, 121900000, 126200000, 135250000],
    "gain": 0,               # 0=自動, それ以外は 0.1dB 単位 (例 400=40.0dB)
    "ppm": 0,
    "volume": 70,            # 0-100
    "squelch": -60.0,        # dBFS。これ未満は無音 (-120 で常時開放)
    "enabled": True,
    "alsa_device": "default",
    "http_port": 8091,
}
DECIM = 5                    # 250k -> 50k
AUDIO_RATE_DIV = DECIM


def load_conf():
    c = dict(DEFAULT)
    try:
        with open(CONF_FILE, encoding="utf-8") as f:
            c.update(json.load(f))
    except Exception:
        pass
    return c


conf = load_conf()
lock = threading.Lock()
status = {"connected": False, "level_db": -120.0, "open": False, "tuner": "",
          "rate_meas": 0, "error": "", "since": 0}
retune = threading.Event()


def save_conf():
    try:
        tmp = CONF_FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(conf, f, ensure_ascii=False, indent=1)
        os.replace(tmp, CONF_FILE)
    except Exception as e:
        status["error"] = "save: %s" % e


def lowpass(ntaps, cutoff):
    """cutoff は入力レートに対する比 (0-0.5)。窓付き sinc"""
    n = np.arange(ntaps) - (ntaps - 1) / 2.0
    h = np.sinc(2 * cutoff * n) * np.hamming(ntaps)
    return (h / h.sum()).astype(np.float32)


class Demod:
    """IQ(uint8) -> AM 音声(int16)。ブロック間の状態を保持"""
    def __init__(self):
        self.h_if = lowpass(63, 4000.0 / 250000)      # IF ±4kHz 相当
        self.h_af = lowpass(31, 3000.0 / 50000)        # 音声 3kHz
        self.z_if = np.zeros(len(self.h_if) - 1, np.complex64)
        self.z_af = np.zeros(len(self.h_af) - 1, np.float32)
        self.phase = 0
        self.dc = 0.0
        self.agc = 0.05
        self.gate = 0.0

    def process(self, raw, vol, sq_db):
        iq = raw.astype(np.float32)
        x = ((iq[0::2] - 127.4) + 1j * (iq[1::2] - 127.4)).astype(np.complex64) / 128.0
        x = np.concatenate([self.z_if, x])
        self.z_if = x[-(len(self.h_if) - 1):]
        y = np.convolve(x, self.h_if, mode="valid")
        y = y[self.phase::DECIM]
        n_in = len(x) - (len(self.h_if) - 1)
        self.phase = (self.phase - n_in) % DECIM
        env = np.abs(y).astype(np.float32)
        p = float(np.mean(env * env)) if len(env) else 0.0
        level = 10 * math.log10(p + 1e-12)
        # DC(搬送波)除去
        out = np.empty_like(env)
        dc = self.dc
        a = 0.0005
        # ブロック平均で近似 (軽量)
        m = float(env.mean()) if len(env) else 0.0
        dc = dc + (m - dc) * min(1.0, a * len(env))
        self.dc = dc
        out = env - dc
        # 音声 LPF
        z = np.concatenate([self.z_af, out])
        self.z_af = z[-(len(self.h_af) - 1):]
        af = np.convolve(z, self.h_af, mode="valid")
        # AGC (搬送波レベル基準)
        tgt = max(dc, 1e-4)
        self.agc += (tgt - self.agc) * 0.3
        af = af / (self.agc * 1.2)
        # スケルチ (滑らかに開閉)
        want = 1.0 if level >= sq_db else 0.0
        g0 = self.gate
        g1 = g0 + (want - g0) * 0.5
        self.gate = g1
        ramp = np.linspace(g0, g1, len(af), dtype=np.float32)
        af = af * ramp * (vol / 100.0) * 0.8
        np.clip(af, -1.0, 1.0, out=af)
        return (af * 32000).astype(np.int16).tobytes(), level, g1 > 0.5


class Audio:
    def __init__(self):
        self.p = None

    def ensure(self):
        if self.p and self.p.poll() is None:
            return
        rate = int(conf["rate"]) // AUDIO_RATE_DIV
        self.p = subprocess.Popen(
            ["aplay", "-q", "-D", conf.get("alsa_device", "default"),
             "-t", "raw", "-f", "S16_LE", "-c", "1", "-r", str(rate),
             "--buffer-time=400000"],
            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def write(self, b):
        try:
            self.ensure()
            self.p.stdin.write(b)
        except Exception:
            try:
                self.p.kill()
            except Exception:
                pass
            self.p = None

    def close(self):
        if self.p:
            try:
                self.p.kill()
            except Exception:
                pass
            self.p = None


def cmd(s, c, v):
    s.sendall(struct.pack(">BI", c, int(v) & 0xFFFFFFFF))


def apply_tuning(s):
    with lock:
        cmd(s, 0x02, conf["rate"])
        cmd(s, 0x05, conf["ppm"] & 0xFFFFFFFF)
        if int(conf["gain"]) == 0:
            cmd(s, 0x03, 0)
            cmd(s, 0x08, 1)
        else:
            cmd(s, 0x03, 1)
            cmd(s, 0x04, conf["gain"])
            cmd(s, 0x08, 0)
        cmd(s, 0x01, conf["freq"])


def rx_loop():
    audio = Audio()
    dem = Demod()
    blk = int(conf["rate"]) // 10 * 2      # 0.1 秒分 (I/Q 2バイト/サンプル)
    while True:
        if not conf.get("enabled", True):
            status["connected"] = False
            audio.close()
            time.sleep(0.5)
            continue
        s = None
        try:
            s = socket.create_connection((conf["host"], int(conf["port"])), timeout=5)
            s.settimeout(5)
            hdr = b""
            while len(hdr) < 12:
                d = s.recv(12 - len(hdr))
                if not d:
                    raise IOError("header")
                hdr += d
            tuner = struct.unpack(">I", hdr[4:8])[0]
            status["tuner"] = {1: "E4000", 2: "FC0012", 3: "FC0013", 4: "FC2580", 5: "R820T", 6: "R828D"}.get(tuner, str(tuner))
            apply_tuning(s)
            status.update(connected=True, error="", since=time.time())
            buf = bytearray()
            t0, nbytes = time.time(), 0
            while conf.get("enabled", True):
                if retune.is_set():
                    retune.clear()
                    apply_tuning(s)
                d = s.recv(65536)
                if not d:
                    raise IOError("切断")
                buf += d
                nbytes += len(d)
                if time.time() - t0 >= 2:
                    status["rate_meas"] = int(nbytes / 2 / (time.time() - t0))
                    t0, nbytes = time.time(), 0
                while len(buf) >= blk:
                    raw = np.frombuffer(bytes(buf[:blk]), np.uint8)
                    del buf[:blk]
                    pcm, lvl, op = dem.process(raw, conf["volume"], conf["squelch"])
                    status["level_db"] = round(lvl, 1)
                    status["open"] = op
                    audio.write(pcm)
        except Exception as e:
            status.update(connected=False, error=str(e))
            audio.close()
        finally:
            if s:
                try:
                    s.close()
                except Exception:
                    pass
        time.sleep(3)


PAGE = r"""<!doctype html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>エアバンド受信</title><style>
:root{--bg:#0b0f14;--fg:#e8eef5;--mut:#8a97a6;--acc:#39c;--ok:#3c9;--ng:#e55;--card:#151c24}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px -apple-system,system-ui,sans-serif;padding:14px}
.card{background:var(--card);border-radius:14px;padding:14px;margin-bottom:12px}
.f{font:600 44px ui-monospace,Menlo,monospace;text-align:center;letter-spacing:1px}
.row{display:flex;gap:8px;flex-wrap:wrap}.row>*{flex:1}
button{background:#223040;color:var(--fg);border:0;border-radius:10px;padding:14px 6px;font-size:17px}
button.on{background:var(--acc)}button:active{filter:brightness(1.4)}
input[type=range]{width:100%}label{color:var(--mut);font-size:14px}
.bar{height:14px;background:#223;border-radius:7px;overflow:hidden}.bar i{display:block;height:100%;background:var(--ok)}
.st{color:var(--mut);font-size:13px;text-align:center}.dot{display:inline-block;width:10px;height:10px;border-radius:5px;margin-right:6px}
</style></head><body>
<div class="card"><div class="f" id="f">---.---</div>
<div class="st"><span class="dot" id="dot"></span><span id="st">接続中…</span></div></div>
<div class="card"><div class="row">
<button onclick="step(-25000)">−25k</button><button onclick="step(-8333)">−8.33k</button>
<button onclick="step(8333)">+8.33k</button><button onclick="step(25000)">+25k</button></div>
<div class="row" style="margin-top:8px"><button onclick="step(-1000000)">−1M</button><button onclick="ask()">入力</button><button onclick="step(1000000)">+1M</button></div></div>
<div class="card"><label>プリセット（長押しで今の周波数を登録/削除）</label><div class="row" id="pre" style="margin-top:8px"></div></div>
<div class="card"><label>信号 <span id="lv"></span></label><div class="bar"><i id="bar"></i></div>
<label>スケルチ <span id="sqv"></span> dBFS</label><input type="range" id="sq" min="-100" max="-10" step="1">
<label>音量 <span id="vov"></span></label><input type="range" id="vo" min="0" max="100" step="1">
<label>ゲイン <span id="gav"></span></label><input type="range" id="ga" min="0" max="496" step="4"></div>
<div class="card row"><button id="en" onclick="post({enabled:!S.enabled})">受信 ON/OFF</button></div>
<script>
let S={};const $=i=>document.getElementById(i);
const mhz=f=>(f/1e6).toFixed(3);
async function post(o){const r=await fetch('/api/set',{method:'POST',body:JSON.stringify(o)});S=await r.json();draw(true)}
function step(d){let f=S.freq+d;if(Math.abs(d)<30000&&Math.abs(d)!=25000){}post({freq:Math.round(f)})}
function ask(){const v=prompt('周波数 MHz',mhz(S.freq));if(v){post({freq:Math.round(parseFloat(v)*1e6)})}}
function draw(all){$('f').textContent=mhz(S.freq);
 $('dot').style.background=S.connected?(S.open?'#3c9':'#39c'):'#e55';
 $('st').textContent=S.connected?(`${S.host} ${S.tuner}  ${(S.rate_meas/1000).toFixed(0)} kS/s`+(S.open?'  ● 受信中':'')):(S.enabled?('未接続 '+(S.error||'')):'停止中');
 const p=Math.max(0,Math.min(100,(S.level_db+100)/90*100));$('bar').style.width=p+'%';
 $('bar').style.background=S.open?'#3c9':'#567';$('lv').textContent=S.level_db+' dBFS';
 $('en').className=S.enabled?'on':'';
 if(all){$('sq').value=S.squelch;$('vo').value=S.volume;$('ga').value=S.gain;}
 $('sqv').textContent=S.squelch;$('vov').textContent=S.volume;$('gav').textContent=S.gain==0?'自動':(S.gain/10).toFixed(1)+' dB';
 if(all){const P=$('pre');P.innerHTML='';(S.presets||[]).forEach(f=>{const b=document.createElement('button');
  b.textContent=mhz(f);if(f==S.freq)b.className='on';b.onclick=()=>post({freq:f});
  let t;b.ontouchstart=b.onmousedown=()=>{t=setTimeout(()=>{t=null;post({preset_toggle:f})},700)};
  b.ontouchend=b.onmouseup=()=>{if(t)clearTimeout(t)};P.appendChild(b)});
  const a=document.createElement('button');a.textContent='＋登録';a.onclick=()=>post({preset_toggle:S.freq});P.appendChild(a)}}
$('sq').oninput=e=>{$('sqv').textContent=e.target.value};$('sq').onchange=e=>post({squelch:+e.target.value});
$('vo').oninput=e=>{$('vov').textContent=e.target.value};$('vo').onchange=e=>post({volume:+e.target.value});
$('ga').onchange=e=>post({gain:+e.target.value});
let first=true;async function poll(){try{const r=await fetch('/api/status');S=await r.json();draw(first);first=false}catch(e){$('st').textContent='Pi に接続できません'}setTimeout(poll,500)}
poll();
</script></body></html>"""


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, o):
        b = json.dumps(o, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def state(self):
        d = dict(conf)
        d.update(status)
        return d

    def do_GET(self):
        if self.path.startswith("/api/status"):
            return self._json(self.state())
        b = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        try:
            req = json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            req = {}
        tune = False
        with lock:
            if "freq" in req:
                f = int(req["freq"])
                if 24000000 <= f <= 1766000000:
                    conf["freq"] = f
                    tune = True
            if "gain" in req:
                conf["gain"] = max(0, min(496, int(req["gain"])))
                tune = True
            if "ppm" in req:
                conf["ppm"] = int(req["ppm"])
                tune = True
            if "volume" in req:
                conf["volume"] = max(0, min(100, int(req["volume"])))
            if "squelch" in req:
                conf["squelch"] = float(req["squelch"])
            if "enabled" in req:
                conf["enabled"] = bool(req["enabled"])
            if "preset_toggle" in req:
                f = int(req["preset_toggle"])
                ps = conf.setdefault("presets", [])
                if f in ps:
                    ps.remove(f)
                else:
                    ps.append(f)
                    ps.sort()
            if "host" in req:
                conf["host"] = str(req["host"])
        if tune:
            retune.set()
        save_conf()
        self._json(self.state())


def main():
    threading.Thread(target=rx_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", int(conf.get("http_port", 8091))), H)
    print("airband_rx: http://0.0.0.0:%s/  rtl_tcp=%s:%s" % (conf.get("http_port"), conf["host"], conf["port"]), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
