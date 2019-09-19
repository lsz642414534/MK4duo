/**
 * MK4duo Firmware for 3D Printer, Laser and CNC
 *
 * Based on Marlin, Sprinter and grbl
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 * Copyright (c) 2019 Alberto Cotronei @MagoKimbra
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * delta_mechanics.cpp
 *
 * Copyright (c) 2019 Alberto Cotronei @MagoKimbra
 */

#include "../../../MK4duo.h"

#if MECH(DELTA)

Delta_Mechanics mechanics;

#if ENABLED(DELTA_FAST_SQRT) && ENABLED(__AVR__)
  #define _SQRT(n) (1.0f / Q_rsqrt(n))
#else
  #define _SQRT(n) SQRT(n)
#endif

/** Public Parameters */
mechanics_data_t Delta_Mechanics::data;

abc_float_t Delta_Mechanics::delta{0.0f};
float       Delta_Mechanics::delta_clip_start_height = 0;

/** Private Parameters */
abc_float_t Delta_Mechanics::D2{0.0f},      // Diagonal rod ^2
            Delta_Mechanics::towerX{0.0f},  // The X coordinate of each tower
            Delta_Mechanics::towerY{0.0f};  // The Y coordinate of each tower
float       Delta_Mechanics::Xbc    = 0.0,
            Delta_Mechanics::Xca    = 0.0,
            Delta_Mechanics::Xab    = 0.0,
            Delta_Mechanics::Ybc    = 0.0,
            Delta_Mechanics::Yca    = 0.0,
            Delta_Mechanics::Yab    = 0.0,
            Delta_Mechanics::coreKa = 0.0,
            Delta_Mechanics::coreKb = 0.0,
            Delta_Mechanics::coreKc = 0.0,
            Delta_Mechanics::Q      = 0.0,
            Delta_Mechanics::Q2     = 0.0;

/** Public Function */
void Delta_Mechanics::factory_parameters() {

  static const float    tmp_step[]          PROGMEM = DEFAULT_AXIS_STEPS_PER_UNIT,
                        tmp_maxfeedrate[]   PROGMEM = DEFAULT_MAX_FEEDRATE;

  static const uint32_t tmp_maxacc[]        PROGMEM = DEFAULT_MAX_ACCELERATION,
                        tmp_retract[]       PROGMEM = DEFAULT_RETRACT_ACCELERATION;

  LOOP_XYZE_N(i) {
    data.axis_steps_per_mm[i]           = pgm_read_float(&tmp_step[i < COUNT(tmp_step) ? i : COUNT(tmp_step) - 1]);
    data.max_feedrate_mm_s[i]           = pgm_read_float(&tmp_maxfeedrate[i < COUNT(tmp_maxfeedrate) ? i : COUNT(tmp_maxfeedrate) - 1]);
    data.max_acceleration_mm_per_s2[i]  = pgm_read_dword_near(&tmp_maxacc[i < COUNT(tmp_maxacc) ? i : COUNT(tmp_maxacc) - 1]);
  }

  LOOP_EXTRUDER()
    data.retract_acceleration[e]        = pgm_read_dword_near(&tmp_retract[e < COUNT(tmp_retract) ? e : COUNT(tmp_retract) - 1]);

  data.acceleration               = DEFAULT_ACCELERATION;
  data.travel_acceleration        = DEFAULT_TRAVEL_ACCELERATION;
  data.min_feedrate_mm_s          = DEFAULT_MIN_FEEDRATE;
  data.min_segment_time_us        = DEFAULT_MIN_SEGMENT_TIME;
  data.min_travel_feedrate_mm_s   = DEFAULT_MIN_TRAVEL_FEEDRATE;

  #if ENABLED(JUNCTION_DEVIATION)
    data.junction_deviation_mm = float(JUNCTION_DEVIATION_MM);
  #endif

  static const float tmp_ejerk[] PROGMEM = DEFAULT_EJERK;
  data.max_jerk.set(DEFAULT_XJERK, DEFAULT_YJERK, DEFAULT_ZJERK);
  #if DISABLED(JUNCTION_DEVIATION) || DISABLED(LIN_ADVANCE)
    LOOP_EXTRUDER()
      data.max_jerk.e[e] = pgm_read_float(&tmp_ejerk[e < COUNT(tmp_ejerk) ? e : COUNT(tmp_ejerk) - 1]);
  #endif

  data.diagonal_rod         = DELTA_DIAGONAL_ROD;
  data.radius               = DELTA_RADIUS;
  data.segments_per_second  = DELTA_SEGMENTS_PER_SECOND;
  data.segments_per_line    = DELTA_SEGMENTS_PER_LINE;
  data.print_radius         = DELTA_PRINTABLE_RADIUS;
  data.probe_radius         = DELTA_PROBEABLE_RADIUS;
  data.height               = DELTA_HEIGHT;
  delta_clip_start_height   = DELTA_HEIGHT;

  data.endstop_adj.set(TOWER_A_ENDSTOP_ADJ, TOWER_B_ENDSTOP_ADJ, TOWER_C_ENDSTOP_ADJ);
  data.tower_angle_adj.set(TOWER_A_ANGLE_ADJ, TOWER_B_ANGLE_ADJ, TOWER_C_ANGLE_ADJ);
  data.tower_radius_adj.set(TOWER_A_RADIUS_ADJ, TOWER_B_RADIUS_ADJ, TOWER_C_RADIUS_ADJ);
  data.diagonal_rod_adj.set(TOWER_A_DIAGROD_ADJ, TOWER_B_DIAGROD_ADJ, TOWER_C_DIAGROD_ADJ);

}

