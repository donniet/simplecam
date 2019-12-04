#include <stdio.h>
#include <stdlib.h>

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"


// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

#define VIDEO_OUTPUT_BUFFERS_NUM 3

#define DEFAULT_FRAMERATE 25
#define DEFAULT_WIDTH 1440
#define DEFAULT_HEIGHT 1080
#define DEFAULT_CAMERA_NUM 0

#define DEFAULT_ENCODING MMAL_ENCODING_H264
#define DEFAULT_ENCODING_PROFILE MMAL_VIDEO_PROFILE_H264_HIGH
#define DEFAULT_ENCODING_LEVEL MMAL_VIDEO_LEVEL_H264_42

const int camera_num = 0;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t framerate;
    uint32_t bitrate;
    uint32_t cameraNum;
    MMAL_COMPONENT_T * camera;
    MMAL_COMPONENT_T * encoder;
    MMAL_CONNECTION_T * encoder_connection;
    MMAL_POOL_T * encoder_pool;
    MMAL_FOURCC_T encoding;
    int profile;
    int level;
} state_t;

void initialize_state(state_t * state) {
    state->camera = NULL;
    state->encoder = NULL;
    state->encoder_pool = NULL;
    state->encoder_connection = NULL;
    state->cameraNum = camera_num;
    state->framerate = DEFAULT_FRAMERATE;
    state->width = DEFAULT_WIDTH;
    state->height = DEFAULT_HEIGHT;
    state->encoding = DEFAULT_ENCODING;
    state->profile = DEFAULT_ENCODING_PROFILE;
    state->level = DEFAULT_ENCODING_LEVEL;
}


/**
 * Convert a MMAL status return value to a simple boolean of success
 * ALso displays a fault if code is not success
 *
 * @param status The error code to convert
 * @return 0 if status is success, 1 otherwise
 */
int mmal_status_to_int(MMAL_STATUS_T status)
{
   if (status == MMAL_SUCCESS)
      return 0;
   else
   {
      switch (status)
      {
      case MMAL_ENOMEM :
         vcos_log_error("Out of memory");
         break;
      case MMAL_ENOSPC :
         vcos_log_error("Out of resources (other than memory)");
         break;
      case MMAL_EINVAL:
         vcos_log_error("Argument is invalid");
         break;
      case MMAL_ENOSYS :
         vcos_log_error("Function not implemented");
         break;
      case MMAL_ENOENT :
         vcos_log_error("No such file or directory");
         break;
      case MMAL_ENXIO :
         vcos_log_error("No such device or address");
         break;
      case MMAL_EIO :
         vcos_log_error("I/O error");
         break;
      case MMAL_ESPIPE :
         vcos_log_error("Illegal seek");
         break;
      case MMAL_ECORRUPT :
         vcos_log_error("Data is corrupt \attention FIXME: not POSIX");
         break;
      case MMAL_ENOTREADY :
         vcos_log_error("Component is not ready \attention FIXME: not POSIX");
         break;
      case MMAL_ECONFIG :
         vcos_log_error("Component is not configured \attention FIXME: not POSIX");
         break;
      case MMAL_EISCONN :
         vcos_log_error("Port is already connected ");
         break;
      case MMAL_ENOTCONN :
         vcos_log_error("Port is disconnected");
         break;
      case MMAL_EAGAIN :
         vcos_log_error("Resource temporarily unavailable. Try again later");
         break;
      case MMAL_EFAULT :
         vcos_log_error("Bad address");
         break;
      default :
         vcos_log_error("Unknown status error");
         break;
      }

      return 1;
   }
}

void default_camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   fprintf(stderr, "Camera control callback  cmd=0x%08x", buffer->cmd);

   if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED)
   {
      MMAL_EVENT_PARAMETER_CHANGED_T *param = (MMAL_EVENT_PARAMETER_CHANGED_T *)buffer->data;
      switch (param->hdr.id)
      {
      case MMAL_PARAMETER_CAMERA_SETTINGS:
      {
         MMAL_PARAMETER_CAMERA_SETTINGS_T *settings = (MMAL_PARAMETER_CAMERA_SETTINGS_T*)param;
         fprintf(stderr, "Exposure now %u, analog gain %u/%u, digital gain %u/%u",
                        settings->exposure,
                        settings->analog_gain.num, settings->analog_gain.den,
                        settings->digital_gain.num, settings->digital_gain.den);
         fprintf(stderr, "AWB R=%u/%u, B=%u/%u",
                        settings->awb_red_gain.num, settings->awb_red_gain.den,
                        settings->awb_blue_gain.num, settings->awb_blue_gain.den);
      }
      break;
      }
   }
   else if (buffer->cmd == MMAL_EVENT_ERROR)
   {
      fprintf(stderr, "No data received from sensor. Check all connections, including the Sunny one on the camera board");
   }
   else
   {
      fprintf(stderr, "Received unexpected camera control callback event, 0x%08x", buffer->cmd);
   }

   mmal_buffer_header_release(buffer);
}

