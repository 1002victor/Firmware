/****************************************************************************
 *
 *   Copyright (c) 2014 PX4 Development Team. All rights reserved.
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
/**
 * @file navigator_mode.cpp
 *
 * Base class for different modes in navigator
 *
 * @author Julian Oes <julian@oes.ch>
 * @author Anton Babushkin <anton.babushkin@me.com>
 * @author Martins Frolovs <martins.f@airdog.com>
 */

#include "navigator_mode.h"
#include "navigator.h"

#include <uORB/uORB.h>
#include <uORB/topics/position_setpoint_triplet.h>
#include <uORB/topics/parameter_update.h>

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <drivers/device/device.h>
#include <drivers/drv_hrt.h>
#include <arch/board/board.h>

#include <systemlib/err.h>
#include <systemlib/systemlib.h>
#include <geo/geo.h>
#include <dataman/dataman.h>
#include <mathlib/mathlib.h>
#include <mavlink/mavlink_log.h>


NavigatorMode::NavigatorMode(Navigator *navigator, const char *name) :
	SuperBlock(navigator, name),
	_navigator(navigator),
	_first_run(true)
{
	updateParams();
	on_inactive();

	_mavlink_fd = open(MAVLINK_LOG_DEVICE, 0);

}

NavigatorMode::~NavigatorMode()
{
}

void
NavigatorMode::updateParameters() {

	updateParamHandles();
	updateParamValues();

}

void
NavigatorMode::updateParamHandles() {

	_parameter_handles.takeoff_alt = param_find("NAV_TAKEOFF_ALT");
	_parameter_handles.takeoff_acceptance_radius = param_find("NAV_TAKEOFF_ACR");
	_parameter_handles.acceptance_radius = param_find("NAV_ACC_RAD");

	_parameter_handles.loi_step_len = param_find("LOI_STEP_LEN");

	_parameter_handles.rtl_ret_alt = param_find("RTL_RET_ALT");
}

void
NavigatorMode::updateParamValues() {

	param_get(_parameter_handles.takeoff_alt, &(_parameters.takeoff_alt));
	param_get(_parameter_handles.takeoff_acceptance_radius, &(_parameters.takeoff_acceptance_radius));
	param_get(_parameter_handles.acceptance_radius, &(_parameters.acceptance_radius));

	param_get(_parameter_handles.loi_step_len, &(_parameters.loi_step_len));

	param_get(_parameter_handles.rtl_ret_alt, &(_parameters.rtl_ret_alt));

}


void
NavigatorMode::run(bool active, bool parameters_updated) {

    if (parameters_updated) {
        updateParameters();    
    }

	if (active) {
		if (_first_run) {
			/* first run */
			_first_run = false;
			/* Reset stay in failsafe flag */
			_navigator->get_mission_result()->stay_in_failsafe = false;
			_navigator->publish_mission_result();
			on_activation();

		} else {
			/* periodic updates when active */
			on_active();
		}

	} else {
		/* periodic updates when inactive */
		_first_run = true;
		on_inactive();
	}
}

void
NavigatorMode::on_inactive()
{
}

void
NavigatorMode::on_activation()
{
	/* invalidate position setpoint by default */
	_navigator->get_position_setpoint_triplet()->current.valid = false;
}

void
NavigatorMode::on_active()
{
}

bool
NavigatorMode::update_vehicle_command()
{
	bool vcommand_updated = false;
	orb_check(_navigator->get_vehicle_command_sub(), &vcommand_updated);

	if (vcommand_updated) {

		if (orb_copy(ORB_ID(vehicle_command), _navigator->get_vehicle_command_sub(), &_vcommand) == OK) {
			return true;
		}
		else
			return false;
	}

	return false;
}

void
NavigatorMode::execute_vehicle_command()
{
}

void
NavigatorMode::point_camera_to_target(position_setpoint_s *sp)
{
	target_pos = _navigator->get_target_position();
	global_pos = _navigator->get_global_position();

	// Calculate offset values for later use.
	float offset_x;
	float offset_y;
	float offset_z = target_pos->alt - global_pos->alt;

	get_vector_to_next_waypoint(
			target_pos->lat,
			target_pos->lon,
			global_pos->lat,
			global_pos->lon,
			&offset_x,
			&offset_y
	);

	math::Vector<3> offset(offset_x, offset_y, offset_z);
	math::Vector<2> offset_xy(offset_x, offset_y);

	float offset_xy_len = offset_xy.length();

	if (offset_xy_len > 5.0f)
		sp->yaw = _wrap_pi(atan2f(-offset_xy(1), -offset_xy(0)));

	sp->camera_pitch = atan2f(offset(2), offset_xy_len);
}

