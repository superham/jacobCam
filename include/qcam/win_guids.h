// SPDX-License-Identifier: GPL-2.0-or-later
//
// GUIDs owned by this project. They are generated once and must stay fixed:
// the device interface GUID is baked into qcamusb.inf and into every
// already-installed device, and the CLSIDs are what the Frame Server uses to
// find the virtual camera's COM server.

#ifndef QCAM_WIN_GUIDS_H_
#define QCAM_WIN_GUIDS_H_

#ifdef _WIN32

#include <guiddef.h>

// {B17CB711-B823-4C31-89EA-E0FC9DC46605}
// Must match Dev_AddReg/DeviceInterfaceGUIDs in driver/qcamusb.inf.
DEFINE_GUID(GUID_DEVINTERFACE_QCAM,
            0xb17cb711, 0xb823, 0x4c31, 0x89, 0xea, 0xe0, 0xfc, 0x9d, 0xc4, 0x66, 0x05);

// {9BB2B860-94A0-4B47-ADF7-7F3FBCA2FB6E}
// The media source COM class the virtual camera registers.
DEFINE_GUID(CLSID_QcamMediaSource,
            0x9bb2b860, 0x94a0, 0x4b47, 0xad, 0xf7, 0x7f, 0x3f, 0xbc, 0xa2, 0xfb, 0x6e);

// {B3EC32E0-9B8B-41F0-BD3B-C7D4DFE1FB36}
DEFINE_GUID(CLSID_QcamActivator,
            0xb3ec32e0, 0x9b8b, 0x41f0, 0xbd, 0x3b, 0xc7, 0xd4, 0xdf, 0xe1, 0xfb, 0x36);

// Shared-memory object names used between qcamsvc and the virtual camera.
#define QCAM_RING_NAME    L"Global\\qcam.frames.4EA75BBB"
#define QCAM_RING_EVENT   L"Global\\qcam.frame.4EA75BBB"
#define QCAM_DEMAND_EVENT L"Global\\qcam.demand.4EA75BBB"
#define QCAM_RING_MUTEX   L"Global\\qcam.lock.4EA75BBB"
#define QCAM_CONTROLS_NAME L"Global\\qcam.controls.4EA75BBB"
#define QCAM_CONTROL_PIPE L"\\\\.\\pipe\\qcam.control.4EA75BBB"

// The per-service SID qcamsvc runs with. It is the only non-administrator
// principal allowed to write the frame ring.
#define QCAM_SERVICE_ACCOUNT L"NT SERVICE\\qcamsvc"

// The Windows Camera Frame Server, which hosts qcamvcam.dll. It is the only
// non-administrator principal allowed to read frames.
#define QCAM_READER_ACCOUNT  L"NT SERVICE\\FrameServer"

#endif  // _WIN32
#endif  // QCAM_WIN_GUIDS_H_
