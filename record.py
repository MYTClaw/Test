import cv2

def record_rtsp(rtsp_url, output_file="output.mp4", duration=30):
    # Open RTSP stream
    cap = cv2.VideoCapture(rtsp_url)

    if not cap.isOpened():
        print("Error: Could not open RTSP stream")
        return

    # Get native properties
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS)

    # Some RTSP streams don't provide FPS metadata, fallback
    if fps == 0 or fps is None:
        fps = 25.0  

    print(f"Recording at {width}x{height} @ {fps}fps")

    # Define video writer (MP4 container, H.264 encoding)
    fourcc = cv2.VideoWriter_fourcc(*'mp4v')
    out = cv2.VideoWriter(output_file, fourcc, fps, (width, height))

    frame_count = 0
    max_frames = int(duration * fps)

    while cap.isOpened() and frame_count < max_frames:
        ret, frame = cap.read()
        if not ret:
            print("Stream ended or error.")
            break
        out.write(frame)
        frame_count += 1

    cap.release()
    out.release()
    print(f"Saved recording to {output_file}")


# Example usage
record_rtsp("rtsp://username:password@ip:port/stream", "recorded.mp4", duration=60)
