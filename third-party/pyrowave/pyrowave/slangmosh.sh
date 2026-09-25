#!/bin/bash

slangmosh --output shaders/slangmosh.hpp shaders/slangmosh.json --namespace PyroWave -O --strip
slangmosh --output shaders/slangmosh_scaler.hpp Granite/video/slangmosh_encode.json --namespace PyroWave -O --strip