/**
 * Get the stepper positions in the cartesian_position[] array.
 * Forward kinematics are applied for DELTA.
 *
 * The result is in the current coordinate space with
 * leveling applied. The coordinates need to be run through
 * unapply_leveling to obtain the "ideal" coordinates
 * suitable for current_position, etc.
 */
void Delta_Mechanics::get_cartesian_from_steppers() {
  InverseTransform(
    planner.get_axis_position_mm(A_AXIS),
    planner.get_axis_position_mm(B_AXIS),
    planner.get_axis_position_mm(C_AXIS),
    cartesian_position
  );
}

#if DISABLED(AUTO_BED_LEVELING_UBL)

  /**
   * Prepare a linear move in a DELTA setup.
   *
   * This calls buffer_line several times, adding
   * small incremental moves for DELTA.
   */
  bool Delta_Mechanics::prepare_move_to_destination_mech_specific() {

    // Get the top feedrate of the move in the XY plane
    const float _feedrate_mm_s = MMS_SCALED(feedrate_mm_s);

    // Get the cartesian distances moved in XYZE
    const xyze_float_t difference = destination - current_position;

    // If the move is only in Z/E don't split up the move
    if (!difference.x && !difference.y) {
      planner.buffer_line(destination, _feedrate_mm_s, tools.data.extruder.active);
      return false; // caller will update current_position.x
    }

    // Fail if attempting move outside printable radius
    if (endstops.isSoftEndstop() && !position_is_reachable(destination.x, destination.y)) return true;

    // Get the cartesian distance in XYZ
    float cartesian_distance = SQRT(sq(difference.x) + sq(difference.y) + sq(difference.z));

    // If the move is very short, check the E move distance
    if (UNEAR_ZERO(cartesian_distance)) cartesian_distance = ABS(difference.e);

    // No E move either? Game over.
    if (UNEAR_ZERO(cartesian_distance)) return true;

    // Minimum number of seconds to move the given distance
    const float seconds = cartesian_distance / _feedrate_mm_s;

    // The number of segments-per-second times the duration
    // gives the number of segments we should produce
    const uint16_t segments = MAX(1U, data.segments_per_second * seconds);

    // Now compute the number of lines needed
    uint16_t numLines = (segments + data.segments_per_line - 1) / data.segments_per_line;

    // The approximate length of each segment
    const float         inv_numLines = 1.0f / float(numLines),
                        cartesian_segment_mm = cartesian_distance * inv_numLines;
    const xyze_float_t  segment_distance = difference * inv_numLines;

    /*
    DEBUG_MV("mm=", cartesian_distance);
    DEBUG_MV(" seconds=", seconds);
    DEBUG_MV(" segments=", segments);
    DEBUG_MV(" numLines=", numLines);
    DEBUG_MV(" segment_mm=", cartesian_segment_mm);
    DEBUG_EOL();
    //*/

    // Get the current position as starting point
    xyze_pos_t raw = current_position;

    // Calculate and execute the segments
    while (--numLines) {

      static millis_s next_idle_ms = 0;
      if (expired(&next_idle_ms, 200U)) printer.idle();

      raw += segment_distance;

      if (!planner.buffer_line(raw, _feedrate_mm_s, tools.data.extruder.active, cartesian_segment_mm))
        break;

    }

    planner.buffer_line(destination, _feedrate_mm_s, tools.data.extruder.active, cartesian_segment_mm);

    return false; // caller will update current_position.x

  }

#endif // DISABLED(AUTO_BED_LEVELING_UBL)

/**
 *  Plan a move to (X, Y, Z) and set the current_position.x
 *  The final current_position.x may not be the one that was requested
 */
void Delta_Mechanics::do_blocking_move_to(const float rx, const float ry, const float rz, const float &fr_mm_s /*=0.0*/) {

  if (printer.debugFeature()) DEBUG_XYZ(">>> do_blocking_move_to", rx, ry, rz);

  const float z_feedrate  = fr_mm_s ? fr_mm_s : homing_feedrate_mm_s.z,
              xy_feedrate = fr_mm_s ? fr_mm_s : XY_PROBE_FEEDRATE_MM_S;

  if (!position_is_reachable(rx, ry)) return;

  REMEMBER(fr, feedrate_mm_s, xy_feedrate);

  destination = current_position;          // sync destination at the start

  if (printer.debugFeature()) DEBUG_POS("set_destination_to_current", destination);

  // when in the danger zone
  if (current_position.z > delta_clip_start_height) {
    if (rz > delta_clip_start_height) {   // staying in the danger zone
      destination.set(rx, ry, rz);        // move directly (uninterpolated)
      prepare_uninterpolated_move_to_destination(); // set_current_to_destination
      if (printer.debugFeature()) DEBUG_POS("danger zone move", current_position);
      return;
    }
    destination.z = delta_clip_start_height;
    prepare_uninterpolated_move_to_destination(); // set_current_to_destination
    if (printer.debugFeature()) DEBUG_POS("zone border move", current_position);
  }

  if (rz > current_position.z) {    // raising?
    destination.z = rz;
    prepare_uninterpolated_move_to_destination(z_feedrate);   // set_current_to_destination
    if (printer.debugFeature()) DEBUG_POS("z raise move", current_position);
  }

  destination.set(rx, ry);
  prepare_move_to_destination();         // set_current_to_destination
  if (printer.debugFeature()) DEBUG_POS("xy move", current_position);

  if (rz < current_position.z) {    // lowering?
    destination.z = rz;
    prepare_uninterpolated_move_to_destination(z_feedrate);   // set_current_to_destination
    if (printer.debugFeature()) DEBUG_POS("z lower move", current_position);
  }

  if (printer.debugFeature()) DEBUG_EM("<<< do_blocking_move_to");

  planner.synchronize();

}
void Delta_Mechanics::do_blocking_move_to_x(const float &rx, const float &fr_mm_s/*=0.0*/) {
  do_blocking_move_to(rx, current_position.y, current_position.z, fr_mm_s);
}
void Delta_Mechanics::do_blocking_move_to_y(const float &ry, const float &fr_mm_s/*=0.0*/) {
  do_blocking_move_to(current_position.x, ry, current_position.z, fr_mm_s);
}
void Delta_Mechanics::do_blocking_move_to_z(const float &rz, const float &fr_mm_s/*=0.0*/) {
  do_blocking_move_to(current_position.x, current_position.y, rz, fr_mm_s);
}
void Delta_Mechanics::do_blocking_move_to_xy(const float &rx, const float &ry, const float &fr_mm_s/*=0.0*/) {
  do_blocking_move_to(rx, ry, current_position.z, fr_mm_s);
}

