// src/gstgeminivision.c
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstgeminivision.h"
#include <gst/video/video.h> // For GstVideoInfo if encoding raw to JPEG
#include <curl/curl.h>
#include <json-c/json.h>


#include <glib/gbase64.h> // For Base64
#include <jpeglib.h>  // For JPEG encoding

GST_DEBUG_CATEGORY (gst_gemini_vision_debug_category);
#define GST_CAT_DEFAULT gst_gemini_vision_debug_category

// Add this line to define the type and create the parent class pointer
G_DEFINE_TYPE (GstGeminiVision, gst_gemini_vision, GST_TYPE_BASE_TRANSFORM);

// Properties enum
enum {
	PROP_0,
	PROP_API_KEY,
	PROP_PROMPT,
	PROP_MODEL_NAME,
	PROP_ANALYSIS_INTERVAL,
	PROP_OUTPUT_METADATA,
	PROP_STOP_SEQUENCES,
	PROP_TEMPERATURE,
	PROP_MAX_OUTPUT_TOKENS,
	PROP_TOP_P,
	PROP_TOP_K,
	PROP_LAST
};

// --- Custom Metadata Implementation ---
static GstGeminiDescriptionMeta *
_gst_gemini_description_meta_copy (
	const GstGeminiDescriptionMeta * meta,
    GstGeminiDescriptionMeta * copy
){
	copy->description = g_strdup (meta->description);
	return copy;
}

static void
_gst_gemini_description_meta_free (GstGeminiDescriptionMeta * meta){
	g_free (meta->description);
}

// This defines the "API" for our metadata
GType
gst_gemini_description_meta_api_get_type (void){
	static volatile GType type = 0;
	static const gchar *tags[] = { "description", "text", "gemini", NULL }; // Tags for discovery

	if (g_once_init_enter (&type)) {
		GType _type = gst_meta_api_type_register ("GstGeminiDescriptionMetaAPI", tags);
		g_once_init_leave (&type, _type);
	}
	return type;
}

// Add this function before gst_gemini_description_meta_get_info()
static gboolean
_gst_gemini_description_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer){
	GstGeminiDescriptionMeta *gmeta = (GstGeminiDescriptionMeta *) meta;

	// Initialize to empty state
	gmeta->description = NULL;

	return TRUE;
}

// This defines the GstMetaInfo for our metadata
const GstMetaInfo *
gst_gemini_description_meta_get_info (void){
	static const GstMetaInfo *meta_info = NULL;

	if (g_once_init_enter (&meta_info)) {
		const GstMetaInfo *mi = gst_meta_register (
			GST_GEMINI_DESCRIPTION_META_API_TYPE, // Our API type
			"GstGeminiDescriptionMeta",           // Implementation name
			sizeof (GstGeminiDescriptionMeta),    // Size of our meta struct
			(GstMetaInitFunction) _gst_gemini_description_meta_init, // No special init needed beyond zeroing
			(GstMetaFreeFunction) _gst_gemini_description_meta_free,
			(GstMetaTransformFunction) NULL // No transform needed
		); 
		g_once_init_leave (&meta_info, mi);
	}
	return meta_info;
}

// Helper function to add our metadata to a buffer
GstGeminiDescriptionMeta *
gst_buffer_add_gemini_description_meta(GstBuffer *buffer, const gchar *description) {
    GstGeminiDescriptionMeta *meta;

    g_return_val_if_fail(GST_IS_BUFFER(buffer), NULL);
    g_return_val_if_fail(description != NULL, NULL);

    // Get the GstMetaInfo for our custom metadata
    const GstMetaInfo *info = gst_gemini_description_meta_get_info();
    if (!info) {
        GST_ERROR("Failed to get GstGeminiDescriptionMeta info");
        return NULL;
    }

    // Add the metadata to the buffer
    meta = (GstGeminiDescriptionMeta *) gst_buffer_add_meta(buffer, info, NULL);
    if (!meta) {
        GST_ERROR("Failed to add GstGeminiDescriptionMeta to buffer");
        return NULL;
    }

    // Initialize our custom fields
    meta->meta.flags = GST_META_FLAG_NONE; // Or other flags like GST_META_FLAG_POOLED
    meta->description = g_strdup(description);

    return meta;
}


typedef struct {
	unsigned char *data;
	unsigned long size;
	unsigned long allocated_size;
} JPEGDynamicBuffer;

static void
jpeg_init_destination(j_compress_ptr cinfo) {
	JPEGDynamicBuffer *dest = (JPEGDynamicBuffer*) cinfo->client_data;
	dest->data = g_malloc(16384);  // Initial buffer size
	dest->allocated_size = 16384;
	dest->size = 0;

	cinfo->dest->next_output_byte = dest->data;
	cinfo->dest->free_in_buffer = dest->allocated_size;
}

static boolean
jpeg_empty_output_buffer(j_compress_ptr cinfo) {
	JPEGDynamicBuffer *dest = (JPEGDynamicBuffer*) cinfo->client_data;
	unsigned int new_size = dest->allocated_size * 2;
	unsigned char *new_buffer = g_realloc(dest->data, new_size);

	if (!new_buffer) {
		// Instead of ERREXIT(cinfo, JERR_OUT_OF_MEMORY);
		// Use the standard error reporting method
		(*cinfo->err->error_exit)((j_common_ptr)cinfo);
		return FALSE;
	}

	dest->data = new_buffer;
	cinfo->dest->next_output_byte = dest->data + dest->allocated_size;
	cinfo->dest->free_in_buffer = dest->allocated_size;
	dest->allocated_size = new_size;

	return TRUE;
}

