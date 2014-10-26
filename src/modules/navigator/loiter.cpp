/****************************************************************************
 *
 *   Copyright (c) 2013-2014 PX4 Development Team. All rights reserved.
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
 * @file loiter.cpp
 *
 * Helper class to loiter
 *
 * @author Julian Oes <julian@oes.ch>
 * @author Anton Babushkin <anton.babushkin@me.com>
 * @author Martins Frolovs <martins.f@airdog.vom>
 */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <fcntl.h>
#include <stdio.h>

#include <mavlink/mavlink_log.h>
#include <systemlib/err.h>

#include <uORB/uORB.h>
#include <uORB/topics/position_setpoint_triplet.h>

#include "loiter.h"
#include "navigator.h"

Loiter::Loiter(Navigator *navigator, const char *name) :
	MissionBlock(navigator, name)
{
    updateParameters();
}

Loiter::~Loiter()
{
}

void
Loiter::on_inactive()
{
}

void
Loiter::on_activation()
{
	updateParameters();

	// Determine current loiter sub mode
	struct vehicle_status_s *vstatus = _navigator->get_vstatus();

    pos_sp_triplet = _navigator->get_position_setpoint_triplet();

	_mavlink_fd = _navigator->get_mavlink_fd();

	if (vstatus->condition_landed) {
		set_sub_mode(LOITER_SUB_MODE_LANDED);
	}
	else {
		set_sub_mode(LOITER_SUB_MODE_AIM_AND_SHOOT);
	}
}

void
Loiter::on_active()
{
	target_pos = _navigator->get_target_position();
	global_pos = _navigator->get_global_position();
	pos_sp_triplet = _navigator->get_position_setpoint_triplet();


	if (loiter_sub_mode == LOITER_SUB_MODE_TAKING_OFF && check_current_pos_sp_reached()) {
		set_sub_mode(LOITER_SUB_MODE_AIM_AND_SHOOT);
	}

	if (loiter_sub_mode == LOITER_SUB_MODE_LANDING && check_current_pos_sp_reached()) {
		set_sub_mode(LOITER_SUB_MODE_LANDED);

		disarm();
	}

	if (loiter_sub_mode == LOITER_SUB_MODE_COME_TO_ME && check_current_pos_sp_reached()) {
		set_sub_mode(LOITER_SUB_MODE_AIM_AND_SHOOT);
	}


	if ( (loiter_sub_mode == LOITER_SUB_MODE_AIM_AND_SHOOT) && (target_pos->alt + _parameters.loi_min_alt > pos_sp_triplet->current.alt)){
		pos_sp_triplet->current.alt = target_pos->alt + _parameters.loi_min_alt;
	}

	if ( update_vehicle_command() )
			execute_vehicle_command();

	if (loiter_sub_mode == LOITER_SUB_MODE_AIM_AND_SHOOT || loiter_sub_mode == LOITER_SUB_MODE_COME_TO_ME)
		point_camera_to_target(&(pos_sp_triplet->current));

	_navigator->set_position_setpoint_triplet_updated();
}

void
Loiter::execute_vehicle_command()
{
    updateParams();

	vehicle_command_s cmd = _vcommand;

	switch (loiter_sub_mode){
		case LOITER_SUB_MODE_LANDED:
			execute_command_in_landed(cmd);
			break;
		case LOITER_SUB_MODE_AIM_AND_SHOOT:
			execute_command_in_aim_and_shoot(cmd);
			break;
		case LOITER_SUB_MODE_LOOK_DOWN:
			execute_command_in_look_down(cmd);
			break;
		case LOITER_SUB_MODE_COME_TO_ME:
			execute_command_in_come_to_me(cmd);
			break;
		case LOITER_SUB_MODE_LANDING:
			execute_command_in_landing(cmd);
			break;
		case LOITER_SUB_MODE_TAKING_OFF:
			execute_command_in_taking_off(cmd);
			break;
	}

}

