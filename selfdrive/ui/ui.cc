#include "selfdrive/ui/ui.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include <QtConcurrent>

#include "common/transformations/orientation.hpp"
#include "common/params.h"
#include "common/swaglog.h"
#include "common/util.h"
#include "common/watchdog.h"
#include "system/hardware/hw.h"

#define BACKLIGHT_DT 0.05
#define BACKLIGHT_TS 10.00

// Projects a point in car to space to the corresponding point in full frame
// image space.
static bool calib_frame_to_full_frame(const UIState *s, float in_x, float in_y, float in_z, QPointF *out) {
  const float margin = 500.0f;
  const QRectF clip_region{-margin, -margin, s->fb_w + 2 * margin, s->fb_h + 2 * margin};

  const vec3 pt = (vec3){{in_x, in_y, in_z}};
  const vec3 Ep = matvecmul3(s->scene.wide_cam ? s->scene.view_from_wide_calib : s->scene.view_from_calib, pt);
  const vec3 KEp = matvecmul3(s->scene.wide_cam ? ECAM_INTRINSIC_MATRIX : FCAM_INTRINSIC_MATRIX, Ep);

  // Project.
  QPointF point = s->car_space_transform.map(QPointF{KEp.v[0] / KEp.v[2], KEp.v[1] / KEp.v[2]});
  if (clip_region.contains(point)) {
    *out = point;
    return true;
  }
  return false;
}

int get_path_length_idx(const cereal::XYZTData::Reader &line, const float path_height) {
  const auto line_x = line.getX();
  int max_idx = 0;
  for (int i = 1; i < TRAJECTORY_SIZE && line_x[i] <= path_height; ++i) {
    max_idx = i;
  }
  return max_idx;
}

void update_leads(UIState *s, const cereal::RadarState::Reader &radar_state, const cereal::XYZTData::Reader &line) {
  for (int i = 0; i < 2; ++i) {
    auto lead_data = (i == 0) ? radar_state.getLeadOne() : radar_state.getLeadTwo();
    if (lead_data.getStatus()) {
      float z = line.getZ()[get_path_length_idx(line, lead_data.getDRel())];
      calib_frame_to_full_frame(s, lead_data.getDRel(), -lead_data.getYRel(), z + 1.22, &s->scene.lead_vertices[i]);
    }
  }
}

void update_line_data(const UIState *s, const cereal::XYZTData::Reader &line,
                      float y_off, float z_off, QPolygonF *pvd, int max_idx, bool allow_invert=true) {
  const auto line_x = line.getX(), line_y = line.getY(), line_z = line.getZ();
  QPolygonF left_points, right_points;
  left_points.reserve(max_idx + 1);
  right_points.reserve(max_idx + 1);

  for (int i = 0; i <= max_idx; i++) {
    // highly negative x positions  are drawn above the frame and cause flickering, clip to zy plane of camera
    if (line_x[i] < 0) continue;
    QPointF left, right;
    bool l = calib_frame_to_full_frame(s, line_x[i], line_y[i] - y_off, line_z[i] + z_off, &left);
    bool r = calib_frame_to_full_frame(s, line_x[i], line_y[i] + y_off, line_z[i] + z_off, &right);
    if (l && r) {
      // For wider lines the drawn polygon will "invert" when going over a hill and cause artifacts
      if (!allow_invert && left_points.size() && left.y() > left_points.back().y()) {
        continue;
      }
      left_points.push_back(left);
      right_points.push_front(right);
    }
  }
  *pvd = left_points + right_points;
}

