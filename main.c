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
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_CAMERA_NUM 0

#define DEFAULT_ENCODING MMAL_ENCODING_H264
#define DEFAULT_ENCODING_PROFILE MMAL_VIDEO_PROFILE_H264_HIGH
#define DEFAULT_ENCODING_LEVEL MMAL_VIDEO_LEVEL_H264_42

#define DEFAULT_SENSOR_MODE 0

void __cyg_profile_func_enter (void *this_fn, void *call_site) __attribute__((no_instrument_function));
void __cyg_profile_func_exit  (void *this_fn, void *call_site) __attribute__((no_instrument_function));

void __cyg_profile_func_enter (void *this_fn, void *call_site)
{
// printf("Function Entry : %p %p \n", this_fn, call_site);
}

void __cyg_profile_func_exit (void *this_fn, void *call_site)
{
// printf("Function Exit : %p %p \n", this_fn, call_site);
}

const int camera_num = 0;

int interrupted = 0;

void handle_interrupt(int signal) {
    interrupted = 1;
}

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
    state->sensor_mode = DEFAULT_SENSOR_MODE;
    state->abort = 0;
    state->video_file = NULL;
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
   fprintf(stderr, "Camera control callback  cmd=0x%08x\n", buffer->cmd);

   if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED)
   {
      MMAL_EVENT_PARAMETER_CHANGED_T *param = (MMAL_EVENT_PARAMETER_CHANGED_T *)buffer->data;
      switch (param->hdr.id)
      {
      case MMAL_PARAMETER_CAMERA_SETTINGS:
      {
         MMAL_PARAMETER_CAMERA_SETTINGS_T *settings = (MMAL_PARAMETER_CAMERA_SETTINGS_T*)param;
         fprintf(stderr, "Exposure now %u, analog gain %u/%u, digital gain %u/%u\n",
                        settings->exposure,
                        settings->analog_gain.num, settings->analog_gain.den,
                        settings->digital_gain.num, settings->digital_gain.den);
         fprintf(stderr, "AWB R=%u/%u, B=%u/%u\n",
                        settings->awb_red_gain.num, settings->awb_red_gain.den,
                        settings->awb_blue_gain.num, settings->awb_blue_gain.den);
      }
      break;
      }
   }
   else if (buffer->cmd == MMAL_EVENT_ERROR)
   {
      fprintf(stderr, "No data received from sensor. Check all connections, including the Sunny one on the camera board\n");
   }
   else
   {
      fprintf(stderr, "Received unexpected camera control callback event, 0x%08x\n", buffer->cmd);
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
        fprintf(stderr, "Failed to create camera component\n");
        goto error;
    }

    // MMAL_PARAMETER_STEREOSCOPIC_MODE_T stereo_mode = { { MMAL_STEREOSCOPIC_MODE_NONE }, MMAL_FALSE, MMAL_FALSE };

    // status = raspicamcontrol_set_stereo_mode(camera->output[0], &stereo_mode);
    // status += raspicamcontrol_set_stereo_mode(camera->output[1], &stereo_mode);
    // status += raspicamcontrol_set_stereo_mode(camera->output[2], &stereo_mode);

    // if (status != MMAL_SUCCESS) {
    //     fprintf(stderr, "Could not set stereo mode : error %d\n", status);
    //     goto error;
    // }

    MMAL_PARAMETER_INT32_T camera_num = 
        {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, state->cameraNum};

    status = mmal_port_parameter_set(camera->control, &camera_num.hdr);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Could not select camera : error %d\n", status);
        goto error;
    }

    if (!camera->output_num) {
        status = MMAL_ENOSYS;
        fprintf(stderr, "Camera doesn't have output ports\n");
        goto error;
    }

    if ((status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, state->sensor_mode)) != MMAL_SUCCESS) {
        fprintf(stderr, "Could not set sensor mode\n");
        goto error;
    }


    preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
    video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
    still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

    // Enable the camera, and tell it its control callback function
    status = mmal_port_enable(camera->control, default_camera_control_callback);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Unable to enable control port : error %d\n", status);
        goto error;
    }

    fprintf(stderr, "size: %dx%d - %d\n", state->width, state->height, state->framerate);

    //    i don't know why but this makes us get an error:
    //   rtos_pool_aligned_malloc: Out of heap from allocating 1717988464 bytes 0x4 align (call from 0x3ee6fa06)  

    //  set up the camera configuration {
    MMAL_PARAMETER_CAMERA_CONFIG_T cam_config;
    memset(&cam_config, 0, sizeof(cam_config));


    cam_config.hdr.id = MMAL_PARAMETER_CAMERA_CONFIG;
    // cam_config.hdr.id = 0x3f3aeb70;
    cam_config.hdr.size = sizeof(cam_config);

    cam_config.max_stills_w = state->width;
    cam_config.max_stills_h = state->height;
    cam_config.stills_yuv422 = 0;
    cam_config.one_shot_stills = 0;
    cam_config.max_preview_video_w = state->width;
    cam_config.max_preview_video_h = state->height;
    cam_config.num_preview_video_frames = 3 + (state->framerate < 30 ? 0 : (state->framerate - 30)/10);
    cam_config.stills_capture_circular_buffer_height = 0;
    cam_config.fast_preview_resume = 0;
    cam_config.use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC;

    if ((status = mmal_port_parameter_set(camera->control, &cam_config.hdr)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not set camera config: %s\n", mmal_status_to_string(status));
        goto error;
    }

    if ((status = mmal_port_parameter_get(camera->control, &cam_config.hdr)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not get camera config: %s\n", mmal_status_to_string(status));
        goto error;
    }

    fprintf(stderr, "final camera config: %dx%d - %dx%d - %d\n", 
        cam_config.max_stills_w, cam_config.max_stills_h, 
        cam_config.max_preview_video_w, cam_config.max_preview_video_h,
        cam_config.num_preview_video_frames);

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

    mmal_log_dump_format(format);

    status = mmal_port_format_commit(preview_port);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "camera viewfinder format couldn't be set\n");
        goto error;
    }

    // // Set the encode format on the video  port

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
        fprintf(stderr, "camera video format couldn't be set\n");
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
        fprintf(stderr, "camera still format couldn't be set\n");
        goto error;
    }

    /* Ensure there are enough buffers to avoid dropping frames */
    if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
        still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

    /* Enable component */
    status = mmal_component_enable(camera);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "camera component couldn't be enabled\n");
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
        fprintf(stderr, "Unable to create video encoder component\n");
        goto error;
    }

    if (!encoder->input_num || !encoder->output_num)
    {
        status = MMAL_ENOSYS;
        fprintf(stderr, "Video encoder doesn't have input/output ports\n");
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
        fprintf(stderr, "Unable to set format on video encoder output port\n");
        goto error;
    }

    MMAL_PARAMETER_VIDEO_PROFILE_T  param;
    param.hdr.id = MMAL_PARAMETER_PROFILE;
    param.hdr.size = sizeof(param);

    param.profile[0].profile = state->profile;
    param.profile[0].level = state->level;

    status = mmal_port_parameter_set(encoder_output, &param.hdr);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Unable to set H264 profile\n");
        goto error;
    }

    if (mmal_port_parameter_set_boolean(encoder_input, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, MMAL_TRUE) != MMAL_SUCCESS)
    {
        fprintf(stderr, "Unable to set immutable input flag\n");
        // Continue rather than abort..
    }

    //set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, MMAL_TRUE) != MMAL_SUCCESS)
    {
        fprintf(stderr, "failed to set INLINE HEADER FLAG parameters\n");
        // Continue rather than abort..
    }

    //set flag for add SPS TIMING
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, MMAL_TRUE) != MMAL_SUCCESS)
    {
        fprintf(stderr, "failed to set SPS TIMINGS FLAG parameters\n");
        // Continue rather than abort..
    }

    //set INLINE VECTORS flag to request motion vector estimates
    if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, MMAL_TRUE) != MMAL_SUCCESS)
    {
        fprintf(stderr, "failed to set INLINE VECTORS parameters\n");
        // Continue rather than abort..
    }

    //  Enable component
    status = mmal_component_enable(encoder);

    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr, "Unable to enable video encoder component\n");
        goto error;
    }

    /* Create pool of buffer headers for the output port to consume */
    pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

    if (!pool) {
        fprintf(stderr, "Failed to create buffer header pool for encoder output port %s\n", encoder_output->name);
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

void get_sensor_defaults(int camera_num, char *camera_name, int *width, int *height )
{
   MMAL_COMPONENT_T *camera_info;
   MMAL_STATUS_T status;

   // Default to the OV5647 setup
   strncpy(camera_name, "OV5647", MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN);

   // Try to get the camera name and maximum supported resolution
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_info);
   if (status == MMAL_SUCCESS)
   {
      MMAL_PARAMETER_CAMERA_INFO_T param;
      param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
      param.hdr.size = sizeof(param)-4;  // Deliberately undersize to check firmware version
      status = mmal_port_parameter_get(camera_info->control, &param.hdr);

      if (status != MMAL_SUCCESS)
      {
         fprintf(stderr, "running newer firmware\n");
         // Running on newer firmware
         param.hdr.size = sizeof(param);
         status = mmal_port_parameter_get(camera_info->control, &param.hdr);
         if (status == MMAL_SUCCESS && param.num_cameras > camera_num)
         {
            fprintf(stderr, "successfully got camera info: %dx%d\n", param.cameras[camera_num].max_width, param.cameras[camera_num].max_height);
            // Take the parameters from the first camera listed.
            if (*width == 0)
               *width = param.cameras[camera_num].max_width;
            if (*height == 0)
               *height = param.cameras[camera_num].max_height;
            strncpy(camera_name, param.cameras[camera_num].camera_name, MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN);
            camera_name[MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN-1] = 0;
         }
         else
            fprintf(stderr, "Cannot read camera info, keeping the defaults for OV5647\n");
      }
      else
      {
         // Older firmware
         // Nothing to do here, keep the defaults for OV5647
      }

      mmal_component_destroy(camera_info);
   }
   else
   {
      fprintf(stderr, "Failed to create camera_info component\n");
   }

   // default to OV5647 if nothing detected..
   if (*width == 0)
      *width = 2592;
   if (*height == 0)
      *height = 1944;
}

