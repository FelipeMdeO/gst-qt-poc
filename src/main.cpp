// File: src/main.cpp
#include <QApplication>
#include <QGuiApplication>
#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>

// Qt file/process includes
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QCoreApplication>
#include <QIODevice>
#include <QRegularExpression>

#include <algorithm>
#include <vector>
#include <cstdlib>
#include <cmath>
#include <chrono>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

// Simple percentile computation helpers
static int percentile(std::vector<int>& v, double p) {
  if (v.empty()) return 0;
  size_t idx = static_cast<size_t>(std::floor((p/100.0) * (v.size()-1)));
  std::nth_element(v.begin(), v.begin() + idx, v.end());
  return v[idx];
}

class GstQtPlayer final : public QWidget {
  Q_OBJECT
public:
  explicit GstQtPlayer(const QString& filePath, QWidget* parent=nullptr)
    : QWidget(parent), filePath_(filePath) {

    setWindowTitle("GStreamer + Qt PoC (EN + Metrics)");
    setMinimumSize(800, 480);

    // ---------- UI ----------
    auto *vbox = new QVBoxLayout(this);

    videoArea_ = new QWidget(this);
    videoArea_->setMinimumSize(640, 360);
    // Ensure native window so we get a valid XID/surface
    videoArea_->setAttribute(Qt::WA_NativeWindow);
    videoArea_->setUpdatesEnabled(false);
    vbox->addWidget(videoArea_);

    auto *h = new QHBoxLayout();
    playBtn_ = new QPushButton("Play", this);
    h->addWidget(playBtn_);
    slider_ = new QSlider(Qt::Horizontal, this);
    h->addWidget(slider_);

    throttleBtn_ = new QPushButton("Simulate bitrate drop", this);
    h->addWidget(throttleBtn_);
    vbox->addLayout(h);

    // ---------- GStreamer init ----------
    static bool gstInitted = false;
    if (!gstInitted) {
      gst_init(nullptr, nullptr);
      gstInitted = true;
      qInfo() << "[INIT] GStreamer initialized";
    }

    // ---------- Pipeline construction ----------
    pipeline_  = gst_pipeline_new("poc-pipeline");
    filesrc_   = gst_element_factory_make("filesrc", "src");
    decodebin_ = gst_element_factory_make("decodebin", "dbin");

    qVideo_    = gst_element_factory_make("queue", "qv");
    vconvert_  = gst_element_factory_make("videoconvert", "vconv");
    vscale_    = gst_element_factory_make("videoscale", "vscale");
    vcaps_     = gst_element_factory_make("capsfilter", "vcaps");

    // cencdec element: optional, may be NULL if plugin not installed
    cencdec_   = gst_element_factory_make("cencdec", "cencdec");

    // Select sink based on Qt's real platform (avoids xcb vs wayland mismatch)
    const QString plat = QGuiApplication::platformName().toLower();
    GstElement* chosenSink = nullptr;

    // Allow override via environment variable (useful for troubleshooting)
    if (const char* envSink = std::getenv("GST_VIDEOSINK")) {
      chosenSink = gst_element_factory_make(envSink, "vsink");
      qInfo() << "[INIT] GST_VIDEOSINK override =" << envSink;
    }

    if (!chosenSink) {
      if (plat.contains("wayland")) {
        chosenSink = gst_element_factory_make("waylandsink", "vsink");
        qInfo() << "[INIT] Using waylandsink";
      } else if (plat.contains("xcb")) {
        chosenSink = gst_element_factory_make("ximagesink", "vsink");
        qInfo() << "[INIT] Using ximagesink";
#ifdef Q_OS_WIN
      } else if (plat.contains("windows")) {
        chosenSink = gst_element_factory_make("d3d11videosink", "vsink");
        qInfo() << "[INIT] Using d3d11videosink";
#endif
      } else {
        chosenSink = gst_element_factory_make("autovideosink", "vsink");
        qInfo() << "[INIT] Using autovideosink (fallback)";
      }
    }

    vsink_ = chosenSink ? chosenSink : gst_element_factory_make("autovideosink", "vsink");

    qAudio_    = gst_element_factory_make("queue", "qa");
    aconv_     = gst_element_factory_make("audioconvert", "aconv");
    ares_      = gst_element_factory_make("audioresample", "ares");
    asink_     = gst_element_factory_make("autoaudiosink", "asink");

    if (!pipeline_ ||
        !filesrc_ ||
        !decodebin_ ||
        !qVideo_ ||
        !vconvert_ ||
        !vscale_ ||
        !vcaps_ ||
        !vsink_ ||
        !qAudio_ ||
        !aconv_ ||
        !ares_ ||
        !asink_) {
      qFatal("[FATAL] Failed to create one or more GStreamer elements.");
    }

    if (!cencdec_) {
      qWarning() << "[INIT] cencdec element not found - encrypted playback inside pipeline will be unavailable";
    } else {
      qInfo() << "[INIT] cencdec element created";
    }

    // filesrc -> local path (native path; NOT a URI)
    g_object_set(filesrc_, "location", filePath_.toUtf8().constData(), NULL);
    qInfo() << "[PIPELINE] Source file:" << filePath_;

    // Add elements to bin; gst_bin_add_many tolerates NULL pointers in practice
    gst_bin_add_many(
      GST_BIN(pipeline_),
      filesrc_,
      decodebin_,
      qVideo_,
      vconvert_,
      vscale_,
      vcaps_,
      vsink_,
      qAudio_,
      aconv_,
      ares_,
      asink_,
      cencdec_,   // optional
      NULL);

    if (!gst_element_link(filesrc_, decodebin_)) {
      qFatal("[FATAL] Cannot link filesrc → decodebin");
    }
    if (!gst_element_link_many(qVideo_, vconvert_, vscale_, vcaps_, vsink_, NULL)) {
      qFatal("[FATAL] Cannot link video branch");
    }
    if (!gst_element_link_many(qAudio_, aconv_, ares_, asink_, NULL)) {
      qFatal("[FATAL] Cannot link audio branch");
    }
    qInfo() << "[PIPELINE] Base links established";

    // decodebin creates dynamic pads -> hook defensive callback
    g_signal_connect(decodebin_, "pad-added", G_CALLBACK(&GstQtPlayer::onPadAdded), this);

    // ---------- Bus: async and sync messages ----------
    bus_ = gst_element_get_bus(pipeline_);
    // Regular messages (ERROR/EOS) via light polling
    busTimer_.setInterval(10);
    connect(&busTimer_, &QTimer::timeout, this, &GstQtPlayer::pumpBus);
    busTimer_.start();

    // Synchronous message for "prepare-window-handle" (ensures correct overlay timing)
    gst_bus_enable_sync_message_emission(bus_);
    g_signal_connect(
      bus_,
      "sync-message::element",
      G_CALLBACK(&GstQtPlayer::onSyncMessage),
      this);

    // ---------- Controls ----------
    connect(playBtn_, &QPushButton::clicked, this, &GstQtPlayer::togglePlayPause);
    connect(throttleBtn_, &QPushButton::clicked, this, &GstQtPlayer::toggleQuality);

    sliderTimer_.setInterval(200);
    connect(&sliderTimer_, &QTimer::timeout, this, &GstQtPlayer::updatePosition);
    sliderTimer_.start();
    connect(slider_, &QSlider::sliderReleased, this, &GstQtPlayer::doSeek);
  }