void
Loiter::execute_command_in_landed(vehicle_command_s cmd){

	if (cmd.command == VEHICLE_CMD_NAV_REMOTE_CMD) {

		int remote_cmd = cmd.param1;

		if (remote_cmd == REMOTE_CMD_TAKEOFF) {
			set_sub_mode(LOITER_SUB_MODE_TAKING_OFF);
			takeoff();
		} else if (remote_cmd == REMOTE_CMD_LAND_DISARM) {
			disarm();
		}
	}
}

void
Loiter::execute_command_in_aim_and_shoot(vehicle_command_s cmd){



	// Calculate offset
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


	if (cmd.command == VEHICLE_CMD_DO_SET_MODE){

		//uint8_t base_mode = (uint8_t)cmd.param1;
		uint8_t main_mode = (uint8_t)cmd.param2;

		if (main_mode == PX4_CUSTOM_SUB_MODE_AUTO_RTL) {
			// Make request to COMMANDER do change state to RTL
			commander_request_s *commander_request = _navigator->get_commander_request();
			commander_request->request_type = V_MAIN_STATE_CHANGE;
			commander_request->main_state = MAIN_STATE_AUTO_RTL;
			_navigator->set_commander_request_updated();

		}
	}


	if (cmd.command == VEHICLE_CMD_NAV_REMOTE_CMD) {

		REMOTE_CMD remote_cmd = (REMOTE_CMD)cmd.param1;

		pos_sp_triplet->previous.valid = false;
		pos_sp_triplet->current.valid = true;
		pos_sp_triplet->next.valid = false;

		switch(remote_cmd){
			case  REMOTE_CMD_LAND_DISARM: {
				land();
				set_sub_mode(LOITER_SUB_MODE_LANDING);
				break;
			}
			case REMOTE_CMD_UP: {

				pos_sp_triplet->current.alt = global_pos->alt + _parameters.loi_step_len;
				pos_sp_triplet->current.lat = global_pos->lat;
				pos_sp_triplet->current.lon = global_pos->lon;

				pos_sp_triplet->current.type = SETPOINT_TYPE_POSITION;

				break;
			}
			case REMOTE_CMD_DOWN: {

				pos_sp_triplet->current.alt = global_pos->alt - _parameters.loi_step_len;
				pos_sp_triplet->current.lat = global_pos->lat;
				pos_sp_triplet->current.lon = global_pos->lon;

				pos_sp_triplet->current.type = SETPOINT_TYPE_POSITION;
				break;
			}
			case REMOTE_CMD_LEFT: {

				math::Matrix<3, 3> R_phi;

				double radius = sqrt(offset(0) * offset(0) + offset(1) * offset(1));

				// derived from formula: ( step / ( sqrt(x^2 + y^2)*2PI ) ) *  2PI
				// radius: (sqrt(x^2 + y^2)
				// circumference C: (radius * 2* PI)
				// step length fraction of C: step/C
				// angle of step fraction in radians: step/C * 2PI
				double alpha = (double)_parameters.loi_step_len / radius;

				// vector yaw rotation +alpha or -alpha depending on left or right
				R_phi.from_euler(0.0f, 0.0f, -alpha);
				math::Vector<3> offset_new  = R_phi * offset;

				double lat_new, lon_new;
				add_vector_to_global_position(
						(*target_pos).lat,
						(*target_pos).lon,
						offset_new(0),
						offset_new(1),
						&lat_new,
						&lon_new
				);

				pos_sp_triplet->current.lat = lat_new;
				pos_sp_triplet->current.lon = lon_new;
				pos_sp_triplet->current.type = SETPOINT_TYPE_POSITION;

				break;
			}
			case REMOTE_CMD_RIGHT: {

				math::Matrix<3, 3> R_phi;

				double radius = sqrt(offset(0) * offset(0) + offset(1) * offset(1));

				// derived from formula: ( step / ( sqrt(x^2 + y^2)*2PI ) ) *  2PI
				// radius: (sqrt(x^2 + y^2)
				// circumference C: (radius * 2* PI)
				// step length fraction of C: step/C
				// angle of step fraction in radians: step/C * 2PI
				double alpha = (double)_parameters.loi_step_len / radius;

				// vector yaw rotation +alpha or -alpha depending on left or right
				R_phi.from_euler(0.0f, 0.0f, +alpha);
				math::Vector<3> offset_new  = R_phi * offset;

				double lat_new, lon_new;
				add_vector_to_global_position(
						(*target_pos).lat,
						(*target_pos).lon,
						offset_new(0),
						offset_new(1),
						&lat_new,
						&lon_new
				);

				pos_sp_triplet->current.lat = lat_new;
				pos_sp_triplet->current.lon = lon_new;
				pos_sp_triplet->current.type = SETPOINT_TYPE_POSITION;

				break;
			}
			case REMOTE_CMD_CLOSER: {

				// Calculate vector angle from target to device with atan2(y, x)
				float alpha = atan2f(offset(1), offset(0));

				// Create vector in the same direction, with loiter_step length
				math::Vector<3> offset_delta(
						cosf(alpha) * _parameters.loi_step_len,
						sinf(alpha) * _parameters.loi_step_len,
						0);

				math::Vector<3> offset_new = offset - offset_delta;

				double lat_new, lon_new;
				add_vector_to_global_position(
						(*target_pos).lat,
						(*target_pos).lon,
						offset_new(0),
						offset_new(1),
						&lat_new,
						&lon_new
				);

				pos_sp_triplet->current.lat = lat_new;
				pos_sp_triplet->current.lon = lon_new;
				pos_sp_triplet->current.type = SETPOINT_TYPE_POSITION;

				break;

			}

			case REMOTE_CMD_FURTHER: {

				// Calculate vector angle from target to device with atan2(y, x)
				float alpha = atan2(offset(1), offset(0));

				// Create vector in the same direction, with loiter_step length
				math::Vector<3> offset_delta(
						cosf(alpha) * _parameters.loi_step_len,
						sinf(alpha) * _parameters.loi_step_len,
						0);

				math::Vector<3> offset_new = offset + offset_delta;

				double lat_new, lon_new;
				add_vector_to_global_position(
						target_pos->lat,
						target_pos->lon,
						offset_new(0),
						offset_new(1),
						&lat_new,
						&lon_new
				);

				pos_sp_triplet->current.lat = lat_new;
				pos_sp_triplet->current.lon = lon_new;
				pos_sp_triplet->current.type = SETPOINT_TYPE_POSITION;

				break;
			}
			case REMOTE_CMD_COME_TO_ME: {

				pos_sp_triplet->current.lat = target_pos->lat;
				pos_sp_triplet->current.lon = target_pos->lon;
				pos_sp_triplet->current.type = SETPOINT_TYPE_POSITION;

				set_sub_mode(LOITER_SUB_MODE_COME_TO_ME);

				break;
			}
			case REMOTE_CMD_LOOK_DOWN: {

				pos_sp_triplet->current.camera_pitch = -1.57f;
				set_sub_mode(LOITER_SUB_MODE_LOOK_DOWN);
				break;

			}
			case REMOTE_CMD_PLAY_PAUSE: {

				commander_request_s *commander_request = _navigator->get_commander_request();
				commander_request->request_type = V_MAIN_STATE_CHANGE;
				commander_request->main_state = MAIN_STATE_AUTO_ABS_FOLLOW;
				_navigator->set_commander_request_updated();

				break;
			}

		}

	}

	_navigator->set_position_setpoint_triplet_updated();

}