int raspicamcontrol_set_stereo_mode(MMAL_PORT_T *port, MMAL_PARAMETER_STEREOSCOPIC_MODE_T *stereo_mode)
{
   MMAL_PARAMETER_STEREOSCOPIC_MODE_T stereo = { {MMAL_PARAMETER_STEREOSCOPIC_MODE, sizeof(stereo)},
      MMAL_STEREOSCOPIC_MODE_NONE, MMAL_FALSE, MMAL_FALSE
   };
   if (stereo_mode->mode != MMAL_STEREOSCOPIC_MODE_NONE)
   {
      stereo.mode = stereo_mode->mode;
      stereo.decimate = stereo_mode->decimate;
      stereo.swap_eyes = stereo_mode->swap_eyes;
   }
   return mmal_status_to_int(mmal_port_parameter_set(port, &stereo.hdr));
}

static void check_camera_model(int cam_num)
{
   MMAL_COMPONENT_T *camera_info;
   MMAL_STATUS_T status;

   // Try to get the camera name
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_info);
   if (status == MMAL_SUCCESS)
   {
      MMAL_PARAMETER_CAMERA_INFO_T param;
      param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
      param.hdr.size = sizeof(param)-4;  // Deliberately undersize to check firmware version
      status = mmal_port_parameter_get(camera_info->control, &param.hdr);

      if (status != MMAL_SUCCESS)
      {
         // Running on newer firmware
         param.hdr.size = sizeof(param);
         status = mmal_port_parameter_get(camera_info->control, &param.hdr);
         if (status == MMAL_SUCCESS && param.num_cameras > cam_num)
         {
            if (!strncmp(param.cameras[cam_num].camera_name, "toshh2c", 7))
            {
               fprintf(stderr, "The driver for the TC358743 HDMI to CSI2 chip you are using is NOT supported.\n");
               fprintf(stderr, "They were written for a demo purposes only, and are in the firmware on an as-is\n");
               fprintf(stderr, "basis and therefore requests for support or changes will not be acted on.\n\n");
            }
         }
      }

      mmal_component_destroy(camera_info);
   }
}

