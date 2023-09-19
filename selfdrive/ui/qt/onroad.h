#pragma once

#include <memory>

#include <QPushButton>
#include <QStackedLayout>
#include <QWidget>

#include "common/util.h"
#include "selfdrive/ui/ui.h"
#include "selfdrive/ui/qt/widgets/cameraview.h"


const int btn_size = 192;
const int img_size = (btn_size / 4) * 3;

// FrogPilot global variables
static bool reverseCruiseIncrease;
static bool speedHidden;


// ***** onroad widgets *****
class OnroadAlerts : public QWidget {
  Q_OBJECT

public:
  OnroadAlerts(QWidget *parent = 0) : QWidget(parent) {}
  void updateAlert(const Alert &a);

protected:
  void paintEvent(QPaintEvent*) override;

private:
  QColor bg;
  Alert alert = {};
};

class ExperimentalButton : public QPushButton {
  Q_OBJECT

public:
  explicit ExperimentalButton(QWidget *parent = 0);
  void updateState(const UIState &s);

private:
  void paintEvent(QPaintEvent *event) override;
  void changeMode();

  Params params;
  QPixmap engage_img;
  QPixmap experimental_img;
  bool experimental_mode;
  bool engageable;

  // FrogPilot variables
  int steeringWheel;
  std::map<int, QPixmap> wheel_images;

};


class MapSettingsButton : public QPushButton {
  Q_OBJECT

public:
  explicit MapSettingsButton(QWidget *parent = 0);

private:
  void paintEvent(QPaintEvent *event) override;

  QPixmap settings_img;
};

// container window for the NVG UI
class AnnotatedCameraWidget : public CameraWidget {
  Q_OBJECT

public:
  explicit AnnotatedCameraWidget(VisionStreamType type, QWidget* parent = 0);
  void updateState(const UIState &s);

  MapSettingsButton *map_settings_btn;

private:
  void drawText(QPainter &p, int x, int y, const QString &text, int alpha = 255);

  // FrogPilot widgets
  void drawCompass(QPainter &p);
  void drawDrivingPersonalities(QPainter &p);
  void drawFrogSignals(QPainter &p);
  void drawStatusBar(QPainter &p);

  QVBoxLayout *main_layout;
  ExperimentalButton *experimental_btn;
  QPixmap dm_img;
  float speed;
  QString speedUnit;
  float setSpeed;
  float speedLimit;
  bool is_cruise_set = false;
  bool is_metric = false;
  bool dmActive = false;
  bool hideBottomIcons = false;
  bool rightHandDM = false;
  float dm_fade_state = 1.0;
  bool has_us_speed_limit = false;
  bool has_eu_speed_limit = false;
  bool v_ego_cluster_seen = false;
  int status = STATUS_DISENGAGED;
  std::unique_ptr<PubMaster> pm;

  int skip_frame_count = 0;
  bool wide_cam_requested = false;

  // FrogPilot variables
  bool accelerationPath;
  bool blindSpotLeft;
  bool blindSpotRight;
  bool compass;
  bool conditionalExperimental;
  bool experimentalMode;
  bool frogColors;
  bool frogSignals;
  bool mapOpen;
  bool muteDM;
  bool onroadAdjustableProfiles;
  bool rotatingWheel;
  bool toyotaCar;
  bool turnSignalLeft;
  bool turnSignalRight;
  int animationFrameIndex;
  int bearingDeg;
  int conditionalSpeed;
  int conditionalSpeedLead;
  int conditionalStatus;
  int personalityProfile;
  int steeringAngleDeg;
  int steeringWheel;
  QPixmap compass_inner_img;
  QPixmap engage_img;
  QPixmap experimental_img;
  QVector<std::pair<QPixmap, QString>> profile_data;
  static constexpr int totalFrames = 8;
  std::map<int, QPixmap> wheel_images;
  std::vector<QPixmap> signalImgVector;

protected:
  void paintGL() override;
  void initializeGL() override;
  void showEvent(QShowEvent *event) override;
  void updateFrameMat() override;
  void drawLaneLines(QPainter &painter, const UIState *s);
  void drawLead(QPainter &painter, const cereal::RadarState::LeadData::Reader &lead_data, const QPointF &vd);
  void drawHud(QPainter &p, const UIState *s);
  void drawDriverState(QPainter &painter, const UIState *s);
  inline QColor redColor(int alpha = 255) { return QColor(201, 34, 49, alpha); }
  inline QColor whiteColor(int alpha = 255) { return QColor(255, 255, 255, alpha); }
  inline QColor blackColor(int alpha = 255) { return QColor(0, 0, 0, alpha); }

  // FrogPilot colors
  inline QColor frogColor(int alpha = 242) { return QColor(23, 134, 68, alpha); }

  double prev_draw_t = 0;
  FirstOrderFilter fps_filter;
};

// container for all onroad widgets
class OnroadWindow : public QWidget {
  Q_OBJECT

public:
  OnroadWindow(QWidget* parent = 0);
  bool isMapVisible() const { return map && map->isVisible(); }
  void showMapPanel(bool show) { if (map) map->setVisible(show); }

signals:
  void mapPanelRequested();

private:
  void paintEvent(QPaintEvent *event);
  void mousePressEvent(QMouseEvent* e) override;
  OnroadAlerts *alerts;
  AnnotatedCameraWidget *nvg;
  QColor bg = bg_colors[STATUS_DISENGAGED];
  QWidget *map = nullptr;
  QHBoxLayout* split;

  // FrogPilot variables
  QPoint timeoutPoint = QPoint(420, 69);
  QTimer clickTimer;

private slots:
  void offroadTransition(bool offroad);
  void primeChanged(bool prime);
  void updateState(const UIState &s);
};