void
Loiter::execute_command_in_look_down(vehicle_command_s cmd){

	if (cmd.command == VEHICLE_CMD_NAV_REMOTE_CMD) {
		int remote_cmd = cmd.param1;
		if (remote_cmd == REMOTE_CMD_PLAY_PAUSE) {
			set_sub_mode(LOITER_SUB_MODE_AIM_AND_SHOOT);
		}
	}
}

void
Loiter::execute_command_in_come_to_me(vehicle_command_s cmd){

	if (cmd.command == VEHICLE_CMD_NAV_REMOTE_CMD) {
		int remote_cmd = cmd.param1;
		if (remote_cmd == REMOTE_CMD_PLAY_PAUSE) {
			set_sub_mode(LOITER_SUB_MODE_AIM_AND_SHOOT);
		}
	}

}

void
Loiter::execute_command_in_landing(vehicle_command_s cmd){

	if (cmd.command == VEHICLE_CMD_NAV_REMOTE_CMD) {
		int remote_cmd = cmd.param1;
		if (remote_cmd == REMOTE_CMD_PLAY_PAUSE) {
			set_sub_mode(LOITER_SUB_MODE_AIM_AND_SHOOT);
			loiter_sub_mode = LOITER_SUB_MODE_AIM_AND_SHOOT;
		}
	}
}