static void
jpeg_term_destination(j_compress_ptr cinfo) {
	JPEGDynamicBuffer *dest = (JPEGDynamicBuffer*) cinfo->client_data;
	dest->size = dest->allocated_size - cinfo->dest->free_in_buffer;
}

// Function to encode raw video frame to JPEG
static gboolean
encode_frame_to_jpeg(
	GstGeminiVision *self, 
	GstMapInfo *map_info, 
    unsigned char **jpeg_buffer, 
	unsigned long *jpeg_size
) {
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	JSAMPROW row_pointer[1];
	int row_stride;
	JPEGDynamicBuffer dest_buffer = {NULL, 0, 0};
	struct jpeg_destination_mgr dest_mgr = {
		jpeg_init_destination,
		jpeg_empty_output_buffer,
		jpeg_term_destination
  	};
  
	// Safety checks
	if (!map_info || !map_info->data || map_info->size == 0) {
		GST_ERROR_OBJECT(self, "Invalid map info or buffer data");
		return FALSE;
	}
  
	if (self->input_video_info.width <= 0 || self->input_video_info.height <= 0) {
		GST_ERROR_OBJECT(
			self, 
			"Invalid video dimensions: %dx%d", 
			self->input_video_info.width, self->input_video_info.height
		);
		return FALSE;
	}
  
	GstVideoFormat format = self->input_video_info.finfo->format;
	const char *format_name = gst_video_format_to_string(format);
	GST_DEBUG_OBJECT(
		self, 
		"Encoding video format %s to JPEG, dimensions: %dx%d", 
		format_name, self->input_video_info.width, self->input_video_info.height
	);
  
	// Verify we have enough data for the frame
	gsize expected_size = self->input_video_info.size;
	if (map_info->size < expected_size) {
		GST_ERROR_OBJECT(
			self, 
			"Buffer too small: got %zu bytes, expected %zu bytes",
			map_info->size, expected_size
		);
		return FALSE;
	}
  
	// Set up JPEG compression with error handling
	memset(&cinfo, 0, sizeof(cinfo));
	memset(&jerr, 0, sizeof(jerr));
	cinfo.err = jpeg_std_error(&jerr);
	
	// Create the compressor
	jpeg_create_compress(&cinfo);
	
	// Set up dynamic memory destination
	dest_buffer.data = NULL;
	dest_buffer.size = 0;
	dest_buffer.allocated_size = 0;
	
	dest_mgr.init_destination = jpeg_init_destination;
	dest_mgr.empty_output_buffer = jpeg_empty_output_buffer;
	dest_mgr.term_destination = jpeg_term_destination;
	
	cinfo.client_data = &dest_buffer;
	cinfo.dest = &dest_mgr;
	
	// Set JPEG parameters
	cinfo.image_width = self->input_video_info.width;
	cinfo.image_height = self->input_video_info.height;
	
	// Get stride information
	int n_components;
	J_COLOR_SPACE color_space;
  
	// Handle various formats - focus on common RGB formats for Gemini
	switch (format) {
		case GST_VIDEO_FORMAT_RGB:
			n_components = 3;
			color_space = JCS_RGB;
			break;
		case GST_VIDEO_FORMAT_BGR:
			n_components = 3;
			color_space = JCS_RGB; // We'll need to convert BGR->RGB
			break;
		case GST_VIDEO_FORMAT_RGBA:
		case GST_VIDEO_FORMAT_BGRA:
			n_components = 3; // We'll skip the alpha channel
			color_space = JCS_RGB;
			break;
		case GST_VIDEO_FORMAT_I420:
		case GST_VIDEO_FORMAT_YV12:
		case GST_VIDEO_FORMAT_NV12:
		case GST_VIDEO_FORMAT_NV21:
			// For YUV formats, we should convert to RGB first
			GST_ERROR_OBJECT(self, "YUV formats not directly supported. Please add videoconvert before this element");
			return FALSE;
		default:
			GST_ERROR_OBJECT(self, "Unsupported video format for JPEG encoding: %s", format_name);
			return FALSE;
	}
  
	cinfo.input_components = n_components;
	cinfo.in_color_space = color_space;
	
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, 85, TRUE); // 85% quality
	
	// Start compression
	jpeg_start_compress(&cinfo, TRUE);
	
	// Get row stride - use actual stride from videoinfo
	row_stride = GST_VIDEO_INFO_PLANE_STRIDE(&self->input_video_info, 0);
	GST_DEBUG_OBJECT(self, "Video stride: %d, components: %d", row_stride, n_components);
	
	// Allocate temporary buffer for row if we need format conversion
	guchar *rgb_row = NULL;
	if (format == GST_VIDEO_FORMAT_BGR || format == GST_VIDEO_FORMAT_BGRA || format == GST_VIDEO_FORMAT_RGBA) {
		rgb_row = g_malloc(self->input_video_info.width * 3); // RGB buffer
	}
  
	// Process image data row by row
	while (cinfo.next_scanline < cinfo.image_height) {
		guchar *src_row = map_info->data + (cinfo.next_scanline * row_stride);
		
		// Handle conversions if needed
		if (format == GST_VIDEO_FORMAT_BGR) {
			// Convert BGR -> RGB
			for (int i = 0; i < self->input_video_info.width; i++) {
				rgb_row[i*3 + 0] = src_row[i*3 + 2]; // R <- B
				rgb_row[i*3 + 1] = src_row[i*3 + 1]; // G <- G
				rgb_row[i*3 + 2] = src_row[i*3 + 0]; // B <- R
			}
			row_pointer[0] = rgb_row;
		}
		else if (format == GST_VIDEO_FORMAT_RGBA) {
			// Skip alpha channel in RGBA
			for (int i = 0; i < self->input_video_info.width; i++) {
				rgb_row[i*3 + 0] = src_row[i*4 + 0]; // R
				rgb_row[i*3 + 1] = src_row[i*4 + 1]; // G
				rgb_row[i*3 + 2] = src_row[i*4 + 2]; // B
			}
			row_pointer[0] = rgb_row;
		}
		else if (format == GST_VIDEO_FORMAT_BGRA) {
			// Convert BGRA -> RGB (skip alpha channel)
			for (int i = 0; i < self->input_video_info.width; i++) {
				rgb_row[i*3 + 0] = src_row[i*4 + 2]; // R <- B
				rgb_row[i*3 + 1] = src_row[i*4 + 1]; // G <- G
				rgb_row[i*3 + 2] = src_row[i*4 + 0]; // B <- R
			}
			row_pointer[0] = rgb_row;
		}
		else {
			// RGB or other direct format
			row_pointer[0] = src_row;
		}
		
		if (jpeg_write_scanlines(&cinfo, row_pointer, 1) != 1) {
			GST_ERROR_OBJECT(self, "Error writing JPEG scanline");
			if (rgb_row) g_free(rgb_row);
			jpeg_destroy_compress(&cinfo);
			if (dest_buffer.data) g_free(dest_buffer.data);
			return FALSE;
		}
	}
  
	// Free temporary row buffer if allocated
	if (rgb_row) g_free(rgb_row);
	
	// Finish compression
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	
	// Set output parameters
	*jpeg_buffer = dest_buffer.data;
	*jpeg_size = dest_buffer.size;
	
	GST_DEBUG_OBJECT(
		self, 
		"Successfully encoded JPEG image (%lu bytes)", 
		dest_buffer.size
	);
	return TRUE;
}