static void encoder_buffer_callback(MMAL_PORT_T * port, MMAL_BUFFER_HEADER_T * buffer) {
    MMAL_BUFFER_HEADER_T * new_buffer;
    size_t bytes_written = 0;

    fprintf(stderr, "buffer callback\n");

    state_t * state = (state_t*)port->userdata;

    if (buffer->length > 0) {
        mmal_buffer_header_mem_lock(buffer);

        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) {
            fprintf(stderr, "got motion vectors\n");
            // motion vectors
            // do nothing for now
            bytes_written = buffer->length;
        } else if (state->video_file != NULL) {
            fprintf(stderr, "writing video data\n");
            // video data
            bytes_written = fwrite(buffer->data, 1, buffer->length, state->video_file);

        }

        mmal_buffer_header_mem_unlock(buffer);
    }

    if (bytes_written != buffer->length) {
        // some sort of problem, abort
        fprintf(stderr, "not enough bytes written, got %d written %d\n", buffer->length, bytes_written);
        state->abort = 1;
    }

    mmal_buffer_header_release(buffer);

    if (port->is_enabled) {
        MMAL_STATUS_T status;
        new_buffer = mmal_queue_get(state->encoder_pool->queue);

        if (new_buffer != NULL)
            status = mmal_port_send_buffer(port, new_buffer);
        
        if (new_buffer == NULL || status != MMAL_SUCCESS)
            fprintf(stderr, "unable to return buffer to the encoder port\n");
    }
}


