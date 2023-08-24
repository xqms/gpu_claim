FROM nvidia/cuda:12.2.0-devel-ubuntu20.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install software-properties-common && rm -rf /var/lib/apt/lists/*
RUN add-apt-repository ppa:ubuntu-toolchain-r/test

RUN apt-get update && DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install build-essential libboost-program-options-dev ninja-build cmake g++-13 gcc-13 && rm -rf /var/lib/apt/lists/*

