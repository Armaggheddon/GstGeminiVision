#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include "gstgeminivision.h" // Forward declare your element's registration function

// -------- ADD THIS LINE --------
#ifndef PACKAGE
#define PACKAGE "geminivision" // Or your chosen plugin name
#endif
// -------------------------------

static gboolean
plugin_init (GstPlugin * plugin)
{
	GST_DEBUG_CATEGORY_INIT (
		gst_gemini_vision_debug_category, 
		"geminivision", 
		0,
		"Gemini Vision Plugin"
	);
    // Register your element type here
    return gst_element_register (
		plugin, 
		"geminivision", 
		GST_RANK_NONE,
		GST_TYPE_GEMINI_VISION
    ); // GST_TYPE_GEMINI_VISION will be defined in gstgeminiprocessor.h
}

// GStreamer plugin definition
GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	geminivision, // Plugin name (lowercase, no "gst" prefix here)
	"Processes video frames and sends them to Gemini for description", // Description
	plugin_init,
	"1.0.0",         	// Version
	"MIT",          	// License
	"gstgeminivision", 	// Source module
	"https://github.com/Armaggheddon/GstGeminiVision" // Origin
)