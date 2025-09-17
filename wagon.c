#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
//#include <nvdsmeta.h>
#include "gstnvdsmeta.h"
#include "nvdsmeta.h"
#include "nvdsmeta_schema.h"
#include <nvdsinfer.h>
#include <stdarg.h>
#include <time.h>
#include <MQTTClient.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <uuid/uuid.h>

#define MAX_SOURCES 3
#define MUXER_OUTPUT_WIDTH 480
#define MUXER_OUTPUT_HEIGHT 480
#define MAX_DISPLAY_LEN 64
#define CONFIG_PGIE "aw_signboard_pgie_config_cement.txt"
#define MQTTCLIENT_SUCCESS 0
#define MQTTCLIENT_FAILURE -1
#define MQTTCLIENT_DISCONNECTED -3
#define MQTTCLIENT_MAX_MESSAGES_INFLIGHT -4
#define MQTTCLIENT_BAD_UTF8_STRING -5
#define MQTTCLIENT_NULL_PARAMETER -6
#define MQTTCLIENT_TOPICNAME_TRUNCATED -7
#define MQTTCLIENT_BAD_STRUCTURE -8
#define MQTTCLIENT_BAD_QOS -9
#define MQTTCLIENT_SSL_NOT_SUPPORTED -10
#define CONFIDENCE_THRESHOLD 0.2
#define MISSING_FRAMES_THRESHOLD 60
#define STABLE_SIGN_THRESHOLD 180
#define TRAIN_PASSING_TIME_THRESHOLD 3
#define CLASS_ID_VEHICLE 3
#define CLASS_ID_PERSON 1
#define CLASS_ID_ROAD_SIGN 0
#define CLASS_ID_TWO_WHEELER 2
#define LOG_ERROR   0
#define LOG_WARNING 1
#define LOG_INFO    2
#define LOG_DEBUG   3
#define MQTT_BROKER_DEFAULT "localhost"
#define MQTT_PORT_DEFAULT 1883
#define MQTT_CLIENT_ID "DeepStreamTrainCrossing"
#define MQTT_TOPIC_DEFAULT "NR/WDD/META"
#define MQTT_TRAIN_TOPIC "NR/WDD/META" // New topic for train detection
#define MQTT_LIVEAPI_TOPIC "NR/WDD/LIVEAPI" // New topic for LiveAPI subscription
#define MQTT_QOS 1
#define MQTT_CONNECTION_RETRIES 3
#define MQTT_RETRY_INTERVAL 5
#define INFERENCE_STREAM_ID 0

static int current_log_level = LOG_DEBUG;
static char *train_uuid = NULL;
static GstElement *pipeline, *streammux, *pgie, *nvvidconv, *nvosd;
static GstElement *source_bins[MAX_SOURCES];
static GstElement *tee_elements[MAX_SOURCES];
static GstElement *recording_bins[MAX_SOURCES];
//static GstElement *jpeg_bins[MAX_SOURCES]; // New array for JPEG bins
static GMainLoop *loop;
static guint num_sources = 0;
static guint frame_count = 0;
static gint64 fps_timer = 0;
static guint fps_count = 0;
static guint timer_id, bus_watch_id;
static GHashTable *stream_status;
static GHashTable *last_frame_time;
static GHashTable *car_missing_frames;
static GHashTable *stable_sign_frames;
static GHashTable *is_recording;
static GHashTable *recording_start_time;
static GHashTable *tee_recording_pads;
static GHashTable *tee_jpeg_pads; // New hash table for JPEG tee pads
static GHashTable *recording_filenames;
static GMutex mqtt_mutex;
static MQTTClient mqtt_client = NULL;
static char *mqtt_broker;
static int mqtt_port;
static char *mqtt_topic;
static char *mqtt_train_topic; // New variable for train detection topic
static char *mqtt_liveapi_topic; // New variable for LiveAPI topic
static gboolean liveapi_recording_enabled = FALSE; // Flag for LiveAPI recording control
static GMutex liveapi_mutex; // Mutex for LiveAPI flag protection
static GHashTable *jpeg_probe_ids = NULL;

// LiveAPI data fields
static char *current_vehicle_id = NULL;
static char *current_scheduled_time = NULL;
static char *current_rake_no = NULL;
static char *current_vehicle_name = NULL;
static char *current_train_number = NULL; // challan_no
static GMutex liveapi_data_mutex;

static void log_message(int level, const char *format, ...);
static GstElement *create_source_bin(guint i, const gchar *uri);
static GstElement *create_recording_bin(guint stream_id);
//static GstElement *create_jpeg_bin(guint stream_id); // New function
static void pad_added_handler(GstElement *src, GstPad *new_pad, gpointer data);
static gchar **read_sources(const gchar *filename, guint *num_sources);
static gboolean print_stats(gpointer data);
static GstPadProbeReturn object_detection_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u_data);
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data);
static void start_recording(guint stream_id);
static void stop_recording(guint stream_id);
static gboolean check_recording_timeout(gpointer data);
static void mqtt_connect(void);
static void mqtt_publish(gchar **filenames, guint num_files, time_t timestamp);
static void mqtt_cleanup(void);
static int mqtt_message_arrived(void *context, char *topicName, int topicLen, MQTTClient_message *message);
static gboolean is_liveapi_recording_enabled(void);
static void clear_liveapi_recording_flag(void);

static void log_message(int level, const char *format, ...) {
    if (level > current_log_level) return;
    const char *prefix[] = {"ERROR", "WARNING", "INFO", "DEBUG"};
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(stderr, "[%s] %s: ", time_str, prefix[level]);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (strlen(format) == 0 || format[strlen(format)-1] != '\n') fprintf(stderr, "\n");
}

