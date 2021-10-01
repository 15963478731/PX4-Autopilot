/****************************************************************************
 *
 * Copyright (c) 2021 Autonomous Systems Lab, ETH Zurich. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/*
 * @file npfg.cpp
 * Implementation of a lateral-directional nonlinear path following guidance
 * law with excess wind handling.
 *
 * Authors and acknowledgements in header.
 */

#include "npfg.hpp"
#include <lib/ecl/geo/geo.h>
#include <px4_platform_common/defines.h>
#include <float.h>

using matrix::Vector2d;
using matrix::Vector2f;

void NPFG::evaluate(const Vector2f &ground_vel, const Vector2f &wind_vel,
		    const Vector2f &unit_path_tangent, const float signed_track_error, const float path_curvature)
{
	const float ground_speed = ground_vel.norm();

	Vector2f air_vel = ground_vel - wind_vel;
	const float airspeed = air_vel.norm();

	if (airspeed < MIN_AIRSPEED) {
		// this case should only ever happen if we have not launched, the wind
		// estimator has failed, or the aircraft is legitimately in a very sad
		// situation
		airspeed_ref_ = airspeed_nom_;
		lateral_accel_ = 0.0f;
		feas_ = 0.0f;
		return;
	}

	const float wind_speed = wind_vel.norm();
	const float wind_ratio = wind_speed / airspeed;

	const float track_error = fabsf(signed_track_error);

	const float wind_cross_upt = cross2D(wind_vel, unit_path_tangent);
	const float wind_dot_upt = wind_vel.dot(unit_path_tangent);

	// calculate the bearing feasibility on the track at the current closest point
	feas_on_track_ = bearingFeasibility(wind_cross_upt, wind_dot_upt, wind_speed, wind_ratio);

	// update control parameters considering upper and lower stability bounds (if enabled)
	// must be called before trackErrorBound() as it updates time_const_
	adapted_period_ = adaptPeriod(ground_speed, airspeed, wind_ratio, track_error,
				      path_curvature, wind_vel, unit_path_tangent, feas_on_track_);
	p_gain_ = pGain(adapted_period_, damping_);
	time_const_ = timeConst(adapted_period_, damping_);

	// track error bound is dynamic depending on ground speed
	track_error_bound_ = trackErrorBound(ground_speed, time_const_);
	const float normalized_track_error = normalizedTrackError(track_error, track_error_bound_);

	// look ahead angle based purely on track proximity
	const float look_ahead_ang = lookAheadAngle(normalized_track_error);

	bearing_vec_ = bearingVec(unit_path_tangent, look_ahead_ang, signed_track_error);

	float wind_cross_bearing = cross2D(wind_vel, bearing_vec_);
	float wind_dot_bearing = wind_vel.dot(bearing_vec_);

	// continuous representation of the bearing feasibility
	feas_ = bearingFeasibility(wind_cross_bearing, wind_dot_bearing, wind_speed, wind_ratio);

	// we consider feasibility of both the current bearing as well as that on the track at the current closest point
	float feas_combined = feas_ * feas_on_track_;

	min_ground_speed_ref_ = minGroundSpeed(normalized_track_error, feas_combined);

	// reference air velocity with directional feedforward effect for following
	// curvature in wind and magnitude incrementation depending on minimum ground
	// speed violations and/or high wind conditions in general
	air_vel_ref_ = refAirVelocity(wind_vel, bearing_vec_, wind_cross_bearing,
				      wind_dot_bearing, wind_speed, min_ground_speed_ref_);
	airspeed_ref_ = air_vel_ref_.norm();

	track_proximity_ = trackProximity(look_ahead_ang);

	// lateral acceleration needed to stay on curved track (assuming no heading error)
	lateral_accel_ff_ = lateralAccelFF(unit_path_tangent, ground_vel,
					   wind_dot_upt, wind_cross_upt, airspeed, wind_speed, wind_ratio,
					   signed_track_error, path_curvature, track_proximity_, feas_combined);

	// total lateral acceleration to drive aircaft towards track as well as account
	// for path curvature
	lateral_accel_ = lateralAccel(air_vel, air_vel_ref_, airspeed) + lateral_accel_ff_;
} // evaluate

