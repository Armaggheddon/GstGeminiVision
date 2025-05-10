#!/usr/bin/env python3

import sys
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstVideo', '1.0') # Though not explicitly used after simplification, good to keep for Gst context
from gi.repository import Gst, GLib, GObject # GstVideo might not be needed directly
import signal
import os # For API key environment variable (optional)

class GeminiVisionApp:
    def __init__(self):
        # Initialize GStreamer
        Gst.init(None)

        # --- Configuration for Gemini Vision element ---
        # IMPORTANT: Replace with your actual API key or set GST_GEMINI_API_KEY environment variable
        self.api_key = os.environ.get("GST_GEMINI_API_KEY", "YOUR_API_KEY_HERE")
        self.prompt_text = "Describe this scene in one short sentence. Focus on the main subject."
        self.model_name = "gemini-2.0-flash" # Or another model name
        self.analysis_interval_seconds = 5.0
        self.output_as_metadata = False # Use signal for this example

        # New generationConfig parameters
        self.stop_sequences = ["Title", "Conclusion"] # Python list of strings
        self.temperature = 0.7
        self.max_tokens = 150
        self.top_p_val = 0.9
        self.top_k_val = 20
        # --- End Configuration ---

        # Create the pipeline
        self.pipeline = Gst.Pipeline.new("gemini-vision-pipeline")

        # Create elements
        self.source = Gst.ElementFactory.make("videotestsrc", "source")
        self.convert1 = Gst.ElementFactory.make("videoconvert", "converter1")
        # Assuming your plugin is now named 'geminivision'
        self.gemini = Gst.ElementFactory.make("geminivision", "gemini-vision-element")
        self.convert2 = Gst.ElementFactory.make("videoconvert", "converter2")
        self.sink = Gst.ElementFactory.make("autovideosink", "sink")

        # Check elements were created
        if not all([self.pipeline, self.source, self.convert1, self.gemini, self.convert2, self.sink]):
            print("Error: Not all elements could be created.")
            if not self.gemini:
                print("Failed to create 'geminivision' element. Ensure the plugin is installed and discoverable (GST_PLUGIN_PATH).")
            sys.exit(1)

        # Configure elements
        self.source.set_property("pattern", 0)  # 0 is the smtpe pattern

        if self.api_key == "YOUR_API_KEY_HERE" or not self.api_key:
            print("ERROR: API Key is not set or is a placeholder.")
            print("Please set the GST_GEMINI_API_KEY environment variable or edit the script.")
            sys.exit(1)

        self.gemini.set_property("api-key", self.api_key)
        self.gemini.set_property("prompt", self.prompt_text)
        self.gemini.set_property("model-name", self.model_name)
        self.gemini.set_property("analysis-interval", self.analysis_interval_seconds)
        self.gemini.set_property("output-metadata", self.output_as_metadata)

        # Set new generationConfig properties
        # For G_TYPE_STRV, GObject introspection usually expects a list/tuple of strings from Python
        self.gemini.set_property("stop-sequences", self.stop_sequences)
        self.gemini.set_property("temperature", self.temperature)
        self.gemini.set_property("max-output-tokens", self.max_tokens)
        self.gemini.set_property("top-p", self.top_p_val)
        self.gemini.set_property("top-k", self.top_k_val)

        # Connect to the description-received signal
        self.gemini.connect("description-received", self.on_description_received)

        # Add elements to pipeline
        self.pipeline.add(self.source)
        self.pipeline.add(self.convert1)
        self.pipeline.add(self.gemini)
        self.pipeline.add(self.convert2)
        self.pipeline.add(self.sink)

        # Link elements
        if not self.source.link(self.convert1):
            print("Error: Could not link source to convert1")
            sys.exit(1)
        if not self.convert1.link(self.gemini):
            print("Error: Could not link convert1 to gemini")
            sys.exit(1)
        if not self.gemini.link(self.convert2):
            print("Error: Could not link gemini to convert2")
            sys.exit(1)
        if not self.convert2.link(self.sink):
            print("Error: Could not link convert2 to sink")
            sys.exit(1)

        # Create a main loop
        self.loop = GLib.MainLoop()

        # Add watch for messages from the pipeline
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_message)

        # Handle keyboard interrupt
        signal.signal(signal.SIGINT, self.handle_sigint)

    def on_description_received(self, element, description, buffer):
        pts_ns = buffer.pts # PTS in nanoseconds
        pts_str = f"{pts_ns // 1_000_000_000}.{pts_ns % 1_000_000_000:09d}" # Format as S.NS
        
        print("=================================")
        print(f"Frame time: {pts_str} (PTS: {pts_ns})") # Matches GStreamer time format more closely
        print(f"Description: {description}")
        print("=================================\n")
        sys.stdout.flush() # Ensure it prints immediately like fflush(stdout)
        # Python's GObject signal handlers should typically return None or not return explicitly
        # unless a specific return value is expected by the signal emission.
        # For "run-last" signals without a specific return type, not returning is fine.

    def on_message(self, bus, message):
        t = message.type
        if t == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            print(f"Error from {message.src.get_name()}: {err.message}")
            if debug:
                print(f"Debug info: {debug}")
            self.loop.quit()
        elif t == Gst.MessageType.EOS:
            print("End of stream")
            self.loop.quit()
        elif t == Gst.MessageType.STATE_CHANGED:
            if message.src == self.pipeline:
                old_state, new_state, pending_state = message.parse_state_changed()
                print(f"Pipeline state changed from {Gst.Element.state_get_name(old_state)} to {Gst.Element.state_get_name(new_state)}")

    def handle_sigint(self, sig, frame):
        print("Interrupt received, stopping...")
        self.stop()

    def run(self):
        # Start the pipeline
        print("Setting pipeline to PLAYING state...")
        ret = self.pipeline.set_state(Gst.State.PLAYING)
        if ret == Gst.StateChangeReturn.FAILURE:
            print("Unable to set the pipeline to the playing state.")
            print("Check for errors from the 'geminivision' element, especially API key.")
            # Attempt to get more detailed error from bus if pipeline failed to start
            bus = self.pipeline.get_bus()
            msg = bus.timed_pop_filtered(Gst.CLOCK_TIME_NONE, Gst.MessageType.ERROR | Gst.MessageType.EOS)
            if msg:
                self.on_message(bus, msg)
            sys.exit(1)

        print("Pipeline running...")
        print("Press Ctrl+C to quit")

        try:
            self.loop.run()
        except Exception as e:
            print(f"Error in main loop: {e}")
        finally:
            self.stop()

    def stop(self):
        if hasattr(self, 'pipeline') and self.pipeline:
            print("Cleaning up...")
            self.pipeline.set_state(Gst.State.NULL)
        if hasattr(self, 'loop') and self.loop.is_running():
            self.loop.quit()
        print("Pipeline stopped.")

if __name__ == "__main__":
    app = GeminiVisionApp()
    app.run()