static int mqtt_message_arrived(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
    g_mutex_lock(&liveapi_mutex);

    log_message(LOG_INFO, "MQTT message received on topic: %s", topicName);

    if (strcmp(topicName, mqtt_liveapi_topic) == 0) {
        char *payload = (char*)malloc(message->payloadlen + 1);
        memcpy(payload, message->payload, message->payloadlen);
        payload[message->payloadlen] = '\0';

        log_message(LOG_DEBUG, "LiveAPI payload received: %s", payload);

        // Parse JSON data and store LiveAPI fields
        g_mutex_lock(&liveapi_data_mutex);

        // Simple JSON parsing - you might want to use a proper JSON library like json-c
        char *vehicle_id_start = strstr(payload, "\"vehicleId\":");
        if (vehicle_id_start) {
            vehicle_id_start = strchr(vehicle_id_start, ':') + 1;
            while (*vehicle_id_start == ' ' || *vehicle_id_start == '"') vehicle_id_start++;
            char *vehicle_id_end = strchr(vehicle_id_start, '"');
            if (vehicle_id_end) {
                if (current_vehicle_id) free(current_vehicle_id);
                current_vehicle_id = strndup(vehicle_id_start, vehicle_id_end - vehicle_id_start);
            }
        }

        char *scheduled_time_start = strstr(payload, "\"gps_record_time\":");
        if (scheduled_time_start) {
            scheduled_time_start = strchr(scheduled_time_start, ':') + 1;
            while (*scheduled_time_start == ' ' || *scheduled_time_start == '"') scheduled_time_start++;
            char *scheduled_time_end = strchr(scheduled_time_start, '"');
            if (scheduled_time_end) {
                if (current_scheduled_time) free(current_scheduled_time);
                current_scheduled_time = strndup(scheduled_time_start, scheduled_time_end - scheduled_time_start);
            }
        }

        char *rake_no_start = strstr(payload, "\"rake_no\":");
        if (rake_no_start) {
            rake_no_start = strchr(rake_no_start, ':') + 1;
            while (*rake_no_start == ' ' || *rake_no_start == '"') rake_no_start++;
            char *rake_no_end = strchr(rake_no_start, '"');
            if (rake_no_end) {
                if (current_rake_no) free(current_rake_no);
                current_rake_no = strndup(rake_no_start, rake_no_end - rake_no_start);
            }
        }

        char *vehicle_name_start = strstr(payload, "\"vehicle_name\":");
        if (vehicle_name_start) {
            vehicle_name_start = strchr(vehicle_name_start, ':') + 1;
            while (*vehicle_name_start == ' ' || *vehicle_name_start == '"') vehicle_name_start++;
            char *vehicle_name_end = strchr(vehicle_name_start, '"');
            if (vehicle_name_end) {
                if (current_vehicle_name) free(current_vehicle_name);
                current_vehicle_name = strndup(vehicle_name_start, vehicle_name_end - vehicle_name_start);
            }
        }

        char *challan_no_start = strstr(payload, "\"challan_no\":");
        if (challan_no_start) {
            challan_no_start = strchr(challan_no_start, ':') + 1;
            while (*challan_no_start == ' ' || *challan_no_start == '"') challan_no_start++;
            char *challan_no_end = strchr(challan_no_start, '"');
            if (challan_no_end) {
                if (current_train_number) free(current_train_number);
                current_train_number = strndup(challan_no_start, challan_no_end - challan_no_start);
            }
        }

        g_mutex_unlock(&liveapi_data_mutex);

        // Simple check for valid LiveAPI data - customize based on your message format
        if (strstr(payload, "\"enable\"") || strstr(payload, "\"command\"") ||
            strstr(payload, "\"start\"") || message->payloadlen > 10) {

            if (!liveapi_recording_enabled) {
                liveapi_recording_enabled = TRUE;
                log_message(LOG_INFO, "LiveAPI recording enabled - fresh data received");
            } else {
                log_message(LOG_DEBUG, "LiveAPI recording already enabled, data refreshed");
            }
        } else {
            log_message(LOG_WARNING, "Invalid or empty LiveAPI message received");
        }

        free(payload);
    }

    g_mutex_unlock(&liveapi_mutex);
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

static gboolean is_liveapi_recording_enabled(void) {
    g_mutex_lock(&liveapi_mutex);
    gboolean enabled = liveapi_recording_enabled;
    g_mutex_unlock(&liveapi_mutex);

    if (enabled) {
        log_message(LOG_DEBUG, "LiveAPI recording is enabled, proceeding with recording");
    } else {
        log_message(LOG_INFO, "LiveAPI recording is disabled, skipping recording");
    }

    return enabled;
}

static void clear_liveapi_recording_flag(void) {
    g_mutex_lock(&liveapi_mutex);
    if (liveapi_recording_enabled) {
        liveapi_recording_enabled = FALSE;
        log_message(LOG_INFO, "LiveAPI recording flag cleared after successful video save");
    }
    g_mutex_unlock(&liveapi_mutex);
}

static void mqtt_connect(void) {
    int rc;
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = 60;
    conn_opts.cleansession = 1;

    log_message(LOG_INFO, "Connecting to MQTT broker at %s:%d", mqtt_broker, mqtt_port);

    char broker_url[256];
    snprintf(broker_url, sizeof(broker_url), "tcp://%s:%d", mqtt_broker, mqtt_port);
    rc = MQTTClient_create(&mqtt_client, broker_url, MQTT_CLIENT_ID,
                           MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        log_message(LOG_ERROR, "Failed to create MQTT client: %d", rc);
        mqtt_client = NULL;
        return;
    }

    // Set message callback
    MQTTClient_setCallbacks(mqtt_client, NULL, NULL, mqtt_message_arrived, NULL);

    for (int attempt = 1; attempt <= MQTT_CONNECTION_RETRIES; attempt++) {
        rc = MQTTClient_connect(mqtt_client, &conn_opts);
        if (rc == MQTTCLIENT_SUCCESS) {
            log_message(LOG_INFO, "Connected to MQTT broker at %s:%d", mqtt_broker, mqtt_port);

            // Subscribe to LiveAPI topic
            rc = MQTTClient_subscribe(mqtt_client, mqtt_liveapi_topic, MQTT_QOS);
            if (rc != MQTTCLIENT_SUCCESS) {
                log_message(LOG_ERROR, "Failed to subscribe to LiveAPI topic %s: %d",
                           mqtt_liveapi_topic, rc);
            } else {
                log_message(LOG_INFO, "Subscribed to LiveAPI topic: %s", mqtt_liveapi_topic);
            }
            return;
        }
        log_message(LOG_WARNING, "MQTT connection attempt %d/%d failed, error code: %d",
                    attempt, MQTT_CONNECTION_RETRIES, rc);
        if (attempt < MQTT_CONNECTION_RETRIES) {
            sleep(MQTT_RETRY_INTERVAL);
        }
    }

    log_message(LOG_ERROR, "Failed to connect to MQTT broker at %s:%d after %d attempts, error code: %d",
                mqtt_broker, mqtt_port, MQTT_CONNECTION_RETRIES, rc);
    MQTTClient_destroy(&mqtt_client);
    mqtt_client = NULL;
}

static void mqtt_publish(gchar **filenames, guint num_files, time_t timestamp) {
    g_mutex_lock(&mqtt_mutex);

    const int MAX_PUBLISH_ATTEMPTS = 3;
    const int PUBLISH_RETRY_DELAY = 2;

    if (!mqtt_client) {
        log_message(LOG_WARNING, "MQTT client not connected, attempting to reconnect to topic %s", mqtt_topic);
        mqtt_connect();
        if (!mqtt_client) {
            log_message(LOG_ERROR, "Reconnection failed, cannot publish to topic %s", mqtt_topic);
            g_mutex_unlock(&mqtt_mutex);
            return;
        }
    }

    log_message(LOG_INFO, "Preparing to publish message to topic %s", mqtt_topic);

    char payload[4096]; // Increased size for additional fields
    char time_str[32];
    struct tm *tm_info = localtime(&timestamp);
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", tm_info);

    // Get LiveAPI data with mutex protection
    g_mutex_lock(&liveapi_data_mutex);
    char *vehicle_id = current_vehicle_id ? strdup(current_vehicle_id) : strdup("unknown");
    char *scheduled_time = current_scheduled_time ? strdup(current_scheduled_time) : strdup("unknown");
    char *rake_no = current_rake_no ? strdup(current_rake_no) : strdup("unknown");
    char *vehicle_name = current_vehicle_name ? strdup(current_vehicle_name) : strdup("unknown");
    char *train_number = current_train_number ? strdup(current_train_number) : strdup("unknown");
    g_mutex_unlock(&liveapi_data_mutex);

snprintf(payload, sizeof(payload),
         "{\"device\": \"train_crossing_detector\", "
         "\"status\": \"recording_stopped\", "
         "\"train_uuid\": \"%s\", "
         "\"timestamp\": \"%s\", "
         "\"output_path\": \"/opt/wdd-infer-debris/output_res\", "
         "\"vehicle_id\": \"%s\", "
         "\"scheduled_time\": \"%s\", "
         "\"rake_no\": \"%s\", "
         "\"vehicle_name\": \"%s\", "
         "\"train_number\": \"%s\", ",
         train_uuid, time_str, vehicle_id, scheduled_time, rake_no, vehicle_name, train_number);

    // Clean up temporary strings
    free(vehicle_id);
    free(scheduled_time);
    free(rake_no);
    free(vehicle_name);
    free(train_number);

// Define stream names corresponding to each index
const char* stream_names[] = {"left", "top", "right"};
const int max_streams = sizeof(stream_names) / sizeof(stream_names[0]);

for (guint i = 0; i < num_files && i < max_streams; i++) {
    char stream_field[640];
    if (filenames[i] && *filenames[i]) {
        snprintf(stream_field, sizeof(stream_field),
                 "\"%s\": {\"stream_id\": %u, \"path\": \"%s\"}",
                 stream_names[i], i, filenames[i]);
    } else {
        log_message(LOG_WARNING, "Missing filename for stream %u (%s)", i, stream_names[i]);
        snprintf(stream_field, sizeof(stream_field),
                 "\"%s\": {\"stream_id\": %u, \"path\": \"unknown\"}",
                 stream_names[i], i);
    }

    strcat(payload, stream_field);

    // Add comma if not the last element
    if (i < num_files - 1 && i < max_streams - 1) {
        strcat(payload, ", ");
    }
}

// Close the JSON object
strcat(payload, "}");

log_message(LOG_DEBUG, "Constructed MQTT payload: %s", payload);
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = payload;
    pubmsg.payloadlen = strlen(payload);
    pubmsg.qos = MQTT_QOS;
    pubmsg.retained = 0;
    int attempt;
    int rc = MQTTCLIENT_FAILURE;

    for (attempt = 1; attempt <= MAX_PUBLISH_ATTEMPTS; attempt++) {
        if (!MQTTClient_isConnected(mqtt_client)) {
            log_message(LOG_WARNING, "MQTT client disconnected, attempting to reconnect (attempt %d/%d)",
                       attempt, MAX_PUBLISH_ATTEMPTS);
            mqtt_connect();
            if (!mqtt_client || !MQTTClient_isConnected(mqtt_client)) {
                log_message(LOG_ERROR, "Reconnection attempt %d failed", attempt);
                sleep(PUBLISH_RETRY_DELAY);
                continue;
            }
        }

        MQTTClient_deliveryToken token;
        rc = MQTTClient_publishMessage(mqtt_client, mqtt_topic, &pubmsg, &token);

        if (rc == MQTTCLIENT_SUCCESS) {
            log_message(LOG_INFO, "Published MQTT message to topic %s (attempt %d)", mqtt_topic, attempt);
            rc = MQTTClient_waitForCompletion(mqtt_client, token, 10000);

            if (rc == MQTTCLIENT_SUCCESS) {
                log_message(LOG_DEBUG, "MQTT message delivery confirmed for topic %s", mqtt_topic);
                break;
            } else {
                log_message(LOG_WARNING, "MQTT message delivery to topic %s not confirmed, error code: %d",
                           mqtt_topic, rc);
            }
        } else {
            log_message(LOG_ERROR, "Failed to publish MQTT message to topic %s, error code: %d",
                       mqtt_topic, rc);
        }

        if (attempt < MAX_PUBLISH_ATTEMPTS) {
            log_message(LOG_INFO, "Retrying publish in %d seconds...", PUBLISH_RETRY_DELAY);
            sleep(PUBLISH_RETRY_DELAY);
        }
    }

    if (rc != MQTTCLIENT_SUCCESS) {
        log_message(LOG_ERROR, "Failed to publish MQTT message after %d attempts", MAX_PUBLISH_ATTEMPTS);
    }

    g_mutex_unlock(&mqtt_mutex);
}

static void handle_stop_recording_and_notify() {
    gchar *filenames[MAX_SOURCES];
    time_t stop_time = time(NULL);

    for (guint i = 0; i < num_sources; i++) {
        stop_recording(i);
        log_message(LOG_DEBUG, "Stopped recording for stream %u", i);
        filenames[i] = (gchar *)g_hash_table_lookup(recording_filenames, GUINT_TO_POINTER(i));
        if (!filenames[i]) {
            log_message(LOG_WARNING, "No filename found for stream %u after stopping recording", i);
            filenames[i] = g_strdup("missing_filename");
        }
    }

    mqtt_publish(filenames, num_sources, stop_time);

    // Clear LiveAPI recording flag after successful save
    clear_liveapi_recording_flag();

    for (guint i = 0; i < num_sources; i++) {
        if (filenames[i] && strcmp(filenames[i], "missing_filename") == 0) {
            g_free(filenames[i]);
        }
        g_hash_table_remove(recording_filenames, GUINT_TO_POINTER(i));
    }

    if (train_uuid) {
        free(train_uuid);
    }
    uuid_t uuid;
    uuid_generate_random(uuid);
    train_uuid = (char *)malloc(37);
    uuid_unparse_lower(uuid, train_uuid);
    log_message(LOG_INFO, "Generated new train UUID for next detection: %s", train_uuid);

    for (guint i = 0; i < num_sources; i++) {
        g_hash_table_insert(car_missing_frames, GUINT_TO_POINTER(i), GUINT_TO_POINTER(0));
        g_hash_table_insert(stable_sign_frames, GUINT_TO_POINTER(i), GUINT_TO_POINTER(0));
        g_hash_table_insert(is_recording, GUINT_TO_POINTER(i), GINT_TO_POINTER(FALSE));
        g_hash_table_insert(recording_start_time, GUINT_TO_POINTER(i), GUINT_TO_POINTER(0));
    }
}

static void mqtt_cleanup(void) {
    if (mqtt_client) {
        if (MQTTClient_isConnected(mqtt_client)) {
            MQTTClient_unsubscribe(mqtt_client, mqtt_liveapi_topic);
            log_message(LOG_INFO, "Unsubscribed from LiveAPI topic: %s", mqtt_liveapi_topic);
        }
        MQTTClient_disconnect(mqtt_client, 1000);
        MQTTClient_destroy(&mqtt_client);
        mqtt_client = NULL;
        log_message(LOG_INFO, "Disconnected from MQTT broker");
    }

    // Clean up LiveAPI data
    g_mutex_lock(&liveapi_data_mutex);
    if (current_vehicle_id) { free(current_vehicle_id); current_vehicle_id = NULL; }
    if (current_scheduled_time) { free(current_scheduled_time); current_scheduled_time = NULL; }
    if (current_rake_no) { free(current_rake_no); current_rake_no = NULL; }
    if (current_vehicle_name) { free(current_vehicle_name); current_vehicle_name = NULL; }
    if (current_train_number) { free(current_train_number); current_train_number = NULL; }
    g_mutex_unlock(&liveapi_data_mutex);

    free(mqtt_broker);
    free(mqtt_topic);
    free(mqtt_liveapi_topic);

    g_mutex_clear(&liveapi_mutex);
    g_mutex_clear(&liveapi_data_mutex);
}

static GstElement *create_source_bin(guint i, const gchar *uri) {
    gchar *bin_name = g_strdup_printf("source-bin-%u", i);
    GstElement *bin = gst_bin_new(bin_name);
    GstElement *uridecodebin = gst_element_factory_make("uridecodebin", NULL);
    GstElement *nvvidconv = gst_element_factory_make("nvvideoconvert", NULL);
    GstElement *capsfilter = gst_element_factory_make("capsfilter", NULL);

    if (!bin || !uridecodebin || !nvvidconv || !capsfilter) {
        log_message(LOG_ERROR, "Failed to create source bin elements for source %u", i);
        if (bin) gst_object_unref(bin);
        g_free(bin_name);
        return NULL;
    }

    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM)");
    g_object_set(capsfilter, "caps", caps, NULL);
    g_object_set(uridecodebin, "uri", uri, "connection-speed", 0, NULL);

    gst_bin_add_many(GST_BIN(bin), uridecodebin, nvvidconv, capsfilter, NULL);

    if (!gst_element_link(nvvidconv, capsfilter)) {
        log_message(LOG_ERROR, "Failed to link elements in source bin %u", i);
        gst_object_unref(bin);
        g_free(bin_name);
        gst_caps_unref(caps);
        return NULL;
    }

    GstPad *srcpad = gst_element_get_static_pad(capsfilter, "src");
    GstPad *ghostpad = gst_ghost_pad_new("src", srcpad);
    gst_element_add_pad(bin, ghostpad);

    g_signal_connect(uridecodebin, "pad-added", G_CALLBACK(pad_added_handler),
                     gst_element_get_static_pad(nvvidconv, "sink"));

    log_message(LOG_INFO, "Created source bin %u with URI: %s", i, uri);
    g_free(bin_name);
    gst_caps_unref(caps);
    gst_object_unref(srcpad);
    return bin;
}

