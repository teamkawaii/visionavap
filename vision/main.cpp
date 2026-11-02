#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <atomic>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <shellapi.h>
  #pragma comment(lib, "ws2_32.lib")
  using socket_t = SOCKET;
  #define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
  #define CLOSE_SOCKET(s) closesocket(s)
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  using socket_t = int;
  #define IS_INVALID_SOCKET(s) ((s) < 0)
  #define CLOSE_SOCKET(s) close(s)
#endif





constexpr int PORT = 8080;
constexpr size_t CHUNK_SIZE = 131072; // 128 KB I/O buffer for high throughput
const std::string CONFIG_FILE = "videos.txt";
const std::string LAYOUT_FILE = "layout.json";

const std::string DEFAULT_CONFIG = 
    "# Visual Odometry Stream Sources\n"
    "stream1.mp4\n"
    "stream2.mp4\n"
    "stream3.mp4\n"
    "stream4.mp4\n"
    "stream5.mp4\n"
    "stream6.mp4\n"
    "stream7.mp4\n"
    "stream8.mp4\n";


struct LayoutConfig {
    int left_width = 50;
    int left_top_height = 46;
    int right_top_height = 35;
    int right_mid_height = 35;
};
// Global cache for pre-rendered pages to avoid per-request processing
std::mutex g_cache_mtx;
std::string g_index_html;
std::string g_admin_html;

