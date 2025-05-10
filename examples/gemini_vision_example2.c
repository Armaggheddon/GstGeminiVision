#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h> // For getenv
#include <signal.h> // For signal handling

// Structure to hold all our stuff
typedef struct _AppData {
    GstElement *pipeline;
    GstElement *uri_decode_bin;
    GstElement *video_convert1;
    GstElement *gemini_vision_element;
    GstElement *video_convert2;
    GstElement *auto_video_sink;
    GMainLoop *loop;
} AppData;

// Global pointer for signal handler to access AppData
static AppData *g_app_data_ptr = NULL;

// Forward declarations
static void sigint_handler(int signum);
static void pad_added_handler(GstElement *src, GstPad *new_pad, AppData *data);
static gboolean bus_message_handler(GstBus *bus, GstMessage *msg, AppData *data);
static void on_description_received_from_gemini(GstElement *element, const gchar *description, GstBuffer *buffer, AppData *data);

int main(int argc, char *argv[]) {
    AppData data;
    GstElement *pipeline, *source, *converter1, *gemini, *converter2, *sink;
    GstBus *bus;
    const char *uri = "https://gstreamer.freedesktop.org/data/media/sintel_trailer-480p.webm";

    // Initialize our data structure
    memset(&data, 0, sizeof(data));
    g_app_data_ptr = &data; // Set global pointer for signal handler

    // --- Configuration for Gemini Processor ---
    const gchar *env_api_key = getenv("GST_GEMINI_API_KEY");
    const gchar *hardcoded_api_key = "YOUR_API_KEY_HERE"; // Fallback if env var is not set
    const gchar *api_key_to_use = NULL;

    if (env_api_key && env_api_key[0] != '\0') {
        api_key_to_use = env_api_key;
        g_print("Using API key from GST_GEMINI_API_KEY environment variable.\n");
    } else {
        api_key_to_use = hardcoded_api_key;
        if (strcmp(hardcoded_api_key, "YOUR_API_KEY_HERE") == 0 || hardcoded_api_key[0] == '\0') {
            g_print("Using fallback API key from code, but it's a placeholder or empty.\n");
        } else {
            g_print("Using API key hardcoded in the application.\n");
        }
    }

    const gchar *prompt_text = "Describe this scene in one short sentence. Focus on the main subject.";
    const gchar *model = "gemini-1.5-flash-latest"; // Or another model name
    gdouble analysis_interval_seconds = 5.0;
    gboolean output_as_metadata = FALSE; // Use signal for this example

    // New generationConfig parameters
    const gchar *stop_sequences[] = {"Title", "Conclusion", NULL}; // Example stop sequences, NULL terminated
    gdouble temperature = 0.7;
    gint max_tokens = 150;
    gdouble top_p_val = 0.9;
    gint top_k_val = 20;
    // --- End Configuration ---

    // Initialize GStreamer
    gst_init(&argc, &argv);

    // Create GStreamer elements
    pipeline = gst_pipeline_new("gemini-sintel-pipeline");
    source = gst_element_factory_make("uridecodebin", "uridecodebin-source");
    converter1 = gst_element_factory_make("videoconvert", "videoconvert1");
    gemini = gst_element_factory_make("geminivision", "gemini-processor"); // Use "geminivision"
    converter2 = gst_element_factory_make("videoconvert", "videoconvert2");
    sink = gst_element_factory_make("autovideosink", "autovideosink-output");

    if (!pipeline || !source || !converter1 || !gemini || !converter2 || !sink) {
        g_printerr("One or more elements could not be created. Exiting.\n");
        if (!pipeline) g_printerr("  Pipeline is NULL\n");
        if (!source) g_printerr("  uridecodebin is NULL\n");
        if (!converter1) g_printerr("  videoconvert1 is NULL\n");
        if (!gemini) g_printerr("  geminivision is NULL\n");
        if (!converter2) g_printerr("  videoconvert2 is NULL\n");
        if (!sink) g_printerr("  autovideosink is NULL\n");
        return -1;
    }

    // Initialize AppData
    data.pipeline = pipeline;
    data.loop = g_main_loop_new(NULL, FALSE);
    if (!data.loop) {
        g_printerr("Failed to create main loop\n");
        gst_object_unref(pipeline);
        return -1;
    }
    data.uri_decode_bin = source;
    data.video_convert1 = converter1;
    data.gemini_vision_element = gemini;
    data.video_convert2 = converter2;
    data.auto_video_sink = sink;

    // Condigure the video source
    g_object_set(source, "uri", uri, NULL);

    // Connect the "pad-added" signal for uridecodebin's dynamic pads
    g_signal_connect(source, "pad-added", G_CALLBACK(pad_added_handler), &data);

    // Configure geminivision element
    if (api_key_to_use == NULL || api_key_to_use[0] == '\0' || strcmp(api_key_to_use, "YOUR_API_KEY_HERE") == 0) {
        g_printerr("API Key is not set, is empty, or is a placeholder.\n");
        g_printerr("Please set the GST_GEMINI_API_KEY environment variable or edit the gemini_vision_example.c to provide a valid API key.\n");
        gst_object_unref(pipeline);
        g_main_loop_unref(data.loop);
        return 1;
    }

    g_object_set(G_OBJECT(gemini),
                 "api-key", api_key_to_use,
                 "prompt", prompt_text,
                 "model-name", model,
                 "analysis-interval", analysis_interval_seconds,
                 "output-metadata", output_as_metadata,
                 // Set new generationConfig properties
                 "stop-sequences", stop_sequences, // Pass the gchar** array directly
                 "temperature", temperature,
                 "max-output-tokens", max_tokens,
                 "top-p", top_p_val,
                 "top-k", top_k_val,
                 NULL);

    // Connect to the "description-received" signal from our geminivision element
    g_signal_connect(gemini, "description-received", G_CALLBACK(on_description_received_from_gemini), &data);


    // Add all elements to the pipeline
    gst_bin_add_many(GST_BIN(pipeline), source, converter1,
                     gemini, converter2, sink, NULL);

    // Link static elements: videoconvert1 -> geminivision -> videoconvert2 -> autovideosink
    if (!gst_element_link_many(converter1, gemini, converter2, sink, NULL)) {
        g_printerr("Static elements (videoconvert1 -> geminivision -> videoconvert2 -> autovideosink) could not be linked.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    // Set up the bus watch
    bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, (GstBusFunc)bus_message_handler, &data);
    gst_object_unref(bus);

    // Start playing
    g_print("Setting the pipeline to PLAYING state...\n");
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Unable to set the pipeline to the PLAYING state.\n");
        gst_object_unref(pipeline);
        g_main_loop_unref(data.loop);
        return -1;
    }

    // Wait until an error occurs or EOS
    g_print("Running...\n");
    g_print("Press Ctrl+C to quit\n");
    g_main_loop_run(data.loop);

    // Cleanup after the main loop quits
    g_print("Cleaning up...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline)); // This will also unref all elements contained in the pipeline
    g_main_loop_unref(data.loop);

    return 0;
}

// "pad-added" handler for uridecodebin
static void pad_added_handler(GstElement *src_element, GstPad *new_pad, AppData *data) {
    GstPad *sink_pad = gst_element_get_static_pad(data->video_convert1, "sink");
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    g_print("Received new pad '%s' from '%s'.\n", GST_PAD_NAME(new_pad), GST_ELEMENT_NAME(src_element));

    // If our videoconvert1's sink pad is already linked, we have nothing to do here
    if (gst_pad_is_linked(sink_pad)) {
        g_print("  Video convert sink pad already linked. Ignoring.\n");
        goto exit;
    }

    // Check the new pad's type. We are interested in video.
    new_pad_caps = gst_pad_get_current_caps(new_pad);
    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    if (!g_str_has_prefix(new_pad_type, "video/x-raw")) {
        g_print("  Pad type is '%s', not raw video. Ignoring.\n", new_pad_type);
        goto exit;
    }

    // Attempt to link the new pad to videoconvert1's sink pad
    ret = gst_pad_link(new_pad, sink_pad);
    if (GST_PAD_LINK_FAILED(ret)) {
        g_print("  Failed to link pad of type '%s'.\n", new_pad_type);
    } else {
        g_print("  Successfully linked pad of type '%s'.\n", new_pad_type);
    }

exit:
    if (new_pad_caps != NULL) {
        gst_caps_unref(new_pad_caps);
    }
    gst_object_unref(sink_pad);
}

// Bus message handler
static gboolean bus_message_handler(GstBus *bus, GstMessage *msg, AppData *data) {
    GError *err = NULL;
    gchar *debug_info = NULL;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
            g_printerr("Debugging info: %s\n", debug_info ? debug_info : "none");
            g_error_free(err);
            g_free(debug_info);
            g_main_loop_quit(data->loop);
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data->pipeline)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                g_print("Pipeline state changed from %s to %s\n",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state));
            }
            break;
        case GST_MESSAGE_EOS:
            g_print("End-Of-Stream reached.\n");
            g_main_loop_quit(data->loop);
            break;
        default:
            break;
    }
    return TRUE; // Continue receiving messages
}

// Callback for the "description-received" signal from geminivision element
static void on_description_received_from_gemini(GstElement *element, const gchar *description, GstBuffer *buffer, AppData *data) {
    g_print("(PTS: %" GST_TIME_FORMAT "):\n%s\n========================\n",
            GST_BUFFER_PTS_IS_VALID(buffer) ? GST_TIME_ARGS(GST_BUFFER_PTS(buffer)) : GST_TIME_ARGS(GST_CLOCK_TIME_NONE),
            description ? description : "No description text.");
}