bool
NavigatorMode::check_current_pos_sp_reached()
{
	struct vehicle_status_s *vstatus = _navigator->get_vstatus();
	struct position_setpoint_triplet_s 	*pos_sp_triplet = _navigator->get_position_setpoint_triplet();
	struct vehicle_global_position_s 	*global_pos = _navigator->get_global_position();;

	switch (pos_sp_triplet->current.type)
	{
	case SETPOINT_TYPE_IDLE:
		return true;
		break;

	case SETPOINT_TYPE_LAND:

        if (vstatus->condition_landed){

            commander_request_s *commander_request = _navigator->get_commander_request();
            commander_request->request_type = AIRD_STATE_CHANGE;
            commander_request->airdog_state = AIRD_STATE_LANDED;
            _navigator->set_commander_request_updated();
            return true;

        }

        return false;

		break;

	case SETPOINT_TYPE_TAKEOFF:
	{
		float alt_diff = fabs(pos_sp_triplet->current.alt - global_pos->alt);

        if (_parameters.takeoff_acceptance_radius >= alt_diff) {

            commander_request_s *commander_request = _navigator->get_commander_request();
            commander_request->request_type = AIRD_STATE_CHANGE;
            commander_request->airdog_state = AIRD_STATE_IN_AIR;
            _navigator->set_commander_request_updated();
            return true;

        }

        return false;
        
		break;
	}
	case SETPOINT_TYPE_POSITION:
	{
		float dist_xy = -1;
		float dist_z = -1;

		float distance = get_distance_to_point_global_wgs84(
			global_pos->lat, global_pos->lon, global_pos->alt,
			pos_sp_triplet->current.lat, pos_sp_triplet->current.lon, pos_sp_triplet->current.alt,
			&dist_xy, &dist_z
		);

        if (_parameters.acceptance_radius >= distance){
            return true; 
        }

        return false;

		break;
	}
	default:
		return false;
		break;

	}
}


void
NavigatorMode::land()
{
    pos_sp_triplet = _navigator->get_position_setpoint_triplet();

	pos_sp_triplet->previous.valid = false;
	pos_sp_triplet->current.valid = true;
	pos_sp_triplet->next.valid = false;

	pos_sp_triplet->current.lat = global_pos->lat;
	pos_sp_triplet->current.lon = global_pos->lon;
	pos_sp_triplet->current.alt = global_pos->alt;
	pos_sp_triplet->current.yaw = NAN;
	pos_sp_triplet->current.type = SETPOINT_TYPE_LAND;

	_navigator->set_position_setpoint_triplet_updated();

	commander_request_s *commander_request = _navigator->get_commander_request();
	commander_request->request_type = AIRD_STATE_CHANGE;
    commander_request->airdog_state = AIRD_STATE_LANDING;
	_navigator->set_commander_request_updated();
}

void
NavigatorMode::takeoff()
{
    pos_sp_triplet = _navigator->get_position_setpoint_triplet();

	pos_sp_triplet->previous.valid = false;
	pos_sp_triplet->current.valid = true;
	pos_sp_triplet->next.valid = false;

	pos_sp_triplet->current.lat = global_pos->lat;
	pos_sp_triplet->current.lon = global_pos->lon;
	pos_sp_triplet->current.alt = global_pos->alt + _parameters.takeoff_alt;

	pos_sp_triplet->current.yaw = NAN;
	pos_sp_triplet->current.type = SETPOINT_TYPE_TAKEOFF;

	_navigator->set_position_setpoint_triplet_updated();

	commander_request_s *commander_request = _navigator->get_commander_request();
	commander_request->request_type = AIRD_STATE_CHANGE;
    commander_request->airdog_state = AIRD_STATE_TAKING_OFF;
	_navigator->set_commander_request_updated();

}


void
NavigatorMode::disarm()
{
	commander_request_s *commander_request = _navigator->get_commander_request();
	commander_request->request_type = V_DISARM;
	_navigator->set_commander_request_updated();
}