static GstElement *create_recording_bin(guint stream_id) {
    gchar *bin_name = g_strdup_printf("recording-bin-%u", stream_id);
    GstElement *bin = gst_bin_new(bin_name);

    GstElement *queue = gst_element_factory_make("queue", NULL);
    GstElement *nvvidconv = gst_element_factory_make("nvvideoconvert", NULL);
    GstElement *caps_filter = gst_element_factory_make("capsfilter", NULL);
    GstElement *encoder = gst_element_factory_make("nvv4l2h264enc", NULL);
    GstElement *h264parse = gst_element_factory_make("h264parse", NULL);
    GstElement *mux = gst_element_factory_make("mp4mux", NULL);
    GstElement *filesink = gst_element_factory_make("filesink", NULL);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char year_str[5];
    char month_str[3];
    char day_str[3];
    char datetime_str[20];
    strftime(year_str, sizeof(year_str), "%Y", tm_info);
    strftime(month_str, sizeof(month_str), "%m", tm_info);
    strftime(day_str, sizeof(day_str), "%d", tm_info);
    strftime(datetime_str, sizeof(datetime_str), "%Y-%m-%d_%H-%M-%S", tm_info);

    gchar *dir_name = g_strdup_printf("videos/%s/%s/%s/train_%s", year_str, month_str, day_str, train_uuid);
    if (g_mkdir_with_parents(dir_name, 0755) != 0) {
        log_message(LOG_ERROR, "Failed to create directory: %s", dir_name);
        g_free(dir_name);
        g_free(bin_name);
        return NULL;
    }


    gchar *filename = g_strdup_printf("%s/train_%s_stream%u_%s.mp4",
                                     dir_name, train_uuid, stream_id, datetime_str);

    gchar *abs_path = NULL;
    if (g_path_is_absolute(filename)) {
        abs_path = g_strdup(filename);
    } else {
        gchar *cwd = g_get_current_dir();
        abs_path = g_build_filename(cwd, filename, NULL);
        g_free(cwd);
    }

    g_object_set(filesink, "location", filename, "sync", TRUE, NULL);
    log_message(LOG_INFO, "Creating recording file: %s", filename);
    g_free(filename);

    g_hash_table_insert(recording_filenames, GUINT_TO_POINTER(stream_id), abs_path);

    if (!bin || !queue || !nvvidconv || !caps_filter || !encoder ||
        !h264parse || !mux || !filesink) {
        log_message(LOG_ERROR, "Failed to create recording elements for stream %u", stream_id);
        if (bin) gst_object_unref(bin);
        g_free(bin_name);
        g_free(dir_name);
        return NULL;
    }

    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=NV12");
    g_object_set(caps_filter, "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(encoder,
                "bitrate", 2000000,
                "preset-level", 1,
                "profile", 0,
                "iframeinterval", 30,
                NULL);

    g_object_set(mux, "faststart", TRUE, "streamable", TRUE, NULL);

    gst_bin_add_many(GST_BIN(bin), queue, nvvidconv, caps_filter,
                   encoder, h264parse, mux, filesink, NULL);

    if (!gst_element_link_many(queue, nvvidconv, caps_filter, encoder,
                             h264parse, mux, filesink, NULL)) {
        log_message(LOG_ERROR, "Failed to link recording elements for stream %u", stream_id);
        gst_object_unref(bin);
        g_free(bin_name);
        g_free(dir_name);
        return NULL;
    }

    GstPad *sinkpad = gst_element_get_static_pad(queue, "sink");
    GstPad *ghostpad = gst_ghost_pad_new("sink", sinkpad);
    gst_element_add_pad(bin, ghostpad);
    gst_object_unref(sinkpad);

    log_message(LOG_INFO, "Successfully created recording bin for stream %u", stream_id);
    g_free(bin_name);
    g_free(dir_name);
    return bin;
}