const std::string HTML_TEMPLATE = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Visual Odometry</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@300;400;600;700&display=swap');
  * { box-sizing: border-box; margin: 0; padding: 0; }
  :root {
    --bg: #0a0b0e; --window-bg: #111318; --border: #232731; --border-active: #474e5d;
    --text-primary: #e1e4ea; --text-dim: #5c6370; --accent: #00e5ff; --gap: 8px; --radius: 8px;
    --left-width: {{LAYOUT_LEFT_WIDTH}}%; --left-top-height: {{LAYOUT_LEFT_TOP_HEIGHT}}%;
    --right-top-height: {{LAYOUT_RIGHT_TOP_HEIGHT}}%; --right-mid-height: {{LAYOUT_RIGHT_MID_HEIGHT}}%;
  }
  body {
    background: var(--bg); color: var(--text-primary); font-family: 'JetBrains Mono', monospace;
    height: 100vh; width: 100vw; overflow: hidden; display: flex; flex-direction: column; user-select: none;
  }
  #waybar {
    height: 38px; background: #0d0f14; border-bottom: 1px solid var(--border);
    display: flex; align-items: center; justify-content: space-between; padding: 0 16px;
    font-size: 12px; letter-spacing: 0.5px; z-index: 100;
  }
  .brand-title { font-weight: 700; font-size: 13px; letter-spacing: 4px; text-transform: uppercase; color: var(--text-primary); }
  #viewport {
    flex: 1; display: grid; grid-template-columns: var(--left-width) calc(100% - var(--left-width) - var(--gap));
    gap: var(--gap); padding: var(--gap); height: calc(100vh - 38px);
  }
  .col-left { display: grid; grid-template-rows: var(--left-top-height) calc(100% - var(--left-top-height) - var(--gap)); gap: var(--gap); }
  .col-left-top { display: grid; grid-template-columns: 1fr 1fr; gap: var(--gap); }
  .col-right {
    display: grid; grid-template-rows: var(--right-top-height) var(--right-mid-height) calc(100% - var(--right-top-height) - var(--right-mid-height) - (var(--gap) * 2));
    gap: var(--gap);
  }
  .col-right-top, .col-right-mid { display: grid; grid-template-columns: 1fr 1fr; gap: var(--gap); }
  .hypr-tile {
    background: var(--window-bg); border: 1px solid var(--border); border-radius: var(--radius);
    position: relative; overflow: hidden; display: flex; align-items: center; justify-content: center;
  }
  .tile-tag {
    position: absolute; top: 8px; left: 8px; background: rgba(13, 15, 20, 0.7); border: 1px solid var(--border);
    backdrop-filter: blur(4px); color: #e1e4ea; padding: 2px 7px; font-size: 11px; border-radius: 4px; z-index: 10;
    pointer-events: none; letter-spacing: 1px; font-weight: 600;
    text-shadow: 0 0 6px rgba(0, 229, 255, 0.6), 0 0 12px rgba(0, 229, 255, 0.3); box-shadow: 0 0 10px rgba(0, 0, 0, 0.5);
  }
  video { width: 100%; height: 100%; object-fit: cover; display: block; background: #000; }
  #loading-overlay {
    position: fixed; inset: 0; background: rgba(10, 11, 14, 0.95); z-index: 9999;
    display: flex; flex-direction: column; align-items: center; justify-content: center;
    transition: opacity 0.4s ease, visibility 0.4s ease;
  }
  .neon-spinner {
    width: 60px; height: 60px; border: 4px solid rgba(0, 229, 255, 0.1); border-top: 4px solid var(--accent);
    border-radius: 50%; animation: spin 1s linear infinite; box-shadow: 0 0 20px rgba(0, 229, 255, 0.4); margin-bottom: 20px;
  }
  .loading-text { color: var(--accent); letter-spacing: 4px; font-size: 14px; text-shadow: 0 0 8px rgba(0, 229, 255, 0.8); animation: pulse 1.5s infinite; }
  @keyframes spin { 100% { transform: rotate(360deg); } }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
  #start-modal {
    position: fixed; inset: 0; background: rgba(10, 11, 14, 0.85); backdrop-filter: blur(10px);
    z-index: 9998; display: none; align-items: center; justify-content: center;
  }
  #start-btn {
    background: #1c212a; color: var(--accent); border: 1px solid var(--accent); font-family: inherit;
    font-size: 14px; font-weight: 700; letter-spacing: 3px; padding: 14px 36px; border-radius: 6px;
    cursor: pointer; box-shadow: 0 0 15px rgba(0, 229, 255, 0.2); transition: all 0.2s ease;
  }
  #start-btn:hover { background: var(--accent); color: #000; box-shadow: 0 0 25px rgba(0, 229, 255, 0.6); }
  #admin-panel {
    position: fixed; bottom: 20px; right: 20px; background: rgba(17, 19, 24, 0.95); border: 1px solid var(--accent);
    padding: 16px; border-radius: 8px; z-index: 10000; display: {{ADMIN_DISPLAY}}; flex-direction: column; gap: 12px;
    box-shadow: 0 0 20px rgba(0,0,0,0.8);
  }
  .admin-row { display: flex; justify-content: space-between; align-items: center; gap: 12px; font-size: 11px; }
  input[type=range] { width: 120px; accent-color: var(--accent); }
  .admin-btn { background: var(--accent); color: #000; border: none; padding: 6px 12px; font-weight: bold; cursor: pointer; border-radius: 4px; margin-top: 8px; }
</style>
</head>
<body>
  <div id="loading-overlay">
    <div class="neon-spinner"></div>
    <div class="loading-text">BUFFERING STREAMS (<span id="load-count">0</span>/8)</div>
  </div>
  <div id="start-modal">
    <button id="start-btn" onclick="startStreams()">INITIALIZE PIPELINE</button>
  </div>
  <div id="admin-panel">
    <div style="color: var(--accent); font-weight: bold; margin-bottom: 4px;">LAYOUT EDITOR</div>
    <div class="admin-row"><label>Main Split Width</label><input type="range" id="slider-w" min="20" max="80" value="{{LAYOUT_LEFT_WIDTH}}" oninput="updateLayout()"></div>
    <div class="admin-row"><label>Left Top Height</label><input type="range" id="slider-lt" min="20" max="80" value="{{LAYOUT_LEFT_TOP_HEIGHT}}" oninput="updateLayout()"></div>
    <div class="admin-row"><label>Right Top Height</label><input type="range" id="slider-rt" min="20" max="60" value="{{LAYOUT_RIGHT_TOP_HEIGHT}}" oninput="updateLayout()"></div>
    <div class="admin-row"><label>Right Mid Height</label><input type="range" id="slider-rm" min="20" max="60" value="{{LAYOUT_RIGHT_MID_HEIGHT}}" oninput="updateLayout()"></div>
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
  let loadedCount = 0, expectedVideos = 0;
  for (let i = 0; i < 8; i++) {
    const el = document.getElementById('v' + i);
    videoElements.push(el);
    if (videoSources[i] && videoSources[i].trim() !== '') {
      expectedVideos++;
      el.src = videoSources[i].trim();
      el.addEventListener('canplaythrough', () => {
        if (!el.dataset.loaded) {
          el.dataset.loaded = "true";
          loadedCount++;
          document.getElementById('load-count').innerText = loadedCount;
          checkAllLoaded();
        }
      });
      if (el.readyState >= 3) el.dispatchEvent(new Event('canplaythrough'));
      el.load();
    } else {
      el.parentElement.style.borderStyle = 'dashed';
      el.parentElement.style.borderColor = '#333';
    }
  }
  if (expectedVideos === 0) checkAllLoaded();
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
    videoElements.forEach(v => { if (v.src) { v.currentTime = 0; v.play(); } });
  }
  function updateLayout() {
    document.documentElement.style.setProperty('--left-width', document.getElementById('slider-w').value + '%');
    document.documentElement.style.setProperty('--left-top-height', document.getElementById('slider-lt').value + '%');
    document.documentElement.style.setProperty('--right-top-height', document.getElementById('slider-rt').value + '%');
    document.documentElement.style.setProperty('--right-mid-height', document.getElementById('slider-rm').value + '%');
  }
  function saveLayout() {
    const layoutData = {
      left_width: parseInt(document.getElementById('slider-w').value),
      left_top_height: parseInt(document.getElementById('slider-lt').value),
      right_top_height: parseInt(document.getElementById('slider-rt').value),
      right_mid_height: parseInt(document.getElementById('slider-rm').value)
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
)rawhtml";

// High-performance Thread Pool
class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<socket_t> tasks;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};