MMAL_STATUS_T create_camera_component(state_t * state) {
    MMAL_COMPONENT_T *camera = 0;
    MMAL_ES_FORMAT_T *format;
    MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
    MMAL_STATUS_T status;

    /* Create the component */
    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Failed to create camera component");
        goto error;
    }

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Could not set stereo mode : error %d", status);
        goto error;
    }

    MMAL_PARAMETER_INT32_T camera_num = {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, state->cameraNum};

    status = mmal_port_parameter_set(camera->control, &camera_num.hdr);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Could not select camera : error %d", status);
        goto error;
    }

    if (!camera->output_num) {
        status = MMAL_ENOSYS;
        fprintf(stderr, "Camera doesn't have output ports");
        goto error;
    }

    preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
    video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
    still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

    // Enable the camera, and tell it its control callback function
    status = mmal_port_enable(camera->control, default_camera_control_callback);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Unable to enable control port : error %d", status);
        goto error;
    }

    {
    //  set up the camera configuration {
        MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
        {
            { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
            .max_stills_w = state->width,
            .max_stills_h = state->height,
            .stills_yuv422 = 0,
            .one_shot_stills = 0,
            .max_preview_video_w = state->width,
            .max_preview_video_h = state->height,
            .num_preview_video_frames = 3 + vcos_max(0, (state->framerate-30)/10),
            .stills_capture_circular_buffer_height = 0,
            .fast_preview_resume = 0,
            .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC
        };
        mmal_port_parameter_set(camera->control, &cam_config.hdr);
    }

    // Now set up the port formats

    // Set the encode format on the Preview port
    // HW limitations mean we need the preview to be the same size as the required recorded output

    format = preview_port->format;

    format->encoding = MMAL_ENCODING_OPAQUE;
    format->encoding_variant = MMAL_ENCODING_I420;
    format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
    format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = state->width;
    format->es->video.crop.height = state->height;
    format->es->video.frame_rate.num = state->framerate;
    format->es->video.frame_rate.den = 1;

    status = mmal_port_format_commit(preview_port);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "camera viewfinder format couldn't be set");
        goto error;
    }

    // Set the encode format on the video  port

    format = video_port->format;
    format->encoding_variant = MMAL_ENCODING_I420;
    format->encoding = MMAL_ENCODING_OPAQUE;
    format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
    format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = state->width;
    format->es->video.crop.height = state->height;
    format->es->video.frame_rate.num = state->framerate;
    format->es->video.frame_rate.den = 1;

    status = mmal_port_format_commit(video_port);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "camera video format couldn't be set");
        goto error;
    }

    // Ensure there are enough buffers to avoid dropping frames
    if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
        video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;


    // Set the encode format on the still  port

    format = still_port->format;

    format->encoding = MMAL_ENCODING_OPAQUE;
    format->encoding_variant = MMAL_ENCODING_I420;

    format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
    format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = state->width;
    format->es->video.crop.height = state->height;
    format->es->video.frame_rate.num = 0;
    format->es->video.frame_rate.den = 1;

    status = mmal_port_format_commit(still_port);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "camera still format couldn't be set");
        goto error;
    }

    /* Ensure there are enough buffers to avoid dropping frames */
    if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
        still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

    /* Enable component */
    status = mmal_component_enable(camera);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "camera component couldn't be enabled");
        goto error;
    }

    // Note: this sets lots of parameters that were not individually addressed before.
    // raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

    state->camera = camera;

    return status;

error:

    if (camera)
        mmal_component_destroy(camera);

    return status;
}