static void start_recording(guint stream_id) {
    log_message(LOG_DEBUG, "Attempting to start recording for stream %u", stream_id);

    if (GPOINTER_TO_INT(g_hash_table_lookup(is_recording, GUINT_TO_POINTER(stream_id)))) {
        log_message(LOG_INFO, "Stream %u is already recording", stream_id);
        return;
    }

    // Check if LiveAPI has enabled recording
    if (!is_liveapi_recording_enabled()) {
        log_message(LOG_WARNING, "LiveAPI recording not enabled, not starting recording for stream %u", stream_id);
        return;
    }

    log_message(LOG_INFO, "Starting recording for stream %u (LiveAPI recording enabled)", stream_id);

    // Create and add recording bin
    if (!recording_bins[stream_id]) {
        recording_bins[stream_id] = create_recording_bin(stream_id);
        if (!recording_bins[stream_id]) {
            log_message(LOG_ERROR, "Failed to create recording bin for stream %u", stream_id);
            return;
        }

        if (!gst_bin_add(GST_BIN(pipeline), recording_bins[stream_id])) {
            log_message(LOG_ERROR, "Failed to add recording bin to pipeline for stream %u", stream_id);
            gst_object_unref(recording_bins[stream_id]);
            recording_bins[stream_id] = NULL;
            return;
        }
    }


    if (!tee_elements[stream_id]) {
        log_message(LOG_ERROR, "No tee element for stream %u", stream_id);
        return;
    }

    // Link recording bin
    GstPad *tee_rec_pad = NULL;
    if (!g_hash_table_lookup(tee_recording_pads, GUINT_TO_POINTER(stream_id))) {
        tee_rec_pad = gst_element_request_pad_simple(tee_elements[stream_id], "src_%u");
        if (!tee_rec_pad) {
            log_message(LOG_ERROR, "Failed to get request pad from tee for recording bin stream %u", stream_id);
            return;
        }
        g_hash_table_insert(tee_recording_pads, GUINT_TO_POINTER(stream_id), tee_rec_pad);
    } else {
        tee_rec_pad = GST_PAD(g_hash_table_lookup(tee_recording_pads, GUINT_TO_POINTER(stream_id)));
    }

    GstPad *rec_sinkpad = gst_element_get_static_pad(recording_bins[stream_id], "sink");
    if (!rec_sinkpad) {
        log_message(LOG_ERROR, "Failed to get sink pad from recording bin for stream %u", stream_id);
        return;
    }

    GstPadLinkReturn link_result = gst_pad_link(tee_rec_pad, rec_sinkpad);
    if (GST_PAD_LINK_FAILED(link_result)) {
        log_message(LOG_ERROR, "Failed to link tee to recording bin for stream %u (error: %d)",
                   stream_id, link_result);
        gst_object_unref(rec_sinkpad);
        return;
    }


    if (GST_PAD_LINK_FAILED(link_result)) {
        gst_object_unref(rec_sinkpad);
        return;
    }

    // Sync states BEFORE unreferencing pads
    if (!gst_element_sync_state_with_parent(recording_bins[stream_id])) {
        log_message(LOG_ERROR, "Failed to set recording bin state to PLAYING for stream %u", stream_id);
        gst_object_unref(rec_sinkpad);
        return;
    }


    // Now safe to unref pads
    gst_object_unref(rec_sinkpad);
    // gst_object_unref(jpeg_sinkpad);

    g_hash_table_insert(is_recording, GUINT_TO_POINTER(stream_id), GINT_TO_POINTER(TRUE));
    g_hash_table_insert(recording_start_time, GUINT_TO_POINTER(stream_id),
                      GUINT_TO_POINTER((guint)time(NULL)));

    log_message(LOG_INFO, "Recording and JPEG output started successfully for stream %u", stream_id);
}

