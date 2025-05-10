// gcc -o gemini_example gemini_example.c `pkg-config --cflags --libs gstreamer-1.0`

#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct _AppData {
    GstElement *pipeline;
    GMainLoop *loop;
} AppData;

// Signal handler for "description-received" signal
static void 
on_description_received(GstElement *element, gchar *description, GstBuffer *buffer, gpointer user_data) {
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    printf("=================================\n");
    printf("Frame time: %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(pts));
    printf("Description: %s\n", description);
    printf("=================================\n\n");
    fflush(stdout);
}

// Bus callback to handle pipeline events
static gboolean
bus_callback(GstBus *bus, GstMessage *message, gpointer user_data) { // user_data is AppData*
    AppData *data = (AppData *)user_data;

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *error = NULL;
            gchar *debug_info = NULL;
            
            gst_message_parse_error(message, &error, &debug_info);
            g_printerr("Error from %s: %s\n", GST_OBJECT_NAME(message->src), error->message);
            g_printerr("Debug info: %s\n", debug_info ? debug_info : "none");
            
            g_error_free(error);
            g_free(debug_info);
            g_main_loop_quit(data->loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(data->loop);
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->pipeline)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);
                g_print("Pipeline state changed from %s to %s\n",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state));
            }
            break;
        default:
            break;
    }
    return TRUE;
}

int main(int argc, char *argv[]) {
    GstElement *pipeline, *source, *converter1, *gemini, *converter2, *sink;
    GstBus *bus;
    AppData data; // Declare AppData instance

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

    // Create the pipeline elements
    pipeline = gst_pipeline_new("gemini-test-pipeline");
    source = gst_element_factory_make("videotestsrc", "source");
    converter1 = gst_element_factory_make("videoconvert", "converter1");
    gemini = gst_element_factory_make("geminivision", "gemini");
    converter2 = gst_element_factory_make("videoconvert", "converter2");
    sink = gst_element_factory_make("autovideosink", "sink");

    // Check that all elements were created successfully
    if (!pipeline || !source || !converter1 || !gemini || !converter2 || !sink) {
        g_printerr("Failed to create one or more elements:\n");
        if (!pipeline) g_printerr("  Pipeline is NULL\n");
        if (!source) g_printerr("  Source is NULL\n");
        if (!converter1) g_printerr("  Converter1 is NULL\n");
        if (!gemini) g_printerr("  Gemini (geminivision) is NULL. Check plugin path (GST_PLUGIN_PATH) and plugin installation.\n");
        if (!converter2) g_printerr("  Converter2 is NULL\n");
        if (!sink) g_printerr("  Sink is NULL\n");
        return 1;
    }

    // Initialize AppData
    data.pipeline = pipeline;
    data.loop = g_main_loop_new(NULL, FALSE);
    if (!data.loop) {
        g_printerr("Failed to create main loop\n");
        gst_object_unref(pipeline);
        return 1;
    }

    // Configure the video source
    g_object_set(G_OBJECT(source), "pattern", 18, NULL);  // 18 is GST_VIDEO_TEST_SRC_SMPTE

    // Configure the Gemini vision
    // IMPORTANT: Check if api_key_to_use is valid, otherwise the plugin will error out on start
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

    // Connect the "description-received" signal
    g_signal_connect(gemini, "description-received", G_CALLBACK(on_description_received), NULL);

    // Add all elements to the pipeline
    gst_bin_add_many(GST_BIN(pipeline), source, converter1, gemini, converter2, sink, NULL);

    // Link the elements
    if (!gst_element_link_many(source, converter1, gemini, converter2, sink, NULL)) {
        g_printerr("Elements could not be linked\n");
        gst_object_unref(pipeline);
        g_main_loop_unref(data.loop);
        return 1;
    }

    // Set up the bus watch
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_callback, &data); // Pass address of AppData
    gst_object_unref(bus);

    // Start playing
    g_print("Setting pipeline to PLAYING state...\n");
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Unable to set the pipeline to the playing state.\n");
        gst_object_unref (pipeline);
        g_main_loop_unref(data.loop);
        return -1;
    }


    // Wait until an error occurs or EOS
    g_print("Running...\n");
    g_print("Press Ctrl+C to quit\n");
    g_main_loop_run(data.loop);

    // Clean up
    g_print("Cleaning up...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    g_main_loop_unref(data.loop);

    return 0;
}