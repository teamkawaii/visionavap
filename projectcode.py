import http.server
import socketserver
import webbrowser
import threading
import json
import os
import mimetypes
import re
import socket
import subprocess
import platform
import time

PORT = 8080
CONFIG_FILE = "videos.txt"
LAYOUT_FILE = "layout.json"

DEFAULT_CONFIG = """# Visual Odometry Stream Sources
stream1.mp4
stream2.mp4
stream3.mp4
stream4.mp4
stream5.mp4
stream6.mp4
stream7.mp4
stream8.mp4
"""

DEFAULT_LAYOUT = {
    "left_width": 50,
    "left_top_height": 46,
    "right_top_height": 35,
    "right_mid_height": 35
}

def free_port(port):
    """Forcefully clears the port across any OS using standard commands."""
    system = platform.system()
    try:
        if system == "Windows":
            output = subprocess.check_output(f"netstat -ano | findstr :{port}", shell=True).decode()
            for line in output.strip().split('\n'):
                if 'LISTENING' in line:
                    pid = line.strip().split()[-1]
                    subprocess.call(f"taskkill /F /PID {pid}", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            # Linux, macOS, Termux
            subprocess.call(f"fuser -k {port}/tcp", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass
    time.sleep(0.5)

def load_video_config():
    if not os.path.exists(CONFIG_FILE):
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            f.write(DEFAULT_CONFIG)
    
    paths = []
    with open(CONFIG_FILE, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                paths.append(line)
    
    while len(paths) < 8:
        paths.append("")
    return paths

def load_layout():
    if not os.path.exists(LAYOUT_FILE):
        return DEFAULT_LAYOUT
    try:
        with open(LAYOUT_FILE, "r") as f:
            return json.load(f)
    except Exception:
        return DEFAULT_LAYOUT

HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Visual Odometry</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@300;400;600;700&display=swap');

  * { box-sizing: border-box; margin: 0; padding: 0; }
  
  :root {
    --bg: #0a0b0e;
    --window-bg: #111318;
    --border: #232731;
    --border-active: #474e5d;
    --text-primary: #e1e4ea;
    --text-dim: #5c6370;
    --accent: #00e5ff;
    --gap: 8px;
    --radius: 8px;

    /* Layout Variables assigned from layout.json */
    --left-width: {{LAYOUT_LEFT_WIDTH}}%;
    --left-top-height: {{LAYOUT_LEFT_TOP_HEIGHT}}%;
    --right-top-height: {{LAYOUT_RIGHT_TOP_HEIGHT}}%;
    --right-mid-height: {{LAYOUT_RIGHT_MID_HEIGHT}}%;
  }

  body {
    background: var(--bg);
    color: var(--text-primary);
    font-family: 'JetBrains Mono', monospace;
    height: 100vh;
    width: 100vw;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    user-select: none;
  }

  #waybar {
    height: 38px;
    background: #0d0f14;
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 16px;
    font-size: 12px;
    letter-spacing: 0.5px;
    z-index: 100;
  }

  .brand-title {
    font-weight: 700;
    font-size: 13px;
    letter-spacing: 4px;
    text-transform: uppercase;
    color: var(--text-primary);
  }

  #viewport {
    flex: 1;
    display: grid;
    grid-template-columns: var(--left-width) calc(100% - var(--left-width) - var(--gap));
    gap: var(--gap);
    padding: var(--gap);
    height: calc(100vh - 38px);
  }

  .col-left {
    display: grid;
    grid-template-rows: var(--left-top-height) calc(100% - var(--left-top-height) - var(--gap));
    gap: var(--gap);
  }
  .col-left-top {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: var(--gap);
  }

  .col-right {
    display: grid;
    grid-template-rows: var(--right-top-height) var(--right-mid-height) calc(100% - var(--right-top-height) - var(--right-mid-height) - (var(--gap) * 2));
    gap: var(--gap);
  }
  .col-right-top, .col-right-mid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: var(--gap);
  }

  .hypr-tile {
    background: var(--window-bg);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    position: relative;
    overflow: hidden;
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .tile-tag {
    position: absolute;
    top: 8px;
    left: 8px;
    background: rgba(13, 15, 20, 0.7);
    border: 1px solid var(--border);
    backdrop-filter: blur(4px);
    color: #e1e4ea;
    padding: 2px 7px;
    font-size: 11px;
    border-radius: 4px;
    z-index: 10;
    pointer-events: none;
    letter-spacing: 1px;
    font-weight: 600;
    /* Subtle Neon Glow */
    text-shadow: 0 0 6px rgba(0, 229, 255, 0.6), 0 0 12px rgba(0, 229, 255, 0.3);
    box-shadow: 0 0 10px rgba(0, 0, 0, 0.5);
  }

  video {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
    background: #000;
  }

  /* Neon Loading Overlay */
  #loading-overlay {
    position: fixed;
    inset: 0;
    background: rgba(10, 11, 14, 0.95);
    z-index: 9999;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    transition: opacity 0.4s ease, visibility 0.4s ease;
  }
  .neon-spinner {
    width: 60px;
    height: 60px;
    border: 4px solid rgba(0, 229, 255, 0.1);
    border-top: 4px solid var(--accent);
    border-radius: 50%;
    animation: spin 1s linear infinite;
    box-shadow: 0 0 20px rgba(0, 229, 255, 0.4);
    margin-bottom: 20px;
  }
  .loading-text {
    color: var(--accent);
    letter-spacing: 4px;
    font-size: 14px;
    text-shadow: 0 0 8px rgba(0, 229, 255, 0.8);
    animation: pulse 1.5s infinite;
  }
  
  @keyframes spin { 100% { transform: rotate(360deg); } }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }

  #start-modal {
    position: fixed;
    inset: 0;
    background: rgba(10, 11, 14, 0.85);
    backdrop-filter: blur(10px);
    z-index: 9998;
    display: none;
    align-items: center;
    justify-content: center;
  }
  #start-btn {
    background: #1c212a;
    color: var(--accent);
    border: 1px solid var(--accent);
    font-family: inherit;
    font-size: 14px;
    font-weight: 700;
    letter-spacing: 3px;
    padding: 14px 36px;
    border-radius: 6px;
    cursor: pointer;
    box-shadow: 0 0 15px rgba(0, 229, 255, 0.2);
    transition: all 0.2s ease;
  }
  #start-btn:hover {
    background: var(--accent);
    color: #000;
    box-shadow: 0 0 25px rgba(0, 229, 255, 0.6);
  }

  /* Admin Panel */
  #admin-panel {
    position: fixed;
    bottom: 20px;
    right: 20px;
    background: rgba(17, 19, 24, 0.95);
    border: 1px solid var(--accent);
    padding: 16px;
    border-radius: 8px;
    z-index: 10000;
    display: {{ADMIN_DISPLAY}};
    flex-direction: column;
    gap: 12px;
    box-shadow: 0 0 20px rgba(0,0,0,0.8);
  }
  .admin-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 12px;
    font-size: 11px;
  }
  input[type=range] { width: 120px; accent-color: var(--accent); }
  .admin-btn {
    background: var(--accent);
    color: #000;
    border: none;
    padding: 6px 12px;
    font-weight: bold;
    cursor: pointer;
    border-radius: 4px;
    margin-top: 8px;
  }