static void stop_recording(guint stream_id) {
    log_message(LOG_DEBUG, "Attempting to stop recording for stream %u", stream_id);
    if (!GPOINTER_TO_INT(g_hash_table_lookup(is_recording, GUINT_TO_POINTER(stream_id)))) {
        log_message(LOG_INFO, "Stream %u is not recording, nothing to stop", stream_id);
        return;
    }

    log_message(LOG_INFO, "Stopping recording for stream %u", stream_id);

    gchar *filename = g_strdup((gchar *)g_hash_table_lookup(recording_filenames, GUINT_TO_POINTER(stream_id)));
    if (!filename) {
        log_message(LOG_WARNING, "No filename found for recording on stream %u", stream_id);
    } else {
        log_message(LOG_DEBUG, "Retrieved filename for stream %u: %s", stream_id, filename);
    }

    g_hash_table_insert(is_recording, GUINT_TO_POINTER(stream_id), GINT_TO_POINTER(FALSE));
    guint start_time = GPOINTER_TO_UINT(g_hash_table_lookup(recording_start_time, GUINT_TO_POINTER(stream_id)));
    guint duration = time(NULL) - start_time;
    log_message(LOG_INFO, "Recording stopped for stream %u after %u seconds", stream_id, duration);

    gboolean cleanup_success = TRUE;

    if (recording_bins[stream_id]) {
        log_message(LOG_DEBUG, "Processing recording bin for stream %u", stream_id);

        // Send EOS to flush buffers
        GstPad *rec_sinkpad = gst_element_get_static_pad(recording_bins[stream_id], "sink");
        if (rec_sinkpad) {
            log_message(LOG_DEBUG, "Sending EOS to recording bin %u", stream_id);
            if (!gst_pad_send_event(rec_sinkpad, gst_event_new_eos())) {
                log_message(LOG_WARNING, "Failed to send EOS to recording bin %u sink pad", stream_id);
                cleanup_success = FALSE;
            }
        } else {
            log_message(LOG_ERROR, "Failed to get sink pad for recording bin %u", stream_id);
            cleanup_success = FALSE;
        }

        // Wait for EOS or error
        GstBus *bus = gst_element_get_bus(recording_bins[stream_id]);
        if (!bus) {
            log_message(LOG_WARNING, "Couldn't get bus for recording bin %u", stream_id);
            cleanup_success = FALSE;
        } else {
            gst_bus_set_flushing(bus, FALSE);
            GTimer *timer = g_timer_new();
            gboolean eos_received = FALSE;
            while (!eos_received && g_timer_elapsed(timer, NULL) < 5.0) {
                // Fix: Cast bitwise OR to GstMessageType
                GstMessage *msg = gst_bus_poll(bus, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR), 100 * GST_MSECOND);
                if (msg) {
                    switch (GST_MESSAGE_TYPE(msg)) {
                        case GST_MESSAGE_EOS:
                            log_message(LOG_DEBUG, "EOS received for recording bin %u", stream_id);
                            eos_received = TRUE;
                            break;
                        case GST_MESSAGE_ERROR: {
                            GError *err;
                            gchar *debug;
                            gst_message_parse_error(msg, &err, &debug);
                            log_message(LOG_ERROR, "Error in recording bin %u: %s (debug: %s)",
                                       stream_id, err->message, debug ? debug : "none");
                            g_error_free(err);
                            g_free(debug);
                            cleanup_success = FALSE;
                            break;
                        }
                        default:
                            break;
                    }
                    gst_message_unref(msg);
                }
            }
            g_timer_destroy(timer);
            if (!eos_received) {
                log_message(LOG_WARNING, "Timeout (5s) waiting for EOS on recording bin %u", stream_id);
                cleanup_success = FALSE;
            }
            gst_object_unref(bus);
        }

        // Unlink pads
        if (rec_sinkpad) {
            GstPad *rec_srcpad = gst_pad_get_peer(rec_sinkpad);
            if (rec_srcpad) {
                log_message(LOG_DEBUG, "Unlinking recording bin %u pads", stream_id);
                if (!gst_pad_unlink(rec_srcpad, rec_sinkpad)) {
                    log_message(LOG_WARNING, "Failed to unlink recording bin %u pads", stream_id);
                    cleanup_success = FALSE;
                }
                gst_object_unref(rec_srcpad);
            } else {
                log_message(LOG_WARNING, "No peer pad found for recording bin %u sink pad", stream_id);
                cleanup_success = FALSE;
            }
            gst_object_unref(rec_sinkpad);
        }

        // Set bin to NULL state before removal
        log_message(LOG_DEBUG, "Setting recording bin %u to NULL state", stream_id);
        if (gst_element_set_state(recording_bins[stream_id], GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
            log_message(LOG_WARNING, "Failed to set recording bin %u to NULL state", stream_id);
            cleanup_success = FALSE;
        }

        // Remove bin from pipeline
        log_message(LOG_DEBUG, "Removing recording bin %u from pipeline", stream_id);
        if (!gst_bin_remove(GST_BIN(pipeline), recording_bins[stream_id])) {
            log_message(LOG_WARNING, "Failed to remove recording bin %u from pipeline", stream_id);
            cleanup_success = FALSE;
        }
        recording_bins[stream_id] = NULL;
    } else {
        log_message(LOG_WARNING, "No recording bin found for stream %u", stream_id);
        cleanup_success = FALSE;
    }

    // Release tee pad
    // Fix: Cast gpointer to GstPad*
    GstPad *tee_pad = (GstPad *)g_hash_table_lookup(tee_recording_pads, GUINT_TO_POINTER(stream_id));
    if (tee_pad) {
        log_message(LOG_DEBUG, "Releasing tee pad for stream %u", stream_id);
        gst_element_release_request_pad(tee_elements[stream_id], tee_pad);
        g_hash_table_remove(tee_recording_pads, GUINT_TO_POINTER(stream_id));
    }

    if (!cleanup_success) {
        log_message(LOG_ERROR, "Cleanup failed for stream %u", stream_id);
    } else {
        log_message(LOG_INFO, "Recording cleanup completed for stream %u", stream_id);
    }

    if (filename) {
        // Verify file exists and is non-empty
        struct stat st;
        if (stat(filename, &st) == 0 && st.st_size > 0) {
            log_message(LOG_INFO, "Recording file %s saved successfully, size: %ld bytes", filename, st.st_size);
        } else {
            log_message(LOG_ERROR, "Recording file %s is missing or empty", filename);
        }
        g_free(filename);
    }
}