/**
 * Delta InverseTransform
 *
 * See the Wikipedia article "Trilateration"
 * https://en.wikipedia.org/wiki/Trilateration
 *
 * Establish a new coordinate system in the plane of the
 * three carriage points. This system has its origin at
 * tower1, with tower2 on the X axis. Tower3 is in the X-Y
 * plane with a Z component of zero.
 * We will define unit vectors in this coordinate system
 * in our original coordinate system. Then when we calculate
 * the Xnew, Ynew and Znew values, we can translate back into
 * the original system by moving along those unit vectors
 * by the corresponding values.
 *
 * Variable names matched to Mk4duo, c-version, and avoid the
 * use of any vector library.
 *
 * by Andreas Hardtung 07-06-2016
 * based on a Java function from "Delta Robot Mechanics V3"
 * by Steve Graves
 *
 * The result is stored in the cartesian[] array.
 */
void Delta_Mechanics::InverseTransform(const float Ha, const float Hb, const float Hc, xyz_pos_t &cartesian) {

  // Calculate RSUT such that x = (Uz + S)/Q, y = -(Rz + T)/Q
  const float R = 2 * ((Xbc * Ha) + (Xca * Hb) + (Xab * Hc)),
              U = 2 * ((Ybc * Ha) + (Yca * Hb) + (Yab * Hc));

  const float Ka = coreKa + (sq(Hc) - sq(Hb)),
              Kb = coreKb + (sq(Ha) - sq(Hc)),
              Kc = coreKc + (sq(Hb) - sq(Ha));

  const float S = Ka * towerY.a + Kb * towerY.b + Kc * towerY.c,
              T = Ka * towerX.a + Kb * towerX.b + Kc * towerX.c;

  const float A = sq(U) + sq(R) + Q2;
  
  const float minusHalfB =  Q2 * Ha + Q * (U * towerX.a - R * towerY.a) - (R * T + U * S),
              C = sq(towerX.a * Q - S) + sq(towerY.a * Q + T) + (sq(Ha) - D2.a) * Q2;

  const float z = (minusHalfB - SQRT(sq(minusHalfB) - A * C)) / A;

  cartesian.x =  (U * z + S) / Q;
  cartesian.y = -(R * z + T) / Q;
  cartesian.z = z;
}

/**
 * Delta Transform
 *
 * Calculate the tower positions for a given machine
 * position, storing the result in the delta[] array.
 *
 * This is an expensive calculation, requiring 3 square
 * roots per segmented linear move, and strains the limits
 * of a Mega2560 with a Graphical Display.
 */
void Delta_Mechanics::Transform(const xyz_pos_t &raw) {

  #if HOTENDS > 1
    // Delta hotend offsets must be applied in Cartesian space
    const xyz_pos_t pos = { raw.x - nozzle.data.hotend_offset[ACTIVE_HOTEND].x,
                            raw.y - nozzle.data.hotend_offset[ACTIVE_HOTEND].y,
                            raw.z
    };
    delta.a = pos.z + _SQRT(D2.a - sq(pos.x - towerX.a) - sq(pos.y - towerY.a));
    delta.b = pos.z + _SQRT(D2.b - sq(pos.x - towerX.b) - sq(pos.y - towerY.b));
    delta.c = pos.z + _SQRT(D2.c - sq(pos.x - towerX.c) - sq(pos.y - towerY.c));
  #else
    delta.a = raw.z + _SQRT(D2.a - sq(raw.x - towerX.a) - sq(raw.y - towerY.a));
    delta.b = raw.z + _SQRT(D2.b - sq(raw.x - towerX.b) - sq(raw.y - towerY.b));
    delta.c = raw.z + _SQRT(D2.c - sq(raw.x - towerX.c) - sq(raw.y - towerY.c));
  #endif

}

