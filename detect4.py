#!/usr/bin/env python3
import sys
import time
import gi
import math
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import GObject, Gst, GLib, GstRtspServer
import pyds
import asyncio # For WebSocket
import websockets # Install via pip3 install websockets
import json
import queue # For sending data to WS thread
import threading # For running WS in separate thread
# Global variables
PGIE_CLASS_ID_VEHICLE = 0
PGIE_CLASS_ID_BICYCLE = 1
PGIE_CLASS_ID_PERSON = 2
PGIE_CLASS_ID_ROADSIGN = 3
frame_count = 0
total_people_detected = 0
total_vehicles_detected = 0
last_print_time = 0 # Global for timing
send_queue = queue.Queue() # Queue for sending data to persistent WS
WIDTH = 640 # Stream resolution width
HEIGHT = 640 # Stream resolution height
GRID_SIZE = 300 # Target grid size for rescaling
previous_person_detections = None
previous_vehicle_detections = None
previous_vehicle_count = None
def has_significant_change(curr, prev, threshold=20):
    if len(curr) != len(prev):
        return True
    curr_s = sorted(curr, key=lambda p: (p['x'], p['y']))
    prev_s = sorted(prev, key=lambda p: (p['x'], p['y']))
    for c, p in zip(curr_s, prev_s):
        dx = c['x'] - p['x']
        dy = c['y'] - p['y']
        dist = math.sqrt(dx**2 + dy**2)
        if dist > threshold:
            return True
    return False
async def ws_loop(uri):
    while True: # Reconnect loop in case of disconnection
        try:
            async with websockets.connect(uri) as ws:
                print("WebSocket connected persistently")
                while True:
                    data = await asyncio.to_thread(send_queue.get) # Wait for data from queue (Python 3.9+)
                    await ws.send(json.dumps(data))
                    print(f"Sent via WS: {data}")
        except Exception as e:
            print(f"WS connection failed or closed: {e}. Reconnecting in 5 seconds...")
            await asyncio.sleep(5)
def start_ws_thread(uri):
    def thread_func():
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(ws_loop(uri))
    thread = threading.Thread(target=thread_func, daemon=True)
    thread.start()
