# 📺 Qt + GStreamer PoC

### Overview  
This proof of concept demonstrates **video playback inside a Qt 6 application** using an explicit **GStreamer pipeline**.  
The project validates the integration of Qt and GStreamer for real-time rendering, cross-platform builds (Linux / Windows), and stable operation across **X11** and **Wayland** environments.

---

### 🎯 Objective  
- Implement a modular media player using the pipeline:  
  ```
  filesrc → decodebin → [queue → videoconvert → videoscale → sink]
                     → [queue → audioconvert → audioresample → autoaudiosink]
  ```
- Render decoded video directly inside a Qt widget via `GstVideoOverlay`.  
- Handle backend mismatches (Wayland vs X11) and hardware acceleration issues (VAAPI / DMABUF).  
- Provide an automated build workflow via a portable shell script (`build.sh`).

---

### ⚠️ Common Issues Solved

| Error | Cause | Solution |
|-------|--------|-----------|
| `_dma_fmt_to_dma_drm_fmts` / Segmentation fault | Hardware acceleration (VAAPI / DMABUF) conflict under hybrid XWayland | Disable hardware acceleration:<br>`export GST_GL_NO_DMABUF=1`<br>`export GST_PLUGIN_FEATURE_RANK='vaapidecodebin:0,vaapih264dec:0'` |
| `Application did not provide a wayland display handle` | Qt running under **XWayland (xcb)**, but GStreamer auto-selected **Wayland sink** | Dynamically select the sink according to Qt’s backend:<br>`QGuiApplication::platformName()` → choose `ximagesink` or `waylandsink` |

---

### 🧰 Dependencies
Install Qt, GStreamer, and build tools:
```bash
sudo apt update
sudo apt install -y qt6-base-dev qt6-base-dev-tools     libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev     gstreamer1.0-plugins-good gstreamer1.0-plugins-bad     gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-tools     build-essential cmake ninja-build
```

---

### ⚙️ Automated Build & Run (Using build.sh)

The project includes an **automation script** `build.sh` to simplify all build steps.

#### 1️⃣ Make it executable:
```bash
chmod +x build.sh
```

#### 2️⃣ Clean and build (Linux release preset):
```bash
./build.sh --clean --preset linux-rel
```

#### 3️⃣ Build only:
```bash
./build.sh --preset linux-rel
```

#### 4️⃣ Build and run directly:
```bash
./build.sh --preset linux-rel --run /absolute/path/to/video.mp4
```

#### 5️⃣ Cross-compile example (Windows preset):
```bash
./build.sh --clean --preset win-rel -j 12
```

#### 🧩 Script details
- Automatically detects build presets (`linux-rel`, `win-rel`)
- Cleans previous build outputs when requested (`--clean`)
- Runs `cmake --preset` and `cmake --build` automatically
- Uses all CPU cores unless overridden with `--jobs N`
- Optionally launches the binary with a provided video/URI (`--run <path>`)
- Applies recommended runtime environment variables for stability:
  ```bash
  export GST_GL_NO_DMABUF=1
  export GST_PLUGIN_FEATURE_RANK='vaapidecodebin:0,vaapih264dec:0'
  ```

---

### 🧩 Key Features
- Explicit GStreamer pipeline  
- Qt widget video rendering via `GstVideoOverlay`  
- Dynamic sink selection (`ximagesink` / `waylandsink`)  
- Robust against driver and backend mismatches  
- Ready for ABR and overlay extensions  
- Includes a fully automated build workflow