void Delta_Mechanics::recalc_delta_settings() {

  // Get a minimum radius for clamping
  endstops.soft_endstop_radius_2 = sq(data.print_radius);

  D2.a = sq(data.diagonal_rod + data.diagonal_rod_adj.a);
  D2.b = sq(data.diagonal_rod + data.diagonal_rod_adj.b);
  D2.c = sq(data.diagonal_rod + data.diagonal_rod_adj.c);

  // Effective X/Y positions of the three vertical towers.
  towerX.a = COS(RADIANS(210 + data.tower_angle_adj.a)) * (data.radius + data.tower_radius_adj.a); // front left tower
  towerY.a = SIN(RADIANS(210 + data.tower_angle_adj.a)) * (data.radius + data.tower_radius_adj.a);
  towerX.b = COS(RADIANS(330 + data.tower_angle_adj.b)) * (data.radius + data.tower_radius_adj.b); // front right tower
  towerY.b = SIN(RADIANS(330 + data.tower_angle_adj.b)) * (data.radius + data.tower_radius_adj.b);
  towerX.c = COS(RADIANS( 90 + data.tower_angle_adj.c)) * (data.radius + data.tower_radius_adj.c); // back middle tower
  towerY.c = SIN(RADIANS( 90 + data.tower_angle_adj.c)) * (data.radius + data.tower_radius_adj.c);

  Xbc = towerX.c - towerX.b;
  Xca = towerX.a - towerX.c;
  Xab = towerX.b - towerX.a;
  Ybc = towerY.c - towerY.b;
  Yca = towerY.a - towerY.c;
  Yab = towerY.b - towerY.a;

  Q = 2 * (Xab * towerY.c + Xca * towerY.b + Xbc * towerY.a);
  Q2 = sq(Q);

  const float coreFa = sq(towerX.a) + sq(towerY.a),
              coreFb = sq(towerX.b) + sq(towerY.b),
              coreFc = sq(towerX.c) + sq(towerY.c);

  coreKa = (D2.b - D2.c) + (coreFc - coreFb);
	coreKb = (D2.c - D2.a) + (coreFa - coreFc);
	coreKc = (D2.a - D2.b) + (coreFb - coreFa);

  NOMORE(data.probe_radius, data.print_radius);

  unsetHomedAll();

  Set_clip_start_height();

}

/**
 * Home Delta
 */
void Delta_Mechanics::home(const bool report_position/*=true*/) {

  if (printer.debugSimulation()) {
    LOOP_XYZ(axis) set_axis_is_at_home((AxisEnum)axis);
    #if HAS_NEXTION_LCD && ENABLED(NEXTION_GFX)
      nextion_gfx_clear();
    #endif
    return;
  }

  #if HAS_POWER_SWITCH
    powerManager.power_on(); // Power On if power is off
  #endif

  // Wait for planner moves to finish!
  planner.synchronize();

  // Cancel the active G29 session
  #if HAS_LEVELING && HAS_PROBE_MANUALLY
    bedlevel.flag.g29_in_progress = false;
  #endif

  // Disable the leveling matrix before homing
  #if HAS_LEVELING
    bedlevel.set_bed_leveling_enabled(false);
  #endif

  // Reduce Acceleration and Jerk for Homing
  #if ENABLED(SLOW_HOMING) || ENABLED(IMPROVE_HOMING_RELIABILITY)
    REMEMBER(accel_x, data.max_acceleration_mm_per_s2.x, 100);
    REMEMBER(accel_y, data.max_acceleration_mm_per_s2.y, 100);
    REMEMBER(accel_z, data.max_acceleration_mm_per_s2.z, 100);
    #if HAS_CLASSIC_JERK
      REMEMBER(jerk_x, data.max_jerk.x, 0);
      REMEMBER(jerk_y, data.max_jerk.y, 0);
      REMEMBER(jerk_z, data.max_jerk.z, 0);
    #endif
    planner.reset_acceleration_rates();
  #endif

  // Always home with tool 0 active
  #if HOTENDS > 1
    const uint8_t old_tool_index = tools.data.extruder.active;
    tools.change(0, true);
  #endif

  setup_for_endstop_or_probe_move();
  endstops.setEnabled(true); // Enable endstops for next homing move

  bool come_back = parser.boolval('B');
  REMEMBER(fr, feedrate_mm_s);
  stored_position[0] = current_position;

  if (printer.debugFeature()) DEBUG_POS(">>> home", current_position);

  // Init the current position of all carriages to 0,0,0
  current_position.reset();
  destination.reset();
  sync_plan_position();

  // Disable stealthChop if used. Enable diag1 pin on driver.
  #if ENABLED(SENSORLESS_HOMING)
    sensorless_flag_t stealth_states;
    stealth_states.x = tmc.enable_stallguard(X_DRV);
    stealth_states.y = tmc.enable_stallguard(Y_DRV);
    stealth_states.z = tmc.enable_stallguard(Z_DRV);
  #endif

  // Move all carriages together linearly until an endstop is hit.
  destination.z = data.height + 10;
  planner.buffer_line(destination, homing_feedrate_mm_s.z, tools.data.extruder.active);
  planner.synchronize();

  // Re-enable stealthChop if used. Disable diag1 pin on driver.
  #if ENABLED(SENSORLESS_HOMING)
    tmc.disable_stallguard(X_DRV, stealth_states.x);
    tmc.disable_stallguard(Y_DRV, stealth_states.y);
    tmc.disable_stallguard(Z_DRV, stealth_states.z);
  #endif

  endstops.validate_homing_move();

  // At least one carriage has reached the top.
  // Now re-home each carriage separately.
  homeaxis(A_AXIS);
  homeaxis(B_AXIS);
  homeaxis(C_AXIS);

  // Set all carriages to their home positions
  // Do this here all at once for Delta, because
  // XYZ isn't ABC. Applying this per-tower would
  // give the impression that they are the same.
  LOOP_XYZ(i) set_axis_is_at_home((AxisEnum)i);

  sync_plan_position();

  if (printer.debugFeature()) DEBUG_POS("<<< home", current_position);

  endstops.setNotHoming();

  #if ENABLED(DELTA_HOME_TO_SAFE_ZONE)
    // move to a height where we can use the full xy-area
    do_blocking_move_to_z(delta_clip_start_height, homing_feedrate_mm_s.x);
  #endif

  if (come_back) {
    feedrate_mm_s = homing_feedrate_mm_s.x;
    destination = stored_position[0];
    prepare_move_to_destination();
    RESTORE(fr);
  }

  #if HAS_NEXTION_LCD && ENABLED(NEXTION_GFX)
    nextion_gfx_clear();
  #endif

  // Re-enable bed level correction if it had been on
  #if HAS_LEVELING
    bedlevel.restore_bed_leveling_state();
  #endif

  clean_up_after_endstop_or_probe_move();

  planner.synchronize();

  // Restore the active tool after homing
  #if HOTENDS > 1
    tools.change(old_tool_index, true);
  #endif

  #if ENABLED(SLOW_HOMING) || ENABLED(IMPROVE_HOMING_RELIABILITY)
    RESTORE(accel_x);
    RESTORE(accel_y);
    RESTORE(accel_z);
    #if HAS_CLASSIC_JERK
      RESTORE(jerk_x);
      RESTORE(jerk_y);
      RESTORE(jerk_z);
    #endif
    planner.reset_acceleration_rates();
  #endif

  lcdui.refresh();

  if (report_position) report_current_position();

  if (printer.debugFeature()) DEBUG_EM("<<< G28");

}