void
Loiter::set_sub_mode(LOITER_SUB_MODE new_sub_mode){

	if (new_sub_mode == LOITER_SUB_MODE_AIM_AND_SHOOT) {

		// Reset setpoint position to current global position
		global_pos = _navigator->get_global_position();

		pos_sp_triplet->previous.valid = false;
		pos_sp_triplet->current.valid = true;
		pos_sp_triplet->next.valid = false;

		pos_sp_triplet->current.type = SETPOINT_TYPE_POSITION;
		pos_sp_triplet->current.alt = global_pos->alt;
		pos_sp_triplet->current.lon = global_pos->lon;
		pos_sp_triplet->current.lat = global_pos->lat;

		_navigator->set_position_setpoint_triplet_updated();

	}

	loiter_sub_mode = new_sub_mode;

	char * sub_mode_str;

	switch(new_sub_mode){
		case LOITER_SUB_MODE_AIM_AND_SHOOT:
			sub_mode_str = "Aim-and-shoot";
			break;
		case LOITER_SUB_MODE_LOOK_DOWN:
			sub_mode_str = "Look down";
			break;
		case LOITER_SUB_MODE_COME_TO_ME:
			sub_mode_str = "Come-to-me";
			break;
		case LOITER_SUB_MODE_LANDING:
			sub_mode_str = "Landing";
			break;
		case LOITER_SUB_MODE_TAKING_OFF:
			sub_mode_str = "Taking-off";
			break;
		case LOITER_SUB_MODE_LANDED:
			sub_mode_str = "Landed";
			break;
	}

	mavlink_log_info(_mavlink_fd, "[loiter] Loiter sub mode set to %s ! ", sub_mode_str);

}

void
Loiter::execute_command_in_taking_off(vehicle_command_s cmd) {
}

void
Loiter::takeoff()
{
	pos_sp_triplet->previous.valid = false;
	pos_sp_triplet->current.valid = true;
	pos_sp_triplet->next.valid = false;

	pos_sp_triplet->current.lat = global_pos->lat;
	pos_sp_triplet->current.lon = global_pos->lon;
	pos_sp_triplet->current.alt = global_pos->alt + _parameters.takeoff_alt;

    int toa = (int)_parameters.takeoff_alt;

    mavlink_log_info(_navigator->get_mavlink_fd(), "Use takeoff alt: %d", toa);

	pos_sp_triplet->current.yaw = NAN;
	pos_sp_triplet->current.type = SETPOINT_TYPE_TAKEOFF;

	_navigator->set_position_setpoint_triplet_updated();

}

void
Loiter::land()
{

	pos_sp_triplet->previous.valid = false;
	pos_sp_triplet->current.valid = true;
	pos_sp_triplet->next.valid = false;

	pos_sp_triplet->current.lat = global_pos->lat;
	pos_sp_triplet->current.lon = global_pos->lon;
	pos_sp_triplet->current.alt = global_pos->alt;
	pos_sp_triplet->current.yaw = NAN;
	pos_sp_triplet->current.type = SETPOINT_TYPE_LAND;

	_navigator->set_position_setpoint_triplet_updated();
}

void
Loiter::disarm()
{
	commander_request_s *commander_request = _navigator->get_commander_request();
	commander_request->request_type = V_DISARM;
	_navigator->set_commander_request_updated();
}
