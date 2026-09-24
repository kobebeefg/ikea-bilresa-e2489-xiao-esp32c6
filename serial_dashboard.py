"""Local-only COM24 live log viewer. Stop with Ctrl+C or terminate its PID."""
import argparse
import datetime as dt
import json
from pathlib import Path
import re
import queue
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM24')
parser.add_argument('--http-port', type=int, default=8766)
parser.add_argument('--reset-on-connect', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parent
logs = root / 'logs'
logs.mkdir(exist_ok=True)
log_path = logs / f"serial-{dt.datetime.now():%Y%m%d-%H%M%S}.log"
status_path = logs / 'latest-status.json'
lock = threading.RLock()
lines = []
commands = queue.Queue(maxsize=1)
status = dict(port=args.port, connected=False, firmware_state='UNKNOWN',
              last_received=None, log_file=str(log_path), details={},
              rescan=dict(state='idle', message='', requested_at=0),
              note='等待序列資料；尚未判定配對結果。')
ansi = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')


def record(message, device=False):
    message = ansi.sub('', message).strip()
    if not message:
        return
    now = dt.datetime.now().astimezone().isoformat(timespec='milliseconds')
    with lock:
        if device:
            status['last_received'] = now
        if 'DIAG: state=' in message:
            fields = dict(re.findall(r'(\w+)=([^\s]+)', message))
            status['details'] = fields
            status['firmware_state'] = fields.get('state', 'UNKNOWN')
        if 'PAIRING READY' in message:
            status['firmware_state'] = 'WAITING_FOR_REMOTE'
        if status['rescan']['state'] == 'waiting':
            if 'JOIN_READY duration_s=' in message:
                status['rescan'].update(state='success', message='入網窗口已開啟（180 秒）。遙控器先按 4 次進入 Touchlink，再按 8 次進入 Zigbee 入網。')
            elif 'JOIN_OPEN_REQUEST duration_s=180 result=' in message and 'result=0x0' not in message:
                status['rescan'].update(state='error', message='C6 回報配對啟動失敗，請查看 log。')
        if 'Set On/Off:' in message:
            status['firmware_state'] = 'CONTROL_RECEIVED'
        if 'Guru Meditation' in message or 'abort()' in message:
            status['firmware_state'] = 'FIRMWARE_ERROR'
        entry = f'[{now}] {message}'
        lines.append(entry)
        del lines[:-1500]
        with log_path.open('a', encoding='utf-8') as output:
            output.write(entry + '\n')
        status_path.write_text(json.dumps(status, ensure_ascii=False, indent=2), encoding='utf-8')


def serial_reader():
    reset_pending = args.reset_on_connect
    while True:
        try:
            connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
            connection.port = args.port
            connection.dtr = False
            connection.rts = False
            connection.open()
            with connection:
                with lock:
                    status['connected'] = True
                record(f'HOST: connected {args.port} 115200 baud')
                if reset_pending:
                    from esptool.reset import HardReset
                    record('HOST: resetting board once to capture startup; NVS retained')
                    HardReset(connection, uses_usb=True).reset()
                    reset_pending = False
                pending = b''
                while True:
                    try:
                        commands.get_nowait()
                    except queue.Empty:
                        pass
                    else:
                        with lock:
                            status['rescan'].update(state='waiting', requested_at=time.time(), message='正在重啟 C6，等待配對窗口確認…')
                            status.update(firmware_state='BOOTING', last_received=None, details={})
                        record('HOST: RESCAN_REQUEST restarting C6 through USB Serial/JTAG; pairing data retained')
                        pending = b''
                        connection.reset_input_buffer()
                        from esptool.reset import HardReset
                        HardReset(connection, uses_usb=True).reset()
                    with lock:
                        expired = status['rescan']['state'] in ('queued', 'waiting') and time.time() - status['rescan']['requested_at'] > 15
                        if expired:
                            status['rescan'].update(state='error', message='15 秒內沒有收到配對窗口確認，請檢查 USB 連線或 log。')
                    if expired:
                        record('HOST: RESCAN_TIMEOUT no pairing-window acknowledgement')
                    chunk = connection.read(connection.in_waiting or 1)
                    if not chunk:
                        continue
                    pending += chunk
                    while b'\n' in pending:
                        raw, pending = pending.split(b'\n', 1)
                        record(raw.decode('utf-8', errors='replace'), device=True)
                    if len(pending) > 8192:
                        record(pending.decode('utf-8', errors='replace'), device=True)
                        pending = b''
        except (serial.SerialException, OSError) as error:
            with lock:
                status['connected'] = False
                if status['rescan']['state'] in ('queued', 'waiting'):
                    status['rescan'].update(state='error', message='序列連線中斷，重新掃描未完成。')
                while not commands.empty():
                    try:
                        commands.get_nowait()
                    except queue.Empty:
                        break
            record(f'HOST: serial error: {error}; retry in 3 seconds')
            time.sleep(3)


PAGE = '''<!doctype html><html lang="zh-Hant"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>XIAO C6 配對紀錄</title>
<style>
body{margin:0;background:#0d1520;color:#e3eaf3;font:15px system-ui,sans-serif;padding:24px}
h1{font-size:24px;margin:0 0 8px}.sub{color:#a4b5cb;line-height:1.7}
.cards{display:flex;gap:12px;flex-wrap:wrap;margin:20px 0}.card{background:#192739;padding:15px 20px;border-radius:10px;min-width:160px}
.label{color:#a4b5cb;font-size:12px;margin-bottom:6px}.value{font-size:18px}
#log{background:#060d16;border:1px solid #2a3e57;padding:16px;white-space:pre-wrap;overflow-wrap:anywhere;height:52vh;overflow:auto;font:12px/1.6 Consolas,monospace}
.toolbar{display:flex;justify-content:space-between;align-items:center;margin:10px 0}button{background:#283e58;color:white;border:0;padding:8px 14px;border-radius:6px;cursor:pointer}
button:disabled{opacity:.5;cursor:wait}#rescan{background:#2563b4;font-size:15px}#rescan-result{margin:10px 0;min-height:24px} .actions{margin-top:16px}
#file{overflow-wrap:anywhere;font:12px Consolas,monospace;margin-top:12px;color:#9ab4d0}
</style><h1>XIAO ESP32-C6 × BILRESA</h1>
<div class="sub">COM24 序列埠即時紀錄 · 每秒更新 · 電腦時間標記<br>模式：C6 協調器＋LED 燈（不需 HA）。首次配對：長按 system 約 10 秒至紅燈停止，放開後快按 4 次，再快按 8 次；等待入網後測正面按鍵。</div>
<div class="actions"><button id="rescan">重新掃描</button><span class="sub">　重啟 C6 並開放 Zigbee 入網 180 秒；保留網路與配對資料。</span></div>
<div id="rescan-result" role="status" aria-live="polite"></div>
<div class="cards"><div class="card"><div class="label">序列連線</div><div id="connection" class="value">讀取中</div></div>
<div class="card"><div class="label">韌體目前狀態</div><div id="state" class="value">未知</div></div>
<div class="card"><div class="label">剩餘配對時間</div><div id="remaining" class="value">—</div></div>
<div class="card"><div class="label">裝置宣告 / 授權成功 / 開關更新</div><div id="counts" class="value">—</div></div></div>
<div class="sub" id="details"></div><div class="toolbar"><span>完整序列輸出（畫面保留最新 1,500 行，檔案持續保存）</span><button id="scroll">自動捲動：開</button></div>
<pre id="log"></pre><div id="file"></div>
<script>
const names={FORMING_NETWORK:'正在建立 Zigbee 網路',FORMATION_FAILED:'建立網路失敗',JOIN_OPEN:'已開放 Zigbee 入網',JOIN_WINDOW_CLOSED:'入網窗口已關閉',DEVICE_ANNOUNCED:'收到裝置宣告，待測按鍵',BOOTING:'啟動中',WAITING_FOR_REMOTE:'等待遙控器',WINDOW_EXPIRED:'配對窗口已過期',PAIRING_REQUEST_RECEIVED:'收到配對請求',NETWORK_CONFIGURED:'已建立網路，待測按鍵',NETWORK_RESTORED:'已還原網路，待測按鍵',CONTROL_RECEIVED:'已收到開關控制',FINISHED_WITHOUT_NETWORK:'流程結束，未建立網路',PAIRING_FAILED:'配對失敗',START_FAILED:'配對啟動失敗',LEFT_NETWORK:'已離開網路',FIRMWARE_ERROR:'韌體錯誤',UNKNOWN:'尚無狀態'};
let auto=true;document.getElementById('scroll').onclick=()=>{auto=!auto;document.getElementById('scroll').textContent='自動捲動：'+(auto?'開':'關')};
let submitting=false;
document.getElementById('rescan').onclick=async()=>{
submitting=true;document.getElementById('rescan').disabled=true;
document.getElementById('rescan-result').textContent='正在提交重新掃描…';
try{const r=await fetch('/api/rescan',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});const d=await r.json();document.getElementById('rescan-result').textContent=d.message;}
catch(e){document.getElementById('rescan-result').textContent='無法連線至紀錄程式，請確認程式仍在執行。';}
finally{submitting=false;document.getElementById('rescan').disabled=false;}};
async function update(){try{const r=await fetch('/api',{cache:'no-store'});const d=await r.json();const s=d.status,x=s.details;
const busy=['queued','waiting'].includes(s.rescan.state);
document.getElementById('rescan').disabled=submitting||busy||!s.connected;
document.getElementById('rescan').textContent=busy?'重新掃描中…':'重新掃描';
if(!submitting&&s.rescan.message)document.getElementById('rescan-result').textContent=s.rescan.message;
const age=s.last_received?(Date.now()-Date.parse(s.last_received))/1000:Infinity;
const fresh=s.connected&&age<15;
document.getElementById('connection').textContent=!s.connected?'未連線／重試中':fresh?'收到即時資料 '+s.port:'埠已開啟，但沒有新資料';
document.getElementById('state').textContent=fresh?(names[s.firmware_state]||s.firmware_state):'目前狀態未知（資料過期）';
document.getElementById('remaining').textContent=x.remaining_s===undefined?'—':x.remaining_s+' 秒';
document.getElementById('counts').textContent=[x.joined||0,x.authorized||0,x.onoff||0].join(' / ');
document.getElementById('details').textContent='最後收到資料：'+(s.last_received||'尚無')+' · 頻道：'+(x.channel||'—')+' · factory_new：'+(x.factory_new??'—')+' · PAN：'+(x.pan||'—')+' · 群組 0x549A：'+(x.group_ready==='1'?'已確認':'尚未確認')+'（裝置宣告不代表按鍵已驗證）';
const l=document.getElementById('log');l.textContent=d.lines.join('\\n');if(auto)l.scrollTop=l.scrollHeight;
document.getElementById('file').textContent='Log：'+s.log_file;
}catch(e){document.getElementById('connection').textContent='紀錄程式未回應'}setTimeout(update,1000)}update();
</script></html>'''


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != '/api/rescan':
            self.send_error(404)
            return
        allowed = {f'127.0.0.1:{args.http_port}', f'localhost:{args.http_port}'}
        if (self.headers.get('Host') not in allowed or
                self.headers.get('Origin') not in (None, *(f'http://{host}' for host in allowed)) or
                self.headers.get('Content-Type') != 'application/json'):
            self.send_error(403)
            return
        with lock:
            if not status['connected']:
                code, message = 503, 'COM24 未連線，無法重新掃描。'
            elif status['rescan']['state'] in ('queued', 'waiting'):
                code, message = 409, '重新掃描已在執行，請等待結果。'
            else:
                status['rescan'].update(state='queued', requested_at=time.time(), message='請求已送出，等待 C6 重新開啟配對窗口…')
                commands.put_nowait('rescan')
                code, message = 202, status['rescan']['message']
                record('HOST: webpage queued RESCAN_REQUEST')
        content = json.dumps(dict(message=message), ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def do_GET(self):
        if self.path == '/':
            content = PAGE.encode('utf-8')
            kind = 'text/html; charset=utf-8'
        elif self.path == '/api':
            with lock:
                content = json.dumps(dict(status=status, lines=lines), ensure_ascii=False).encode('utf-8')
            kind = 'application/json; charset=utf-8'
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def log_message(self, *args):
        pass


if __name__ == '__main__':
    server = ThreadingHTTPServer(('127.0.0.1', args.http_port), Handler)
    record(f'HOST: viewer http://127.0.0.1:{args.http_port}; full log {log_path}')
    threading.Thread(target=serial_reader, daemon=True).start()
    server.serve_forever()