float NPFG::adaptPeriod(const float ground_speed, const float airspeed, const float wind_ratio,
			const float track_error, const float path_curvature, const Vector2f &wind_vel,
			const Vector2f &unit_path_tangent, const float feas_on_track) const
{
	float period = period_;
	const float air_turn_rate = fabsf(path_curvature * airspeed);
	const float wind_factor = windFactor(wind_ratio);

	if (en_period_lb_) {
		// lower bound the period for stability w.r.t. roll time constant and current flight condition
		const float period_lb = periodLB(air_turn_rate, wind_factor, feas_on_track);
		period = math::max(period_lb * PERIOD_SAFETY_FACTOR, period);

		// only allow upper bounding ONLY if lower bounding is enabled (is otherwise
		// dangerous to allow period decrements without stability checks)
		const float period_ub = periodUB(air_turn_rate, wind_factor, feas_on_track);

		if (en_period_ub_ && PX4_ISFINITE(period_ub) && period > period_ub) {
			// NOTE: if the roll time constant is not accurately known, reducing
			// the period here can destabilize the system!
			// enable this feature at your own risk!

			// upper bound the period (for track keeping stability), prefer lower bound if violated
			const float period_adapted = math::max(period_lb * PERIOD_SAFETY_FACTOR, period_ub);

			// recalculate time constant and track error bound for lower-bounded
			// period for normalized track error calculation
			const float time_const = timeConst(period, damping_);
			const float track_error_bound = trackErrorBound(ground_speed, time_const);
			const float normalized_track_error = normalizedTrackError(track_error, track_error_bound);

			// calculate nominal track proximity with lower bounded time constant
			// (only a numerical solution can find corresponding track proximity
			// and adapted gains simultaneously)
			const float look_ahead_ang = lookAheadAngle(normalized_track_error);
			const float track_proximity = trackProximity(look_ahead_ang);

			// transition from the nominal period to the adapted period as we get
			// closer to the track
			period = (ramp_in_adapted_period_) ? period_adapted * track_proximity + (1.0f - track_proximity) * period :
				 period_adapted;
		}
	}

	return period;
} // adaptPeriod

float NPFG::normalizedTrackError(const float track_error, const float track_error_bound) const
{
	return math::constrain(track_error / track_error_bound, 0.0f, 1.0f);
}

float NPFG::windFactor(const float wind_ratio) const
{
	// See [TODO: include citation] for definition/elaboration of this approximation.
	return 2.0f * (1.0f - sqrtf(1.0f - math::min(1.0f, wind_ratio)));
} // windFactor

float NPFG::periodUB(const float air_turn_rate, const float wind_factor, const float feas_on_track) const
{
	if (air_turn_rate * wind_factor > EPSILON) {
		// multiply air turn rate by feasibility on track to zero out when we anyway
		// should not consider the curvature
		return 4.0f * M_PI_F * damping_ / (air_turn_rate * wind_factor * feas_on_track);
	}

	return INFINITY;
} // periodUB

float NPFG::periodLB(const float air_turn_rate, const float wind_factor, const float feas_on_track) const
{
	// this method considers a "conservative" lower period bound, i.e. a constant
	// worst case bound for any wind ratio, airspeed, and path curvature

	// the lower bound for zero curvature and no wind OR damping ratio < 0.5
	const float period_lb = M_PI_F * roll_time_const_ / damping_;

	if (air_turn_rate * wind_factor < EPSILON || damping_ < 0.5f) {
		return period_lb;

	} else {
		// the lower bound for tracking a curved path in wind with damping ratio > 0.5
		const float period_windy_curved_damped = 4.0f * M_PI_F * roll_time_const_ * damping_;

		// blend the two together as the bearing on track becomes less feasible
		return period_windy_curved_damped * feas_on_track + (1.0f - feas_on_track) * period_lb;
	}
} // periodLB