// --- Worker Thread Data Structures & Functions ---
typedef struct {
	gchar *data;
	size_t size;
} MemoryStruct;

static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    gchar *ptr = g_realloc(mem->data, mem->size + realsize + 1);
    if (ptr == NULL) {
        GST_ERROR("not enough memory (realloc returned NULL)");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0; // Null-terminate
    return realsize;
}

// --- Worker Thread Function ---
static gpointer
gemini_worker_thread_func (gpointer data) {
    GstGeminiVision *self = GST_GEMINI_VISION (data);
    GeminiRequestData *req_data;
    CURL *curl_handle;
    CURLcode res;
    struct curl_slist *headers = NULL;

    GST_DEBUG_OBJECT (self, "Worker thread started.");

    curl_global_init(CURL_GLOBAL_ALL); // Init curl for this thread

    while (self->worker_running) {
        // Block and wait for a request from the queue
        req_data = g_async_queue_pop (self->request_queue);

        if (!self->worker_running) { // Check again after pop, in case of shutdown
             if (req_data) { // Free any pending request on shutdown
                g_free(req_data->image_data);
                g_free(req_data->api_key);
                g_free(req_data->prompt);
                g_free(req_data->model_name);
                if (req_data->stop_sequences) g_strfreev(req_data->stop_sequences); // Free new prop
                if (req_data->original_buffer) gst_buffer_unref(req_data->original_buffer);
                g_free(req_data);
            }
            break;
        }

        if (!req_data->image_data && !req_data->prompt){
			if (req_data->original_buffer) gst_buffer_unref(req_data->original_buffer); // Should be NULL for dummy
			g_free(req_data->api_key); // Potentially NULL
			g_free(req_data->prompt);  // Potentially NULL
			g_free(req_data->model_name); // Potentially NULL
			if (req_data->stop_sequences) g_strfreev(req_data->stop_sequences); // Potentially NULL
			g_free(req_data);
			continue;
        }

        if (!req_data) { // Should not happen if worker_running is true
            GST_WARNING_OBJECT(self, "Popped NULL request data unexpectedly.");
            continue;
        }

        GST_DEBUG_OBJECT (
			self, 
			"Worker processing request for buffer PTS %" GST_TIME_FORMAT,
            GST_TIME_ARGS(
				req_data->original_buffer ? 
				GST_BUFFER_PTS(req_data->original_buffer) : GST_CLOCK_TIME_NONE
			)
		);

        curl_handle = curl_easy_init();
        if (curl_handle) {
            MemoryStruct chunk;
            chunk.data = g_malloc(1); // Will be grown by realloc
            chunk.size = 0;

            // 1. Base64 encode image_data
            gchar *b64_image_data = g_base64_encode(req_data->image_data, req_data->image_size);
            if (!b64_image_data) {
                GST_ERROR_OBJECT(self, "Failed to Base64 encode image data.");
                goto next_request; // Cleanup and get next item
            }

            // Construct JSON payload correctly matching the API format
            json_object *jobj = json_object_new_object();
            json_object *jcontents_array = json_object_new_array();
            json_object *jcontent_obj = json_object_new_object();
            json_object *jparts_array = json_object_new_array();

            // First part: image (NOTE: order matters for Gemini)
            json_object *jimage_part = json_object_new_object();
            json_object *jinline_data = json_object_new_object();
            json_object_object_add(jinline_data, "mime_type", json_object_new_string("image/jpeg"));
            json_object_object_add(jinline_data, "data", json_object_new_string(b64_image_data));
            json_object_object_add(jimage_part, "inline_data", jinline_data);
            json_object_array_add(jparts_array, jimage_part);

            // Second part: text prompt
            json_object *jtext_part = json_object_new_object();
            json_object_object_add(jtext_part, "text", json_object_new_string(req_data->prompt));
            json_object_array_add(jparts_array, jtext_part);

            // Complete the JSON structure
            json_object_object_add(jcontent_obj, "parts", jparts_array);
            json_object_array_add(jcontents_array, jcontent_obj);
            json_object_object_add(jobj, "contents", jcontents_array);


            // --- Add generationConfig ---
            json_object *jgen_config = json_object_new_object();
            gboolean gen_config_added = FALSE;

            if (req_data->stop_sequences && req_data->stop_sequences[0] != NULL) {
                json_object *jstop_seq_array = json_object_new_array();
                for (int i = 0; req_data->stop_sequences[i] != NULL; i++) {
                    json_object_array_add(jstop_seq_array, json_object_new_string(req_data->stop_sequences[i]));
                }
                json_object_object_add(jgen_config, "stopSequences", jstop_seq_array);
                gen_config_added = TRUE;
            }
            if (req_data->temperature >= 0.0) { // Assuming -1.0 is "not set"
                json_object_object_add(jgen_config, "temperature", json_object_new_double(req_data->temperature));
                gen_config_added = TRUE;
            }
            if (req_data->max_output_tokens > 0) { // Assuming 0 or -1 is "not set"
                json_object_object_add(jgen_config, "maxOutputTokens", json_object_new_int(req_data->max_output_tokens));
                gen_config_added = TRUE;
            }
            if (req_data->top_p >= 0.0) { // Assuming -1.0 is "not set"
                json_object_object_add(jgen_config, "topP", json_object_new_double(req_data->top_p));
                gen_config_added = TRUE;
            }
            if (req_data->top_k > 0) { // Assuming 0 or -1 is "not set"
                json_object_object_add(jgen_config, "topK", json_object_new_int(req_data->top_k));
                gen_config_added = TRUE;
            }

            if (gen_config_added) {
                json_object_object_add(jobj, "generationConfig", jgen_config);
            } else {
                json_object_put(jgen_config); // Not used, free it
            }
            // --- End generationConfig ---

            const char *json_string = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
            GST_DEBUG_OBJECT(self, "Sending JSON: %s", json_string);

            // Set up CURL
            char *api_url = g_strdup_printf(
				"https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s", 
				req_data->model_name, req_data->api_key
			);
            curl_easy_setopt(curl_handle, CURLOPT_URL, api_url);
            g_free(api_url); // Free the URL string
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, json_string);
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, (long)strlen(json_string));

            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

            curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

            res = curl_easy_perform(curl_handle);

            if (res != CURLE_OK) {
                GST_ERROR_OBJECT(self, "curl_easy_perform() failed: %s", curl_easy_strerror(res));
            } else {
                GST_DEBUG_OBJECT(self, "%lu bytes retrieved from API", (unsigned long)chunk.size);
                GST_DEBUG_OBJECT(self, "API Response: %s", chunk.data);

                // Parse JSON response
                json_object *parsed_json = json_tokener_parse(chunk.data);
                json_object *candidates_array, *candidate, *content, *parts_array, *part, *text_obj;
                const char *description_text = "No description found.";

                if (parsed_json &&
                    json_object_object_get_ex(parsed_json, "candidates", &candidates_array) &&
                    json_object_is_type(candidates_array, json_type_array) &&
                    json_object_array_length(candidates_array) > 0) {

                    candidate = json_object_array_get_idx(candidates_array, 0);
                    if (json_object_object_get_ex(candidate, "content", &content) &&
                        json_object_object_get_ex(content, "parts", &parts_array) &&
                        json_object_is_type(parts_array, json_type_array) &&
                        json_object_array_length(parts_array) > 0) {

                        part = json_object_array_get_idx(parts_array, 0); // Assuming first part is text
                        if (json_object_object_get_ex(part, "text", &text_obj)) {
                            description_text = json_object_get_string(text_obj);
                        }
                    }
                } else if (parsed_json && json_object_object_get_ex(parsed_json, "error", &text_obj)) {
                    // Handle API error responses
                     json_object *message_obj;
                     if(json_object_object_get_ex(text_obj, "message", &message_obj)){
                        description_text = json_object_get_string(message_obj);
                        GST_WARNING_OBJECT(self, "Gemini API Error: %s", description_text);
                     } else {
                        GST_WARNING_OBJECT(self, "Gemini API returned an error structure: %s", json_object_to_json_string(text_obj));
                     }
                } else {
                    GST_WARNING_OBJECT(self, "Could not parse Gemini response or find text: %s", chunk.data);
                }

                // Send result (description_text and original_buffer) back to GStreamer thread
                // For simplicity, we use another GAsyncQueue for results here.
                GeminiResultData *result_data = g_new0(GeminiResultData, 1);
                result_data->description = g_strdup(description_text);
                result_data->original_buffer = req_data->original_buffer; // Transfer ownership
                req_data->original_buffer = NULL; // Avoid double unref in cleanup
                // Make sure the GstGeminiVision self pointer is also in result_data if needed by the callback
                result_data->processor_element = req_data->self;

                g_async_queue_push(self->result_queue, result_data);
                // Trigger the GSource in the main GStreamer context to process results
                if(self->result_source) {
					g_source_set_ready_time(self->result_source, 0); // Wake it up if it's an idle source
                }

                if(parsed_json) json_object_put(parsed_json); // Free json-c object
            }

            // Cleanup for this request
            curl_slist_free_all(headers);
            headers = NULL;
            curl_easy_cleanup(curl_handle);
            g_free(b64_image_data);
            json_object_put(jobj); // Free our request json-c object
            g_free(chunk.data);
        }

    next_request:
		g_free(req_data->image_data);
		g_free(req_data->api_key);
		g_free(req_data->prompt);
		g_free(req_data->model_name);
		if (req_data->stop_sequences) g_strfreev(req_data->stop_sequences);
		if (req_data->original_buffer) gst_buffer_unref(req_data->original_buffer);
		g_free(req_data);
    }

    curl_global_cleanup(); // Cleanup curl for this thread
    GST_DEBUG_OBJECT (self, "Worker thread finished.");
    return NULL;
}