  ~GstQtPlayer() override {
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (bus_) {
      gst_object_unref(bus_);
    }
    if (pipeline_) {
      gst_object_unref(pipeline_);
    }
  }

protected:
  void showEvent(QShowEvent* e) override {
    QWidget::showEvent(e);
    // Extra native window guarantee
    videoArea_->winId();
  }

  void resizeEvent(QResizeEvent* e) override {
    QWidget::resizeEvent(e);
    if (GST_IS_VIDEO_OVERLAY(vsink_)) {
      gst_video_overlay_set_render_rectangle(
        GST_VIDEO_OVERLAY(vsink_),
        0,
        0,
        videoArea_->width(),
        videoArea_->height());
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(vsink_));
    }
  }

private slots:
  void togglePlayPause() {
    if (!pipeline_) return;
    GstState cur, pend;
    gst_element_get_state(pipeline_, &cur, &pend, 0);
    if (cur == GST_STATE_PLAYING) {
      gst_element_set_state(pipeline_, GST_STATE_PAUSED);
      playBtn_->setText("Play");
      qInfo() << "[STATE] PLAYING -> PAUSED";
    } else {
      // Start TTFF stopwatch on each transition to PLAYING
      playStartTimer_.restart();
      firstFrameSeen_ = false;
      frames_.clear();
      frameCount_ = 0;
      lastPts_ = GST_CLOCK_TIME_NONE;
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
      playBtn_->setText("Pause");
      qInfo() << "[STATE] -> PLAYING (TTFF timer armed)";
      // Install sink pad probe (if not already) to capture first frame and intervals
      attachSinkProbeIfNeeded();
    }
  }

  void pumpBus() {
    while (true) {
      GstMessage* msg = gst_bus_pop(bus_);
      if (!msg) {
        break;
      }
      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
          GError* err = nullptr;
          gchar* dbg = nullptr;
          gst_message_parse_error(msg, &err, &dbg);
          qCritical() << "[GST][ERROR]" << (err ? err->message : "unknown");
          if (dbg) {
            qCritical() << "[GST][ERROR][DBG]" << dbg;
            g_free(dbg);
          }
          if (err) {
            g_error_free(err);
          }
          gst_element_set_state(pipeline_, GST_STATE_READY);
          playBtn_->setText("Play");
          break;
        }
        case GST_MESSAGE_EOS:
          qInfo() << "[GST] EOS";
          gst_element_set_state(pipeline_, GST_STATE_READY);
          playBtn_->setText("Play");
          break;
        default: break;
      }
      gst_message_unref(msg);
    }
  }

  void updatePosition() {
    if (!pipeline_) {
      return;
    }
    gint64 pos=0, dur=0;
    if (gst_element_query_position(pipeline_, GST_FORMAT_TIME, &pos) &&
        gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &dur) &&
        dur > 0) {
      const int msPos = int(pos / GST_MSECOND);
      const int msDur = int(dur / GST_MSECOND);
      slider_->blockSignals(true);
      slider_->setRange(0, msDur);
      slider_->setValue(msPos);
      slider_->blockSignals(false);
    }
  }

  void doSeek() {
    if (!pipeline_) {
      return;
    }
    const gint64 target = (gint64)slider_->value() * GST_MSECOND;
    qInfo() << "[SEEK] to (ms):" << slider_->value();
    gst_element_seek_simple(
      pipeline_,
      GST_FORMAT_TIME,
      (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
      target);
    // After seeks, we reset metrics to measure new segment if desired
    lastPts_ = GST_CLOCK_TIME_NONE;
    frames_.clear();
    frameCount_ = 0;
    firstFrameSeen_ = false;
    playStartTimer_.restart();
  }

  void toggleQuality() {
    if (!pipeline_ || !vcaps_) return;

    qInfo() << "[ABR] Toggling quality. Current lowQuality =" << (lowQuality_ ? "true" : "false");
    // Pause briefly for safe renegotiation
    gst_element_set_state(pipeline_, GST_STATE_PAUSED);

    if (!lowQuality_) {
      // Force smaller resolution (reduced quality)
      GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "width",  G_TYPE_INT, 640,
        "height", G_TYPE_INT, 360,
        NULL);
      g_object_set(vcaps_, "caps", caps, NULL);
      gst_caps_unref(caps);

      // Signal downstream reconfigure
      gst_element_send_event(vscale_, gst_event_new_reconfigure());

      lowQuality_ = true;
      throttleBtn_->setText("Restore quality");
      qInfo() << "[ABR] Low quality enforced: 640x360";
    } else {
      // Remove restriction → allow renegotiation to full-res
      g_object_set(vcaps_, "caps", NULL, NULL);
      gst_element_send_event(vscale_, gst_event_new_reconfigure());

      lowQuality_ = false;
      throttleBtn_->setText("Simulate bitrate drop");
      qInfo() << "[ABR] Quality restored (caps removed)";
    }

    // Resume playback
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  }