float NPFG::trackProximity(const float look_ahead_ang) const
{
	const float sin_look_ahead_ang = sinf(look_ahead_ang);
	return sin_look_ahead_ang * sin_look_ahead_ang;
} // trackProximity

float NPFG::trackErrorBound(const float ground_speed, const float time_const) const
{
	if (ground_speed > 1.0f) {
		return ground_speed * time_const;

	} else {
		// limit bound to some minimum ground speed to avoid singularities in track
		// error normalization. the following equation assumes ground speed minimum = 1.0
		return 0.5f * time_const * (ground_speed * ground_speed + 1.0f);
	}
} // trackErrorBound

float NPFG::pGain(const float period, const float damping) const
{
	return 4.0f * M_PI_F * damping / period;
} // pGain

float NPFG::timeConst(const float period, const float damping) const
{
	return period * damping;
} // timeConst

float NPFG::lookAheadAngle(const float normalized_track_error) const
{
	return M_PI_F * 0.5f * (normalized_track_error - 1.0f) * (normalized_track_error - 1.0f);
} // lookAheadAngle

Vector2f NPFG::bearingVec(const Vector2f &unit_path_tangent, const float look_ahead_ang,
			  const float signed_track_error) const
{
	const float cos_look_ahead_ang = cosf(look_ahead_ang);
	const float sin_look_ahead_ang = sinf(look_ahead_ang);

	Vector2f unit_path_normal(-unit_path_tangent(1.0f), unit_path_tangent(0.0f)); // right handed 90 deg (clockwise) turn
	Vector2f unit_track_error = -((signed_track_error < 0.0f) ? -1.0f : 1.0f) * unit_path_normal;

	return cos_look_ahead_ang * unit_track_error + sin_look_ahead_ang * unit_path_tangent;
} // bearingVec

float NPFG::minGroundSpeed(const float normalized_track_error, const float feas)
{
	// minimum ground speed demand from track keeping logic
	min_gsp_track_keeping_ = 0.0f;

	if (en_track_keeping_ && en_wind_excess_regulation_) {
		// zero out track keeping speed increment when bearing is feasible
		min_gsp_track_keeping_ = (1.0f - feas) * min_gsp_track_keeping_max_ * math::constrain(
						 normalized_track_error * inv_nte_fraction_, 0.0f,
						 1.0f);
	}

	// minimum ground speed demand from minimum forward ground speed user setting
	float min_gsp_cmd = 0.0f;

	if (en_min_ground_speed_ && en_wind_excess_regulation_) {
		min_gsp_cmd = min_gsp_cmd_;
	}

	return math::max(min_gsp_track_keeping_, min_gsp_cmd);
} // minGroundSpeed