// Called when the object is about to be destroyed.
static void
gst_gemini_vision_dispose(GObject *object) {
  	GstGeminiVision *self = GST_GEMINI_VISION(object);
  
	if (self->worker_running) {
		self->worker_running = FALSE;
		if (self->request_queue) {
			GeminiRequestData *dummy_req = g_new0(GeminiRequestData, 1);
			// Set dummy values for fields that might be checked if it's not a simple NULL check
			dummy_req->image_data = NULL; 
			dummy_req->prompt = NULL; 
			g_async_queue_push(self->request_queue, dummy_req);
		}
		if (self->worker_thread) {
			g_thread_join(self->worker_thread);
			self->worker_thread = NULL;
		}
	}
  
	if (self->request_queue) {
		GeminiRequestData *req;
		while ((req = g_async_queue_try_pop(self->request_queue))) {
			g_free(req->image_data);
			g_free(req->api_key);
			g_free(req->prompt);
			g_free(req->model_name);
			if (req->stop_sequences) g_strfreev(req->stop_sequences);
			if (req->original_buffer) gst_buffer_unref(req->original_buffer);
			g_free(req);
		}
		g_async_queue_unref(self->request_queue);
		self->request_queue = NULL;
	}
  
	if (self->result_queue) {
		GeminiResultData *res;
		while ((res = g_async_queue_try_pop(self->result_queue))) {
			g_free(res->description);
			if (res->original_buffer) gst_buffer_unref(res->original_buffer);
			g_free(res);
		}
		g_async_queue_unref(self->result_queue);
		self->result_queue = NULL;
	}
  
	if (self->result_source) {
		g_source_destroy(self->result_source);
		g_source_unref(self->result_source);
		self->result_source = NULL;
	}
  
	g_free(self->api_key);
	self->api_key = NULL;
	g_free(self->prompt);
	self->prompt = NULL;
	g_free(self->model_name);
	self->model_name = NULL;
  
	// Free new generationConfig properties
	if (self->stop_sequences) g_strfreev(self->stop_sequences);
	self->stop_sequences = NULL;

	g_free(self->pending_description);
	self->pending_description = NULL;
	
	G_OBJECT_CLASS(gst_gemini_vision_parent_class)->dispose(object);
}