void update_model(UIState *s,
                  const cereal::ModelDataV2::Reader &model,
                  const cereal::UiPlan::Reader &plan) {
  UIScene &scene = s->scene;
  auto plan_position = plan.getPosition();
  if (plan_position.getX().size() < TRAJECTORY_SIZE){
    plan_position = model.getPosition();
  }
  float max_distance = scene.unlimited_road_ui_length ? plan_position.getX()[TRAJECTORY_SIZE - 1] : std::clamp(plan_position.getX()[TRAJECTORY_SIZE - 1],
                                                                                                               MIN_DRAW_DISTANCE, MAX_DRAW_DISTANCE);

  // update lane lines
  const auto lane_lines = model.getLaneLines();
  const auto lane_line_probs = model.getLaneLineProbs();
  int max_idx = get_path_length_idx(lane_lines[0], max_distance);
  for (int i = 0; i < std::size(scene.lane_line_vertices); i++) {
    scene.lane_line_probs[i] = lane_line_probs[i];
    update_line_data(s, lane_lines[i], scene.custom_road_ui ? scene.lane_line_width * scene.lane_line_probs[i] : 0.025 * scene.lane_line_probs[i], 0, &scene.lane_line_vertices[i], max_idx);
  }

  // update road edges
  const auto road_edges = model.getRoadEdges();
  const auto road_edge_stds = model.getRoadEdgeStds();
  for (int i = 0; i < std::size(scene.road_edge_vertices); i++) {
    scene.road_edge_stds[i] = road_edge_stds[i];
    update_line_data(s, road_edges[i], scene.custom_road_ui ? scene.road_edge_width : 0.025, 0, &scene.road_edge_vertices[i], max_idx);
  }

  // update path
  auto lead_one = (*s->sm)["radarState"].getRadarState().getLeadOne();
  if (lead_one.getStatus()) {
    const float lead_d = lead_one.getDRel() * 2.;
    max_distance = std::clamp((float)(lead_d - fmin(lead_d * 0.35, 10.)), 0.0f, max_distance);
  }
  max_idx = get_path_length_idx(plan_position, max_distance);
  update_line_data(s, plan_position, scene.custom_road_ui ? scene.path_width * (1 - scene.path_edge_width / 100) : 0.9, 1.22, &scene.track_vertices, max_idx, false);

  // update path edges
  update_line_data(s, plan_position, scene.custom_road_ui ? scene.path_width : 0, 1.22, &scene.track_edge_vertices, max_idx, false);

  // update left adjacent path
  update_line_data(s, lane_lines[4], scene.blind_spot_path || scene.developer_ui ? scene.lane_width_left / 2 : 0, 0, &scene.track_left_adjacent_lane_vertices, max_idx);

  // update right adjacent path
  update_line_data(s, lane_lines[5], scene.blind_spot_path || scene.developer_ui ? scene.lane_width_right / 2 : 0, 0, &scene.track_right_adjacent_lane_vertices, max_idx);
}

void update_dmonitoring(UIState *s, const cereal::DriverStateV2::Reader &driverstate, float dm_fade_state, bool is_rhd) {
  UIScene &scene = s->scene;
  const auto driver_orient = is_rhd ? driverstate.getRightDriverData().getFaceOrientation() : driverstate.getLeftDriverData().getFaceOrientation();
  for (int i = 0; i < std::size(scene.driver_pose_vals); i++) {
    float v_this = (i == 0 ? (driver_orient[i] < 0 ? 0.7 : 0.9) : 0.4) * driver_orient[i];
    scene.driver_pose_diff[i] = fabs(scene.driver_pose_vals[i] - v_this);
    scene.driver_pose_vals[i] = 0.8 * v_this + (1 - 0.8) * scene.driver_pose_vals[i];
    scene.driver_pose_sins[i] = sinf(scene.driver_pose_vals[i]*(1.0-dm_fade_state));
    scene.driver_pose_coss[i] = cosf(scene.driver_pose_vals[i]*(1.0-dm_fade_state));
  }

  const mat3 r_xyz = (mat3){{
    scene.driver_pose_coss[1]*scene.driver_pose_coss[2],
    scene.driver_pose_coss[1]*scene.driver_pose_sins[2],
    -scene.driver_pose_sins[1],

    -scene.driver_pose_sins[0]*scene.driver_pose_sins[1]*scene.driver_pose_coss[2] - scene.driver_pose_coss[0]*scene.driver_pose_sins[2],
    -scene.driver_pose_sins[0]*scene.driver_pose_sins[1]*scene.driver_pose_sins[2] + scene.driver_pose_coss[0]*scene.driver_pose_coss[2],
    -scene.driver_pose_sins[0]*scene.driver_pose_coss[1],

    scene.driver_pose_coss[0]*scene.driver_pose_sins[1]*scene.driver_pose_coss[2] - scene.driver_pose_sins[0]*scene.driver_pose_sins[2],
    scene.driver_pose_coss[0]*scene.driver_pose_sins[1]*scene.driver_pose_sins[2] + scene.driver_pose_sins[0]*scene.driver_pose_coss[2],
    scene.driver_pose_coss[0]*scene.driver_pose_coss[1],
  }};

  // transform vertices
  for (int kpi = 0; kpi < std::size(default_face_kpts_3d); kpi++) {
    vec3 kpt_this = default_face_kpts_3d[kpi];
    kpt_this = matvecmul3(r_xyz, kpt_this);
    scene.face_kpts_draw[kpi] = (vec3){{(float)kpt_this.v[0], (float)kpt_this.v[1], (float)(kpt_this.v[2] * (1.0-dm_fade_state) + 8 * dm_fade_state)}};
  }
}