def osd_sink_pad_buffer_probe(pad, info, u_data):
    """
    Probe function to get frame data and count people
    """
    global frame_count, total_people_detected, total_vehicles_detected
    global last_print_time
    global device_id # Access global
    global previous_person_detections, previous_vehicle_detections, previous_vehicle_count
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
            elif obj_meta.class_id in (PGIE_CLASS_ID_VEHICLE, PGIE_CLASS_ID_BICYCLE):
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
    current_time = time.time() * 1e9 # Nanoseconds for consistency
    interval_ns = 1 * 1e9 # 1 second
    if current_time - last_print_time >= interval_ns:
        person_detections = []
        vehicle_detections = []
        l_frame = batch_meta.frame_meta_list
        while l_frame is not None:
            try:
                frame_meta = pyds.NvDsFrameMeta.cast(l_frame.data)
            except StopIteration:
                break
            l_obj = frame_meta.obj_meta_list
            while l_obj is not None:
                try:
                    obj_meta = pyds.NvDsObjectMeta.cast(l_obj.data)
                except StopIteration:
                    break
                if obj_meta.class_id == PGIE_CLASS_ID_PERSON:
                    rect = obj_meta.rect_params
                    center_x = rect.left + rect.width / 2
                    center_y = rect.top + rect.height / 2
                    norm_x = center_x / WIDTH
                    norm_y = center_y / HEIGHT
                    grid_x = max(0, min(GRID_SIZE, int(norm_x * GRID_SIZE)))
                    grid_y = max(0, min(GRID_SIZE, int(norm_y * GRID_SIZE)))
                    pos = {"x": grid_x, "y": grid_y}
                    person_detections.append(pos)
                elif obj_meta.class_id in (PGIE_CLASS_ID_VEHICLE, PGIE_CLASS_ID_BICYCLE):
                    rect = obj_meta.rect_params
                    center_x = rect.left + rect.width / 2
                    center_y = rect.top + rect.height / 2
                    norm_x = center_x / WIDTH
                    norm_y = center_y / HEIGHT
                    grid_x = max(0, min(GRID_SIZE, int(norm_x * GRID_SIZE)))
                    grid_y = max(0, min(GRID_SIZE, int(norm_y * GRID_SIZE)))
                    pos = {"x": grid_x, "y": grid_y}
                    vehicle_detections.append(pos)
                l_obj = l_obj.next
            l_frame = l_frame.next
        people_count = len(person_detections)
        vehicle_count = len(vehicle_detections)
        significant_change = False
        if previous_person_detections is None or previous_vehicle_detections is None or previous_vehicle_count is None:
            significant_change = True
        elif people_count != len(previous_person_detections) or vehicle_count != previous_vehicle_count:
            significant_change = True
        else:
            significant_change = has_significant_change(person_detections, previous_person_detections) or has_significant_change(vehicle_detections, previous_vehicle_detections)
        if significant_change:
            # Print to terminal
            print(f"Device_id: {device_id}, At timestamp {int(current_time)}: People: {people_count}, Vehicles: {vehicle_count}")
            # Queue data for persistent WS send
            data = {
                "deviceId": device_id,
                "detections": {"person": person_detections, "vehicle": vehicle_detections}, # Rescaled positions
                "people_count": people_count,
                "vehicle_count": vehicle_count
            }
            send_queue.put(data)
            print(f"Queued for WS: {data}")
            previous_person_detections = [p.copy() for p in person_detections]
            previous_vehicle_detections = [p.copy() for p in vehicle_detections]
            previous_vehicle_count = vehicle_count
        last_print_time = current_time
         
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
    global facility_id, zone_id, device_id # Declare globals (facility and zone not used in send)
    global previous_person_detections, previous_vehicle_detections, previous_vehicle_count
    previous_person_detections = None
    previous_vehicle_detections = None
    previous_vehicle_count = None
    # Load JSON config
    config_path = "/home/ubuntu/facility_config.json"
    try:
        with open(config_path, 'r') as f:
            config_data = json.load(f)
    except Exception as e:
        sys.stderr.write(f"Failed to load config file: {e}\n")
        sys.exit(1)
    # Extract required values (assuming single device in array)
    facility_id = config_data['device']['facility']['id']
    if 'devices' in config_data['device'] and len(config_data['device']['devices']) > 0:
        device_id = config_data['device']['devices'][0]['id']
        zone_id = config_data['device']['devices'][0]['zoneId']
        input_rtsp = config_data['device']['devices'][0]['rtsp_link']
    else:
        sys.stderr.write("No devices found in config\n")
        sys.exit(1)
    # Start persistent WebSocket thread
    ws_uri = "ws://10.3.158.111:3001/edge" # Replace with your WS server URI
    start_ws_thread(ws_uri)
 
    print("=== PEOPLE COUNTER (RTSP INPUT) ===")
    print(f"Input RTSP: {input_rtsp}")
 
    # Initialize GStreamer
    Gst.init(None)
 
    print("Creating pipeline...")
 
    # Create pipeline
    pipeline = Gst.Pipeline.new("people-counter")
 
    # Create elements (adjusted for RTSP input, no output)
    source = Gst.ElementFactory.make("rtspsrc", "source")
    depay = Gst.ElementFactory.make("rtph264depay", "depay") # RTP depayloader for H.264
    h264parser = Gst.ElementFactory.make("h264parse", "parser")
    decoder = Gst.ElementFactory.make("nvv4l2decoder", "decoder")
    converter1 = Gst.ElementFactory.make("nvvideoconvert", "converter1")
    converter1.set_property('copy-hw', 1) # VIC mode for stability
    streammux = Gst.ElementFactory.make("nvstreammux", "mux")
    pgie = Gst.ElementFactory.make("nvinfer", "inference")
    converter2 = Gst.ElementFactory.make("nvvideoconvert", "converter2")
    converter2.set_property('copy-hw', 1)
    nvosd = Gst.ElementFactory.make("nvdsosd", "osd")
    fakesink = Gst.ElementFactory.make("fakesink", "fakesink") # Discard output
 
    # Check elements
    elements = [source, depay, h264parser, decoder, converter1, streammux,
                pgie, converter2, nvosd, fakesink]
 
    if not all(elements):
        print("✗ Failed to create pipeline elements")
        return -1
 
    # Set properties
    source.set_property("location", input_rtsp)
    source.set_property("latency", 0) # Low latency for live stream
 
    streammux.set_property("width", 640)
    streammux.set_property("height", 640)
    streammux.set_property("batch-size", 1)
    streammux.set_property("batched-push-timeout", 8000000) # Increased for stability
    streammux.set_property("enable-padding", 0) # Avoid extra padding
    streammux.set_property('live-source', 1)
 
    pgie.set_property("config-file-path", "/opt/nvidia/deepstream/deepstream-7.1/samples/configs/deepstream-app/config_infer_primary.txt")
 
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
        (streammux, pgie), (pgie, converter2),
        (converter2, nvosd), (nvosd, fakesink)
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
 
    # Create event loop
    loop = GLib.MainLoop()
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", bus_call, loop)
 
    print("Starting processing...")
    print("Press Ctrl+C to stop.")
 
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
