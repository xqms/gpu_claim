gpu_claim
=========

Very simple GPU job queuing system.

User HowTo
----------

Query the current state with `gpu`:

```console
$ gpu
[0] NVIDIA TITAN X (Pascal) |  0% |     98 /  12884 MB |                  free |
[1] NVIDIA TITAN X (Pascal) |  0% |     98 /  12884 MB |                  free |
[2] NVIDIA TITAN X (Pascal) |  0% |     98 /  12884 MB |                  free |
[3] NVIDIA TITAN X (Pascal) |  0% |     98 /  12884 MB |                  free |
```

Run a job:

```console
$ gpu run nvidia-smi -L
GPU 0: NVIDIA TITAN X (Pascal) (UUID: GPU-05deeaa6-3899-bc94-5660-0f1a7d93f221)
```

Run a job on two GPUs:

```console
$ gpu -n 2 run nvidia-smi -L
GPU 0: NVIDIA TITAN X (Pascal) (UUID: GPU-05deeaa6-3899-bc94-5660-0f1a7d93f221)
GPU 1: NVIDIA TITAN X (Pascal) (UUID: GPU-92161328-5ab6-9b3f-042e-eee46ba5d7aa)
```

Run a singularity container with PyTorch:

```console
$ singularity pull docker://nvcr.io/nvidia/pytorch:22.03-py3
$ gpu -n 2 run singularity run --nv pytorch_22.03-py3.sif python

=============
== PyTorch ==
=============

NVIDIA Release 22.03 (build 33569136)
PyTorch Version 1.12.0a0+2c916ef

Container image Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

Copyright (c) 2014-2022 Facebook Inc.
Copyright (c) 2011-2014 Idiap Research Institute (Ronan Collobert)
Copyright (c) 2012-2014 Deepmind Technologies    (Koray Kavukcuoglu)
Copyright (c) 2011-2012 NEC Laboratories America (Koray Kavukcuoglu)
Copyright (c) 2011-2013 NYU                      (Clement Farabet)
Copyright (c) 2006-2010 NEC Laboratories America (Ronan Collobert, Leon Bottou, Iain Melvin, Jason Weston)
Copyright (c) 2006      Idiap Research Institute (Samy Bengio)
Copyright (c) 2001-2004 Idiap Research Institute (Ronan Collobert, Samy Bengio, Johnny Mariethoz)
Copyright (c) 2015      Google Inc.
Copyright (c) 2015      Yangqing Jia
Copyright (c) 2013-2016 The Caffe contributors
All rights reserved.

Various files include modifications (c) NVIDIA CORPORATION & AFFILIATES.  All rights reserved.

This container image and its contents are governed by the NVIDIA Deep Learning Container License.
By pulling and using the container, you accept the terms and conditions of this license:
https://developer.nvidia.com/ngc/nvidia-deep-learning-container-license

Python 3.8.12 | packaged by conda-forge | (default, Jan 30 2022, 23:42:07)
[GCC 9.4.0] on linux
Type "help", "copyright", "credits" or "license" for more information.
>>> import torch
>>> torch.cuda.device_count()
2
>>>
```

Admin HowTo
-----------

Installation on Debian/Ubuntu:
```
# Repo setup
sudo mkdir -p /etc/apt/keyrings
curl -sL https://xqms.github.io/gpu_claim/gpg.key | sudo tee /etc/apt/keyrings/gpu-claim-keyring.asc > /dev/null
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/gpu-claim-keyring.asc] https://xqms.github.io/gpu_claim/ all main" | sudo tee /etc/apt/sources.list.d/gpu-claim.list > /dev/null

# Installation
sudo apt update && sudo apt install gpu-claim
```

Debian package files are also available on the GitHub releases page for manual installation.

The server features a maintenance mode, which blocks accepting new jobs
and displays an appropriate message to the user. You can switch to
maintenance mode by creating a file `/var/run/gpu_claim_maintenance`.
Removing the file disables maintenance mode again.