/**
 * Home an individual linear axis
 */
void Delta_Mechanics::do_homing_move(const AxisEnum axis, const float distance, const float fr_mm_s/*=0.0*/) {

  if (printer.debugFeature()) {
    DEBUG_MC(">>> do_homing_move(", axis_codes[axis]);
    DEBUG_MV(", ", distance);
    DEBUG_MSG(", ");
    if (fr_mm_s)
      DEBUG_VAL(fr_mm_s);
    else {
      DEBUG_MV(" [", homing_feedrate_mm_s[axis]);
      DEBUG_CHR(']');
    }
    DEBUG_CHR(')');
    DEBUG_EOL();
  }

  // Only do some things when moving towards an endstop
  const bool is_home_dir = distance > 0;

  #if ENABLED(SENSORLESS_HOMING)
    sensorless_flag_t stealth_states;
  #endif

  if (is_home_dir) {
    // Disable stealthChop if used. Enable diag1 pin on driver.
    #if ENABLED(SENSORLESS_HOMING)
      stealth_states = start_sensorless_homing_per_axis(axis);
    #endif
  }

  abce_pos_t target = { planner.get_axis_position_mm(A_AXIS), planner.get_axis_position_mm(B_AXIS), planner.get_axis_position_mm(C_AXIS), planner.get_axis_position_mm(E_AXIS) };
  target[axis] = 0;
  planner.set_machine_position_mm(target);
  target[axis] = distance;

  #if ENABLED(JUNCTION_DEVIATION)
    const xyze_pos_t delta_mm_cart{0};
  #endif

  // Set delta axes directly
  planner.buffer_segment(target
    #if ENABLED(JUNCTION_DEVIATION)
      , delta_mm_cart
    #endif
    , fr_mm_s ? fr_mm_s : homing_feedrate_mm_s[axis], tools.data.extruder.active
  );

  planner.synchronize();

  if (is_home_dir) {
    endstops.validate_homing_move();

    // Re-enable stealthChop if used. Disable diag1 pin on driver.
    #if ENABLED(SENSORLESS_HOMING)
      stop_sensorless_homing_per_axis(axis, stealth_states);
    #endif
  }

  if (printer.debugFeature()) {
    DEBUG_MC("<<< do_homing_move(", axis_codes[axis]);
    DEBUG_CHR(')'); DEBUG_EOL();
  }

}

/**
 * Set an axis' current position to its home position (after homing).
 *
 * DELTA should wait until all homing is done before setting the XYZ
 * current_position.x to home, because homing is a single operation.
 * In the case where the axis positions are already known and previously
 * homed, DELTA could home to X or Y individually by moving either one
 * to the center. However, homing Z always homes XY and Z.
 *
 * Callers must sync the planner position after calling this!
 */
void Delta_Mechanics::set_axis_is_at_home(const AxisEnum axis) {

  if (printer.debugFeature()) {
    DEBUG_MC(">>> set_axis_is_at_home(", axis_codes[axis]);
    DEBUG_CHR(')'); DEBUG_EOL();
  }

  setAxisHomed(axis, true);

  current_position[axis] = (axis == C_AXIS ? data.height : 0.0);

  #if ENABLED(BABYSTEPPING) && ENABLED(BABYSTEP_DISPLAY_TOTAL)
    babystep.reset_total(axis);
  #endif

  if (printer.debugFeature()) {
    DEBUG_POS("", current_position);
    DEBUG_MC("<<< set_axis_is_at_home(", axis_codes[axis]);
    DEBUG_CHR(')'); DEBUG_EOL();
  }

}

// Return true if the given point is within the printable area
bool Delta_Mechanics::position_is_reachable(const float &rx, const float &ry) {
  return HYPOT2(rx, ry) <= sq(data.print_radius);
}
// Return true if the both nozzle and the probe can reach the given point.
bool Delta_Mechanics::position_is_reachable_by_probe(const float &rx, const float &ry) {
  return position_is_reachable(rx, ry)
      && position_is_reachable(rx - probe.data.offset.x, ry - probe.data.offset.y);
}

