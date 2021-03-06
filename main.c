
#include "state.h"
#include "components.h"
#include "server.h"

#include "bcm_host.h"
#include "interface/vcos/vcos.h"
#include "interface/vcos/vcos_semaphore.h"
#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"


#include "interface/mmal/mmal.h"


#include <stdio.h>
#include <stdlib.h>



// Standard port setting for the camera component



#define DEFAULT_FRAMERATE 25
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_BITRATE 25000000
#define DEFAULT_CAMERA_NUM 0

#define DEFAULT_ENCODING MMAL_ENCODING_H264
#define DEFAULT_ENCODING_PROFILE MMAL_VIDEO_PROFILE_H264_HIGH
#define DEFAULT_ENCODING_LEVEL MMAL_VIDEO_LEVEL_H264_42

#define DEFAULT_SENSOR_MODE 0
#define DEFAULT_ISO 0
#define DEFAULT_METERING_MODE MMAL_PARAM_EXPOSUREMETERINGMODE_AVERAGE
#define DEFAULT_VIDEO_STABILIZATION 0
#define DEFAULT_FLICKERAVOID_MODE MMAL_PARAM_FLICKERAVOID_60HZ

VCOS_SEMAPHORE_T interrupt;

void handle_interrupt(int signal) {
    vcos_semaphore_post(&interrupt);
}


void initialize_state(state_t * state) {
    state->camera = NULL;
    state->encoder = NULL;
    state->encoder_pool = NULL;
    state->image_encoder = NULL;
    state->image_encoder_pool = NULL;
    state->encoder_connection = NULL;
    state->image_encoder_connection = NULL;
    state->cameraNum = DEFAULT_CAMERA_NUM;
    state->framerate = DEFAULT_FRAMERATE;
    state->width = DEFAULT_WIDTH;
    state->height = DEFAULT_HEIGHT;
    state->encoding = DEFAULT_ENCODING;
    state->profile = DEFAULT_ENCODING_PROFILE;
    state->level = DEFAULT_ENCODING_LEVEL;
    state->sensor_mode = DEFAULT_SENSOR_MODE;
    state->bitrate = DEFAULT_BITRATE;
    state->iso = DEFAULT_ISO;
    state->metering_mode = DEFAULT_METERING_MODE;
    state->video_stabilization = DEFAULT_VIDEO_STABILIZATION;
    state->flicker_avoid_mode = DEFAULT_FLICKERAVOID_MODE;
    state->jpeg_quality = 85;
    state->jpeg_restart_interval = 0;
    state->image_buffer_list = NULL;
    state->image_buffer_end = &state->image_buffer_list;

    state->abort = 0;
    // state->video_file = NULL;
}


static void image_buffer_callback(MMAL_PORT_T * port, MMAL_BUFFER_HEADER_T * buffer) {
    state_t * state = (state_t*)port->userdata;

    if (buffer->length > 0) {
        mmal_buffer_header_mem_lock(buffer);

        *state->image_buffer_end = (buffer_list*)malloc(sizeof(buffer_list));

        buffer_list * p = *state->image_buffer_end;

        p->data = (uint8_t*)malloc(buffer->length);
        p->length = buffer->length;
        p->next = NULL;

        memcpy(p->data, buffer->data, buffer->length);
        state->image_buffer_end = &p->next;

        mmal_buffer_header_mem_unlock(buffer);

        // Now flag if we have completed
        if (buffer->flags & (MMAL_BUFFER_HEADER_FLAG_FRAME_END)) {
            // write the whole buffer list to the http server.

            size_t total_size = 0;
            for(buffer_list * p = state->image_buffer_list; p; p = p->next) {
                total_size += p->length;
            }

            if (total_size > 0) {
                uint8_t * buf = (uint8_t*)malloc(total_size);

                size_t cur = 0;
                for(buffer_list * p = state->image_buffer_list; p; p = p->next) {
                    memcpy(buf + cur, p->data, p->length);
                    cur += p->length;
                }

                http_server_frame_jpeg(&state->http_server, buf, total_size);

                // free all the memory
                for(buffer_list * p = state->image_buffer_list; p;) {
                    buffer_list * t = p->next;

                    free(p->data);
                    p->data = NULL;
                    p->length = 0;
                    p->next = NULL;
                    free(p);

                    p = t;
                }
            }
        }

        if (buffer->flags & (MMAL_BUFFER_HEADER_FLAG_FRAME_END | MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED))
            fprintf(stderr, "frame ended or something: %x\n", buffer->flags);
    }

    mmal_buffer_header_release(buffer);

    if (port->is_enabled) {
        MMAL_STATUS_T status;
        MMAL_BUFFER_HEADER_T * new_buffer = mmal_queue_get(state->image_encoder_pool->queue);

        if (new_buffer != NULL) {
            status = mmal_port_send_buffer(port, new_buffer);
        }

        if (new_buffer == NULL || status != MMAL_SUCCESS) {
            fprintf(stderr, "could not return the buffer to the encoder pool");
        }
    }
}