// --- Update result callback to apply to current buffer ---
static gboolean
process_gemini_result_callback (gpointer data){
	GstGeminiVision *self = GST_GEMINI_VISION (data);
	GeminiResultData *result;
  
	// Process all available results
	while ((result = g_async_queue_try_pop(self->result_queue))) {
		GST_INFO_OBJECT(self, "Received description: %s", result->description);
		
		// Store the result for the next buffer
		g_free(self->pending_description);
		self->pending_description = g_strdup(result->description);
		
		// Mark that we're done with this analysis and can start another
		self->analysis_in_progress = FALSE;
		
		// Emit signal (if configured)
		if (!self->output_metadata) {
			// Fix: Don't try to pass NULL as a buffer - either use result->original_buffer 
			// or change the signal to only take a string parameter
			if (result->original_buffer) {
				g_signal_emit(
					self, 
					g_signal_lookup("description-received", GST_TYPE_GEMINI_VISION), 
					0,
					result->description, 
					result->original_buffer
				);
			} else {
				GST_WARNING_OBJECT(self, "No buffer available for signal emission");
			}
		}
		
		// Clean up
		g_free(result->description);
		if (result->original_buffer) gst_buffer_unref(result->original_buffer);
		g_free(result);
	}
		
	return G_SOURCE_CONTINUE;
}


static gboolean
gst_gemini_vision_start (GstBaseTransform * trans) {
	GstGeminiVision *self = GST_GEMINI_VISION (trans);
	GST_INFO_OBJECT (self, "Starting");
	self->last_analysis_time_ns = 0;
	self->worker_running = TRUE;

	if (!self->worker_thread) {
		gchar *thread_name = g_strdup_printf("%s-worker", GST_OBJECT_NAME(self));
		self->worker_thread = g_thread_new (thread_name, gemini_worker_thread_func, self);
		g_free(thread_name);
		GST_INFO_OBJECT (self, "Worker thread created.");
	}

	// Create and attach GSource for results
	if (!self->result_source) {
		self->result_source = g_idle_source_new(); // Simple idle source
		g_source_set_priority(self->result_source, G_PRIORITY_DEFAULT_IDLE);
		g_source_set_callback(self->result_source, process_gemini_result_callback, self, NULL);
		g_source_attach(self->result_source, g_main_context_get_thread_default()); // Attach to current thread's default main context
	}

	return TRUE;
}

