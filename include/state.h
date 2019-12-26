#ifndef __STATE_H__
#define __STATE_H__

#include "server.h"
#include "http_server.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_connection.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct buffer_list_tag {
    uint8_t * data;
    size_t length;

    struct buffer_list_tag * next;
} buffer_list;

typedef struct {
    char camera_name[MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN];
    int width;
    int height;
    uint32_t framerate;
    uint32_t bitrate;
    int cameraNum;
    MMAL_COMPONENT_T * camera;
    MMAL_COMPONENT_T * encoder;
    MMAL_COMPONENT_T * image_encoder;
    MMAL_COMPONENT_T * splitter;
    MMAL_CONNECTION_T * splitter_connection;
    MMAL_CONNECTION_T * encoder_connection;
    MMAL_CONNECTION_T * image_encoder_connection;
    MMAL_POOL_T * encoder_pool;
    MMAL_POOL_T * image_encoder_pool;
    buffer_list * image_buffer_list;
    buffer_list ** image_buffer_end;

    MMAL_FOURCC_T encoding;
    int profile;
    int level;
    int sensor_mode;
    int abort;

    uint32_t jpeg_restart_interval;
    uint32_t jpeg_quality;

    int iso;
    int video_stabilization;
    MMAL_PARAM_EXPOSUREMETERINGMODE_T metering_mode;
    MMAL_PARAM_FLICKERAVOID_T flicker_avoid_mode;

    server_t video_server;
    server_t motion_server;
    http_server_t http_server;
} state_t;

#endif