static void update_sockets(UIState *s) {
  s->sm->update(0);
}

static void update_state(UIState *s) {
  SubMaster &sm = *(s->sm);
  UIScene &scene = s->scene;

  if (sm.updated("liveCalibration")) {
    auto live_calib = sm["liveCalibration"].getLiveCalibration();
    auto rpy_list = live_calib.getRpyCalib();
    auto wfde_list = live_calib.getWideFromDeviceEuler();
    Eigen::Vector3d rpy;
    Eigen::Vector3d wfde;
    if (rpy_list.size() == 3) rpy << rpy_list[0], rpy_list[1], rpy_list[2];
    if (wfde_list.size() == 3) wfde << wfde_list[0], wfde_list[1], wfde_list[2];
    Eigen::Matrix3d device_from_calib = euler2rot(rpy);
    Eigen::Matrix3d wide_from_device = euler2rot(wfde);
    Eigen::Matrix3d view_from_device;
    view_from_device << 0, 1, 0,
                        0, 0, 1,
                        1, 0, 0;
    Eigen::Matrix3d view_from_calib = view_from_device * device_from_calib;
    Eigen::Matrix3d view_from_wide_calib = view_from_device * wide_from_device * device_from_calib;
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        scene.view_from_calib.v[i*3 + j] = view_from_calib(i, j);
        scene.view_from_wide_calib.v[i*3 + j] = view_from_wide_calib(i, j);
      }
    }
    scene.calibration_valid = live_calib.getCalStatus() == cereal::LiveCalibrationData::Status::CALIBRATED;
    scene.calibration_wide_valid = wfde_list.size() == 3;
  }
  if (sm.updated("pandaStates")) {
    auto pandaStates = sm["pandaStates"].getPandaStates();
    if (pandaStates.size() > 0) {
      scene.pandaType = pandaStates[0].getPandaType();

      if (scene.pandaType != cereal::PandaState::PandaType::UNKNOWN) {
        scene.ignition = false;
        for (const auto& pandaState : pandaStates) {
          scene.ignition |= pandaState.getIgnitionLine() || pandaState.getIgnitionCan();
        }
      }
    }
  } else if ((s->sm->frame - s->sm->rcv_frame("pandaStates")) > 5*UI_FREQ) {
    scene.pandaType = cereal::PandaState::PandaType::UNKNOWN;
  }
  if (sm.updated("carControl")) {
    const auto carControl = sm["carControl"].getCarControl();
    if (scene.always_on_lateral) {
      scene.always_on_lateral_active = !scene.enabled && carControl.getAlwaysOnLateral();
    }
  }
  if (sm.updated("carParams")) {
    const auto carParams = sm["carParams"].getCarParams();
    scene.always_on_lateral = carParams.getAlwaysOnLateral();
    scene.longitudinal_control = carParams.getOpenpilotLongitudinalControl();
    if (scene.longitudinal_control) {
      scene.conditional_experimental = carParams.getConditionalExperimentalMode();
      scene.driving_personalities_ui_wheel = carParams.getDrivingPersonalitiesUIWheel();
      scene.experimental_mode_via_wheel = carParams.getExperimentalModeViaWheel();
    }
  }
  if (sm.updated("carState")) {
    const auto carState = sm["carState"].getCarState();
    if (scene.blind_spot_path || scene.frog_signals) {
      scene.blind_spot_left = carState.getLeftBlindspot();
      scene.blind_spot_right = carState.getRightBlindspot();
    }
    if (scene.developer_ui || scene.frog_signals) {
      scene.turn_signal_left = carState.getLeftBlinker();
      scene.turn_signal_right = carState.getRightBlinker();
    }
    if (scene.blind_spot_path || scene.developer_ui || scene.rotating_wheel) {
      scene.steering_angle_deg = carState.getSteeringAngleDeg();
    }
    if (scene.started) {
      scene.toyota_car = carState.getToyotaCar();
    }
  }
  if (sm.updated("controlsState")) {
    const auto controlsState = sm["controlsState"].getControlsState();
    scene.enabled = controlsState.getEnabled();
    scene.experimental_mode = controlsState.getExperimentalMode();
  }
  if (sm.updated("gpsLocationExternal")) {
    const auto gpsLocationExternal = sm["gpsLocationExternal"].getGpsLocationExternal();
    if (scene.compass) {
      scene.bearing_deg = gpsLocationExternal.getBearingDeg();
    }
  }
  if (sm.updated("lateralPlan")) {
    const auto lateralPlan = sm["lateralPlan"].getLateralPlan();
    if (scene.blind_spot_path || scene.developer_ui) {
      scene.lane_width_left = lateralPlan.getLaneWidthLeft();
      scene.lane_width_right = lateralPlan.getLaneWidthRight();
    }
  }
  if (sm.updated("longitudinalPlan")) {
    const auto longitudinalPlan = sm["longitudinalPlan"].getLongitudinalPlan();
    if (scene.developer_ui) {
      scene.desired_follow = longitudinalPlan.getDesiredFollowDistance();
      scene.obstacle_distance = longitudinalPlan.getSafeObstacleDistance();
      scene.obstacle_distance_stock = longitudinalPlan.getSafeObstacleDistanceStock();
      scene.stopped_equivalence = longitudinalPlan.getStoppedEquivalenceFactor();
      scene.stopped_equivalence_stock = longitudinalPlan.getStoppedEquivalenceFactorStock();
    }
  }
  if (sm.updated("wideRoadCameraState")) {
    auto cam_state = sm["wideRoadCameraState"].getWideRoadCameraState();
    float scale = (cam_state.getSensor() == cereal::FrameData::ImageSensor::AR0231) ? 6.0f : 1.0f;
    scene.light_sensor = std::max(100.0f - scale * cam_state.getExposureValPercent(), 0.0f);
  }
  scene.started = sm["deviceState"].getDeviceState().getStarted() && scene.ignition;
}