static void pad_added_handler(GstElement *src, GstPad *new_pad, gpointer data) {
    GstPad *sink_pad = (GstPad *)data;
    if (!gst_pad_is_linked(sink_pad)) {
        GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
        if (ret == GST_PAD_LINK_OK) {
            log_message(LOG_DEBUG, "Linked pad from uridecodebin to nvvidconv");
        } else {
            log_message(LOG_ERROR, "Failed to link pad from uridecodebin to nvvidconv");
        }
    }
}

static gchar **read_sources(const gchar *filename, guint *num_sources) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        log_message(LOG_ERROR, "Failed to open sources file: %s", filename);
        return NULL;
    }

    gchar **sources = (gchar **)g_malloc0(MAX_SOURCES * sizeof(gchar *));
    gchar line[1024];
    *num_sources = 0;

    while (fgets(line, 1024, file) && *num_sources < MAX_SOURCES) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) && line[0] != '#' && line[0] != '[') {
            gchar **parts = g_strsplit(line, "=", 2);
            if (parts[1]) {
                sources[(*num_sources)++] = g_strdup(g_strstrip(parts[1]));
                log_message(LOG_DEBUG, "Added source %u: %s", *num_sources - 1, parts[1]);
            }
            g_strfreev(parts);
        }
    }

    fclose(file);

    if (*num_sources != MAX_SOURCES) {
        log_message(LOG_ERROR, "Exactly %d sources required, found %u", MAX_SOURCES, *num_sources);
        g_free(sources);
        return NULL;
    }

    log_message(LOG_INFO, "Loaded %u source(s) from %s", *num_sources, filename);
    return sources;
}

static GstPadProbeReturn object_detection_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u_data) {
  //  log_message(LOG_DEBUG, "objectdetectionProbe triggered on pgie src pad");
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    frame_count++;
    fps_count++;

//log_message(LOG_DEBUG, "Processing frame %u", frame_count);

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta) {
        log_message(LOG_DEBUG, "No batch metadata found in buffer");
        return GST_PAD_PROBE_OK;
    }

    gboolean should_start_recording = FALSE;
    gboolean should_stop_recording = FALSE;

    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame; l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)l_frame->data;
        guint stream_id = frame_meta->pad_index;

        if (stream_id != INFERENCE_STREAM_ID) {
            continue;
        }

        gboolean road_sign_detected = FALSE;
        float max_confidence = 0.0;
        guint road_sign_count = 0;

        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj; l_obj = l_obj->next) {
            NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)l_obj->data;

            if (obj_meta->class_id == CLASS_ID_ROAD_SIGN) {
                if (obj_meta->confidence > max_confidence) {
                    max_confidence = obj_meta->confidence;
                }
            }
        }

        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj; l_obj = l_obj->next) {
            NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)l_obj->data;

            if (obj_meta->confidence < CONFIDENCE_THRESHOLD || obj_meta->class_id != CLASS_ID_ROAD_SIGN) {
                continue;
            }

            road_sign_detected = TRUE;
            road_sign_count++;
        }

        guint missing_frames = GPOINTER_TO_UINT(g_hash_table_lookup(car_missing_frames,
                                                                  GUINT_TO_POINTER(stream_id)));
        guint stable_frames = GPOINTER_TO_UINT(g_hash_table_lookup(stable_sign_frames,
                                                                 GUINT_TO_POINTER(stream_id)));

        if (road_sign_detected) {
            stable_frames++;
            g_hash_table_insert(stable_sign_frames, GUINT_TO_POINTER(stream_id),
                              GUINT_TO_POINTER(stable_frames));

            if (stable_frames >= STABLE_SIGN_THRESHOLD) {
                g_hash_table_insert(car_missing_frames, GUINT_TO_POINTER(stream_id), GUINT_TO_POINTER(0));
                missing_frames = 0;
            }

            if (stable_frames >= STABLE_SIGN_THRESHOLD &&
                GPOINTER_TO_INT(g_hash_table_lookup(is_recording, GUINT_TO_POINTER(stream_id)))) {
                should_stop_recording = TRUE;
                stable_frames = 0;
                g_hash_table_insert(stable_sign_frames, GUINT_TO_POINTER(stream_id),
                                  GUINT_TO_POINTER(stable_frames));
            }
        } else {
            stable_frames = 0;
            g_hash_table_insert(stable_sign_frames, GUINT_TO_POINTER(stream_id),
                              GUINT_TO_POINTER(stable_frames));

            missing_frames++;
            g_hash_table_insert(car_missing_frames, GUINT_TO_POINTER(stream_id),
                              GUINT_TO_POINTER(missing_frames));

            if (missing_frames >= MISSING_FRAMES_THRESHOLD &&
                !GPOINTER_TO_INT(g_hash_table_lookup(is_recording, GUINT_TO_POINTER(stream_id)))) {
                should_start_recording = TRUE;
            }
        }
    }

    if (should_start_recording) {
          time_t detection_time = time(NULL);
        for (guint i = 0; i < num_sources; i++) {
            start_recording(i);
        }
    }

    if (should_stop_recording) {
        handle_stop_recording_and_notify();
    }

    return GST_PAD_PROBE_OK;
}

static gboolean check_recording_timeout(gpointer data) {
    for (guint i = 0; i < num_sources; i++) {
        if (GPOINTER_TO_INT(g_hash_table_lookup(is_recording, GUINT_TO_POINTER(i)))) {
            guint start_time = GPOINTER_TO_UINT(g_hash_table_lookup(recording_start_time,
                                                                  GUINT_TO_POINTER(i)));
            guint duration = time(NULL) - start_time;

            if (duration > 600) {
                log_message(LOG_WARNING, "Recording timeout for stream %u after %u seconds", i, duration);
                stop_recording(i);
            }
        }
    }
    return TRUE;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(msg, &err, &debug);
            log_message(LOG_ERROR, "Pipeline error: %s (debug: %s)", err->message, debug ? debug : "none");
            g_error_free(err);
            g_free(debug);

            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                g_main_loop_quit(loop);
            }
            break;
        }
        case GST_MESSAGE_EOS: {
            GstObject *src = GST_MESSAGE_SRC(msg);
            const gchar *src_name = GST_OBJECT_NAME(src);
            log_message(LOG_INFO, "End of stream received from %s", src_name);

            if (!g_str_has_prefix(src_name, "recording-bin")) {
                if (g_str_has_prefix(src_name, "source-bin")) {
                    guint stream_id;
                    if (sscanf(src_name, "source-bin-%u", &stream_id) == 1 && stream_id < num_sources) {
                        log_message(LOG_INFO, "Attempting to recover source %u", stream_id);
                        if (source_bins[stream_id]) {
                            gst_element_set_state(source_bins[stream_id], GST_STATE_NULL);
                            gst_element_set_state(source_bins[stream_id], GST_STATE_PLAYING);
                        }
                    }
                }
            }
            break;
        }
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                GstState old, new_, pending;
                gst_message_parse_state_changed(msg, &old, &new_, &pending);
                log_message(LOG_DEBUG, "Pipeline state changed: %s -> %s",
                          gst_element_state_get_name(old), gst_element_state_get_name(new_));
            }
            break;
        default:
            break;
    }
    return TRUE;
}

