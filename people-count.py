#!/usr/bin/env python3
import sys
import time
import gi
gi.require_version('Gst', '1.0')
from gi.repository import GObject, Gst
from common.is_aarch64 import is_aarch64
from common.bus_call import bus_call
import pyds
import asyncio  # For WebSocket
import websockets  # Install via pip3 install websockets
import json

PGIE_CLASS_ID_VEHICLE = 0  # Adapt to your classes from labels.txt (e.g., 0=vehicle, 2=person)
PGIE_CLASS_ID_BICYCLE = 1
PGIE_CLASS_ID_PERSON = 2
PGIE_CLASS_ID_ROADSIGN = 3

last_print_time = 0  # Global for timing

async def send_via_ws(facility_id, zone_id, device_id, count, timestamp):
    uri = "ws://192.168.0.164:3001/edge"  # Replace with your WS server URI
    try:
        async with websockets.connect(uri) as ws:
            data = {
                "facility_id": facility_id,
                "zone_id": zone_id,
                "device_id": device_id,
                "count": count,
                "timestamp": timestamp
            }
            await ws.send(json.dumps(data))
            print(f"Sent via WS: {data}")
    except Exception as e:
        print(f"WS send failed: {e}")

def osd_sink_pad_buffer_probe(pad, info, u_data):
    global last_print_time
    global facility_id, zone_id, device_id  # Access globals
    gst_buffer = info.get_buffer()
    if not gst_buffer:
        print("Unable to get GstBuffer")
        return
    batch_meta = pyds.gst_buffer_get_nvds_batch_meta(hash(gst_buffer))
    l_frame = batch_meta.frame_meta_list
    current_time = time.time() * 1e9  # Nanoseconds for consistency
    interval_ns = 5 * 1e9  # 5 seconds
    if current_time - last_print_time >= interval_ns:
        total_detections = 0
        while l_frame is not None:
            try:
                frame_meta = pyds.NvDsFrameMeta.cast(l_frame.data)
            except StopIteration:
                break
            total_detections += frame_meta.num_obj_meta  # Total detections in this frame
            l_frame = l_frame.next  # Move to next frame (if multi-source; yours is single)
        # Print to terminal
        print(f"Facility_id: {facility_id}, Zone_id: {zone_id}, Device_id: {device_id}, At timestamp {int(current_time)}: Total detections in frame: {total_detections}")
        # Send via WebSocket (run async in loop)
        loop = asyncio.new_event_loop()
        loop.run_until_complete(send_via_ws(facility_id, zone_id, device_id, total_detections, int(current_time)))
        last_print_time = current_time
    return Gst.PadProbeReturn.OK

def cb_newpad(source, pad, depay):
    caps = pad.get_current_caps()
    if not caps:
        return
    gststruct = caps.get_structure(0)
    gstname = gststruct.get_name()
    if gstname.find("application/x-rtp") != -1:  # RTP pad for video
        depay_pad = depay.get_static_pad("sink")
        if pad.link(depay_pad) == Gst.PadLinkReturn.OK:
            print("Dynamic pad linked to depay")
        else:
            print("Dynamic pad link failed")

def main(args):
    global facility_id, zone_id, device_id  # Declare globals

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

    # No need for input args now, using rtsp_uri from config

    GObject.threads_init()
    Gst.init(None)

    # Create gstreamer elements
    print("Creating Pipeline")
    pipeline = Gst.Pipeline()
    if not pipeline:
        sys.stderr.write(" Unable to create Pipeline\n")

    print("Creating Source")
    source = Gst.ElementFactory.make("rtspsrc", "rtspsrc")
    source.set_property('location', rtsp_uri)
    source.set_property('latency', 100)  # From your config

    depay = Gst.ElementFactory.make("rtph264depay", "depay")
    parser = Gst.ElementFactory.make("h264parse", "parser")
    decoder = Gst.ElementFactory.make("nvv4l2decoder", "decoder")

    print("Creating Streamux")
    streammux = Gst.ElementFactory.make("nvstreammux", "Stream-muxer")
    streammux.set_property('width', 1280)
    streammux.set_property('height', 720)
    streammux.set_property('batch-size', 1)
    streammux.set_property('batched-push-timeout', 40000)
    streammux.set_property('live-source', 1)  # From your config

    print("Creating Inference")
    pgie = Gst.ElementFactory.make("nvinfer", "primary-inference")
    pgie.set_property('config-file-path', "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_infer_primary.txt")  # Your primary config full path

    tracker = Gst.ElementFactory.make("nvtracker", "tracker")
    tracker.set_property('ll-lib-file', '/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so')
    tracker.set_property('ll-config-file', '/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml')  # From your config

    nvanalytics = Gst.ElementFactory.make("nvdsanalytics", "analytics")
    nvanalytics.set_property('config-file', '/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_nvdsanalytics_roi.txt')  # Your analytics config full path

    nvvidconv = Gst.ElementFactory.make("nvvideoconvert", "convertor")
    nvosd = Gst.ElementFactory.make("nvdsosd", "onscreendisplay")

    print("Creating EGLGLES Sink")
    if is_aarch64():
        transform = Gst.ElementFactory.make("nvegltransform", "nvegl-transform")
    sink = Gst.ElementFactory.make("nveglglessink", "nvvideo-renderer")  # Type 2 from your config
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
    sinkpad = streammux.get_request_pad("sink_0")
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

    loop = GObject.MainLoop()
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
