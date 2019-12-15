#ifndef __STATE_H__
#define __STATE_H__

#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_connection.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    char camera_name[MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN];
    int width;
    int height;
    uint32_t framerate;
    uint32_t bitrate;
    int cameraNum;
    MMAL_COMPONENT_T * camera;
    MMAL_COMPONENT_T * encoder;
    MMAL_CONNECTION_T * encoder_connection;
    MMAL_POOL_T * encoder_pool;
    MMAL_FOURCC_T encoding;
    int profile;
    int level;
    int sensor_mode;
    int abort;

    FILE * video_file;
} state_t;

#endif