void ui_update_params(UIState *s) {
  auto params = Params();
  s->scene.is_metric = params.getBool("IsMetric");
  s->scene.map_on_left = params.getBool("NavSettingLeftSide");

  // FrogPilot variables
  static UIScene &scene = s->scene;
  static bool toggles_checked = false;
  static float conversion = scene.is_metric ? 0.06 : 0.1524;
  if (!scene.default_params_set) {
    scene.default_params_set = params.getBool("DefaultParamsSet");
  }
  if (!toggles_checked && scene.default_params_set) {
    scene.custom_theme = params.getBool("CustomTheme");

    scene.custom_colors = scene.custom_theme ? params.getInt("CustomColors") : 0;
    scene.frog_colors = scene.custom_colors == 1;

    scene.custom_signals = scene.custom_theme ? params.getInt("CustomSignals") : 0;
    scene.frog_signals = scene.custom_signals == 1;

    scene.compass = params.getBool("Compass");
    scene.conditional_speed = params.getInt("ConditionalExperimentalModeSpeed");
    scene.conditional_speed_lead = params.getInt("ConditionalExperimentalModeSpeedLead");
    scene.custom_road_ui = params.getBool("CustomRoadUI");
    scene.acceleration_path = scene.custom_road_ui && params.getBool("AccelerationPath");
    scene.blind_spot_path = scene.custom_road_ui && params.getBool("BlindSpotPath");
    scene.developer_ui = params.getInt("DeveloperUI");
    scene.lane_line_width = params.getInt("LaneLinesWidth") / 12.0 * conversion;
    scene.path_edge_width = params.getInt("PathEdgeWidth");
    scene.path_width = params.getInt("PathWidth") / 10.0 * (scene.is_metric ? 0.5 : 0.1524);
    scene.road_edge_width = params.getInt("RoadEdgesWidth") / 12.0 * conversion;
    scene.unlimited_road_ui_length = scene.custom_road_ui && params.getBool("UnlimitedLength");

    scene.mute_dm = params.getBool("FireTheBabysitter") && params.getBool("MuteDM");
    scene.personality_profile = params.getInt("LongitudinalPersonality");
    scene.rotating_wheel = params.getBool("RotatingWheel");
    scene.screen_brightness = params.getInt("ScreenBrightness");
    scene.steering_wheel = params.getInt("SteeringWheel");
    scene.wide_camera_disabled = params.getBool("WideCameraDisable");
    toggles_checked = true;
  }
}