Vector2f NPFG::refAirVelocity(const Vector2f &wind_vel, const Vector2f &bearing_vec,
			      const float wind_cross_bearing, const float wind_dot_bearing, const float wind_speed,
			      const float min_ground_speed) const
{
	Vector2f air_vel_ref;

	if (min_ground_speed > wind_dot_bearing && (en_min_ground_speed_ || en_track_keeping_) && en_wind_excess_regulation_) {
		// minimum ground speed and/or track keeping

		// airspeed required to achieve minimum ground speed along bearing vector
		const float airspeed_min = sqrtf((min_ground_speed - wind_dot_bearing) * (min_ground_speed - wind_dot_bearing) +
						 wind_cross_bearing * wind_cross_bearing);

		if (airspeed_min > airspeed_max_) {
			if (bearingIsFeasible(wind_cross_bearing, wind_dot_bearing, airspeed_max_, wind_speed)) {
				// we will not maintain the minimum ground speed, but can still achieve the bearing at maximum airspeed
				const float airsp_dot_bearing = projectAirspOnBearing(airspeed_max_, wind_cross_bearing);
				air_vel_ref = solveWindTriangle(wind_cross_bearing, airsp_dot_bearing, bearing_vec);

			} else {
				// bearing is maximally infeasible, employ mitigation law
				air_vel_ref = infeasibleAirVelRef(wind_vel, bearing_vec, wind_speed, airspeed_max_);
			}

		} else if (airspeed_min > airspeed_nom_) {
			// the minimum ground speed is achievable within the nom - max airspeed range
			// solve wind triangle with for air velocity reference with minimum airspeed
			const float airsp_dot_bearing = projectAirspOnBearing(airspeed_min, wind_cross_bearing);
			air_vel_ref = solveWindTriangle(wind_cross_bearing, airsp_dot_bearing, bearing_vec);

		} else {
			// the minimum required airspeed is less than nominal, so we can track the bearing and minimum
			// ground speed with our nominal airspeed reference
			const float airsp_dot_bearing = projectAirspOnBearing(airspeed_nom_, wind_cross_bearing);
			air_vel_ref = solveWindTriangle(wind_cross_bearing, airsp_dot_bearing, bearing_vec);
		}

	} else {
		// wind excess regulation and/or mitigation

		if (bearingIsFeasible(wind_cross_bearing, wind_dot_bearing, airspeed_nom_, wind_speed)) {
			// bearing is nominally feasible, solve wind triangle for air velocity reference using nominal airspeed
			const float airsp_dot_bearing = projectAirspOnBearing(airspeed_nom_, wind_cross_bearing);
			air_vel_ref = solveWindTriangle(wind_cross_bearing, airsp_dot_bearing, bearing_vec);

		} else if (bearingIsFeasible(wind_cross_bearing, wind_dot_bearing, airspeed_max_, wind_speed)
			   && en_wind_excess_regulation_) {
			// bearing is maximally feasible
			if (wind_dot_bearing <= 0.0f) {
				// we only increment the airspeed to regulate, but not overcome, excess wind
				// NOTE: in the terminal condition, this will result in a zero ground velocity configuration
				air_vel_ref = wind_vel;

			} else {
				// the bearing is achievable within the nom - max airspeed range
				// solve wind triangle with for air velocity reference with minimum airspeed
				const float airsp_dot_bearing = 0.0f; // right angle to the bearing line gives minimal airspeed usage
				air_vel_ref = solveWindTriangle(wind_cross_bearing, airsp_dot_bearing, bearing_vec);
			}

		} else {
			// bearing is maximally infeasible, employ mitigation law
			const float airspeed_input = (en_wind_excess_regulation_) ? airspeed_max_ : airspeed_nom_;
			air_vel_ref = infeasibleAirVelRef(wind_vel, bearing_vec, wind_speed, airspeed_input);
		}
	}

	return air_vel_ref;
} // refAirVelocity

float NPFG::projectAirspOnBearing(const float airspeed, const float wind_cross_bearing) const
{
	// NOTE: wind_cross_bearing must be less than airspeed to use this function
	// it is assumed that bearing feasibility is checked and found feasible (e.g. bearingIsFeasible() = true) prior to entering this method
	// otherwise the return will be erroneous
	return sqrtf(math::max(airspeed * airspeed - wind_cross_bearing * wind_cross_bearing, 0.0f));
} // projectAirspOnBearing

int NPFG::bearingIsFeasible(const float wind_cross_bearing, const float wind_dot_bearing, const float airspeed,
			    const float wind_speed) const
{
	return (fabsf(wind_cross_bearing) < airspeed) && ((wind_dot_bearing > 0.0f) || (wind_speed < airspeed));
} // bearingIsFeasible

Vector2f NPFG::solveWindTriangle(const float wind_cross_bearing, const float airsp_dot_bearing,
				 const Vector2f &bearing_vec) const
{
	// essentially a 2D rotation with the speeds (magnitudes) baked in
	return Vector2f{airsp_dot_bearing * bearing_vec(0) - wind_cross_bearing * bearing_vec(1),
			wind_cross_bearing * bearing_vec(0) + airsp_dot_bearing * bearing_vec(1)};
} // solveWindTriangle

