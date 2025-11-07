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

#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QCoreApplication>
#include <QIODevice>

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

    // filesrc -> local path (native path; NOT a URI)
    g_object_set(filesrc_, "location", filePath_.toUtf8().constData(), NULL);
    qInfo() << "[PIPELINE] Source file:" << filePath_;

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

  static void onPadAdded(GstElement* dbin, GstPad* newPad, gpointer userData) {
    auto* self = static_cast<GstQtPlayer*>(userData);
    if (gst_pad_get_direction(newPad) != GST_PAD_SRC) {
      return;
    }

    GstCaps* caps = gst_pad_get_current_caps(newPad);
    if (!caps) {
      caps = gst_pad_query_caps(newPad, nullptr);
    }
    if (!caps || gst_caps_is_empty(caps)) {
      if (caps) {
        gst_caps_unref(caps);
      }
      qWarning() << "[DECODEBIN] pad-added with empty caps";
      return;
    }

    const GstStructure* st = gst_caps_get_structure(caps, 0);
    if (!st) {
      gst_caps_unref(caps);
      return;
    }
    const gchar* name = gst_structure_get_name(st);
    if (!name) {
      gst_caps_unref(caps);
      return;
    }

    const bool isVideo = g_str_has_prefix(name, "video/");
    const bool isAudio = g_str_has_prefix(name, "audio/");

    GstElement* targetQueue = isVideo ? self->qVideo_ : (isAudio ? self->qAudio_ : nullptr);
    if (targetQueue) {
      GstPad* sinkpad = gst_element_get_static_pad(targetQueue, "sink");
      if (sinkpad && !gst_pad_is_linked(sinkpad)) {
        const GstPadLinkReturn r = gst_pad_link(newPad, sinkpad);
        if (r != GST_PAD_LINK_OK) {
          qWarning() << "[LINK] Failed to link decodebin pad (" << name << ") → queue. Code:" << r;
        } else {
          qInfo() << "[LINK] Linked decodebin pad →" << (isVideo ? "video" : "audio") << "queue";
        }
      }
      if (sinkpad) {
        gst_object_unref(sinkpad);
      }
    } else {
      qWarning() << "[DECODEBIN] Ignoring pad with caps:" << name;
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

  const QString baseName = fi.completeBaseName(); // without extension
  const QString dirPath = fi.absolutePath();

  // Build a list of candidate keys files to be robust to suffixes like "_encrypted"
  QStringList candidates;
  candidates << dirPath + "/" + baseName + "_keys.txt";

  // if the filename ends with common suffixes, also try the base without them
  const QStringList suffixesToStrip = {"_encrypted", "_frag", "_fragged"};
  for (const QString& suf : suffixesToStrip) {
    if (baseName.endsWith(suf)) {
      const QString shortened = baseName.left(baseName.size() - suf.size());
      candidates << dirPath + "/" + shortened + "_keys.txt";
      // also try direct name without suffix just in case
      candidates << dirPath + "/" + shortened;
    }
  }

  // Also try legacy possibilities (just in case)
  candidates << dirPath + "/" + baseName;                   // keys file named exactly like base
  candidates << dirPath + "/" + baseName + "_encrypted";    // less common variant

  QString keysPath;
  for (const QString &c : candidates) {
    if (QFile::exists(c)) {
      keysPath = c;
      qInfo() << "[MAIN] Using keys file candidate:" << keysPath;
      break;
    }
  }

  QString pathToPlay = originalPath; // default

  if (!keysPath.isEmpty()) {
    qInfo() << "[MAIN] Found keys file:" << keysPath << " — attempting mp4decrypt to temporary file";

    // read keys file (expected format: "1:<HEX>" and "2:<HEX>" lines)
    QString key1, key2;
    QFile kf(keysPath);
    if (kf.open(QIODevice::ReadOnly | QIODevice::Text)) {
      while (!kf.atEnd()) {
        const QString line = QString::fromUtf8(kf.readLine()).trimmed();
        if (line.startsWith("1:")) key1 = line.mid(2);
        else if (line.startsWith("2:")) key2 = line.mid(2);
      }
      kf.close();
    } else {
      qCritical() << "[MAIN] Failed to open keys file:" << keysPath;
    }

    if (!key1.isEmpty() && !key2.isEmpty()) {
      // build temp output path
      const QString tmpOut = QDir::tempPath() + "/" + baseName + "_decrypted_" + QString::number(QCoreApplication::applicationPid()) + ".mp4";

      // construct mp4decrypt args: --key 1:<key1> --key 2:<key2> <input> <output>
      QStringList args;
      args << "--key" << ("1:" + key1) << "--key" << ("2:" + key2) << originalPath << tmpOut;

      qInfo() << "[MAIN] Executing mp4decrypt (blocking): mp4decrypt" << args.join(" ");

      // Execute synchronously. QProcess::execute returns exit code.
      int rc = QProcess::execute("mp4decrypt", args);
      if (rc == 0 && QFile::exists(tmpOut)) {
        qInfo() << "[MAIN] mp4decrypt succeeded. Playing decrypted temp file:" << tmpOut;
        pathToPlay = tmpOut;
      } else {
        qCritical() << "[MAIN] mp4decrypt failed (rc=" << rc << "). Falling back to original path for debugging.";
      }
    } else {
      qCritical() << "[MAIN] Keys file present but keys not found (expected '1:HEX' and '2:HEX').";
    }
  } else {
    qInfo() << "[MAIN] No keys file found alongside media. Playing provided path.";
  }


  GstQtPlayer w(pathToPlay);
  w.show();
  return app.exec();
}
