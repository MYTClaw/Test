#!/usr/bin/env python3
import sys
import time
import gi
gi.require_version('Gst', '1.0')
from gi.repository import GLib, Gst # Updated to GLib.MainLoop
from common.is_aarch64 import is_aarch64
from common.bus_call import bus_call
import pyds
import asyncio # For WebSocket
import websockets # Install via pip3 install websockets
import json
import queue # For sending data to WS thread
import threading # For running WS in separate thread
PGIE_CLASS_ID_VEHICLE = 0 # Adapt to your classes from labels.txt (e.g., 0=vehicle, 2=person)
PGIE_CLASS_ID_BICYCLE = 1
PGIE_CLASS_ID_PERSON = 2
PGIE_CLASS_ID_ROADSIGN = 3
last_print_time = 0 # Global for timing
send_queue = queue.Queue() # Queue for sending data to persistent WS
WIDTH = 1280  # Stream resolution width
HEIGHT = 720  # Stream resolution height
GRID_SIZE = 300  # Target grid size for rescaling
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
    global last_print_time
    global device_id # Access global
    gst_buffer = info.get_buffer()
    if not gst_buffer:
        print("Unable to get GstBuffer")
        return
    batch_meta = pyds.gst_buffer_get_nvds_batch_meta(hash(gst_buffer))
    l_frame = batch_meta.frame_meta_list
    current_time = time.time() * 1e9 # Nanoseconds for consistency
    interval_ns = 5 * 1e9 # 5 seconds
    if current_time - last_print_time >= interval_ns:
        vehicle_count = 0
        person_detections = []
        vehicle_detections = []  # Added to collect vehicle positions if needed
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
                    vehicle_count += 1
                    # Optionally add vehicle position rescaling if desired
                    # rect = obj_meta.rect_params
                    # center_x = rect.left + rect.width / 2
                    # center_y = rect.top + rect.height / 2
                    # norm_x = center_x / WIDTH
                    # norm_y = center_y / HEIGHT
                    # grid_x = max(0, min(GRID_SIZE, int(norm_x * GRID_SIZE)))
                    # grid_y = max(0, min(GRID_SIZE, int(norm_y * GRID_SIZE)))
                    # pos = {"x": grid_x, "y": grid_y}
                    # vehicle_detections.append(pos)
                # Ignore roadsign
                l_obj = l_obj.next
            l_frame = l_frame.next
        people_count = len(person_detections)
        # Print to terminal
        print(f"Device_id: {device_id}, At timestamp {int(current_time)}: People: {people_count}, Vehicles: {vehicle_count}")
        # Queue data for persistent WS send
        data = {
            "deviceId": device_id,
            "detections": {"person": person_detections},  # Rescaled positions
            "people_count": people_count,
            "vehicle_count": vehicle_count
            # If adding vehicle detections: ,"vehicle": vehicle_detections
        }
        send_queue.put(data)
        print(f"Queued for WS: {data}")
        last_print_time = current_time
    return Gst.PadProbeReturn.OK
def cb_newpad(source, pad, depay):
    caps = pad.get_current_caps()
    if not caps:
        return
    gststruct = caps.get_structure(0)
    gstname = gststruct.get_name()
    if gstname.find("application/x-rtp") != -1: # RTP pad for video
        depay_pad = depay.get_static_pad("sink")
        link_result = pad.link(depay_pad)
        if link_result == Gst.PadLinkReturn.OK:
            print("Dynamic pad linked to depay")
        else:
            print(f"Dynamic pad link failed: {link_result}")