</style>
</head>
<body>

  <!-- Loading Sync -->
  <div id="loading-overlay">
    <div class="neon-spinner"></div>
    <div class="loading-text">BUFFERING STREAMS (<span id="load-count">0</span>/8)</div>
  </div>

  <!-- Start Overlay (Hidden until loaded) -->
  <div id="start-modal">
    <button id="start-btn" onclick="startStreams()">INITIALIZE PIPELINE</button>
  </div>

  <!-- Admin Controls (Only visible on /admin) -->
  <div id="admin-panel">
    <div style="color: var(--accent); font-weight: bold; margin-bottom: 4px;">LAYOUT EDITOR</div>
    <div class="admin-row">
      <label>Main Split Width</label>
      <input type="range" id="slider-w" min="20" max="80" value="{{LAYOUT_LEFT_WIDTH}}" oninput="updateLayout()">
    </div>
    <div class="admin-row">
      <label>Left Top Height</label>
      <input type="range" id="slider-lt" min="20" max="80" value="{{LAYOUT_LEFT_TOP_HEIGHT}}" oninput="updateLayout()">
    </div>
    <div class="admin-row">
      <label>Right Top Height</label>
      <input type="range" id="slider-rt" min="20" max="60" value="{{LAYOUT_RIGHT_TOP_HEIGHT}}" oninput="updateLayout()">
    </div>
    <div class="admin-row">
      <label>Right Mid Height</label>
      <input type="range" id="slider-rm" min="20" max="60" value="{{LAYOUT_RIGHT_MID_HEIGHT}}" oninput="updateLayout()">
    </div>
    <button class="admin-btn" onclick="saveLayout()">SAVE CONFIG</button>
  </div>

  <div id="waybar">
    <div class="brand-title">Visual Odometry</div>
    <div style="color: var(--text-dim);">SYNC: LOCKED | RATE: OPTIMAL</div>
  </div>

  <div id="viewport">
    <div class="col-left">
      <div class="col-left-top">
        <div class="hypr-tile"><span class="tile-tag">Car driving</span><video id="v0" loop muted playsinline preload="auto"></video></div>
        <div class="hypr-tile"><span class="tile-tag">Thermal camera</span><video id="v1" loop muted playsinline preload="auto"></video></div>
      </div>
      <div class="hypr-tile"><span class="tile-tag">3D surroundings</span><video id="v2" loop muted playsinline preload="auto"></video></div>
    </div>
    <div class="col-right">
      <div class="col-right-top">
        <div class="hypr-tile"><span class="tile-tag">frame t</span><video id="v3" loop muted playsinline preload="auto"></video></div>
        <div class="hypr-tile"><span class="tile-tag">frame t+k</span><video id="v4" loop muted playsinline preload="auto"></video></div>
      </div>
      <div class="col-right-mid">
        <div class="hypr-tile"><span class="tile-tag">Graph 1</span><video id="v5" loop muted playsinline preload="auto"></video></div>
        <div class="hypr-tile"><span class="tile-tag">Graph 2</span><video id="v6" loop muted playsinline preload="auto"></video></div>
      </div>
      <div class="hypr-tile"><span class="tile-tag">Road tracking</span><video id="v7" loop muted playsinline preload="auto"></video></div>
    </div>
  </div>

