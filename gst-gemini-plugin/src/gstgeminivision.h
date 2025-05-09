#ifndef __GST_GEMINI_VISION_H__
#define __GST_GEMINI_VISION_H__

#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h> // For GstVideoMeta and GstVideoInfo
#include <json-c/json.h>           // For json-c
#include <curl/curl.h>             // For CURL

GST_DEBUG_CATEGORY_EXTERN (gst_gemini_vision_debug_category);

G_BEGIN_DECLS

#define GST_TYPE_GEMINI_VISION (gst_gemini_vision_get_type())
#define GST_GEMINI_VISION(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_GEMINI_VISION, GstGeminiVision))
#define GST_GEMINI_VISION_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_GEMINI_VISION, GstGeminiVisionClass))
#define GST_IS_GEMINI_VISION(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_GEMINI_VISION))
#define GST_IS_GEMINI_VISION_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_GEMINI_VISION))
#define GST_GEMINI_VISION_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_GEMINI_VISION, GstGeminiVisionClass))

typedef struct _GstGeminiVision GstGeminiVision;
typedef struct _GstGeminiVisionClass GstGeminiVisionClass;

// Custom Metadata for Gemini Description
#define GST_GEMINI_DESCRIPTION_META_API_TYPE (gst_gemini_description_meta_api_get_type())
#define GST_GEMINI_DESCRIPTION_META_INFO (gst_gemini_description_meta_get_info())

typedef struct _GstGeminiDescriptionMeta GstGeminiDescriptionMeta;

struct _GstGeminiDescriptionMeta {
	GstMeta meta;
	gchar *description;
};

GType gst_gemini_description_meta_api_get_type (void);
const GstMetaInfo *gst_gemini_description_meta_get_info (void);
GstGeminiDescriptionMeta *gst_buffer_add_gemini_description_meta (GstBuffer *buffer, const gchar *description);


// Structure to hold data for asynchronous requests
typedef struct _GeminiRequestData {
	guchar *image_data;
	gsize image_size;
	gchar *api_key;
	gchar *prompt;
	gchar *model_name;
	GstBuffer *original_buffer;
	GstGeminiVision *self; // Changed from GstGeminiProcessor

	// generationConfig parameters
	gchar **stop_sequences;
	gdouble temperature;
	gint max_output_tokens;
	gdouble top_p;
	gint top_k;
} GeminiRequestData;

// Structure to hold results from asynchronous processing
typedef struct _GeminiResultData {
	gchar *description;
	GstBuffer *original_buffer;
	GstGeminiVision *processor_element; // Changed from GstGeminiProcessor
} GeminiResultData;


struct _GstGeminiVision {
	GstBaseTransform parent;

	// Properties
	gchar *api_key;
	gchar *prompt;
	gchar *model_name;
	gdouble analysis_interval_sec;
	gboolean output_metadata;

	// generationConfig properties
	gchar **stop_sequences;
	gdouble temperature;
	gint max_output_tokens;
	gdouble top_p;
	gint top_k;

	// Internal state
	GAsyncQueue *request_queue;
	GAsyncQueue *result_queue;
	GThread *worker_thread;
	gboolean worker_running;
	GSource *result_source; // For main context processing of results

	GstVideoInfo input_video_info;
	gboolean input_is_jpeg;

	gboolean analysis_in_progress;
	GstClockTime analysis_interval;
	GstClockTime last_analysis_time_ns;

	gchar *pending_description; // Description to be applied to subsequent buffers
};

struct _GstGeminiVisionClass {
	GstBaseTransformClass parent_class;

	// Signals
	void (*description_received) (GstGeminiVision *self, const gchar *description, GstBuffer *buffer);
};

GType gst_gemini_vision_get_type (void);

// Signals (optional, if not using metadata primarily)
enum {
	SIGNAL_DESCRIPTION_RECEIVED,
	LAST_SIGNAL
};

G_END_DECLS

#endif /* __GST_GEMINI_VISION_H__ */