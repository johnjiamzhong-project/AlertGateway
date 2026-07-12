#!/usr/bin/env python3
import sys
import json
import base64
import time
from paho.mqtt import client as mqtt_client

broker = '127.0.0.1'
port = 1883
topic = 'desk/testsrc2_detect'
client_id = 'mqtt-capture-script'
output_file = sys.argv[1] if len(sys.argv) > 1 else 'mqtt_captured.json'

messages = []

def connect_mqtt():
    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            print("Connected to MQTT Broker!")
        else:
            print(f"Failed to connect, return code {rc}\n")
    client = mqtt_client.Client(client_id=client_id, callback_api_version=mqtt_client.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.connect(broker, port)
    return client

def subscribe(client: mqtt_client.Client):
    def on_message(client, userdata, msg):
        payload_str = msg.payload.decode()
        print(f"Received msg: {payload_str[:200]}...")
        try:
            payload = json.loads(payload_str)
            messages.append(payload)
            # Validate thumbnail if present
            if "thumbnail" in payload:
                thumb = payload["thumbnail"]
                width = thumb.get("width")
                height = thumb.get("height")
                fmt = thumb.get("format")
                data_b64 = thumb.get("data")
                if data_b64:
                    raw_data = base64.b64decode(data_b64)
                    expected_len = int(width * height * 1.5)
                    print(f"[Validation] Thumbnail: {width}x{height} {fmt}. Base64 data length: {len(data_b64)}, Decoded binary length: {len(raw_data)}, Expected: {expected_len}. Match: {len(raw_data) == expected_len}")
            # Validate ROI events if present
            if "roi_events" in payload:
                print(f"[Validation] ROI Events: {payload['roi_events']}")
        except Exception as e:
            print(f"Error parsing message: {e}")

    client.subscribe(topic)
    client.on_message = on_message

def main():
    client = connect_mqtt()
    subscribe(client)
    client.loop_start()

    start_time = time.time()
    # Let it run for up to 75 seconds
    while time.time() - start_time < 75:
        time.sleep(1)
        if len(messages) >= 5:
            pass

    client.loop_stop()
    client.disconnect()

    with open(output_file, 'w') as f:
        json.dump(messages, f, indent=2)
    print(f"Saved {len(messages)} messages to {output_file}")

if __name__ == '__main__':
    main()
