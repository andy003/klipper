#!/usr/bin/env python3
"""
TCP client test script for testing the multi-threaded serial communication
Usage: python3 tcp_test_client.py
"""

import socket
import time
import threading
import sys

TCP_HOST = 'localhost'
TCP_PORT = 8080

def receive_data(sock):
    """Thread function to receive data from server"""
    try:
        while True:
            data = sock.recv(1024)
            if not data:
                print("Server disconnected")
                break
            print(f"Received: {data.decode('ascii', errors='ignore')}")
    except Exception as e:
        print(f"Receive error: {e}")

def main():
    try:
        # Connect to server
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        print(f"Connecting to {TCP_HOST}:{TCP_PORT}...")
        sock.connect((TCP_HOST, TCP_PORT))
        print("Connected successfully!")
        
        # Start receiver thread
        receiver_thread = threading.Thread(target=receive_data, args=(sock,))
        receiver_thread.daemon = True
        receiver_thread.start()
        
        # Send test commands
        test_commands = [
            "M105\n",  # Get temperature
            "M114\n",  # Get position
            "G28\n",   # Home
        ]
        
        print("Sending test commands...")
        for cmd in test_commands:
            print(f"Sending: {cmd.strip()}")
            sock.send(cmd.encode('ascii'))
            time.sleep(1)
        
        print("\nPress Enter to send custom commands (or 'quit' to exit):")
        while True:
            try:
                user_input = input("> ")
                if user_input.lower() in ['quit', 'exit']:
                    break
                if user_input:
                    sock.send((user_input + '\n').encode('ascii'))
            except KeyboardInterrupt:
                break
                
    except ConnectionRefusedError:
        print(f"Failed to connect to {TCP_HOST}:{TCP_PORT}")
        print("Make sure the simulator is running with TCP support")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        try:
            sock.close()
        except:
            pass
        print("Disconnected")

if __name__ == "__main__":
    main()