static gboolean
gst_gemini_vision_stop (GstBaseTransform * trans) {
	GstGeminiVision *self = GST_GEMINI_VISION (trans);
	GST_INFO_OBJECT (self, "Stopping");

	// Worker thread is stopped in dispose, which is called after stop
	// But you might want to signal it earlier or clear queues here
	if (self->worker_running) {
		self->worker_running = FALSE;
		// Push a NULL to potentially unblock the worker from g_async_queue_pop
		// The worker thread checks self->worker_running after popping.
		if(self->request_queue) {
			GeminiRequestData *dummy_req = g_new0(GeminiRequestData, 1);
			g_async_queue_push(self->request_queue, dummy_req);
			// Don't join here, join in dispose. GStreamer handles object lifecycle.
		}
		if (self->result_source) { // Detach and destroy source on stop
			g_source_destroy(self->result_source);
			g_source_unref(self->result_source);
			self->result_source = NULL;
		}

		return TRUE;
	}
}

static gboolean
gst_gemini_vision_set_caps (
	GstBaseTransform * trans, 
	GstCaps * incaps,
    GstCaps * outcaps
) {
	GstGeminiVision *self = GST_GEMINI_VISION (trans);
	GST_INFO_OBJECT (
		self, 
		"Setting caps: in %" GST_PTR_FORMAT ", out %" GST_PTR_FORMAT,
		incaps, 
		outcaps
	);

	// Determine if input is JPEG or raw video
	GstStructure *s = gst_caps_get_structure(incaps, 0);
	const gchar *name = gst_structure_get_name(s);
  
	if (g_str_equal(name, "image/jpeg")) {
		self->input_is_jpeg = TRUE;
	} else {
		self->input_is_jpeg = FALSE;
		// Parse video info for raw video
		if (!gst_video_info_from_caps(&self->input_video_info, incaps)) {
			GST_ERROR_OBJECT(self, "Failed to parse video info from caps");
			return FALSE;
		}
	}
	
	return TRUE;
}

// --- Modify transform_ip to add pending description to each buffer ---
static GstFlowReturn
gst_gemini_vision_transform_ip (GstBaseTransform * trans, GstBuffer * buf) {
	GstGeminiVision *self = GST_GEMINI_VISION (trans);
	GstClockTime current_time = GST_BUFFER_PTS(buf); // Or GST_BUFFER_DTS or calculate running time
  
	if (!self->analysis_in_progress && 
		(self->last_analysis_time_ns == 0 || 
		(
			GST_CLOCK_TIME_IS_VALID(current_time) && 
			GST_CLOCK_TIME_IS_VALID(self->last_analysis_time_ns) &&
			current_time - self->last_analysis_time_ns >= self->analysis_interval
		) ||
		!GST_CLOCK_TIME_IS_VALID(current_time) || 
		!GST_CLOCK_TIME_IS_VALID(self->last_analysis_time_ns) // Handle invalid timestamps by analyzing
		)
	) {
    
		GST_INFO_OBJECT(
			self, 
			"Analyzing frame at PTS %" GST_TIME_FORMAT, 
			GST_TIME_ARGS(current_time)
		);
		
		if (!self->api_key || self->api_key[0] == '\0') {
			GST_WARNING_OBJECT(self, "API Key not set. Skipping analysis.");
			// Still apply pending description if any
			if (self->output_metadata && self->pending_description && gst_buffer_is_writable(buf)) {
				gst_buffer_add_gemini_description_meta(buf, self->pending_description);
			}
			return GST_FLOW_OK;
		}
		
		if (!self->worker_running || !self->worker_thread) {
			GST_WARNING_OBJECT(self, "Worker not running. Skipping analysis.");
			if (self->output_metadata && self->pending_description && gst_buffer_is_writable(buf)) {
				gst_buffer_add_gemini_description_meta(buf, self->pending_description);
			}
			return GST_FLOW_OK;
		}
		
		GstMapInfo map;
		GST_INFO_OBJECT(self, "Mapping buffer for analysis.");
		if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
			GST_INFO_OBJECT(self, "Buffer mapped successfully.");
			unsigned char *jpeg_data = NULL;
			unsigned long jpeg_size = 0;
		
			if (!self->input_is_jpeg) {
				GST_INFO_OBJECT(self, "Input is not jpg");
				if (!encode_frame_to_jpeg(self, &map, &jpeg_data, &jpeg_size)) {
					GST_ERROR_OBJECT(self, "Failed to encode frame to JPEG");
					gst_buffer_unmap(buf, &map);
					return GST_FLOW_ERROR; 
				}
			} else {
				GST_INFO_OBJECT(self, "Input is jpg");
				jpeg_data = g_malloc(map.size);
				if (jpeg_data) { // Check malloc success
					memcpy(jpeg_data, map.data, map.size);
					jpeg_size = map.size;
				} else {
					GST_ERROR_OBJECT(self, "Failed to allocate memory for JPEG data copy");
					gst_buffer_unmap(buf, &map);
					return GST_FLOW_ERROR;
				}
			}
		
			GeminiRequestData *req = g_new0(GeminiRequestData, 1);
			req->image_data = jpeg_data;
			req->image_size = jpeg_size;
			req->api_key = g_strdup(self->api_key);
			req->prompt = g_strdup(self->prompt);
			req->model_name = g_strdup(self->model_name);
			req->original_buffer = gst_buffer_ref(buf);
			req->self = self;

			// Copy generationConfig properties to request data
			if (self->stop_sequences) {
				req->stop_sequences = g_strdupv(self->stop_sequences);
			} else {
				req->stop_sequences = NULL;
			}
			req->temperature = self->temperature;
			req->max_output_tokens = self->max_output_tokens;
			req->top_p = self->top_p;
			req->top_k = self->top_k;
			
			gst_buffer_unmap(buf, &map);
			
			self->analysis_in_progress = TRUE;
			self->last_analysis_time_ns = current_time;
			
			g_async_queue_push(self->request_queue, req);
			GST_DEBUG_OBJECT(self, "Queued frame for analysis.");
		} else {
			GST_WARNING_OBJECT(self, "Failed to map buffer for analysis.");
		}
	}
  
	if (self->output_metadata && self->pending_description) {
		if (gst_buffer_is_writable(buf)) { // Check if original buffer is writable
			gst_buffer_add_gemini_description_meta(buf, self->pending_description);
		} else {
			// If not writable, we'd need to copy the buffer to add metadata.
			// For simplicity here, we'll skip if not writable.
			// A more robust solution might involve gst_buffer_make_writable or copying.
			GST_DEBUG_OBJECT(
				self, 
				"Buffer not writable, skipping metadata addition to this buffer. Description will apply to next writable one or be lost if output-metadata is true."
			);
		}
	}
  
  	return GST_FLOW_OK;
}

