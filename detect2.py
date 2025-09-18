#!/usr/bin/env python3
import sys
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import GObject, Gst, GLib, GstRtspServer
import pyds

# Global variables
PGIE_CLASS_ID_PERSON = 0
PGIE_CLASS_ID_VEHICLE = 2
frame_count = 0
total_people_detected = 0
total_vehicles_detected = 0

def osd_sink_pad_buffer_probe(pad, info, u_data):
    """
    Probe function to get frame data and count people
    """
    global frame_count, total_people_detected, total_vehicles_detected
    frame_count += 1
   
    gst_buffer = info.get_buffer()
    if not gst_buffer:
        return Gst.PadProbeReturn.OK
    # Retrieve batch metadata from the gst_buffer
    batch_meta = pyds.gst_buffer_get_nvds_batch_meta(hash(gst_buffer))
    l_frame = batch_meta.frame_meta_list
   
    while l_frame is not None:
        try:
            frame_meta = pyds.NvDsFrameMeta.cast(l_frame.data)
        except StopIteration:
            break
        frame_number = frame_meta.frame_num
        l_obj = frame_meta.obj_meta_list
        num_people = 0
        num_vehicles = 0
       
        while l_obj is not None:
            try:
                obj_meta = pyds.NvDsObjectMeta.cast(l_obj.data)
            except StopIteration:
                break
           
            # Count people and vehicles correctly
            if obj_meta.class_id == PGIE_CLASS_ID_PERSON:
                num_people += 1
            elif obj_meta.class_id == PGIE_CLASS_ID_VEHICLE:
                num_vehicles += 1
           
            try:
                l_obj = l_obj.next
            except StopIteration:
                break
        # Update totals
        if num_people > 0:
            total_people_detected += num_people
        if num_vehicles > 0:
            total_vehicles_detected += num_vehicles
        # Add display meta for counts
        display_meta = pyds.nvds_acquire_display_meta_from_pool(batch_meta)
        display_meta.num_labels = 1
        py_nvosd_text_params = display_meta.text_params[0]
       
        # Set text properties
        py_nvosd_text_params.display_text = f"People: {num_people} | Vehicles: {num_vehicles} | Frame: {frame_number}"
        py_nvosd_text_params.x_offset = 10
        py_nvosd_text_params.y_offset = 12
        py_nvosd_text_params.font_params.font_name = "Serif"
        py_nvosd_text_params.font_params.font_size = 18
        py_nvosd_text_params.font_params.font_color.red = 1.0
        py_nvosd_text_params.font_params.font_color.green = 1.0
        py_nvosd_text_params.font_params.font_color.blue = 0.0
        py_nvosd_text_params.font_params.font_color.alpha = 1.0
        py_nvosd_text_params.set_bg_clr = 1
        py_nvosd_text_params.text_bg_clr.red = 0.0
        py_nvosd_text_params.text_bg_clr.green = 0.0
        py_nvosd_text_params.text_bg_clr.blue = 0.0
        py_nvosd_text_params.text_bg_clr.alpha = 1.0
        pyds.nvds_add_display_meta_to_frame(frame_meta, display_meta)
       
        # Print progress every 120 frames (less spam)
        if frame_count % 120 == 0:
            print(f"Processing... Frame {frame_number} | People: {num_people} | Vehicles: {num_vehicles}")
       
        try:
            l_frame = l_frame.next
        except StopIteration:
            break
           
    return Gst.PadProbeReturn.OK

def bus_call(bus, message, loop):
    """Bus callback - minimal messages"""
    t = message.type
    if t == Gst.MessageType.EOS:
        print("✓ End of stream")
        loop.quit()
    elif t == Gst.MessageType.ERROR:
        err, debug = message.parse_error()
        print(f"✗ Error: {err}")
        loop.quit()
    return True