int main(int ac, char ** av) {
    MMAL_STATUS_T status = MMAL_SUCCESS;
    state_t state;
    int exit_code = 0;

    initialize_state(&state);

    MMAL_PORT_T * camera_preview_port = NULL;
    MMAL_PORT_T * camera_video_port = NULL;
    MMAL_PORT_T * camera_still_port = NULL;
    MMAL_PORT_T * encoder_input_port = NULL;
    MMAL_PORT_T * encoder_output_port = NULL;

    if (ac > 1) {
        if ((state.video_file = fopen(av[1], "wb+")) == NULL) {
            perror(av[1]);
            exit(-1);
        }
    }

    bcm_host_init();
    vcos_log_register("SimpleCam", VCOS_LOG_CATEGORY);

    get_sensor_defaults(state.cameraNum, state.camera_name, &state.width, &state.height);

    fprintf(stderr, "sensor defaults: %s -- %dx%d\n", state.camera_name, state.width, state.height);

    check_camera_model(state.cameraNum);

    if ((status = create_camera_component(&state)) != MMAL_SUCCESS) {
        fprintf(stderr, "failed to create camera component\n"); 
        goto cleanup;
    }

    if ((status = create_encoder_component(&state)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not create encoder component\n");
        goto cleanup;
    }

    camera_preview_port = state.camera->output[MMAL_CAMERA_PREVIEW_PORT];
    camera_video_port = state.camera->output[MMAL_CAMERA_VIDEO_PORT];
    camera_still_port = state.camera->output[MMAL_CAMERA_CAPTURE_PORT];
    encoder_input_port = state.encoder->input[0];
    encoder_output_port = state.encoder->output[0];

    if ((status = mmal_connection_create(&state.encoder_connection, camera_video_port, encoder_input_port,  
        MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT)) != MMAL_SUCCESS) 
    {
        fprintf(stderr, "error creating camera to encoder connections\n");
        goto cleanup;
    }

    if ((status = mmal_connection_enable(state.encoder_connection)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not enable connection\n");
        goto cleanup;
    }

    encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T*)&state;

    if ((status = mmal_port_enable(encoder_output_port, encoder_buffer_callback)) != MMAL_SUCCESS) {
        fprintf(stderr, "error enabling encoder output port\n");
        goto cleanup;
    }

    int num = mmal_queue_length(state.encoder_pool->queue);
    for(; num > 0; num--) {
        MMAL_BUFFER_HEADER_T * buffer = mmal_queue_get(state.encoder_pool->queue);

        if (!buffer) {
            fprintf(stderr, "unable to get a required buffer %d from the pool\n", num);
            continue;
        }

        if(mmal_port_send_buffer(encoder_output_port, buffer) != MMAL_SUCCESS) {
            fprintf(stderr, "unable to send buffer to encoder output port\n");
            continue;
        }
        fprintf(stderr, "enabled buffer %d\n", num);
    }

    
    if((status = mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, MMAL_TRUE)) != MMAL_SUCCESS) {
        fprintf(stderr, "unable to start capture: %s\n", mmal_status_to_string(status));
        goto cleanup;
    }

    signal(SIGINT, handle_interrupt);

    // TODO: temporary, instead use condition variables
    while(!state.abort && !interrupted)        
        vcos_sleep(100);

    signal(SIGINT, SIG_DFL);
    

cleanup:
    fprintf(stderr, "cleaning up...\n");

    mmal_status_to_int(status);

    if (state.encoder_connection != NULL) {
        if (state.encoder_connection->is_enabled) {
            mmal_connection_disable(state.encoder_connection);
        }
        mmal_connection_destroy(state.encoder_connection);
    }

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

    if (state.video_file != NULL) {
        fclose(state.video_file);
    }

    return exit_code;
}