Vector2f NPFG::infeasibleAirVelRef(const Vector2f &wind_vel, const Vector2f &bearing_vec, const float wind_speed,
				   const float airspeed) const
{
	// NOTE: wind speed must be greater than airspeed, and airspeed must be greater than zero to use this function
	// it is assumed that bearing feasibility is checked and found infeasible (e.g. bearingIsFeasible() = false) prior to entering this method
	// otherwise the normalization of the air velocity vector could have a division by zero
	Vector2f air_vel_ref = sqrtf(math::max(wind_speed * wind_speed - airspeed * airspeed, 0.0f)) * bearing_vec - wind_vel;
	return air_vel_ref.normalized() * airspeed;
} // infeasibleAirVelRef

float NPFG::bearingFeasibility(const float wind_cross_bearing, const float wind_dot_bearing, const float wind_speed,
			       const float wind_ratio) const
{
	float sin_cross_wind_ang; // in [0, 1] (constant after 90 deg)

	if (wind_dot_bearing <= 0.0f) {
		sin_cross_wind_ang = 1.0f;

	} else {
		sin_cross_wind_ang = fabsf(wind_cross_bearing / wind_speed);
	}

	// upper and lower feasibility barriers
	float wind_ratio_ub, wind_ratio_lb;

	if (sin_cross_wind_ang < CROSS_WIND_ANG_CO) { // small angle approx.
		// linear feasibility function (avoid singularity)

		const float wind_ratio_ub_co = ONE_DIV_SIN_CROSS_WIND_ANG_CO;
		wind_ratio_ub = wind_ratio_ub_co + CO_SLOPE * (CROSS_WIND_ANG_CO - sin_cross_wind_ang);

		const float wind_ratio_lb_co = (ONE_DIV_SIN_CROSS_WIND_ANG_CO - 2.0f) * wind_ratio_buffer_ + 1.0f;
		wind_ratio_lb = wind_ratio_lb_co + wind_ratio_buffer_ * CO_SLOPE * (CROSS_WIND_ANG_CO - sin_cross_wind_ang);

	} else {
		const float one_div_sin_cross_wind_ang = 1.0f / sin_cross_wind_ang;
		wind_ratio_ub = one_div_sin_cross_wind_ang;
		wind_ratio_lb = (one_div_sin_cross_wind_ang - 2.0f) * wind_ratio_buffer_ + 1.0f;
	}

	// calculate bearing feasibility
	float feas = 1.0f; // feasible

	if (wind_ratio > wind_ratio_ub) {
		// infeasible
		feas = 0.0f;

	} else if (wind_ratio > wind_ratio_lb) {
		// partially feasible
		// smoothly transition from fully feasible to fully infeasible
		feas = cosf(M_PI_F * 0.5f * math::constrain((wind_ratio - wind_ratio_lb) / (wind_ratio_ub - wind_ratio_lb), 0.0f,
				1.0f));
		feas *= feas;
	}

	return feas;
} // bearingFeasibility

float NPFG::lateralAccelFF(const Vector2f &unit_path_tangent, const Vector2f &ground_vel,
			   const float wind_dot_upt, const float wind_cross_upt, const float airspeed,
			   const float wind_speed, const float wind_ratio, const float signed_track_error,
			   const float path_curvature, const float track_proximity, const float feas) const
{
	// NOTE: all calculations within this function take place at the closet point
	// on the path, as if the aircraft were already tracking the given path at
	// this point with zero angular error. this allows us to evaluate curvature
	// induced requirements for lateral acceleration incrementation and ramp them
	// in with the track proximity. further the bearing feasibility is considered
	// in excess wind conditions.

	// path frame curvature is the instantaneous curvature at our current distance
	// from the actual path (considering e.g. concentric circles emanating outward/inward)
	const float path_frame_curvature = path_curvature / math::max(1.0f - path_curvature * signed_track_error,
					   path_curvature * MIN_RADIUS);

	// limit tangent ground speed to along track (forward moving) direction
	const float tangent_ground_speed = math::max(ground_vel.dot(unit_path_tangent), 0.0f);

	const float path_frame_rate = path_frame_curvature * tangent_ground_speed;

	// speed ratio = projection of ground vel on track / projection of air velocity
	// on track
	const float speed_ratio = (1.0f + wind_dot_upt / math::max(projectAirspOnBearing(airspeed, wind_cross_upt), EPSILON));

	// note the use of airspeed * speed_ratio as oppose to ground_speed^2 here --
	// the prior considers that we command lateral acceleration in the air mass
	// relative frame while the latter does not
	return airspeed * track_proximity * feas * speed_ratio * path_frame_rate;
} // lateralAccelFF