def main():
    global total_people_detected, total_vehicles_detected
   
    # RTSP input URL
    input_rtsp = "rtsp://192.168.0.178:8554/person"
   
    print("=== PEOPLE COUNTER (RTSP INPUT to RTSP OUTPUT) ===")
    print(f"Input RTSP: {input_rtsp}")
   
    # Initialize GStreamer
    Gst.init(None)
   
    print("Creating pipeline...")
   
    # Create pipeline
    pipeline = Gst.Pipeline.new("people-counter")
   
    # Create elements (adjusted for RTSP input)
    source = Gst.ElementFactory.make("rtspsrc", "source")
    depay = Gst.ElementFactory.make("rtph264depay", "depay")  # RTP depayloader for H.264
    h264parser = Gst.ElementFactory.make("h264parse", "parser")
    decoder = Gst.ElementFactory.make("nvv4l2decoder", "decoder")
    converter1 = Gst.ElementFactory.make("nvvideoconvert", "converter1")
    streammux = Gst.ElementFactory.make("nvstreammux", "mux")
    pgie = Gst.ElementFactory.make("nvinfer", "inference")
    tiler = Gst.ElementFactory.make("nvmultistreamtiler", "tiler")
    converter2 = Gst.ElementFactory.make("nvvideoconvert", "converter2")
    nvosd = Gst.ElementFactory.make("nvdsosd", "osd")
    converter3 = Gst.ElementFactory.make("nvvideoconvert", "converter3")
    videoconvert = Gst.ElementFactory.make("videoconvert", "videoconv")
    capsfilter = Gst.ElementFactory.make("capsfilter", "caps")
    encoder = Gst.ElementFactory.make("x264enc", "encoder")
    payloader = Gst.ElementFactory.make("rtph264pay", "payloader")
    payloader.set_property("pt", 96)
   
    # Check elements
    elements = [source, depay, h264parser, decoder, converter1, streammux,
                pgie, tiler, converter2, nvosd, converter3, videoconvert,
                capsfilter, encoder, payloader]
   
    if not all(elements):
        print("✗ Failed to create pipeline elements")
        return -1
   
    # Set properties
    source.set_property("location", input_rtsp)
    source.set_property("latency", 0)  # Low latency for live stream
   
    streammux.set_property("width", 1920)
    streammux.set_property("height", 1080)
    streammux.set_property("batch-size", 1)
    streammux.set_property("batched-push-timeout", 4000000)
   
    pgie.set_property("config-file-path", "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_infer_primary.txt")
   
    tiler.set_property("rows", 1)
    tiler.set_property("columns", 1)
    tiler.set_property("width", 1280)
    tiler.set_property("height", 720)
   
    # Set output format
    caps = Gst.Caps.from_string("video/x-raw,format=I420")
    capsfilter.set_property("caps", caps)
   
    # Encoder settings
    encoder.set_property("bitrate", 1500)
    encoder.set_property("speed-preset", 2)
    encoder.set_property("threads", 0)
   
    # Add elements to pipeline
    for element in elements:
        pipeline.add(element)
   
    # Link static elements after depay
    depay.link(h264parser)
    h264parser.link(decoder)
    decoder.link(converter1)
   
    # Get streammux sink pad
    sinkpad = streammux.request_pad_simple("sink_0")
    converter1_srcpad = converter1.get_static_pad("src")
    converter1_srcpad.link(sinkpad)
   
    # Link remaining elements
    links = [
        (streammux, pgie), (pgie, tiler), (tiler, converter2),
        (converter2, nvosd), (nvosd, converter3), (converter3, videoconvert),
        (videoconvert, capsfilter), (capsfilter, encoder), (encoder, payloader)
    ]
   
    for src, dst in links:
        if not src.link(dst):
            print(f"✗ Failed to link {src.get_name()} to {dst.get_name()}")
            return -1
   
    # Dynamic pad connection for rtspsrc
    def source_pad_added(element, pad, user_data):
        caps_str = pad.get_current_caps().to_string() if pad.get_current_caps() else ""
        if "video" in caps_str:
            depay_sinkpad = depay.get_static_pad("sink")
            if pad.link(depay_sinkpad) != Gst.PadLinkReturn.OK:
                print("✗ Failed to link rtspsrc to depay")
   
    source.connect("pad-added", source_pad_added, None)
   
    # Add probe to nvosd
    osdsinkpad = nvosd.get_static_pad("sink")
    if osdsinkpad:
        osdsinkpad.add_probe(Gst.PadProbeType.BUFFER, osd_sink_pad_buffer_probe, 0)
   
    # Set up RTSP server (output)
    server = GstRtspServer.RTSPServer.new()
    server.set_service("8554")  # Output port 8554
    mounts = server.get_mount_points()
    
    # NEW: Create a custom media factory to use the existing pipeline
    class CustomMediaFactory(GstRtspServer.RTSPMediaFactory):
        def __init__(self, pipeline):
            super(CustomMediaFactory, self).__init__()
            self.pipeline = pipeline
            self.set_shared(True)
        
        def do_create_pipeline(self, uri):
            # Take ownership of the existing pipeline
            return GstRtspServer.RTSPMediaFactory.create_pipeline(self, self.pipeline)
    
    factory = CustomMediaFactory(pipeline)
    
    # Set launch for the payloader (but since using custom, adjust)
    # Alternative: Use set_launch with udpsrc, but for direct pipeline, custom is better
    # For simplicity, use set_launch with udpsrc connected to payloader
    # But to integrate pipeline, we need to add udpsrc to pipeline and link to payloader? Wait, better way:
    # Actually, for RTSP out, standard is to end pipeline with rtph264pay, then use udpsrc in factory launch
    # But to fix the error, let's adjust: Add udpsrc and link it as source for the factory
    
    # Better: Create the pipeline up to payloader, then use set_launch with udpsrc ! rtph264depay ! ... but no.
    # Standard DeepStream way: End the processing pipeline with rtph264pay, then create a separate sink pipeline? No.
    # From search: The factory launch is "( udpsrc name=pay0 port=5000 buffer-size=524288 caps=\"application/x-rtp, media=video, clock-rate=90000, encoding-name=H264, payload=96 \" )"
    # Then, you need to add udpsink to the main pipeline, linked to payloader, sending to localhost:5000
    
    # Yes, that's the fix for the linking error: The pipeline needs to output via udpsink to a local UDP port, and the RTSP factory listens on that port with udpsrc.
    
    # So, revert to adding udpsink to pipeline
    udpsink = Gst.ElementFactory.make("udpsink", "udpsink")
    udpsink.set_property("host", "127.0.0.1")
    udpsink.set_property("port", 5000)
    udpsink.set_property("sync", False)
    udpsink.set_property("async", False)
    
    pipeline.add(udpsink)
    
    if not payloader.link(udpsink):
        print("✗ Failed to link payloader to udpsink")
        return -1
    
    # Now, for factory
    factory = GstRtspServer.RTSPMediaFactory.new()
    factory.set_launch('( udpsrc name=pay0 port=5000 buffer-size=524288 caps="application/x-rtp, media=video, clock-rate=90000, encoding-name=(string)H264, payload=96 " )')
    factory.set_shared(True)
    
    mounts.add_factory("/output", factory)
    server.attach(None)
    
    print("RTSP output stream ready at rtsp://127.0.0.1:8554/output")
   
    # Create event loop
    loop = GLib.MainLoop()
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", bus_call, loop)
   
    print("Starting processing...")
    print("Press Ctrl+C to stop.")
    print("Connect to output with: vlc rtsp://127.0.0.1:8554/output")
   
    # Start pipeline
    ret = pipeline.set_state(Gst.State.PLAYING)
    if ret == Gst.StateChangeReturn.FAILURE:
        print("✗ Failed to start pipeline")
        return -1
   
    try:
        loop.run()
    except KeyboardInterrupt:
        print("\n⚠ Stopped by user")
        print(f"\n=== RESULTS ===")
        print(f"✓ Frames processed: {frame_count}")
        print(f"✓ People detected: {total_people_detected}")
        print(f"✓ Vehicles detected: {total_vehicles_detected}")
   
    # Stop pipeline
    pipeline.set_state(Gst.State.NULL)
   
    return 0

if __name__ == '__main__':
    sys.exit(main())