// Report detail current position to host
void Delta_Mechanics::report_current_position_detail() {

  SERIAL_MSG("\nLogical:");
  report_xyz(current_position.asLogical());

  SERIAL_MSG("Raw:    ");
  report_xyz(current_position);

  xyze_pos_t leveled = current_position;

  #if HAS_LEVELING
    SERIAL_MSG("Leveled:");
    bedlevel.apply_leveling(leveled);
    report_xyz(leveled);

    SERIAL_MSG("UnLevel:");
    xyze_pos_t unleveled = leveled;
    bedlevel.unapply_leveling(unleveled);
    report_xyz(unleveled);
  #endif

  SERIAL_MSG("DeltaK: ");
  Transform(leveled);
  report_xyz(delta);

  planner.synchronize();

  SERIAL_MSG("Stepper:");
  LOOP_XYZE(i) {
    SERIAL_CHR(' ');
    SERIAL_CHR(axis_codes[i]);
    SERIAL_CHR(':');
    SERIAL_VAL(stepper.position((AxisEnum)i));
  }
  SERIAL_EOL();

  SERIAL_MSG("FromStp:");
  get_cartesian_from_steppers();
  xyze_pos_t from_steppers = { cartesian_position.x, cartesian_position.y, cartesian_position.z, planner.get_axis_position_mm(E_AXIS) };
  report_xyze(from_steppers);

  const xyze_float_t diff = from_steppers - leveled;
  SERIAL_MSG("Differ: ");
  report_xyze(diff);

}

#if ENABLED(DELTA_AUTO_CALIBRATION_1)

  // Compute the derivative of height with respect to a parameter at the specified motor endpoints.
  // 'deriv' indicates the parameter as follows:
  // 0, 1, 2 = X, Y, Z tower endstop adjustments
  // 3 = delta radius
  // 4 = X tower correction
  // 5 = Y tower correction
  // 6 = data.diagonal_rod rod length
  // 7, 8 = X tilt, Y tilt. We scale these by the printable radius to get sensible values in the range -1..1
  float Delta_Mechanics::ComputeDerivative(unsigned int deriv, float ha, float hb, float hc) {
    constexpr float perturb = 0.2;      // perturbation amount in mm or degrees
    float zHi = 0.0,
          zLo = 0.0;
    abc_float_t newPos  = { 0.0, 0.0, 0.0 };

    switch (deriv) {
      case 0:
      case 1:
      case 2:
        // Endstop corrections
        InverseTransform((deriv == 0) ? ha + perturb : ha, (deriv == 1) ? hb + perturb : hb, (deriv == 2) ? hc + perturb : hc, newPos);
        zHi = newPos.c;
        InverseTransform((deriv == 0) ? ha - perturb : ha, (deriv == 1) ? hb - perturb : hb, (deriv == 2) ? hc - perturb : hc, newPos);
        zLo = newPos.c;
        break;

      case 3: {
        const float old_delta_radius = data.radius;

        // Calc High parameters
        data.radius += perturb;
        recalc_delta_settings();
        InverseTransform(ha, hb, hc, newPos);
        zHi = newPos.c;

        // Reset Delta Radius
        data.radius = old_delta_radius;

        // Calc Low parameters
        data.radius -= perturb;
        recalc_delta_settings();
        InverseTransform(ha, hb, hc, newPos);
        zLo = newPos.c;

        // Reset Delta Radius
        data.radius = old_delta_radius;
        break;
      }

      case 4: {
        const float old_delta_tower_angle_adj = data.tower_angle_adj.a;

        // Calc High parameters
        data.tower_angle_adj.a += perturb;
        recalc_delta_settings();
        InverseTransform(ha, hb, hc, newPos);
        zHi = newPos.c;

        // Reset Delta tower Alpha angle adj 
        data.tower_angle_adj.a = old_delta_tower_angle_adj;

        // Calc Low parameters
        data.tower_angle_adj.a -= perturb;
        recalc_delta_settings();
        InverseTransform(ha, hb, hc, newPos);
        zLo = newPos.c;

        // Reset Delta tower Alpha angle adj 
        data.tower_angle_adj.a = old_delta_tower_angle_adj;
        break;
      }

      case 5: {
        const float old_delta_tower_angle_adj = data.tower_angle_adj.b;

        // Calc High parameters
        data.tower_angle_adj.b += perturb;
        recalc_delta_settings();
        InverseTransform(ha, hb, hc, newPos);
        zHi = newPos.c;

        // Reset Delta tower Beta angle adj 
        data.tower_angle_adj.b = old_delta_tower_angle_adj;

        // Calc Low parameters
        data.tower_angle_adj.b -= perturb;
        recalc_delta_settings();
        InverseTransform(ha, hb, hc, newPos);
        zLo = newPos.c;

        // Reset Delta tower Beta angle adj 
        data.tower_angle_adj.b = old_delta_tower_angle_adj;
        break;
      }

      case 6: {
        const float old_delta_diagonal_rod = data.diagonal_rod;

        // Calc High parameters
        data.diagonal_rod += perturb;
        recalc_delta_settings();
        InverseTransform(ha, hb, hc, newPos);
        zHi = newPos.c;

        // Reset Delta Diagonal Rod
        data.diagonal_rod = old_delta_diagonal_rod;

        // Calc Low parameters
        data.diagonal_rod -= perturb;
        recalc_delta_settings();
        InverseTransform(ha, hb, hc, newPos);
        zLo = newPos.c;

        // Reset Delta Diagonal Rod
        data.diagonal_rod = old_delta_diagonal_rod;
        break;
      }
    }

    recalc_delta_settings();

    return (zHi - zLo) / (2.0f * perturb);
  }

#endif // ENABLED(DELTA_AUTO_CALIBRATION_1)