private:
  // ---------- GStreamer callbacks ----------
  static void onSyncMessage(GstBus*, GstMessage* msg, gpointer userData) {
    if (!gst_is_video_overlay_prepare_window_handle_message(msg)) {
      return;
    }
    auto* self = static_cast<GstQtPlayer*>(userData);

    // Ensure a native window exists
    self->videoArea_->setAttribute(Qt::WA_NativeWindow);
    WId wid = self->videoArea_->winId();
    if (!wid || wid < 0x10) {
      qWarning() << "[OVERLAY] Invalid window id during prepare-window-handle";
      return; // avoid BadWindow
    }

    if (GST_IS_VIDEO_OVERLAY(self->vsink_)) {
      gst_video_overlay_set_window_handle(
        GST_VIDEO_OVERLAY(self->vsink_),
        (guintptr)wid);
      gst_video_overlay_set_render_rectangle(
        GST_VIDEO_OVERLAY(self->vsink_),
        0,
        0,
        self->videoArea_->width(),
        self->videoArea_->height());
      gst_video_overlay_expose(GST_VIDEO_OVERLAY(self->vsink_));
      qInfo() << "[OVERLAY] Window handle set";
    }
  }

  // Robust, instrumented onPadAdded implementing cencdec routing + fallback
  static void onPadAdded(GstElement* dbin, GstPad* newPad, gpointer userData) {
    auto* self = static_cast<GstQtPlayer*>(userData);
    if (!self) return;

    if (gst_pad_get_direction(newPad) != GST_PAD_SRC) {
      return;
    }

    GstCaps* caps = gst_pad_get_current_caps(newPad);
    if (!caps) {
      caps = gst_pad_query_caps(newPad, nullptr);
    }
    if (!caps || gst_caps_is_empty(caps)) {
      if (caps) gst_caps_unref(caps);
      qWarning() << "[DECODEBIN] pad-added with empty caps";
      return;
    }

    const GstStructure* st = gst_caps_get_structure(caps, 0);
    if (!st) {
      gst_caps_unref(caps);
      qWarning() << "[DECODEBIN] pad-added but no structure";
      return;
    }
    const gchar* name = gst_structure_get_name(st);
    if (!name) {
      gst_caps_unref(caps);
      qWarning() << "[DECODEBIN] pad-added but structure has no name";
      return;
    }

    // log caps (free string afterwards)
    gchar* caps_str = gst_caps_to_string(caps);
    qInfo() << "[DECODEBIN] pad-added caps:" << (caps_str ? caps_str : "(null)") << " name=" << name;
    if (caps_str) g_free(caps_str);

    const bool isVideo = g_str_has_prefix(name, "video/");
    const bool isAudio = g_str_has_prefix(name, "audio/");

    // Detect encrypted variants commonly used with CENC
    const bool isCenc = g_str_has_prefix(name, "application/x-cenc") ||
                        g_str_has_prefix(name, "video/encv") ||
                        g_str_has_prefix(name, "audio/enca");

    GstElement* targetQueue = isVideo ? self->qVideo_ : (isAudio ? self->qAudio_ : nullptr);
    if (!targetQueue) {
      qWarning() << "[DECODEBIN] Ignoring pad with caps:" << name;
      gst_caps_unref(caps);
      return;
    }

    // If it's encrypted and we have a cencdec element, attempt route: demux-pad -> cencdec -> queue
    if (isCenc && self->cencdec_) {
      qInfo() << "[CENC] Encrypted pad detected; attempting routing via cencdec";

      // Some plugins allocate resources only after state change; request READY to be safe
      GstStateChangeReturn st_ret = gst_element_set_state(self->cencdec_, GST_STATE_READY);
      qInfo() << "[CENC] cencdec state change requested (to READY), return code =" << st_ret;

      // link demux pad -> cencdec sink
      GstPad* cenc_sink = gst_element_get_static_pad(self->cencdec_, "sink");
      if (!cenc_sink) {
        qWarning() << "[CENC] cencdec sink pad not available";
        // fallback: try direct linking demux -> queue
      } else {
        if (!gst_pad_is_linked(cenc_sink)) {
          GstPadLinkReturn r = gst_pad_link(newPad, cenc_sink);
          qInfo() << "[CENC] Attempted link: demux-pad -> cencdec sink, result code =" << r;
          if (r != GST_PAD_LINK_OK) {
            qWarning() << "[CENC] demux-pad -> cencdec sink link FAILED (code =" << r << ")";
          }
        } else {
          qInfo() << "[CENC] cencdec sink pad already linked";
        }
        gst_object_unref(cenc_sink);
      }

      // Now link cencdec src -> queue sink (if not already linked)
      GstPad* cenc_src = gst_element_get_static_pad(self->cencdec_, "src");
      GstPad* q_sink   = gst_element_get_static_pad(targetQueue, "sink");
      if (cenc_src && q_sink) {
        if (!gst_pad_is_linked(q_sink)) {
          GstPadLinkReturn r2 = gst_pad_link(cenc_src, q_sink);
          qInfo() << "[CENC] Attempted link: cencdec src -> queue sink, result code =" << r2;
          if (r2 != GST_PAD_LINK_OK) {
            qWarning() << "[CENC] cencdec src -> queue sink link FAILED (code =" << r2 << ")";
          } else {
            qInfo() << "[CENC] Successfully linked cencdec ->" << (isVideo ? "video" : "audio") << "queue";
            if (cenc_src) gst_object_unref(cenc_src);
            if (q_sink) gst_object_unref(q_sink);
            gst_caps_unref(caps);
            return;
          }
        } else {
          qInfo() << "[CENC] target queue sink already linked";
          if (cenc_src) gst_object_unref(cenc_src);
          if (q_sink) gst_object_unref(q_sink);
          gst_caps_unref(caps);
          return;
        }
      }
      if (cenc_src) gst_object_unref(cenc_src);
      if (q_sink) gst_object_unref(q_sink);

      qWarning() << "[CENC] Routing via cencdec did not succeed; attempting fallback direct link demux -> queue";
      // fall through to fallback direct linking
    }

    // Fallback / non-encrypted path: link demux pad -> queue sink
    GstPad* sinkpad = gst_element_get_static_pad(targetQueue, "sink");
    if (sinkpad) {
      if (!gst_pad_is_linked(sinkpad)) {
        GstPadLinkReturn r = gst_pad_link(newPad, sinkpad);
        if (r != GST_PAD_LINK_OK) {
          qWarning() << "[LINK] Failed to link decodebin pad (" << name << ") -> queue. Code:" << r;
        } else {
          qInfo() << "[LINK] Linked decodebin pad ->" << (isVideo ? "video" : "audio") << "queue";
        }
      } else {
        qInfo() << "[LINK] queue sink pad already linked";
      }
      gst_object_unref(sinkpad);
    } else {
      qWarning() << "[LINK] target queue sink pad not available";
    }

    gst_caps_unref(caps);
  }

  static GstPadProbeReturn onSinkBufferProbe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer userData) {
    auto* self = static_cast<GstQtPlayer*>(userData);
    if (!self) return GST_PAD_PROBE_OK;

    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    GstClockTime pts = GST_BUFFER_PTS(buf);
    if (!self->firstFrameSeen_) {
      self->firstFrameSeen_ = true;
      // Time To First Frame = wallclock since we entered PLAYING
      const qint64 ttff_ms = self->playStartTimer_.elapsed();
      qInfo() << "[METRICS] TTFF(ms):" << ttff_ms;
    }

    if (pts != GST_CLOCK_TIME_NONE) {
      if (self->lastPts_ != GST_CLOCK_TIME_NONE) {
        // Compute frame interval in milliseconds based on PTS delta
        gint64 delta_ns = (gint64)pts - (gint64)self->lastPts_;
        if (delta_ns > 0) {
          int delta_ms = static_cast<int>(delta_ns / GST_MSECOND);
          self->frames_.push_back(delta_ms);
          self->frameCount_++;

          if (self->frameCount_ % 60 == 0) {
            // Compute q50 and q95 on a copy (percentile does nth_element and mutates).
            std::vector<int> copy = self->frames_;
            int q50 = percentile(copy, 50.0);
            // Re-copy to restore distribution before next percentile
            copy = self->frames_;
            int q95 = percentile(copy, 95.0);
            qInfo() << "[METRICS] frame-interval-ms q50=" << q50 << " q95=" << q95 << " (n=" << self->frameCount_ << ")";
          }

          // Keep vector from growing unbounded; retain last ~1000 samples
          if (self->frames_.size() > 1200) {
            self->frames_.erase(self->frames_.begin(), self->frames_.begin() + 200);
          }
        }
      }
      self->lastPts_ = pts;
    }
    return GST_PAD_PROBE_OK;
  }

  void attachSinkProbeIfNeeded() {
    if (sinkProbeAttached_) return;
    GstPad* sinkpad = gst_element_get_static_pad(vsink_, "sink");
    if (!sinkpad) {
      qWarning() << "[PROBE] vsink sink pad not available yet";
      return;
    }
    gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER, &GstQtPlayer::onSinkBufferProbe, this, nullptr);
    sinkProbeAttached_ = true;
    gst_object_unref(sinkpad);
    qInfo() << "[PROBE] Buffer probe attached to video sink";
  }

