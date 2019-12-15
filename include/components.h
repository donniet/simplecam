
#ifndef __COMPONENTS_H__
#define __COMPONENTS_H__

#include "state.h"

#include "interface/mmal/mmal.h"

#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2


#define VIDEO_OUTPUT_BUFFERS_NUM 3


int mmal_status_to_int(MMAL_STATUS_T status);
void default_camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
MMAL_STATUS_T create_camera_component(state_t * state);
MMAL_STATUS_T create_encoder_component(state_t * state);
void get_sensor_defaults(int camera_num, char *camera_name, int *width, int *height );
int raspicamcontrol_set_stereo_mode(MMAL_PORT_T *port, MMAL_PARAMETER_STEREOSCOPIC_MODE_T *stereo_mode);
void check_camera_model(int cam_num);



#endif