float NPFG::lateralAccel(const Vector2f &air_vel, const Vector2f &air_vel_ref, const float airspeed) const
{
	// lateral acceleration demand only from the heading error

	const float dot_air_vel_err = air_vel.dot(air_vel_ref);
	const float cross_air_vel_err = cross2D(air_vel, air_vel_ref);

	if (dot_air_vel_err < 0.0f) {
		// hold max lateral acceleration command above 90 deg heading error
		return p_gain_ * ((cross_air_vel_err < 0.0f) ? -airspeed *airspeed : airspeed * airspeed);

	} else {
		// airspeed/airspeed_ref is used to scale any incremented airspeed reference back to the current airspeed
		// for acceleration commands in a "feedback" sense (i.e. at the current vehicle airspeed)
		return p_gain_ * cross_air_vel_err * airspeed / airspeed_ref_;
	}
} // lateralAccel

/*******************************************************************************
 * PX4 NAVIGATION INTERFACE FUNCTIONS (provide similar functionality to ECL_L1_Pos_Controller)
 */

void NPFG::navigateWaypoints(const Vector2d &waypoint_A, const Vector2d &waypoint_B,
			     const Vector2d &vehicle_pos, const Vector2f &ground_vel, const Vector2f &wind_vel)
{
	// similar to logic found in ECL_L1_Pos_Controller method of same name

	path_type_loiter_ = false;

	Vector2f vector_A_to_B = getLocalPlanarVector(waypoint_A, waypoint_B);
	Vector2f vector_A_to_vehicle = getLocalPlanarVector(waypoint_A, vehicle_pos);

	if (vector_A_to_B.norm() < EPSILON || vector_A_to_B.dot(vector_A_to_vehicle) < 0.0f) {
		// the waypoints are either on top of each other and should be considered
		// as single waypoint, or we are in front of waypoint A. in either case,
		// fly directly to A.
		unit_path_tangent_ = -vector_A_to_vehicle.normalized();
		signed_track_error_ = 0.0f;

	} else {
		// track the line segment between A and B
		unit_path_tangent_ = vector_A_to_B.normalized();
		signed_track_error_ = cross2D(unit_path_tangent_, vector_A_to_vehicle);
	}

	evaluate(ground_vel, wind_vel, unit_path_tangent_, signed_track_error_, 0.0f);

	updateRollSetpoint();
} // navigateWaypoints

