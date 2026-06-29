cmake_minimum_required(VERSION 3.20)
project(Hermes VERSION 0.1.0 LANGUAGES CXX)

# Hermes is the connection/fan-out layer that sits in front of Hephaestus.
# Clients connect to Hermes; it multiplexes each channel to a single Hephaestus
# ffmpeg session and fans the byte stream out to all connected clients.
# Implementation is deferred — this stub keeps the root build clean.
message(STATUS "[hermes] connection handler — not yet implemented")