private:
  // UI
  QWidget*     videoArea_{nullptr};
  QPushButton* playBtn_{nullptr};
  QPushButton* throttleBtn_{nullptr};
  QSlider*     slider_{nullptr};

  // GStreamer
  QString    filePath_;
  GstElement* pipeline_{nullptr};
  GstElement* filesrc_{nullptr};
  GstElement* decodebin_{nullptr};

  // Video
  GstElement* qVideo_{nullptr};
  GstElement* vconvert_{nullptr};
  GstElement* vscale_{nullptr};
  GstElement* vcaps_{nullptr};
  GstElement* vsink_{nullptr};

  // Audio
  GstElement* qAudio_{nullptr};
  GstElement* aconv_{nullptr};
  GstElement* ares_{nullptr};
  GstElement* asink_{nullptr};

  // Optional decrypt element (cencdec)
  GstElement* cencdec_{nullptr};

  GstBus*     bus_{nullptr};
  QTimer      busTimer_;
  QTimer      sliderTimer_;

  // ABR simulation
  bool        lowQuality_{false};

  // Metrics
  QElapsedTimer playStartTimer_;
  bool          firstFrameSeen_{false};
  bool          sinkProbeAttached_{false};
  GstClockTime  lastPts_{GST_CLOCK_TIME_NONE};
  std::vector<int> frames_;
  int           frameCount_{0};
};

