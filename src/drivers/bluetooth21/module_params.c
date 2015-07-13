#include <systemlib/param/param.h>

#include "module_params.hpp"

/*
 * Does telemetry port have CTS and RTS signals connected to MCU?
 * 0 -- no, anything else -- yes.
 */
PARAM_DEFINE_INT32(A_TELEMETRY_FLOW, CONFIG_TELEMETRY_HAS_CTSRTS);

/*
 * Telemetry mode:
 *    0 -- Plain radio modem.
 *    1 -- Long-range bluetooth listen.
 *    2 -- Long-range bluetooth listen on BL600 port, FMU #22.
 * 1001 -- Long-range bluetooth one-connect.
 * 1002 -- Long-range bluetooth one-connect on BL600 port, FMU #22.
 *
 * Warning: Default and invalid values keep telemetry disabled!
 *
 */
PARAM_DEFINE_INT32(A_TELEMETRY_MODE, BT_PARAM_DEFAULT);

/*
 * Device ID visible to user as a bluetooth name as one of
 *   AirDog %i, AirLeash %i, PX4 %i.
 */
PARAM_DEFINE_INT32(A_DEVICE_ID, BT_PARAM_DEFAULT);

/*
 * Factory address index to connect to (for non-listen modes).
 *
 * A value in the range [0, n_factory_addresses) sets the connect mode
 * and the value is number of factory address to connect to.
 *
 * Default `-1` and any value outside the range:
 *  at leash means "search nearest drone",
 *  at drone   --   listen.
 */
PARAM_DEFINE_INT32(A_BT_CONNECT_TO, BT_PARAM_DEFAULT);