#if DISABLED(DISABLE_M503)

  void Delta_Mechanics::print_parameters() {
    print_M92();
    print_M201();
    print_M203();
    print_M204();
    print_M205();
    print_M666();
  }

  void Delta_Mechanics::print_M92() {
    SERIAL_LM(CFG, "Steps per unit:");
    SERIAL_LMV(CFG, "  M92 X", LINEAR_UNIT(data.axis_steps_per_mm.x), 3);
    LOOP_EXTRUDER() {
      SERIAL_SMV(CFG, "  M92 T", (int)e);
      SERIAL_EMV(" E", VOLUMETRIC_UNIT(data.axis_steps_per_mm.e[e]), 3);
    }
  }

  void Delta_Mechanics::print_M201() {
    SERIAL_LM(CFG, "Maximum Acceleration (units/s2):");
    SERIAL_LMV(CFG, "  M201 X", LINEAR_UNIT(data.max_acceleration_mm_per_s2.x));
    LOOP_EXTRUDER() {
      SERIAL_SMV(CFG, "  M201 T", (int)e);
      SERIAL_EMV(" E", VOLUMETRIC_UNIT(data.max_acceleration_mm_per_s2.e[e]));
    }
  }

  void Delta_Mechanics::print_M203() {
    SERIAL_LM(CFG, "Maximum feedrates (units/s):");
    SERIAL_LMV(CFG, "  M203 X", LINEAR_UNIT(data.max_feedrate_mm_s.x), 3);
    LOOP_EXTRUDER() {
      SERIAL_SMV(CFG, "  M203 T", (int)e);
      SERIAL_EMV(" E", VOLUMETRIC_UNIT(data.max_feedrate_mm_s.e[e]), 3);
    }
  }

  void Delta_Mechanics::print_M204() {
    SERIAL_LM(CFG, "Acceleration (units/s2): P<DEFAULT_ACCELERATION> V<DEFAULT_TRAVEL_ACCELERATION> T* R<DEFAULT_RETRACT_ACCELERATION>");
    SERIAL_SMV(CFG,"  M204 P", LINEAR_UNIT(data.acceleration), 3);
    SERIAL_EMV(" V", LINEAR_UNIT(data.travel_acceleration), 3);
    LOOP_EXTRUDER() {
      SERIAL_SMV(CFG, "  M204 T", (int)e);
      SERIAL_EMV(" R", LINEAR_UNIT(data.retract_acceleration[e]), 3);
    }
  }

  void Delta_Mechanics::print_M205() {
    SERIAL_LM(CFG, "Advanced: B<DEFAULT_MIN_SEGMENT_TIME> S<DEFAULT_MIN_FEEDRATE> V<DEFAULT_MIN_TRAVEL_FEEDRATE>");
    SERIAL_SMV(CFG, "  M205 B", data.min_segment_time_us);
    SERIAL_MV(" S", LINEAR_UNIT(data.min_feedrate_mm_s), 3);
    SERIAL_EMV(" V", LINEAR_UNIT(data.min_travel_feedrate_mm_s), 3);

    #if ENABLED(JUNCTION_DEVIATION)
      SERIAL_LM(CFG, "Junction Deviation: J<JUNCTION_DEVIATION_MM>");
      SERIAL_LMV(CFG, "  M205 J", data.junction_deviation_mm, 2);
    #endif

    SERIAL_SM(CFG, "Jerk: X<DEFAULT_XJERK>");
    #if DISABLED(JUNCTION_DEVIATION) || DISABLED(LIN_ADVANCE)
      SERIAL_MSG(" T* E<DEFAULT_EJERK>");
    #endif
    SERIAL_EOL();

    SERIAL_SMV(CFG, "  M205 X", LINEAR_UNIT(data.max_jerk.x), 3);

    #if DISABLED(JUNCTION_DEVIATION) || DISABLED(LIN_ADVANCE)
      #if EXTRUDERS == 1
        SERIAL_MV(" T0 E", LINEAR_UNIT(data.max_jerk.e[0]), 3);
      #endif
      SERIAL_EOL();
      #if (EXTRUDERS > 1)
        LOOP_EXTRUDER() {
          SERIAL_SMV(CFG, "  M205 T", (int)e);
          SERIAL_EMV(" E" , LINEAR_UNIT(data.max_jerk.e[e]), 3);
        }
      #endif
    #else
      SERIAL_EOL();
    #endif
  }

  void Delta_Mechanics::print_M666() {
    SERIAL_LM(CFG, "Endstop adjustment:");
    SERIAL_SM(CFG, "  M666");
    SERIAL_MV(" X", LINEAR_UNIT(data.endstop_adj.a));
    SERIAL_MV(" Y", LINEAR_UNIT(data.endstop_adj.b));
    SERIAL_MV(" Z", LINEAR_UNIT(data.endstop_adj.c));
    SERIAL_EOL();

    SERIAL_LM(CFG, "Delta Geometry adjustment: ABC<TOWER_*_DIAGROD_ADJ> IJK<TOWER_*_ANGLE_ADJ> UVW<TOWER_*_RADIUS_ADJ>");
    SERIAL_SM(CFG, "  M666");
    SERIAL_MV(" A", LINEAR_UNIT(data.diagonal_rod_adj.a), 3);
    SERIAL_MV(" B", LINEAR_UNIT(data.diagonal_rod_adj.b), 3);
    SERIAL_MV(" C", LINEAR_UNIT(data.diagonal_rod_adj.c), 3);
    SERIAL_MV(" I", data.tower_angle_adj.a, 3);
    SERIAL_MV(" J", data.tower_angle_adj.b, 3);
    SERIAL_MV(" K", data.tower_angle_adj.c, 3);
    SERIAL_MV(" U", LINEAR_UNIT(data.tower_radius_adj.a), 3);
    SERIAL_MV(" V", LINEAR_UNIT(data.tower_radius_adj.b), 3);
    SERIAL_MV(" W", LINEAR_UNIT(data.tower_radius_adj.c), 3);
    SERIAL_EOL();
    SERIAL_LM(CFG, "Delta Geometry adjustment: R<DELTA_RADIUS> D<DELTA_DIAGONAL_ROD>");
    SERIAL_SM(CFG, "  M666");
    SERIAL_MV(" R", LINEAR_UNIT(data.radius));
    SERIAL_MV(" D", LINEAR_UNIT(data.diagonal_rod));
    SERIAL_EOL();
    SERIAL_LM(CFG, "Delta Geometry adjustment: S<DELTA_SEGMENTS_PER_SECOND> L<DELTA_SEGMENTS_PER_LINE>");
    SERIAL_SM(CFG, "  M666");
    SERIAL_MV(" S", data.segments_per_second);
    SERIAL_MV(" L", data.segments_per_line);
    SERIAL_EOL();
    SERIAL_LM(CFG, "Delta Geometry adjustment: O<DELTA_PRINTABLE_RADIUS> P<DELTA_PROBEABLE_RADIUS> H<DELTA_HEIGHT>");
    SERIAL_SM(CFG, "  M666");
    SERIAL_MV(" O", LINEAR_UNIT(data.print_radius));
    SERIAL_MV(" P", LINEAR_UNIT(data.probe_radius));
    SERIAL_MV(" H", LINEAR_UNIT(data.height), 3);
    SERIAL_EOL();
  }