public:
    ThreadPool(size_t threads) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this]() {
                while (true) {
                    socket_t sock;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->cv.wait(lock, [this]() { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) return;
                        sock = this->tasks.front();
                        this->tasks.pop();
                    }
                    handle_client(sock);
                }
            });
        }
    }

    void enqueue(socket_t sock) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.push(sock);
        }
        cv.notify_one();
    }

    ~ThreadPool() {
        stop = true;
        cv.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

    static void handle_client(socket_t client_sock);
};

void replace_all(std::string& target, std::string_view from, std::string_view to) {
    size_t pos = 0;
    while ((pos = target.find(from, pos)) != std::string::npos) {
        target.replace(pos, from.length(), to);
        pos += to.length();
    }
}

std::string_view guess_mime_type(std::string_view path) {
    size_t idx = path.rfind('.');
    if (idx != std::string_view::npos) {
        std::string_view ext = path.substr(idx);
        if (ext == ".mp4") return "video/mp4";
        if (ext == ".webm") return "video/webm";
        if (ext == ".ogg" || ext == ".ogv") return "video/ogg";
        if (ext == ".html" || ext == ".htm") return "text/html";
        if (ext == ".css") return "text/css";
        if (ext == ".js") return "application/javascript";
        if (ext == ".json") return "application/json";
    }
    return "application/octet-stream";
}

LayoutConfig load_layout() {
    LayoutConfig cfg;
    std::ifstream in(LAYOUT_FILE);
    if (!in.is_open()) return cfg;

    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parse_val = [&](const std::string& key, int& out) {
        size_t pos = s.find(key);
        if (pos != std::string::npos) {
            pos = s.find(':', pos);
            if (pos != std::string::npos) {
                while (pos < s.size() && !isdigit(s[pos])) pos++;
                if (pos < s.size()) out = std::stoi(&s[pos]);
            }
        }
    };
    parse_val("left_width", cfg.left_width);
    parse_val("left_top_height", cfg.left_top_height);
    parse_val("right_top_height", cfg.right_top_height);
    parse_val("right_mid_height", cfg.right_mid_height);
    return cfg;
}