#include "main.moc"

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  if (argc < 2) {
    qCritical() << "Usage: gst_qt_poc <absolute-file-path>";
    return 1;
  }

  const QString originalPath = QString::fromLocal8Bit(argv[1]);
  if (originalPath.isEmpty()) {
    qCritical() << "Invalid path.";
    return 1;
  }

  qInfo() << "[MAIN] Starting with media:" << originalPath;

  // === Auto-provision /tmp/<KID>.key from companion keys file if possible ===
  // This helps cencdec locate the key by KID without manual steps.
  QFileInfo fi(originalPath);
  const QString baseName = fi.completeBaseName();
  const QString dirPath = fi.absolutePath();
  const QString keysPath = dirPath + "/" + baseName + "_keys.txt";

  if (QFile::exists(keysPath)) {
    qInfo() << "[MAIN] Companion keys file found:" << keysPath << " — attempting to extract KEY1 and map to default_KID";

    // read KEY1 from keys file (format expected: 1:<HEX>)
    QString key1;
    QFile kf(keysPath);
    if (kf.open(QIODevice::ReadOnly | QIODevice::Text)) {
      while (!kf.atEnd()) {
        const QString line = QString::fromUtf8(kf.readLine()).trimmed();
        if (line.startsWith("1:")) {
          key1 = line.mid(2).trimmed();
          break;
        }
      }
      kf.close();
    } else {
      qWarning() << "[MAIN] Failed to open keys file for reading:" << keysPath;
    }

    if (!key1.isEmpty()) {
      // run mp4dump to extract default_KID
      QProcess p;
      p.start("mp4dump", QStringList() << originalPath);
      if (p.waitForFinished(3000)) {
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        QRegularExpression re("default_KID[^0-9A-Fa-f]*([0-9A-Fa-f]{32})", QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch m = re.match(out);
        if (m.hasMatch()) {
          const QString kidHex = m.captured(1).toLower();
          const QString tmpKeyPath = QDir::tempPath() + "/" + kidHex + ".key";
          qInfo() << "[MAIN] Found default_KID in container:" << kidHex << " — writing" << tmpKeyPath;

          // convert hex string to binary and write file
          QByteArray bin;
          bool ok = true;
          QString kclean = key1;
          kclean = kclean.trimmed();
          kclean = kclean.toLower();
          if (kclean.size() % 2 != 0) ok = false;
          for (int i = 0; ok && i < kclean.length(); i += 2) {
            bool convOk = false;
            const QString byteStr = kclean.mid(i, 2);
            const uint val = byteStr.toUInt(&convOk, 16);
            if (!convOk) { ok = false; break; }
            bin.append(static_cast<char>(val));
          }
          if (ok && !bin.isEmpty()) {
            QFile outF(tmpKeyPath);
            if (outF.open(QIODevice::WriteOnly)) {
              outF.write(bin);
              outF.close();
              QFile::setPermissions(outF.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
              qInfo() << "[MAIN] Wrote binary key file:" << tmpKeyPath;
            } else {
              qWarning() << "[MAIN] Failed to open" << tmpKeyPath << "for writing";
            }
          } else {
            qWarning() << "[MAIN] KEY1 parse failed or empty; not writing" << tmpKeyPath;
          }
        } else {
          qWarning() << "[MAIN] default_KID not found in mp4dump output";
        }
      } else {
        qWarning() << "[MAIN] mp4dump did not finish in time or failed when inspecting container";
      }
    } else {
      qWarning() << "[MAIN] KEY1 not present in" << keysPath << "; cannot write /tmp/<KID>.key";
    }
  } else {
    qInfo() << "[MAIN] No companion keys file found near media; skipping /tmp/<KID>.key provisioning";
  }

  GstQtPlayer w(originalPath);
  w.show();
  return app.exec();
}