void ui_update_live_params(UIState *s) {
  static UIScene &scene = s->scene;
  static auto params = Params();
  static Params params_memory = Params("/dev/shm/params");
  static float conversion = scene.is_metric ? 0.06 : 0.1524;

  // Update FrogPilot variables when they are changed
  static bool live_toggles_checked = false;
  if (params_memory.getBool("FrogPilotTogglesUpdated")) {
    if (scene.conditional_experimental) {
      scene.conditional_speed = params.getInt("ConditionalExperimentalModeSpeed");
      scene.conditional_speed_lead = params.getInt("ConditionalExperimentalModeSpeedLead");
    }
    if (scene.custom_theme) {
      scene.custom_colors = params.getInt("CustomColors");
      scene.frog_colors = scene.custom_colors == 1;

      scene.custom_signals = params.getInt("CustomSignals");
      scene.frog_signals = scene.custom_signals == 1;
    }
    if (scene.custom_road_ui) {
      scene.lane_line_width = params.getInt("LaneLinesWidth") / 12.0 * conversion;
      scene.path_edge_width = params.getInt("PathEdgeWidth");
      scene.path_width = params.getInt("PathWidth") / 10.0 * (scene.is_metric ? 0.5 : 0.1524);
      scene.road_edge_width = params.getInt("RoadEdgesWidth") / 12.0 * conversion;
    }
    scene.developer_ui = params.getInt("DeveloperUI");
    if (scene.driving_personalities_ui_wheel && !scene.toyota_car) {
      scene.personality_profile = params.getInt("LongitudinalPersonality");
    }
    scene.screen_brightness = params.getInt("ScreenBrightness");
    scene.steering_wheel = params.getInt("SteeringWheel");
    if (live_toggles_checked && scene.enabled) {
      params_memory.putBool("FrogPilotTogglesUpdated", false);
    }
    live_toggles_checked = !live_toggles_checked;
  }

  // FrogPilot live variables that need to be constantly checked
  if (scene.conditional_experimental) {
    scene.conditional_status = params_memory.getInt("ConditionalStatus");
  }
  scene.map_open = params_memory.getBool("MapOpen");
}

void UIState::updateStatus() {
  if (scene.started && sm->updated("controlsState")) {
    auto controls_state = (*sm)["controlsState"].getControlsState();
    auto state = controls_state.getState();
    if (state == cereal::ControlsState::OpenpilotState::PRE_ENABLED || state == cereal::ControlsState::OpenpilotState::OVERRIDING) {
      status = STATUS_OVERRIDE;
    } else if (scene.always_on_lateral_active) {
      status = STATUS_LATERAL_ACTIVE;
    } else {
      status = controls_state.getEnabled() ? STATUS_ENGAGED : STATUS_DISENGAGED;
    }
  }

  // Handle onroad/offroad transition
  if (scene.started != started_prev || sm->frame == 1) {
    if (scene.started) {
      status = STATUS_DISENGAGED;
      scene.started_frame = sm->frame;
    }
    started_prev = scene.started;
    emit offroadTransition(!scene.started);
  }
}