def main(args):
    global facility_id, zone_id, device_id # Declare globals (facility and zone not used in send)
    # Load JSON config
    config_path = "/home/nano_anarr/facility_config.json"
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
        rtsp_uri = config_data['device']['devices'][0]['rtsp_link']
    else:
        sys.stderr.write("No devices found in config\n")
        sys.exit(1)
    # Start persistent WebSocket thread
    ws_uri = "ws://192.168.0.129:3001/edge" # Replace with your WS server URI
    start_ws_thread(ws_uri)
    # No need for input args now, using rtsp_uri from config
    Gst.init(None)
    # Create gstreamer elements
    print("Creating Pipeline")
    pipeline = Gst.Pipeline()
    if not pipeline:
        sys.stderr.write(" Unable to create Pipeline\n")
    print("Creating Source")
    source = Gst.ElementFactory.make("rtspsrc", "rtspsrc")
    source.set_property('location', rtsp_uri)
    source.set_property('latency', 100) # From your config
    depay = Gst.ElementFactory.make("rtph264depay", "depay")
    parser = Gst.ElementFactory.make("h264parse", "parser")
    decoder = Gst.ElementFactory.make("nvv4l2decoder", "decoder")
    print("Creating Streamux")
    streammux = Gst.ElementFactory.make("nvstreammux", "Stream-muxer")
    streammux.set_property('width', 1280)
    streammux.set_property('height', 720)
    streammux.set_property('batch-size', 1)
    streammux.set_property('batched-push-timeout', 40000)
    streammux.set_property('live-source', 1) # From your config
    print("Creating Inference")
    pgie = Gst.ElementFactory.make("nvinfer", "primary-inference")
    pgie.set_property('config-file-path', "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_infer_primary.txt") # Your primary config full path
    tracker = Gst.ElementFactory.make("nvtracker", "tracker")
    tracker.set_property('ll-lib-file', '/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so')
    tracker.set_property('ll-config-file', '/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml') # From your config
    nvanalytics = Gst.ElementFactory.make("nvdsanalytics", "analytics")
    nvanalytics.set_property('config-file', '/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_nvdsanalytics_roi.txt') # Your analytics config full path
    nvvidconv = Gst.ElementFactory.make("nvvideoconvert", "convertor")
    nvosd = Gst.ElementFactory.make("nvdsosd", "onscreendisplay")
    print("Creating EGLGLES Sink")
    if is_aarch64():
        transform = Gst.ElementFactory.make("nvegltransform", "nvegl-transform")
    sink = Gst.ElementFactory.make("nveglglessink", "nvvideo-renderer") # Type 2 from your config
    sink.set_property('sync', 0)
    # Add elements to pipeline
    pipeline.add(source)
    pipeline.add(depay)
    pipeline.add(parser)
    pipeline.add(decoder)
    pipeline.add(streammux)
    pipeline.add(pgie)
    pipeline.add(tracker)
    pipeline.add(nvanalytics)
    pipeline.add(nvvidconv)
    pipeline.add(nvosd)
    if is_aarch64():
        pipeline.add(transform)
    pipeline.add(sink)
    # Connect pad-added for dynamic linking
    source.connect("pad-added", cb_newpad, depay)
    # Link the rest (after depay)
    depay.link(parser)
    parser.link(decoder)
    sinkpad = streammux.request_pad_simple("sink_0") # Updated to avoid deprecation warning
    srcpad = decoder.get_static_pad("src")
    srcpad.link(sinkpad)
    streammux.link(pgie)
    pgie.link(tracker)
    tracker.link(nvanalytics)
    nvanalytics.link(nvvidconv)
    nvvidconv.link(nvosd)
    if is_aarch64():
        nvosd.link(transform)
        transform.link(sink)
    else:
        nvosd.link(sink)
    # Add the probe for custom logic
    osd_sink_pad = nvosd.get_static_pad("sink")
    if not osd_sink_pad:
        sys.stderr.write(" Unable to get sink pad of nvosd \n")
    osd_sink_pad.add_probe(Gst.PadProbeType.BUFFER, osd_sink_pad_buffer_probe, None)
    loop = GLib.MainLoop() # Updated to GLib.MainLoop
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", bus_call, loop)
    print("Starting pipeline")
    pipeline.set_state(Gst.State.PLAYING)
    try:
        loop.run()
    except:
        pass
    pipeline.set_state(Gst.State.NULL)
if __name__ == '__main__':
    sys.exit(main(sys.argv))