MMAL_STATUS_T create_encoder_component(state_t * state) {
    MMAL_COMPONENT_T *encoder = NULL;
    MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);

    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr, "Unable to create video encoder component");
        goto error;
    }

    if (!encoder->input_num || !encoder->output_num)
    {
        status = MMAL_ENOSYS;
        fprintf(stderr, "Video encoder doesn't have input/output ports");
        goto error;
    }

    encoder_input = encoder->input[0];
    encoder_output = encoder->output[0];

    // We want same format on input and output
    mmal_format_copy(encoder_output->format, encoder_input->format);

    // Only supporting H264 at the moment
    encoder_output->format->encoding = state->encoding;
    encoder_output->format->bitrate = state->bitrate;
    encoder_output->buffer_size = encoder_output->buffer_size_recommended;

    if (encoder_output->buffer_size < encoder_output->buffer_size_min)
        encoder_output->buffer_size = encoder_output->buffer_size_min;

    encoder_output->buffer_num = encoder_output->buffer_num_recommended;

    if (encoder_output->buffer_num < encoder_output->buffer_num_min)
        encoder_output->buffer_num = encoder_output->buffer_num_min;

    // We need to set the frame rate on output to 0, to ensure it gets
    // updated correctly from the input framerate when port connected
    encoder_output->format->es->video.frame_rate.num = 0;
    encoder_output->format->es->video.frame_rate.den = 1;

    // Commit the port changes to the output port
    status = mmal_port_format_commit(encoder_output);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Unable to set format on video encoder output port");
        goto error;
    }

    MMAL_PARAMETER_VIDEO_PROFILE_T  param;
    param.hdr.id = MMAL_PARAMETER_PROFILE;
    param.hdr.size = sizeof(param);

    param.profile[0].profile = state->profile;
    param.profile[0].level = state->level;

    status = mmal_port_parameter_set(encoder_output, &param.hdr);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Unable to set H264 profile");
        goto error;
    }

    if (mmal_port_parameter_set_boolean(encoder_input, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, MMAL_TRUE) != MMAL_SUCCESS)
    {
        fprintf(stderr, "Unable to set immutable input flag");
        // Continue rather than abort..
    }

    //set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, MMAL_TRUE) != MMAL_SUCCESS)
    {
        fprintf(stderr, "failed to set INLINE HEADER FLAG parameters");
        // Continue rather than abort..
    }

    //set flag for add SPS TIMING
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, MMAL_TRUE) != MMAL_SUCCESS)
    {
        fprintf(stderr, "failed to set SPS TIMINGS FLAG parameters");
        // Continue rather than abort..
    }

    //set INLINE VECTORS flag to request motion vector estimates
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, MMAL_TRUE) != MMAL_SUCCESS)
    {
        fprintf(stderr, "failed to set INLINE VECTORS parameters");
        // Continue rather than abort..
    }

    //  Enable component
    status = mmal_component_enable(encoder);

    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr, "Unable to enable video encoder component");
        goto error;
    }

    /* Create pool of buffer headers for the output port to consume */
    pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

    if (!pool) {
        fprintf(stderr, "Failed to create buffer header pool for encoder output port %s", encoder_output->name);
    }

    state->encoder_pool = pool;
    state->encoder = encoder;

    return status;

error:
    if (encoder)
        mmal_component_destroy(encoder);

    state->encoder = NULL;

    return status;
}


int main(int ac, char ** av) {
    MMAL_STATUS_T status = MMAL_SUCCESS;
    state_t state;
    int exit_code = 0;

    initialize_state(&state);

    MMAL_PORT_T * camera_video_port = NULL;
    MMAL_PORT_T * camera_still_port = NULL;
    MMAL_PORT_T * encoder_input_port = NULL;
    MMAL_PORT_T * encoder_output_port = NULL;

    bcm_host_init();

    if ((status = create_camera_component(&state)) != MMAL_SUCCESS) {
        fprintf(stderr, "failed to create camera component"); 
        goto cleanup;
    }

    if ((status = create_encoder_component(&state)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not create encoder component");
        goto cleanup;
    }

    camera_video_port = state.camera->output[MMAL_CAMERA_VIDEO_PORT];
    camera_still_port = state.camera->output[MMAL_CAMERA_CAPTURE_PORT];
    encoder_input_port = state.encoder->input[0];
    encoder_output_port = state.encoder->output[0];

    if ((status = mmal_connection_create(&state.encoder_connection, camera_video_port, encoder_input_port,  
        MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT)) != MMAL_SUCCESS) 
    {
        fprintf(stderr, "error creating camera to encoder connections");
        goto cleanup;
    }

    if ((status = mmal_connection_enable(state.encoder_connection)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not enable connection");
        goto cleanup;
    }

    

cleanup:

    mmal_status_to_int(status);

    if (camera_video_port != NULL && camera_video_port->is_enabled) {
        mmal_port_disable(camera_video_port);
    }
    if (encoder_output_port != NULL && encoder_output_port->is_enabled) {
        mmal_port_disable(encoder_output_port);
    }
    if (state.encoder_connection != NULL) {
        mmal_connection_destroy(state.encoder_connection);
    }
    if (state.camera != NULL) {
        mmal_component_destroy(state.camera);
    }
    if (state.encoder != NULL) {
        mmal_component_destroy(state.encoder);
    }

    return exit_code;
}