static void
gst_gemini_vision_set_property (
	GObject * object, 
	guint prop_id,
    const GValue * value, 
	GParamSpec * pspec
){
  	GstGeminiVision *self = GST_GEMINI_VISION (object);

	switch (prop_id) {
		case PROP_API_KEY:
			g_free(self->api_key);
			self->api_key = g_value_dup_string (value);
			break;
		case PROP_PROMPT:
			g_free(self->prompt);
			self->prompt = g_value_dup_string (value);
			break;
		case PROP_MODEL_NAME:
			g_free(self->model_name);
			self->model_name = g_value_dup_string (value);
			break;
		case PROP_ANALYSIS_INTERVAL:
			self->analysis_interval_sec = g_value_get_double (value);
			self->analysis_interval = GST_SECOND * self->analysis_interval_sec;
			break;
		case PROP_OUTPUT_METADATA:
			self->output_metadata = g_value_get_boolean (value);
			break;
		case PROP_STOP_SEQUENCES:
			if (self->stop_sequences) g_strfreev(self->stop_sequences);
			self->stop_sequences = g_value_dup_boxed(value); // For G_TYPE_STRV
			break;
		case PROP_TEMPERATURE:
			self->temperature = g_value_get_double(value);
			break;
		case PROP_MAX_OUTPUT_TOKENS:
			self->max_output_tokens = g_value_get_int(value);
			break;
		case PROP_TOP_P:
			self->top_p = g_value_get_double(value);
			break;
		case PROP_TOP_K:
			self->top_k = g_value_get_int(value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
gst_gemini_vision_get_property (
	GObject * object, 
	guint prop_id,
    GValue * value, 
	GParamSpec * pspec
) {
  	GstGeminiVision *self = GST_GEMINI_VISION (object);

	switch (prop_id) {
		case PROP_API_KEY:
			g_value_set_string (value, self->api_key);
			break;
		case PROP_PROMPT:
			g_value_set_string (value, self->prompt);
			break;
		case PROP_MODEL_NAME:
			g_value_set_string (value, self->model_name);
			break;
		case PROP_ANALYSIS_INTERVAL:
			g_value_set_double (value, self->analysis_interval_sec);
			break;
		case PROP_OUTPUT_METADATA:
			g_value_set_boolean (value, self->output_metadata);
			break;
		case PROP_STOP_SEQUENCES:
			g_value_set_boxed(value, self->stop_sequences); // For G_TYPE_STRV
			break;
		case PROP_TEMPERATURE:
			g_value_set_double(value, self->temperature);
			break;
		case PROP_MAX_OUTPUT_TOKENS:
			g_value_set_int(value, self->max_output_tokens);
			break;
		case PROP_TOP_P:
			g_value_set_double(value, self->top_p);
			break;
		case PROP_TOP_K:
			g_value_set_int(value, self->top_k);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
gst_gemini_vision_class_init (GstGeminiVisionClass * klass) {
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS (klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	
	GST_DEBUG_CATEGORY_INIT (
		gst_gemini_vision_debug_category, 
		"geminivision", 
		0, 
		"Gemini Vision Plugin"
	);

	static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
		"sink",
		GST_PAD_SINK,
		GST_PAD_ALWAYS,
		GST_STATIC_CAPS(
			"video/x-raw, format={RGB, BGR, RGBA, BGRA}; "
			"image/jpeg"
		)
	);

	static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
		"src",
		GST_PAD_SRC,
		GST_PAD_ALWAYS,
		GST_STATIC_CAPS(
			"video/x-raw, format={RGB, BGR, RGBA, BGRA}; "
			"image/jpeg"
		)
	);

	gst_element_class_add_pad_template(
		element_class,
		gst_static_pad_template_get(&sink_template)
	);
	gst_element_class_add_pad_template(
		element_class,
		gst_static_pad_template_get(&src_template)
	);

	gst_element_class_set_static_metadata(
		element_class,
		"Gemini Vision Processor",
		"Filter/Analyzer/Video",
		"Processes video frames with Google Gemini Vision API",
		"Armaggheddon https://github.com/Armaggheddon/GstGeminiVision"
	);

	gobject_class->dispose = gst_gemini_vision_dispose;
	gobject_class->set_property = gst_gemini_vision_set_property;
	gobject_class->get_property = gst_gemini_vision_get_property;
  
	base_transform_class->start = gst_gemini_vision_start;
	base_transform_class->stop = gst_gemini_vision_stop;
	base_transform_class->set_caps = gst_gemini_vision_set_caps;
	base_transform_class->transform_ip = gst_gemini_vision_transform_ip;

	g_object_class_install_property (
		gobject_class, 
		PROP_API_KEY,
		g_param_spec_string (
			"api-key", 
			"API Key", 
			"Google Gemini API key",
			NULL, 
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));
	g_object_class_install_property (
		gobject_class, 
		PROP_PROMPT,
		g_param_spec_string (
			"prompt", 
			"Prompt", 
			"Text prompt to send to Gemini",
			"Describe what you see in this image", 
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));
	g_object_class_install_property (
		gobject_class, 
		PROP_MODEL_NAME,
		g_param_spec_string (
			"model-name", 
			"Model Name", 
			"Gemini model name",
			"gemini-1.5-flash", 
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));
	g_object_class_install_property (
		gobject_class, 
		PROP_ANALYSIS_INTERVAL,
		g_param_spec_double (
			"analysis-interval", 
			"Analysis Interval", 
			"The time interval in seconds between consecutive vision analysis operations. Controls how frequently the Gemini Vision API is called to analyze video frames.",
			0.1, 
			3600.0, 
			5.0, 
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));
	g_object_class_install_property (
		gobject_class, 
		PROP_OUTPUT_METADATA,
		g_param_spec_boolean (
			"output-metadata", 
			"Output Metadata", 
			"If TRUE, output description as GstMeta. If FALSE, emit a signal.",
			TRUE, 
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));
	g_object_class_install_property(
		gobject_class, 
		PROP_STOP_SEQUENCES,
		g_param_spec_boxed(
			"stop-sequences", 
			"Stop Sequences",
			"A list of strings that will stop generation if generated.",
			G_TYPE_STRV, // Use G_TYPE_STRV for gchar**
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));
	g_object_class_install_property(
		gobject_class, 
		PROP_TEMPERATURE,
		g_param_spec_double(
			"temperature", 
			"Temperature",
			"Controls randomness. Lower for less random. Range: 0.0 to 2.0.",
			0.0, 
			2.0, 
			1.0, // Default to 1.0, or -1.0 if you want "unset"
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));

	g_object_class_install_property(
		gobject_class, 
		PROP_MAX_OUTPUT_TOKENS,
		g_param_spec_int(
			"max-output-tokens", 
			"Max Output Tokens",
			"Maximum number of tokens to generate.",
			1, 
			G_MAXINT, 
			800, // Default to 800, or 0/-1 for "unset"
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));

	g_object_class_install_property(
		gobject_class, 
		PROP_TOP_P,
		g_param_spec_double(
			"top-p", 
			"Top P",
			"Cumulative probability cutoff for token selection. Range: 0.0 to 1.0.",
			0.0, 
			1.0, 
			0.8, // Default to 0.8, or -1.0 for "unset"
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));

	g_object_class_install_property(
		gobject_class, 
		PROP_TOP_K,
		g_param_spec_int(
			"top-k", 
			"Top K",
			"Number of highest probability tokens to consider. Range: 1 to 40 (example).",
			1, 
			40, 
			10, // Default to 10, or 0/-1 for "unset"
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
		));

	// --- Define Signals ---
	guint signals[LAST_SIGNAL] = {0};
	signals[SIGNAL_DESCRIPTION_RECEIVED] =
		g_signal_new (
			"description-received", 
			G_TYPE_FROM_CLASS (klass),
			G_SIGNAL_RUN_LAST,
			0,// G_STRUCT_OFFSET (GstGeminiVisionClass, description_received),
			NULL, NULL, NULL, /* no marshaller needed for basic types */
			G_TYPE_NONE, 
			2, 
			G_TYPE_STRING, 
			GST_TYPE_BUFFER
		);

	// Ensure metadata is registered
	gst_gemini_description_meta_get_info();
}

static void
gst_gemini_vision_init (GstGeminiVision * self) {
	self->request_queue = g_async_queue_new();
	self->result_queue = g_async_queue_new();

	self->api_key = NULL;
	self->prompt = g_strdup("Describe what you see in this image");
	self->model_name = g_strdup("gemini-1.5-flash"); // Updated default
	self->analysis_interval_sec = 5.0;
	self->output_metadata = TRUE;

	// Initialize generationConfig properties of API request with defaults
	// For "unset" state, use NULL for stop_sequences, -1.0 for doubles, 0 or -1 for ints
	self->stop_sequences = NULL; // Default to no stop sequences
	self->temperature = 1.0;     // Default from example
	self->max_output_tokens = 800; // Default from example
	self->top_p = 0.8;           // Default from example
	self->top_k = 10;            // Default from example


	self->worker_running = FALSE;
	self->worker_thread = NULL;
	self->result_source = NULL;
	
	gst_video_info_init(&self->input_video_info);
	self->analysis_in_progress = FALSE;
	self->analysis_interval = GST_SECOND * self->analysis_interval_sec;
	self->last_analysis_time_ns = 0;
	self->pending_description = NULL;
	self->input_is_jpeg = FALSE;
}