static gboolean print_stats(gpointer data) {
    gint64 current_time = g_get_monotonic_time();
    if (fps_timer != 0) {
        double elapsed = (current_time - fps_timer) / (double)G_USEC_PER_SEC;
        double fps = fps_count / elapsed;

        for (guint i = 0; i < num_sources; i++) {
            if (GPOINTER_TO_INT(g_hash_table_lookup(is_recording, GUINT_TO_POINTER(i)))) {
                guint start_time = GPOINTER_TO_UINT(g_hash_table_lookup(recording_start_time,
                                                                      GUINT_TO_POINTER(i)));
                guint duration = time(NULL) - start_time;
                log_message(LOG_INFO, "Stream %u: Recording active for %u seconds", i, duration);
            } else {
                guint missing_frames = GPOINTER_TO_UINT(g_hash_table_lookup(car_missing_frames,
                                                                          GUINT_TO_POINTER(i)));
                guint stable_frames = GPOINTER_TO_UINT(g_hash_table_lookup(stable_sign_frames,
                                                                         GUINT_TO_POINTER(i)));
                log_message(LOG_INFO, "Stream %u: No recording, Missing Frames: %u/%u, Stable Frames: %u/%u",
                          i, missing_frames, MISSING_FRAMES_THRESHOLD, stable_frames, STABLE_SIGN_THRESHOLD);
            }
        }
    }
    fps_timer = current_time;
    fps_count = 0;
    return TRUE;
}