#endif // DISABLED(DISABLE_M503)

#if HAS_NEXTION_LCD && ENABLED(NEXTION_GFX)

  void Delta_Mechanics::nextion_gfx_clear() {
    nexlcd.gfx_clear(data.print_radius * 2, data.print_radius * 2, data.height);
    nexlcd.gfx_cursor_to(current_position);
  }

#endif

/** Private Function */
void Delta_Mechanics::homeaxis(const AxisEnum axis) {

  #define CAN_HOME(A) \
    (axis == A##_AXIS && ((A##_MIN_PIN > -1 && A##_HOME_DIR < 0) || (A##_MAX_PIN > -1 && A##_HOME_DIR > 0)))
  if (!CAN_HOME(X) && !CAN_HOME(Y) && !CAN_HOME(Z)) return;

  if (printer.debugFeature()) {
    DEBUG_MC(">>> homeaxis(", axis_codes[axis]);
    DEBUG_CHR(')'); DEBUG_EOL();
  }

  // Fast move towards endstop until triggered
  do_homing_move(axis, 1.5f * data.height);

  // When homing Z with probe respect probe clearance
  const float bump = home_bump_mm[axis];

  // If a second homing move is configured...
  if (bump) {
    // Move away from the endstop by the axis HOME_BUMP_MM
    if (printer.debugFeature()) DEBUG_EM("Move Away:");
    do_homing_move(axis, -bump);

    // Slow move towards endstop until triggered
    if (printer.debugFeature()) DEBUG_EM("Home 2 Slow:");
    do_homing_move(axis, 2 * bump, get_homing_bump_feedrate(axis));
  }

  // Delta has already moved all three towers up in G28
  // so here it re-homes each tower in turn.
  // Delta homing treats the axes as normal linear axes.

  // retrace by the amount specified in data.endstop_adj + additional 0.1mm in order to have minimum steps
  if (data.endstop_adj[axis] < 0) {
    if (printer.debugFeature()) DEBUG_EM("endstop_adj:");
    do_homing_move(axis, data.endstop_adj[axis] - (MIN_STEPS_PER_SEGMENT + 1) * steps_to_mm[axis]);
  }

  // Clear retracted status if homing the Z axis
  #if ENABLED(FWRETRACT)
    if (axis == Z_AXIS) fwretract.current_hop = 0.0;
  #endif

  if (printer.debugFeature()) {
    DEBUG_MC("<<< homeaxis(", axis_codes[axis]);
    DEBUG_CHR(')'); DEBUG_EOL();
  }

}

/**
 * Calculate delta, start a line, and set current_position.x to destination
 */
void Delta_Mechanics::prepare_uninterpolated_move_to_destination(const float &fr_mm_s/*=0.0*/) {

  if (printer.debugFeature()) DEBUG_POS("prepare_uninterpolated_move_to_destination", destination);

  #if UBL_DELTA
    // ubl segmented line will do z-only moves in single segment
    ubl.prepare_segmented_line_to(destination, MMS_SCALED(fr_mm_s ? fr_mm_s : feedrate_mm_s));
  #else
    if (current_position == destination) return;
    planner.buffer_line(destination, MMS_SCALED(fr_mm_s ? fr_mm_s : feedrate_mm_s), tools.data.extruder.active);
  #endif

  current_position = destination;
}

void Delta_Mechanics::Set_clip_start_height() {
  xyz_pos_t cartesian{0};
  Transform(cartesian);
  float distance = delta.a;
  cartesian.y = data.print_radius;
  Transform(cartesian);
  delta_clip_start_height = data.height - ABS(distance - delta.a);
}

#if ENABLED(DELTA_FAST_SQRT) && ENABLED(__AVR__)

  /**
   * Fast inverse SQRT from Quake III Arena
   * See: https://en.wikipedia.org/wiki/Fast_inverse_square_root
   */
  float Delta_Mechanics::Q_rsqrt(float number) {
    long i;
    float x2, y;
    const float threehalfs = 1.5f;
    x2 = number * 0.5f;
    y  = number;
    i  = * ( long * ) &y;                         // evil floating point bit level hacking
    i  = 0x5F3759DF - ( i >> 1 );
    y  = * ( float * ) &i;
    y  = y * ( threehalfs - ( x2 * y * y ) );     // 1st iteration
    // y  = y * ( threehalfs - ( x2 * y * y ) );  // 2nd iteration, this can be removed
    return y;
  }

#endif

#endif // MECH(DELTA)