<script>
  const videoSources = {{VIDEO_SOURCES_JSON}};
  const videoElements = [];
  let loadedCount = 0;
  let expectedVideos = 0;

  for (let i = 0; i < 8; i++) {
    const el = document.getElementById('v' + i);
    videoElements.push(el);
    if (videoSources[i] && videoSources[i].trim() !== '') {
      expectedVideos++;
      el.src = videoSources[i].trim();
      
      // Ensure we wait for the browser to buffer enough to play smoothly
      el.addEventListener('canplaythrough', () => {
        if (!el.dataset.loaded) {
          el.dataset.loaded = "true";
          loadedCount++;
          document.getElementById('load-count').innerText = loadedCount;
          checkAllLoaded();
        }
      });
      
      // Fallback for cached videos that don't re-fire events
      if (el.readyState >= 3) {
          el.dispatchEvent(new Event('canplaythrough'));
      }
      
      el.load();
    } else {
      el.parentElement.style.borderStyle = 'dashed';
      el.parentElement.style.borderColor = '#333';
    }
  }

  if (expectedVideos === 0) checkAllLoaded(); // Skip if no videos

  function checkAllLoaded() {
    if (loadedCount >= expectedVideos) {
      document.getElementById('loading-overlay').style.opacity = '0';
      setTimeout(() => {
        document.getElementById('loading-overlay').style.display = 'none';
        document.getElementById('start-modal').style.display = 'flex';
      }, 400);
    }
  }

  function startStreams() {
    document.getElementById('start-modal').style.display = 'none';
    videoElements.forEach(v => {
      if (v.src) {
        v.currentTime = 0;
        v.play();
      }
    });
  }

  // Admin live layout adjustments
  function updateLayout() {
    document.documentElement.style.setProperty('--left-width', document.getElementById('slider-w').value + '%');
    document.documentElement.style.setProperty('--left-top-height', document.getElementById('slider-lt').value + '%');
    document.documentElement.style.setProperty('--right-top-height', document.getElementById('slider-rt').value + '%');
    document.documentElement.style.setProperty('--right-mid-height', document.getElementById('slider-rm').value + '%');
  }

  function saveLayout() {
    const layoutData = {
      left_width: document.getElementById('slider-w').value,
      left_top_height: document.getElementById('slider-lt').value,
      right_top_height: document.getElementById('slider-rt').value,
      right_mid_height: document.getElementById('slider-rm').value
    };
    
    fetch('/save_layout', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(layoutData)
    }).then(() => {
      const btn = document.querySelector('.admin-btn');
      btn.innerText = "SAVED!";
      setTimeout(() => btn.innerText = "SAVE CONFIG", 2000);
    });
  }