int main(int argc, char *argv[]) {
    g_mutex_init(&mqtt_mutex);
    g_mutex_init(&liveapi_mutex);
    g_mutex_init(&liveapi_data_mutex);

    mqtt_broker = strdup(getenv("MQTT_BROKER") ? getenv("MQTT_BROKER") : MQTT_BROKER_DEFAULT);
    mqtt_port = getenv("MQTT_PORT") ? atoi(getenv("MQTT_PORT")) : MQTT_PORT_DEFAULT;
    mqtt_topic = strdup(getenv("MQTT_TOPIC") ? getenv("MQTT_TOPIC") : MQTT_TOPIC_DEFAULT);
    mqtt_train_topic = strdup(getenv("MQTT_TRAIN_TOPIC") ? getenv("MQTT_TRAIN_TOPIC") : MQTT_TRAIN_TOPIC);
    mqtt_liveapi_topic = strdup(getenv("MQTT_LIVEAPI_TOPIC") ? getenv("MQTT_LIVEAPI_TOPIC") : MQTT_LIVEAPI_TOPIC);

    log_message(LOG_INFO, "MQTT Configuration - Broker: %s, Port: %d, Topic: %s, LiveAPI Topic: %s",
                mqtt_broker, mqtt_port, mqtt_topic, mqtt_liveapi_topic);

    // Initialize LiveAPI recording flag as disabled
    liveapi_recording_enabled = FALSE;
    log_message(LOG_INFO, "LiveAPI recording initialized as disabled");

    uuid_t uuid;
    uuid_generate_random(uuid);
    train_uuid = (char *)malloc(37);
    uuid_unparse_lower(uuid, train_uuid);
    log_message(LOG_INFO, "Generated train UUID: %s", train_uuid);

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    stream_status = g_hash_table_new(g_direct_hash, g_direct_equal);
    last_frame_time = g_hash_table_new(g_direct_hash, g_direct_equal);
    car_missing_frames = g_hash_table_new(g_direct_hash, g_direct_equal);
    stable_sign_frames = g_hash_table_new(g_direct_hash, g_direct_equal);
    is_recording = g_hash_table_new(g_direct_hash, g_direct_equal);
    recording_start_time = g_hash_table_new(g_direct_hash, g_direct_equal);
    tee_recording_pads = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)gst_object_unref);
    // tee_jpeg_pads = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)gst_object_unref);
    recording_filenames = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    log_message(LOG_INFO, "DeepStream Train Crossing Detection application starting");

    mqtt_connect();

    const char *sources_file = "./sources.ini";
    gchar **sources = read_sources(sources_file, &num_sources);
    if (!sources || num_sources != 3) {
        log_message(LOG_ERROR, "Exactly 3 sources required, found %u, exiting", num_sources);
        mqtt_cleanup();
        free(train_uuid);
        g_strfreev(sources);
        return -1;
    }

    for (guint i = 0; i < num_sources; i++) {
        g_hash_table_insert(stream_status, GUINT_TO_POINTER(i), GINT_TO_POINTER(FALSE));
        g_hash_table_insert(last_frame_time, GUINT_TO_POINTER(i), GINT_TO_POINTER(0));
        g_hash_table_insert(car_missing_frames, GUINT_TO_POINTER(i), GUINT_TO_POINTER(0));
        g_hash_table_insert(stable_sign_frames, GUINT_TO_POINTER(i), GINT_TO_POINTER(0));
        g_hash_table_insert(is_recording, GUINT_TO_POINTER(i), GINT_TO_POINTER(FALSE));
        g_hash_table_insert(recording_start_time, GUINT_TO_POINTER(i), GINT_TO_POINTER(0));
        recording_bins[i] = NULL;
        // jpeg_bins[i] = NULL;
    }

    pipeline = gst_pipeline_new("train-crossing-detection-pipeline");
    streammux = gst_element_factory_make("nvstreammux", "streammux");
    pgie = gst_element_factory_make("nvinfer", "pgie");
    nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvidconv");
    nvosd = gst_element_factory_make("nvdsosd", "nvosd");
    GstElement *streamdemux = gst_element_factory_make("nvstreamdemux", "streamdemux");

    if (!pipeline || !streammux || !pgie || !nvvidconv || !nvosd || !streamdemux) {
        log_message(LOG_ERROR, "Failed to create one or more pipeline elements");
        mqtt_cleanup();
        free(train_uuid);
        g_strfreev(sources);
        return -1;
    }

    log_message(LOG_INFO, "Configuring pipeline elements");

    g_object_set(G_OBJECT(streammux),
                "batch-size", num_sources,
                "width", MUXER_OUTPUT_WIDTH,
                "height", MUXER_OUTPUT_HEIGHT,
                "batched-push-timeout", 40000,
                "live-source", FALSE,
                NULL);

    g_object_set(G_OBJECT(pgie),
                "config-file-path", CONFIG_PGIE,
                NULL);

    g_object_set(G_OBJECT(nvosd),
                "display-bbox", FALSE,
                "display-text", FALSE,
                NULL);

    gst_bin_add_many(GST_BIN(pipeline), streammux, pgie, nvvidconv, nvosd, streamdemux, NULL);

    for (guint i = 0; i < num_sources; i++) {
        source_bins[i] = create_source_bin(i, sources[i]);
        if (!source_bins[i]) {
            log_message(LOG_ERROR, "Failed to create source bin for source %d: %s", i, sources[i]);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        gst_bin_add(GST_BIN(pipeline), source_bins[i]);

        GstPad *srcpad = gst_element_get_static_pad(source_bins[i], "src");
        if (!srcpad) {
            log_message(LOG_ERROR, "Failed to get src pad from source bin %d", i);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        gchar *padname = g_strdup_printf("sink_%u", i);
        GstPad *sinkpad = gst_element_request_pad_simple(streammux, padname);
        if (!sinkpad) {
            log_message(LOG_ERROR, "Failed to get sink pad from streammux for source %d", i);
            gst_object_unref(srcpad);
            g_free(padname);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
            log_message(LOG_ERROR, "Failed to link source bin %d to streammux", i);
            gst_object_unref(srcpad);
            gst_object_unref(sinkpad);
            g_free(padname);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        log_message(LOG_INFO, "Linked source %d: %s", i, sources[i]);
        gst_object_unref(srcpad);
        gst_object_unref(sinkpad);
        g_free(padname);

        // Create tee and display branch
        gchar *tee_name = g_strdup_printf("tee_stream_%u", i);
        tee_elements[i] = gst_element_factory_make("tee", tee_name);
        g_free(tee_name);

        if (!tee_elements[i]) {
            log_message(LOG_ERROR, "Failed to create tee element for stream %u", i);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }
        log_message(LOG_INFO, "Tee element created for stream %u", i);

        // Display branch
        gchar *queue_disp_name = g_strdup_printf("queue_disp_%u", i);
        GstElement *queue_disp = gst_element_factory_make("queue", queue_disp_name);
        g_free(queue_disp_name);

        if (!queue_disp) {
            log_message(LOG_ERROR, "Failed to create display queue element for stream %u", i);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        gchar *sink_name = g_strdup_printf("fakesink_%u", i);
        GstElement *fakesink = gst_element_factory_make("fakesink", sink_name);
        g_free(sink_name);

        if (!fakesink) {
            log_message(LOG_ERROR, "Failed to create fakesink element for stream %u", i);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        g_object_set(G_OBJECT(fakesink), "sync", FALSE, "async", FALSE, NULL);

        gst_bin_add_many(GST_BIN(pipeline), tee_elements[i], queue_disp, fakesink, NULL);

        gchar *demux_pad_name = g_strdup_printf("src_%u", i);
        GstPad *demux_src_pad = gst_element_request_pad_simple(streamdemux, demux_pad_name);
        GstPad *tee_sink_pad = gst_element_get_static_pad(tee_elements[i], "sink");

        if (!demux_src_pad || !tee_sink_pad) {
            log_message(LOG_ERROR, "Failed to get pads for linking streamdemux to tee for stream %u", i);
            g_free(demux_pad_name);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        if (gst_pad_link(demux_src_pad, tee_sink_pad) != GST_PAD_LINK_OK) {
            log_message(LOG_ERROR, "Failed to link streamdemux to tee for stream %u", i);
            gst_object_unref(demux_src_pad);
            gst_object_unref(tee_sink_pad);
            g_free(demux_pad_name);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        gst_object_unref(demux_src_pad);
        gst_object_unref(tee_sink_pad);
        g_free(demux_pad_name);

        // Link display branch
        GstPad *teepad_disp = gst_element_request_pad_simple(tee_elements[i], "src_%u");
        GstPad *queue_disp_pad = gst_element_get_static_pad(queue_disp, "sink");

        if (gst_pad_link(teepad_disp, queue_disp_pad) != GST_PAD_LINK_OK) {
            log_message(LOG_ERROR, "Failed to link tee to display queue for stream %u", i);
            gst_object_unref(teepad_disp);
            gst_object_unref(queue_disp_pad);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }

        gst_object_unref(teepad_disp);
        gst_object_unref(queue_disp_pad);

        if (!gst_element_link(queue_disp, fakesink)) {
            log_message(LOG_ERROR, "Failed to link display queue to fakesink for stream %u", i);
            mqtt_cleanup();
            free(train_uuid);
            g_strfreev(sources);
            return -1;
        }
    }

    if (!gst_element_link_many(streammux, pgie, nvvidconv, nvosd, streamdemux, NULL)) {
        log_message(LOG_ERROR, "Failed to link main pipeline elements");
        mqtt_cleanup();
        free(train_uuid);
        g_strfreev(sources);
        return -1;
    }

    GstPad *pgie_src_pad = gst_element_get_static_pad(pgie, "src");
    if (!pgie_src_pad) {
        log_message(LOG_ERROR, "Failed to get src pad from pgie");
        mqtt_cleanup();
        free(train_uuid);
        g_strfreev(sources);
        return -1;
    }
    log_message(LOG_INFO, "Attaching probe to pgie src pad");

    gst_pad_add_probe(pgie_src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                     object_detection_probe, NULL, NULL);
    gst_object_unref(pgie_src_pad);

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
    gst_object_unref(bus);

    timer_id = g_timeout_add_seconds(5, print_stats, NULL);
    g_timeout_add_seconds(1, check_recording_timeout, NULL);

    log_message(LOG_INFO, "Starting pipeline with %u sources", num_sources);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        log_message(LOG_ERROR, "Failed to set pipeline to PLAYING state");
        mqtt_cleanup();
        free(train_uuid);
        g_strfreev(sources);
        return -1;
    }

    g_main_loop_run(loop);

    log_message(LOG_INFO, "Exiting...");

    for (guint i = 0; i < num_sources; i++) {
        if (g_hash_table_lookup(is_recording, GUINT_TO_POINTER(i)) &&
            GPOINTER_TO_INT(g_hash_table_lookup(is_recording, GUINT_TO_POINTER(i)))) {
            stop_recording(i);
        }
    }

    if (bus_watch_id > 0) {
        g_source_remove(bus_watch_id);
        bus_watch_id = 0;
    }
    if (timer_id > 0) {
        g_source_remove(timer_id);
        timer_id = 0;
    }

    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    }

    for (guint i = 0; i < num_sources; i++) {
        if (recording_bins[i]) {
            gst_bin_remove(GST_BIN(pipeline), recording_bins[i]);
            recording_bins[i] = NULL;
        }
        // if (jpeg_bins[i]) {
        //     gst_bin_remove(GST_BIN(pipeline), jpeg_bins[i]);
        //     jpeg_bins[i] = NULL;
        // }
    }

    if (recording_filenames) {
        g_hash_table_foreach(recording_filenames, (GHFunc)g_free, NULL);
        g_hash_table_destroy(recording_filenames);
        recording_filenames = NULL;
    }
    if (tee_recording_pads) {
        g_hash_table_remove_all(tee_recording_pads);
        g_hash_table_destroy(tee_recording_pads);
        tee_recording_pads = NULL;
    }
    if (is_recording) g_hash_table_destroy(is_recording);
    if (recording_start_time) g_hash_table_destroy(recording_start_time);
    if (car_missing_frames) g_hash_table_destroy(car_missing_frames);
    if (stable_sign_frames) g_hash_table_destroy(stable_sign_frames);
    if (stream_status) g_hash_table_destroy(stream_status);
    if (last_frame_time) g_hash_table_destroy(last_frame_time);

    mqtt_cleanup();

    if (pipeline) {
        gst_object_unref(GST_OBJECT(pipeline));
        pipeline = NULL;
    }

    if (sources) {
        g_strfreev(sources);
        sources = NULL;
    }

    if (loop) {
        g_main_loop_unref(loop);
        loop = NULL;
    }

    if (train_uuid) {
        free(train_uuid);
        train_uuid = NULL;
    }

    g_mutex_clear(&mqtt_mutex);
    g_mutex_clear(&liveapi_mutex);
    g_mutex_clear(&liveapi_data_mutex);

    log_message(LOG_INFO, "Train crossing detection application terminated");
    return 0;
}
