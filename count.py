#!/usr/bin/env python3

import sys
import gi
import os

gi.require_version('Gst', '1.0')
from gi.repository import GObject, Gst, GLib

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
        
        # Set text properties - FIXED: Show correct counts
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
        print("✓ Video processing completed")
        loop.quit()
    elif t == Gst.MessageType.ERROR:
        err, debug = message.parse_error()
        print(f"✗ Error: {err}")
        loop.quit()
    # Suppress warnings and other messages
    return True


def main():
    global total_people_detected, total_vehicles_detected
    
    # File paths
    input_file = "/home/nano_anarr/Videos/carx.mp4"
    output_file = "/home/nano_anarr/Videos/people_count_final.mp4"
    
    print("=== PEOPLE COUNTER ===")
    print(f"Input: {input_file}")
    print(f"Output: {output_file}")
    
    # Check input file
    if not os.path.exists(input_file):
        print("✗ Input file not found")
        return -1
    
    # Remove existing output file
    if os.path.exists(output_file):
        os.remove(output_file)
    
    # Initialize GStreamer
    Gst.init(None)
    
    print("Creating pipeline...")
    
    # Create pipeline
    pipeline = Gst.Pipeline.new("people-counter")
    
    # Create elements
    source = Gst.ElementFactory.make("filesrc", "source")
    demuxer = Gst.ElementFactory.make("qtdemux", "demuxer") 
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
    h264parser2 = Gst.ElementFactory.make("h264parse", "parser2")
    muxer = Gst.ElementFactory.make("mp4mux", "muxer")
    sink = Gst.ElementFactory.make("filesink", "sink")
    
    # Check elements
    elements = [source, demuxer, h264parser, decoder, converter1, streammux, 
                pgie, tiler, converter2, nvosd, converter3, videoconvert,
                capsfilter, encoder, h264parser2, muxer, sink]
    
    if not all(elements):
        print("✗ Failed to create pipeline elements")
        return -1
    
    # Set properties
    source.set_property("location", input_file)
    
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
    
    sink.set_property("location", output_file)
    sink.set_property("sync", False)
    
    # Add elements to pipeline
    for element in elements:
        pipeline.add(element)
    
    # Link static elements
    source.link(demuxer)
    
    # Get streammux sink pad
    sinkpad = streammux.request_pad_simple("sink_0")
    converter1_srcpad = converter1.get_static_pad("src")
    converter1_srcpad.link(sinkpad)
    
    # Link remaining elements
    links = [
        (streammux, pgie), (pgie, tiler), (tiler, converter2),
        (converter2, nvosd), (nvosd, converter3), (converter3, videoconvert),
        (videoconvert, capsfilter), (capsfilter, encoder), (encoder, h264parser2),
        (h264parser2, muxer), (muxer, sink)
    ]
    
    for src, dst in links:
        if not src.link(dst):
            print(f"✗ Failed to link {src.get_name()} to {dst.get_name()}")
            return -1
    
    # Dynamic pad connection
    def demux_pad_added(demux, pad, user_data):
        if pad.get_name().startswith("video"):
            parser_sinkpad = h264parser.get_static_pad("sink")
            if pad.link(parser_sinkpad) == Gst.PadLinkReturn.OK:
                h264parser.link(decoder)
                decoder.link(converter1)
    
    demuxer.connect("pad-added", demux_pad_added, None)
    
    # Add probe to nvosd
    osdsinkpad = nvosd.get_static_pad("sink")
    if osdsinkpad:
        osdsinkpad.add_probe(Gst.PadProbeType.BUFFER, osd_sink_pad_buffer_probe, 0)
    
    # Create event loop
    loop = GLib.MainLoop()
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", bus_call, loop)
    
    print("Starting processing...")
    print("This may take several minutes. Press Ctrl+C to stop.")
    
    # Start pipeline
    ret = pipeline.set_state(Gst.State.PLAYING)
    if ret == Gst.StateChangeReturn.FAILURE:
        print("✗ Failed to start pipeline")
        return -1
    
    try:
        loop.run()
    except KeyboardInterrupt:
        print("\n⚠ Stopped by user")
    
    # Stop pipeline
    pipeline.set_state(Gst.State.NULL)
    
    # Check output and show summary
    if os.path.exists(output_file):
        size = os.path.getsize(output_file)
        print(f"\n=== RESULTS ===")
        print(f"✓ Output: {output_file}")
        print(f"✓ Size: {size:,} bytes ({size/(1024*1024):.1f} MB)")
        print(f"✓ Frames processed: {frame_count}")
        print(f"✓ People detected: {total_people_detected}")
        print(f"✓ Vehicles detected: {total_vehicles_detected}")
        print(f"\nTo view: vlc {output_file}")
    else:
        print("✗ Output file not created")
        return -1
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