</script>
</body>
</html>
"""

class FastVideoHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Disable console spam for maximum streaming performance

    def do_GET(self):
        if self.path in ("/", "/index.html", "/admin"):
            sources = load_video_config()
            layout = load_layout()
            
            content = HTML_TEMPLATE.replace("{{VIDEO_SOURCES_JSON}}", json.dumps(sources))
            content = content.replace("{{ADMIN_DISPLAY}}", "flex" if self.path == "/admin" else "none")
            content = content.replace("{{LAYOUT_LEFT_WIDTH}}", str(layout['left_width']))
            content = content.replace("{{LAYOUT_LEFT_TOP_HEIGHT}}", str(layout['left_top_height']))
            content = content.replace("{{LAYOUT_RIGHT_TOP_HEIGHT}}", str(layout['right_top_height']))
            content = content.replace("{{LAYOUT_RIGHT_MID_HEIGHT}}", str(layout['right_mid_height']))
            
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(content.encode("utf-8"))
            return

        path = self.translate_path(self.path)
        if not os.path.exists(path) or os.path.isdir(path):
            self.send_error(404)
            return

        ctype, _ = mimetypes.guess_type(path)
        ctype = ctype or "application/octet-stream"
        file_size = os.path.getsize(path)
        range_header = self.headers.get("Range")

        try:
            with open(path, "rb") as f:
                if range_header:
                    range_match = re.match(r"bytes=(\d+)-(\d*)", range_header)
                    if range_match:
                        start = int(range_match.group(1))
                        end = int(range_match.group(2)) if range_match.group(2) else file_size - 1
                        end = min(end, file_size - 1)
                        length = end - start + 1

                        self.send_response(206)
                        self.send_header("Content-Type", ctype)
                        self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
                        self.send_header("Content-Length", str(length))
                        self.send_header("Accept-Ranges", "bytes")
                        self.end_headers()

                        f.seek(start)
                        self.wfile.write(f.read(length))
                        return

                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(file_size))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                
                # Fast chunking
                while chunk := f.read(65536):
                    self.wfile.write(chunk)

        except (ConnectionResetError, BrokenPipeError):
            pass

    def do_POST(self):
        if self.path == "/save_layout":
            content_length = int(self.headers['Content-Length'])
            post_data = self.rfile.read(content_length)
            
            with open(LAYOUT_FILE, "w") as f:
                f.write(post_data.decode('utf-8'))
                
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"OK")

class ThreadingReusableServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

def run_server():
    free_port(PORT)
    try:
        with ThreadingReusableServer(("", PORT), FastVideoHandler) as httpd:
            print(f"[*] Visual Odometry System running at http://localhost:{PORT}")
            print(f"[*] Admin Panel: http://localhost:{PORT}/admin")
            httpd.serve_forever()
    except Exception as e:
        print(f"[!] Failed to bind port {PORT}. Error: {e}")

if __name__ == "__main__":
    load_video_config()
    load_layout()
    
    t = threading.Thread(target=run_server, daemon=True)
    t.start()
    
    time.sleep(1) # Give server time to bind
    url = f"http://localhost:{PORT}"
    try:
        webbrowser.open(url)
    except Exception:
        pass

    print("[*] Press Ctrl+C in terminal to stop.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nExiting...")