void NPFG::navigateLoiter(const Vector2d &loiter_center, const Vector2d &vehicle_pos,
			  float radius, int8_t loiter_direction, const Vector2f &ground_vel, const Vector2f &wind_vel)
{
	path_type_loiter_ = true;

	radius = math::max(radius, MIN_RADIUS);

	Vector2f vector_center_to_vehicle = getLocalPlanarVector(loiter_center, vehicle_pos);
	const float dist_to_center = vector_center_to_vehicle.norm();

	// find the direction from the circle center to the closest point on its perimeter
	// from the vehicle position
	Vector2f unit_vec_center_to_closest_pt;

	if (dist_to_center < 0.1f) {
		// the logic breaks down at the circle center, employ some mitigation strategies
		// until we exit this region
		if (ground_vel.norm() < 0.1f) {
			// arbitrarily set the point in the northern top of the circle
			unit_vec_center_to_closest_pt = Vector2f{1.0f, 0.0f};

		} else {
			// set the point in the direction we are moving
			unit_vec_center_to_closest_pt = ground_vel.normalized();
		}

	} else {
		// set the point in the direction of the aircraft
		unit_vec_center_to_closest_pt = vector_center_to_vehicle.normalized();
	}

	// 90 deg clockwise rotation * loiter direction
	unit_path_tangent_ = float(loiter_direction) * Vector2f{-unit_vec_center_to_closest_pt(1), unit_vec_center_to_closest_pt(0)};

	// positive in direction of path normal
	signed_track_error_ = -loiter_direction * (dist_to_center - radius);

	float path_curvature = float(loiter_direction) / radius;

	evaluate(ground_vel, wind_vel, unit_path_tangent_, signed_track_error_, path_curvature);

	updateRollSetpoint();
} // navigateLoiter

void NPFG::navigateHeading(float heading_ref, const Vector2f &ground_vel, const Vector2f &wind_vel)
{
	path_type_loiter_ = false;

	Vector2f air_vel = ground_vel - wind_vel;
	unit_path_tangent_ = Vector2f{cosf(heading_ref), sinf(heading_ref)};
	signed_track_error_ = 0.0f;

	// use the guidance law to regular heading error - ignoring wind or inertial position
	evaluate(air_vel, Vector2f{0.0f, 0.0f}, unit_path_tangent_, signed_track_error_, 0.0f);

	updateRollSetpoint();
} // navigateHeading

void NPFG::navigateBearing(float bearing, const Vector2f &ground_vel, const Vector2f &wind_vel)
{
	path_type_loiter_ = false;

	unit_path_tangent_ = Vector2f{cosf(bearing), sinf(bearing)};

	signed_track_error_ = 0.0f;

	// no track error or path curvature to consider, just regulate ground velocity
	// to bearing vector
	evaluate(ground_vel, wind_vel, unit_path_tangent_, signed_track_error_, 0.0f);

	updateRollSetpoint();
} // navigateBearing

void NPFG::navigateLevelFlight(const float heading)
{
	path_type_loiter_ = false;

	airspeed_ref_ = airspeed_nom_;
	lateral_accel_ = 0.0f;
	feas_ = 1.0f;
	bearing_vec_ = Vector2f{cosf(heading), sinf(heading)};
	unit_path_tangent_ = bearing_vec_;
	signed_track_error_ = 0.0f;

	updateRollSetpoint();
} // navigateLevelFlight

float NPFG::switchDistance(float wp_radius) const
{
	return math::min(wp_radius, track_error_bound_);
} // switchDistance

Vector2f NPFG::getLocalPlanarVector(const Vector2d &origin, const Vector2d &target) const
{
	/* this is an approximation for small angles, proposed by [2] */
	const double x_angle = math::radians(target(0) - origin(0));
	const double y_angle = math::radians(target(1) - origin(1));
	const double x_origin_cos = cos(math::radians(origin(0)));

	return Vector2f{
		static_cast<float>(x_angle * CONSTANTS_RADIUS_OF_EARTH),
		static_cast<float>(y_angle *x_origin_cos * CONSTANTS_RADIUS_OF_EARTH),
	};
} // getLocalPlanarVector

void NPFG::updateRollSetpoint()
{
	float roll_new = atanf(lateral_accel_ * 1.0f / CONSTANTS_ONE_G);
	roll_new = math::constrain(roll_new, -roll_lim_rad_, roll_lim_rad_);

	if (dt_ > 0.0f && roll_slew_rate_ > 0.0f) {
		// slew rate limiting active
		roll_new = math::constrain(roll_new, roll_setpoint_ - roll_slew_rate_ * dt_, roll_setpoint_ + roll_slew_rate_ * dt_);
	}

	if (PX4_ISFINITE(roll_new)) {
		roll_setpoint_ = roll_new;
	}
} // updateRollSetpoint