UIState::UIState(QObject *parent) : QObject(parent) {
  sm = std::make_unique<SubMaster, const std::initializer_list<const char *>>({
    "modelV2", "controlsState", "liveCalibration", "radarState", "deviceState", "roadCameraState",
    "pandaStates", "carParams", "driverMonitoringState", "carState", "liveLocationKalman", "driverStateV2",
    "wideRoadCameraState", "managerState", "navInstruction", "navRoute", "uiPlan", "carControl",
    "gpsLocationExternal", "lateralPlan", "longitudinalPlan"
  });

  Params params;
  language = QString::fromStdString(params.get("LanguageSetting"));
  auto prime_value = params.get("PrimeType");
  if (!prime_value.empty()) {
    prime_type = static_cast<PrimeType>(std::atoi(prime_value.c_str()));
  }

  // update timer
  timer = new QTimer(this);
  QObject::connect(timer, &QTimer::timeout, this, &UIState::update);
  timer->start(1000 / UI_FREQ);
}

void UIState::update() {
  update_sockets(this);
  update_state(this);
  updateStatus();
  ui_update_live_params(uiState());

  if (sm->frame % UI_FREQ == 0) {
    watchdog_kick(nanos_since_boot());
  }
  emit uiUpdate(*this);
}

void UIState::setPrimeType(PrimeType type) {
  if (type != prime_type) {
    bool prev_prime = hasPrime();

    prime_type = type;
    Params().put("PrimeType", std::to_string(prime_type));
    emit primeTypeChanged(prime_type);

    bool prime = hasPrime();
    if (prev_prime != prime) {
      emit primeChanged(prime);
    }
  }
}

Device::Device(QObject *parent) : brightness_filter(BACKLIGHT_OFFROAD, BACKLIGHT_TS, BACKLIGHT_DT), QObject(parent) {
  setAwake(true);
  resetInteractiveTimeout();

  QObject::connect(uiState(), &UIState::uiUpdate, this, &Device::update);
}

void Device::update(const UIState &s) {
  updateBrightness(s);
  updateWakefulness(s);
}

void Device::setAwake(bool on) {
  if (on != awake) {
    awake = on;
    Hardware::set_display_power(awake);
    LOGD("setting display power %d", awake);
    emit displayPowerChanged(awake);
  }
}

void Device::resetInteractiveTimeout(int timeout) {
  if (timeout == -1) {
    timeout = (ignition_on ? 10 : 30);
  }
  interactive_timeout = timeout * UI_FREQ;
}

void Device::updateBrightness(const UIState &s) {
  float clipped_brightness = offroad_brightness;
  if (s.scene.started) {
    clipped_brightness = s.scene.light_sensor;

    // CIE 1931 - https://www.photonstophotos.net/GeneralTopics/Exposure/Psychometric_Lightness_and_Gamma.htm
    if (clipped_brightness <= 8) {
      clipped_brightness = (clipped_brightness / 903.3);
    } else {
      clipped_brightness = std::pow((clipped_brightness + 16.0) / 116.0, 3.0);
    }

    // Scale back to 10% to 100%
    clipped_brightness = std::clamp(100.0f * clipped_brightness, 10.0f, 100.0f);
  }

  int brightness = brightness_filter.update(clipped_brightness);
  if (!awake) {
    brightness = 0;
  } else if (s.scene.screen_brightness <= 100) {
    // Bring the screen brightness up to 5% upon screen tap
    brightness = fmax(5, s.scene.screen_brightness);
  }

  if (brightness != last_brightness) {
    if (!brightness_future.isRunning()) {
      brightness_future = QtConcurrent::run(Hardware::set_brightness, brightness);
      last_brightness = brightness;
    }
  }
}

void Device::updateWakefulness(const UIState &s) {
  bool ignition_just_turned_off = !s.scene.ignition && ignition_on;
  ignition_on = s.scene.ignition;

  if (ignition_just_turned_off) {
    resetInteractiveTimeout();
  } else if (interactive_timeout > 0 && --interactive_timeout == 0) {
    emit interactiveTimeout();
  }

  if (s.scene.screen_brightness != 0) {
    setAwake(s.scene.ignition || interactive_timeout > 0);
  } else {
    setAwake(interactive_timeout > 0);
  }
}

UIState *uiState() {
  static UIState ui_state;
  return &ui_state;
}

Device *device() {
  static Device _device;
  return &_device;
}