std::vector<std::string> load_video_config() {
    std::vector<std::string> paths;
    std::ifstream in(CONFIG_FILE);
    if (!in.is_open()) {
        std::ofstream out(CONFIG_FILE);
        out << DEFAULT_CONFIG;
        out.close();
        in.open(CONFIG_FILE);
    }
    std::string line;
    while (std::getline(in, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(first, (last - first + 1));
        if (!trimmed.empty() && trimmed[0] != '#') paths.push_back(trimmed);
    }
    while (paths.size() < 8) paths.push_back("");
    return paths;
}

void rebuild_html_cache() {
    auto sources = load_video_config();
    auto layout = load_layout();

    std::string sources_json = "[";
    for (size_t i = 0; i < sources.size(); ++i) {
        sources_json += "\"" + sources[i] + "\"";
        if (i + 1 < sources.size()) sources_json += ", ";
    }
    sources_json += "]";

    std::string base = HTML_TEMPLATE;
    replace_all(base, "{{VIDEO_SOURCES_JSON}}", sources_json);
    replace_all(base, "{{LAYOUT_LEFT_WIDTH}}", std::to_string(layout.left_width));
    replace_all(base, "{{LAYOUT_LEFT_TOP_HEIGHT}}", std::to_string(layout.left_top_height));
    replace_all(base, "{{LAYOUT_RIGHT_TOP_HEIGHT}}", std::to_string(layout.right_top_height));
    replace_all(base, "{{LAYOUT_RIGHT_MID_HEIGHT}}", std::to_string(layout.right_mid_height));

    std::string index = base;
    replace_all(index, "{{ADMIN_DISPLAY}}", "none");

    std::string admin = base;
    replace_all(admin, "{{ADMIN_DISPLAY}}", "flex");

    std::ostringstream ss_idx, ss_adm;
    ss_idx << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
           << index.size() << "\r\nConnection: close\r\n\r\n" << index;
    ss_adm << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
           << admin.size() << "\r\nConnection: close\r\n\r\n" << admin;

    std::lock_guard<std::mutex> lock(g_cache_mtx);
    g_index_html = ss_idx.str();
    g_admin_html = ss_adm.str();
}

inline bool send_all(socket_t sock, const char* buffer, size_t length) {
    size_t total = 0;
    while (total < length) {
        int bytes = send(sock, buffer + total, static_cast<int>(length - total), 0);
        if (bytes <= 0) return false;
        total += bytes;
    }
    return true;
}

// Fast string tokenizer without regex overhead
bool parse_byte_range(std::string_view range_str, size_t file_size, size_t& start, size_t& end) {
    size_t eq = range_str.find('=');
    if (eq == std::string_view::npos) return false;
    std::string_view specs = range_str.substr(eq + 1);
    size_t dash = specs.find('-');
    if (dash == std::string_view::npos) return false;

    try {
        start = std::stoull(std::string(specs.substr(0, dash)));
        std::string_view end_sv = specs.substr(dash + 1);
        if (end_sv.empty()) {
            end = file_size - 1;
        } else {
            end = std::min((size_t)std::stoull(std::string(end_sv)), file_size - 1);
        }
        return start <= end;
    } catch (...) {
        return false;
    }
}

void ThreadPool::handle_client(socket_t client_sock) {
    // Enable TCP_NODELAY to immediately dispatch stream chunks without Nagle delay
    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

    char req_buf[4096];
    int bytes_read = recv(client_sock, req_buf, sizeof(req_buf) - 1, 0);
    if (bytes_read <= 0) {
        CLOSE_SOCKET(client_sock);
        return;
    }
    req_buf[bytes_read] = '\0';
    std::string_view req(req_buf, bytes_read);

    size_t line_end = req.find("\r\n");
    if (line_end == std::string_view::npos) {
        CLOSE_SOCKET(client_sock);
        return;
    }
    std::string_view first_line = req.substr(0, line_end);

    size_t s1 = first_line.find(' ');
    size_t s2 = first_line.rfind(' ');
    if (s1 == std::string_view::npos || s2 == std::string_view::npos || s1 == s2) {
        CLOSE_SOCKET(client_sock);
        return;
    }
    std::string_view method = first_line.substr(0, s1);
    std::string_view path = first_line.substr(s1 + 1, s2 - s1 - 1);

    if (method == "GET") {
        if (path == "/" || path == "/index.html") {
            std::lock_guard<std::mutex> lock(g_cache_mtx);
            send_all(client_sock, g_index_html.data(), g_index_html.size());
            CLOSE_SOCKET(client_sock);
            return;
        }
        if (path == "/admin") {
            std::lock_guard<std::mutex> lock(g_cache_mtx);
            send_all(client_sock, g_admin_html.data(), g_admin_html.size());
            CLOSE_SOCKET(client_sock);
            return;
        }

        std::string rel_path(path.substr(1));
        size_t query = rel_path.find('?');
        if (query != std::string::npos) rel_path = rel_path.substr(0, query);

        std::ifstream file(rel_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            static const char not_found[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client_sock, not_found, sizeof(not_found) - 1);
            CLOSE_SOCKET(client_sock);
            return;
        }

        size_t file_size = file.tellg();
        std::string_view mime = guess_mime_type(rel_path);

        size_t range_pos = req.find("Range: ");
        if (range_pos != std::string_view::npos) {
            size_t r_end = req.find("\r\n", range_pos);
            std::string_view range_line = req.substr(range_pos, r_end - range_pos);
            size_t start = 0, end = 0;

            if (parse_byte_range(range_line, file_size, start, end)) {
                size_t length = end - start + 1;
                std::string header = "HTTP/1.1 206 Partial Content\r\nContent-Type: " + std::string(mime) +
                                     "\r\nContent-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) +
                                     "/" + std::to_string(file_size) +
                                     "\r\nContent-Length: " + std::to_string(length) +
                                     "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n";
                if (!send_all(client_sock, header.data(), header.size())) {
                    CLOSE_SOCKET(client_sock);
                    return;
                }

                file.seekg(start);
                std::vector<char> buf(CHUNK_SIZE);
                size_t remaining = length;
                while (remaining > 0) {
                    size_t to_read = std::min(remaining, buf.size());
                    file.read(buf.data(), to_read);
                    size_t bytes = file.gcount();
                    if (bytes == 0 || !send_all(client_sock, buf.data(), bytes)) break;
                    remaining -= bytes;
                }
                CLOSE_SOCKET(client_sock);
                return;
            }
        }

        std::string header = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(mime) +
                             "\r\nContent-Length: " + std::to_string(file_size) +
                             "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n";
        if (send_all(client_sock, header.data(), header.size())) {
            file.seekg(0);
            std::vector<char> buf(CHUNK_SIZE);
            while (file) {
                file.read(buf.data(), buf.size());
                size_t bytes = file.gcount();
                if (bytes == 0 || !send_all(client_sock, buf.data(), bytes)) break;
            }
        }
    } else if (method == "POST" && path == "/save_layout") {
        size_t cl_pos = req.find("Content-Length: ");
        int cl = 0;
        if (cl_pos != std::string_view::npos) {
            cl = std::stoi(std::string(req.substr(cl_pos + 16, 8)));
        }

        size_t b_pos = req.find("\r\n\r\n");
        std::string body;
        if (b_pos != std::string_view::npos) {
            body = std::string(req.substr(b_pos + 4));
        }

        while ((int)body.size() < cl) {
            char tmp[1024];
            int n = recv(client_sock, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            body.append(tmp, n);
        }

        std::ofstream out(LAYOUT_FILE, std::ios::trunc);
        if (out.is_open()) {
            out << body;
            out.close();
            rebuild_html_cache();
        }

        static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        send_all(client_sock, ok, sizeof(ok) - 1);
    }

    CLOSE_SOCKET(client_sock);
}

void open_browser(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif __APPLE__
    system(("open " + url).c_str());
#else
    system(("xdg-open " + url).c_str());
#endif
}

int main() {
    load_video_config();
    rebuild_html_cache();

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (IS_INVALID_SOCKET(server_fd)) return 1;

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[!] Could not bind port " << PORT << "\n";
        CLOSE_SOCKET(server_fd);
        return 1;
    }

    listen(server_fd, 512); // High connection backlog

    // Spawn a fixed thread pool sized to hardware concurrency + 4 worker headroom
    size_t hw_threads = std::max(4u, std::thread::hardware_concurrency());
    ThreadPool pool(hw_threads + 4);

    std::cout << "[*] Running on http://localhost:" << PORT << "\n";
    std::cout << "[*] Admin:   http://localhost:" << PORT << "/admin\n";

    std::thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        open_browser("http://localhost:" + std::to_string(PORT));
    }).detach();

    while (true) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int addrlen = sizeof(client_addr);
#else
        socklen_t addrlen = sizeof(client_addr);
#endif
        socket_t client = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (!IS_INVALID_SOCKET(client)) {
            pool.enqueue(client);
        }
    }

    CLOSE_SOCKET(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
