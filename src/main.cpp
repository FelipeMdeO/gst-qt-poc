#include <QApplication>
#include <QGuiApplication>
#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QTimer>
#include <QDebug>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>


class GstQtPlayer final : public QWidget {
  Q_OBJECT
public:
  explicit GstQtPlayer(const QString& filePath, QWidget* parent=nullptr)
    : QWidget(parent), filePath_(filePath) {

    setWindowTitle("GStreamer + Qt PoC");
    setMinimumSize(800, 480);

    // ---------- UI ----------
    auto *vbox = new QVBoxLayout(this);

    videoArea_ = new QWidget(this);
    videoArea_->setMinimumSize(640, 360);
    // **Ensure native window** so we get a valid XID/surface
    videoArea_->setAttribute(Qt::WA_NativeWindow);
    videoArea_->setUpdatesEnabled(false);
    vbox->addWidget(videoArea_);

    auto *h = new QHBoxLayout();
    playBtn_ = new QPushButton("Play", this);
    h->addWidget(playBtn_);
    slider_ = new QSlider(Qt::Horizontal, this);
    h->addWidget(slider_);
    vbox->addLayout(h);

    // ---------- GStreamer init ----------
    static bool gstInitted = false;
    if (!gstInitted) {
      gst_init(nullptr, nullptr);
      gstInitted = true;
    }

    // ---------- Pipeline construction ----------
    pipeline_  = gst_pipeline_new("poc-pipeline");
    filesrc_   = gst_element_factory_make("filesrc", "src");
    decodebin_ = gst_element_factory_make("decodebin", "dbin");

    qVideo_    = gst_element_factory_make("queue", "qv");
    vconvert_  = gst_element_factory_make("videoconvert", "vconv");
    vscale_   = gst_element_factory_make("videoscale", "vscale");

    // Select sink based on Qt's real platform (avoids xcb vs wayland conflict)
    const QString plat = QGuiApplication::platformName().toLower();
    GstElement* chosenSink = nullptr;

    // Allow override via environment variable (useful for troubleshooting)
    if (const char* envSink = std::getenv("GST_VIDEOSINK")) {
      chosenSink = gst_element_factory_make(envSink, "vsink");
    }

    if (!chosenSink) {
      if (plat.contains("wayland")) {
        chosenSink = gst_element_factory_make("waylandsink", "vsink");
      } else if (plat.contains("xcb")) {
        chosenSink = gst_element_factory_make("ximagesink", "vsink");
    #ifdef Q_OS_WIN
      } else if (plat.contains("windows")) {
        chosenSink = gst_element_factory_make("d3d11videosink", "vsink");
    #endif
      } else {
        chosenSink = gst_element_factory_make("autovideosink", "vsink");
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
        !vsink_ ||
        !qAudio_ ||
        !aconv_ ||
        !ares_ ||
        !asink_) {
      qFatal("Falha criando elementos GStreamer.");
    }

    // filesrc -> local path (native path; NOT a URI)
    g_object_set(filesrc_, "location", filePath_.toUtf8().constData(), NULL);

    gst_bin_add_many(
      GST_BIN(pipeline_),
      filesrc_,
      decodebin_,
      qVideo_,
      vconvert_,
      vscale_,
      vsink_,
      qAudio_,
      aconv_,
      ares_,
      asink_,
      NULL);

    if (!gst_element_link(filesrc_, decodebin_)) {
      qFatal("Cannot link filesrc → decodebin");
    }
    if (!gst_element_link_many(qVideo_, vconvert_, vscale_, vsink_, NULL)) {
      qFatal("Cannot link video branch");
    }
    if (!gst_element_link_many(qAudio_, aconv_, ares_, asink_, NULL)) {
      qFatal("Cannot link audio branch");
    }

    // decodebin creates dynamic pads -> hook defensive callback
    g_signal_connect(decodebin_, "pad-added", G_CALLBACK(&GstQtPlayer::onPadAdded), this);

    // ---------- Bus: asynchronous and synchronous messages ----------
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

signals:
  void overlayReady();

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
    GstState cur, pend;
    gst_element_get_state(pipeline_, &cur, &pend, 0);
    if (cur == GST_STATE_PLAYING) {
      gst_element_set_state(pipeline_, GST_STATE_PAUSED);
      playBtn_->setText("Play");
    } else {
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
      playBtn_->setText("Pause");
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
          qCritical() << "GST ERROR:" << (err ? err->message : "unknown");
          if (dbg) {
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
          qInfo() << "EOS";
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
    gst_element_seek_simple(
      pipeline_,
      GST_FORMAT_TIME,
      (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
      target);
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
          g_warning("Falha ao linkar decodebin pad (%s) → queue: %d", name, r);
        }
      }
      if (sinkpad) {
        gst_object_unref(sinkpad);
      }
    }
    gst_caps_unref(caps);
  }

private:
  // UI
  QWidget*     videoArea_{nullptr};
  QPushButton* playBtn_{nullptr};
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
  GstElement* vsink_{nullptr};

  // Audio
  GstElement* qAudio_{nullptr};
  GstElement* aconv_{nullptr};
  GstElement* ares_{nullptr};
  GstElement* asink_{nullptr};

  GstBus*     bus_{nullptr};
  QTimer      busTimer_;
  QTimer      sliderTimer_;
};

#include "main.moc"

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  if (argc < 2) {
    qCritical() << "Uso: gst_qt_poc <caminho-absoluto-arquivo>";
    return 1;
  }

  const QString path = QString::fromLocal8Bit(argv[1]);
  if (path.isEmpty()) {
    qCritical() << "Caminho inválido.";
    return 1;
  }

  GstQtPlayer w(path);
  w.show();
  return app.exec();
}
