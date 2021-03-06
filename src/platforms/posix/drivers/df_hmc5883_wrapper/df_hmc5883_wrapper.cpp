/****************************************************************************
 *
 * Copyright (c) 2016 PX4 Development Team. All rights reserved.
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
 * @file df_hmc5883_wrapper.cpp
 * Lightweight driver to access the HMC5883 of the DriverFramework.
 *
 * @author Julian Oes <julian@oes.ch>
 */

#include <px4_config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <px4_getopt.h>
#include <errno.h>

#include <systemlib/perf_counter.h>
#include <systemlib/err.h>

#include <drivers/drv_mag.h>

#include <board_config.h>
//#include <mathlib/math/filter/LowPassFilter2p.hpp>
//#include <lib/conversion/rotation.h>

#include <hmc5883/HMC5883.hpp>
#include <DevMgr.hpp>


extern "C" { __EXPORT int df_hmc5883_wrapper_main(int argc, char *argv[]); }

using namespace DriverFramework;


class DfHmc9250Wrapper : public HMC5883
{
public:
	DfHmc9250Wrapper(/*enum Rotation rotation*/);
	~DfHmc9250Wrapper();


	/**
	 * Start automatic measurement.
	 *
	 * @return 0 on success
	 */
	int		start();

	/**
	 * Stop automatic measurement.
	 *
	 * @return 0 on success
	 */
	int		stop();

private:
	int _publish(struct mag_sensor_data &data);

	//enum Rotation		_rotation;

	orb_advert_t		_mag_topic;

	int			_mag_orb_class_instance;

	perf_counter_t		_mag_sample_perf;

};

DfHmc9250Wrapper::DfHmc9250Wrapper(/*enum Rotation rotation*/) :
	HMC5883(MAG_DEVICE_PATH),
	_mag_topic(nullptr),
	_mag_orb_class_instance(-1),
	_mag_sample_perf(perf_alloc(PC_ELAPSED, "df_mag_read"))
	/*_rotation(rotation)*/
{
}

DfHmc9250Wrapper::~DfHmc9250Wrapper()
{
	perf_free(_mag_sample_perf);
}

int DfHmc9250Wrapper::start()
{
	// TODO: don't publish garbage here
	mag_report mag_report = {};
	_mag_topic = orb_advertise_multi(ORB_ID(sensor_mag), &mag_report,
					 &_mag_orb_class_instance, ORB_PRIO_DEFAULT);

	if (_mag_topic == nullptr) {
		PX4_ERR("sensor_mag advert fail");
		return -1;
	}

	/* Init device and start sensor. */
	int ret = init();

	if (ret != 0) {
		PX4_ERR("HMC5883 init fail: %d", ret);
		return ret;
	}

	ret = HMC5883::start();

	if (ret != 0) {
		PX4_ERR("HMC5883 start fail: %d", ret);
		return ret;
	}

	return 0;
}

int DfHmc9250Wrapper::stop()
{
	/* Stop sensor. */
	int ret = HMC5883::stop();

	if (ret != 0) {
		PX4_ERR("HMC5883 stop fail: %d", ret);
		return ret;
	}

	return 0;
}

int DfHmc9250Wrapper::_publish(struct mag_sensor_data &data)
{
	/* Publish mag first. */
	perf_begin(_mag_sample_perf);

	mag_report mag_report = {};
	mag_report.timestamp = data.last_read_time_usec;

	/* The standard external mag by 3DR has x pointing to the
	 * right, y pointing backwards, and z down, therefore switch x
	 * and y and invert y. */
	const float tmp = data.field_x_ga;
	data.field_x_ga = -data.field_y_ga;
	data.field_y_ga = tmp;

	// TODO: remove these (or get the values)
	mag_report.x_raw = NAN;
	mag_report.y_raw = NAN;
	mag_report.z_raw = NAN;
	mag_report.x = data.field_x_ga;
	mag_report.y = data.field_y_ga;
	mag_report.z = data.field_z_ga;

	// TODO: get these right
	//mag_report.scaling = -1.0f;
	//mag_report.range_m_s2 = -1.0f;

	// TODO: when is this ever blocked?
	if (!(m_pub_blocked)) {

		if (_mag_topic != nullptr) {
			orb_publish(ORB_ID(sensor_mag), _mag_topic, &mag_report);
		}

	}

	perf_end(_mag_sample_perf);

	/* Notify anyone waiting for data. */
	DevMgr::updateNotify(*this);

	return 0;
};


namespace df_hmc5883_wrapper
{

DfHmc9250Wrapper *g_dev = nullptr;

int start(/* enum Rotation rotation */);
int stop();
int info();
void usage();

int start(/*enum Rotation rotation*/)
{
	g_dev = new DfHmc9250Wrapper(/*rotation*/);

	if (g_dev == nullptr) {
		PX4_ERR("failed instantiating DfHmc9250Wrapper object");
		return -1;
	}

	int ret = g_dev->start();

	if (ret != 0) {
		PX4_ERR("DfHmc9250Wrapper start failed");
		return ret;
	}

	// Open the MAG sensor
	DevHandle h;
	DevMgr::getHandle(MAG_DEVICE_PATH, h);

	if (!h.isValid()) {
		DF_LOG_INFO("Error: unable to obtain a valid handle for the receiver at: %s (%d)",
			    MAG_DEVICE_PATH, h.getError());
		return -1;
	}

	DevMgr::releaseHandle(h);

	return 0;
}

int stop()
{
	if (g_dev == nullptr) {
		PX4_ERR("driver not running");
		return 1;
	}

	int ret = g_dev->stop();

	if (ret != 0) {
		PX4_ERR("driver could not be stopped");
		return ret;
	}

	delete g_dev;
	g_dev = nullptr;
	return 0;
}

/**
 * Print a little info about the driver.
 */
int
info()
{
	if (g_dev == nullptr) {
		PX4_ERR("driver not running");
		return 1;
	}

	PX4_DEBUG("state @ %p", g_dev);

	return 0;
}

void
usage()
{
	PX4_WARN("Usage: df_hmc5883_wrapper 'start', 'info', 'stop'");
	PX4_WARN("options:");
	//PX4_WARN("    -R rotation");
}

} // namespace df_hmc5883_wrapper


int
df_hmc5883_wrapper_main(int argc, char *argv[])
{
	int ch;
	// enum Rotation rotation = ROTATION_NONE;
	int ret = 0;
	int myoptind = 1;
	const char *myoptarg = NULL;

	/* jump over start/off/etc and look at options first */
	while ((ch = px4_getopt(argc, argv, "R:", &myoptind, &myoptarg)) != EOF) {
		switch (ch) {
		//case 'R':
		//	rotation = (enum Rotation)atoi(myoptarg);
		//	break;

		default:
			df_hmc5883_wrapper::usage();
			return 0;
		}
	}

	if (argc <= 1) {
		df_hmc5883_wrapper::usage();
		return 1;
	}

	const char *verb = argv[myoptind];


	if (!strcmp(verb, "start")) {
		ret = df_hmc5883_wrapper::start(/*rotation*/);
	}

	else if (!strcmp(verb, "stop")) {
		ret = df_hmc5883_wrapper::stop();
	}

	else if (!strcmp(verb, "info")) {
		ret = df_hmc5883_wrapper::info();
	}

	else {
		df_hmc5883_wrapper::usage();
		return 1;
	}

	return ret;
}