static void encoder_buffer_callback(MMAL_PORT_T * port, MMAL_BUFFER_HEADER_T * buffer) {
    MMAL_BUFFER_HEADER_T * new_buffer;
    size_t bytes_written = 0;

    // fprintf(stderr, "buffer callback\n");

    state_t * state = (state_t*)port->userdata;

    if (buffer->length > 0) {
        mmal_buffer_header_mem_lock(buffer);

        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) {
            // fprintf(stderr, "got motion vectors\n");
            // motion vectors
            // do nothing for now
            server_write(&state->motion_server, buffer->data, buffer->length);
            http_server_motion(&state->http_server, buffer->data, buffer->length);
            bytes_written = buffer->length;
        } else {
            // fprintf(stderr, "writing video data\n");
            // video data
            server_write(&state->video_server, buffer->data, buffer->length);
            bytes_written = buffer->length;
            // bytes_written = fwrite(buffer->data, 1, buffer->length, state->video_file);

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

#define DEFAULT_VIDEO_PORT 8888
#define DEFAULT_MOTION_PORT 8889
#define DEFAULT_HTTP_PORT 8080

int main(int ac, char ** av) {
    MMAL_STATUS_T status = MMAL_SUCCESS;
    state_t state;
    int exit_code = 0;

    initialize_state(&state);

    MMAL_PORT_T * camera_preview_port = NULL;
    MMAL_PORT_T * camera_video_port = NULL;
    MMAL_PORT_T * camera_still_port = NULL;
    MMAL_PORT_T * splitter_input_port = NULL;
    MMAL_PORT_T * splitter_output_port0 = NULL;
    MMAL_PORT_T * splitter_output_port1 = NULL;
    MMAL_PORT_T * encoder_input_port = NULL;
    MMAL_PORT_T * encoder_output_port = NULL;
    MMAL_PORT_T * image_encoder_input = NULL;
    MMAL_PORT_T * image_encoder_output = NULL;

    bcm_host_init();
    vcos_log_register("simplecam", VCOS_LOG_CATEGORY);

    if (server_create(&state.video_server, DEFAULT_VIDEO_PORT) != 0) {
        fprintf(stderr, "could not create server\n");
        vcos_log_error("could not create server");
        goto cleanup;
    }

    if (server_create(&state.motion_server, DEFAULT_MOTION_PORT) != 0) {
        fprintf(stderr, "could not create motion vector server\n");
        goto cleanup;
    }

    if (http_server_create(&state.http_server, DEFAULT_HTTP_PORT) != 0) {
        fprintf(stderr, "could not create http server\n");
        goto cleanup;
    }

    char config[4096];
    int config_length = snprintf(config, sizeof(config),
            "video: \":%d\"\n"
            "motion: \":%d\"\n"
            "api: \":%d\"\n",
            DEFAULT_VIDEO_PORT,
            DEFAULT_MOTION_PORT,
            DEFAULT_HTTP_PORT);

    http_server_config(&state.http_server, (uint8_t*)config, config_length);

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

    if ((status = mmal_component_create(MMAL_COMPONENT_DEFAULT_SPLITTER, &state.splitter)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not create splitter component %s\n", mmal_status_to_string(status));
        goto cleanup;
    }

    if ((status = create_image_encoder_component(&state)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not create image encoder component\n");
        goto cleanup;
    }



    camera_preview_port = state.camera->output[MMAL_CAMERA_PREVIEW_PORT];
    camera_video_port = state.camera->output[MMAL_CAMERA_VIDEO_PORT];
    camera_still_port = state.camera->output[MMAL_CAMERA_CAPTURE_PORT];
    splitter_input_port = state.splitter->input[0];
    splitter_output_port0 = state.splitter->output[0];
    splitter_output_port1 = state.splitter->output[1];
    encoder_input_port = state.encoder->input[0];
    encoder_output_port = state.encoder->output[0];
    image_encoder_input = state.image_encoder->input[0];
    image_encoder_output = state.image_encoder->output[0];


    if ((status = mmal_connection_create(&state.splitter_connection, camera_preview_port, splitter_input_port,
        MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT)) != MMAL_SUCCESS)
    {
        fprintf(stderr, "error creating camera to splitter component\n");
        goto cleanup;
    }

    if ((status = mmal_connection_enable(state.splitter_connection)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not enable connection\n");
        goto cleanup;
    }

    if ((status = mmal_connection_create(&state.encoder_connection, camera_video_port, encoder_input_port,  
        MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT)) != MMAL_SUCCESS) 
    {
        fprintf(stderr, "error creating splitter to encoder connections\n");
        goto cleanup;
    }

    if ((status = mmal_connection_enable(state.encoder_connection)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not enable connection\n");
        goto cleanup;
    }


    if ((status = mmal_connection_create(&state.image_encoder_connection, splitter_output_port1, image_encoder_input,
        MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT)) != MMAL_SUCCESS) 
    {
        fprintf(stderr, "could not create splitter to image_encoder connection\n");
        goto cleanup;
    }

    if ((status = mmal_connection_enable(state.image_encoder_connection)) != MMAL_SUCCESS) {
        fprintf(stderr, "could not enable image_encoder connection\n");
        goto cleanup;
    }

    encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T*)&state;
    image_encoder_output->userdata = (struct MMAL_PORT_USERDATA_T*)&state;

    if ((status = mmal_port_enable(encoder_output_port, encoder_buffer_callback)) != MMAL_SUCCESS) {
        fprintf(stderr, "error enabling encoder output port\n");
        goto cleanup;
    }

    for(int num = mmal_queue_length(state.encoder_pool->queue); num > 0; num--) {
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

    if ((status = mmal_port_enable(image_encoder_output, image_buffer_callback)) != MMAL_SUCCESS) {
        fprintf(stderr, "error enabling image encoder output port\n");
        goto cleanup;
    }
     
    for(int num = mmal_queue_length(state.image_encoder_pool->queue); num > 0; num--) {
        MMAL_BUFFER_HEADER_T * buffer = mmal_queue_get(state.image_encoder_pool->queue);

        if (!buffer) {
            fprintf(stderr, "unable to get a required buffer %d from the pool\n", num);
            continue;
        }

        if(mmal_port_send_buffer(image_encoder_output, buffer) != MMAL_SUCCESS) {
            fprintf(stderr, "unable to send buffer to encoder output port\n");
            continue;
        }
        fprintf(stderr, "enabled buffer %d\n", num);
    }

    // start capturing
    if((status = mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, MMAL_TRUE)) != MMAL_SUCCESS) {
        fprintf(stderr, "unable to start capture: %s\n", mmal_status_to_string(status));
        goto cleanup;
    }


    vcos_semaphore_create(&interrupt, "simplecam_interrupt", 0);

    // wait until interrupted
    signal(SIGINT, handle_interrupt);
    vcos_semaphore_wait(&interrupt);
    signal(SIGINT, SIG_DFL);
    

cleanup:
    fprintf(stderr, "cleaning up...\n");

    mmal_status_to_int(status);

    server_close(&state.video_server);
    server_close(&state.motion_server);
    http_server_destroy(&state.http_server);

    if (state.encoder_connection != NULL) {
        if (state.encoder_connection->is_enabled) {
            mmal_connection_disable(state.encoder_connection);
        }
        mmal_connection_destroy(state.encoder_connection);
    }

    if (state.image_encoder_connection != NULL) {
        if (state.image_encoder_connection->is_enabled) {
            mmal_connection_disable(state.image_encoder_connection);
        }
        mmal_connection_destroy(state.image_encoder_connection);
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
    if (state.image_encoder_pool != NULL) {
        mmal_port_pool_destroy(state.image_encoder->output[0], state.image_encoder_pool);
    }
    if (state.image_encoder != NULL) {
        mmal_component_destroy(state.image_encoder);
    }

    // if (state.video_file != NULL) {
    //     fclose(state.video_file);
    // }

    return exit_code;
}
