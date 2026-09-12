# ESP-OpenClaw Native Node

This directory is the first implementation of an OpenClaw Native Node for ESP32.
The device is a hardware capability host, not an Agent: LLM calls, memory, chat
routing, and policy remain on the OpenClaw Gateway.

## Current status

- `components/openclaw_node`: reusable WebSocket transport component
- Implements the v4 node connection shape, challenge capture, v3 device-auth
  payload callback, command claims, `node.invoke.request` dispatch, and
  `node.invoke.result` replies
- Keeps command execution behind an application callback and a declared-command
  allowlist
- `application/openclaw_node`: reserved standalone application target
- M5StickS3 board support remains in `application/edge_agent`; it has not yet
  been wired to the new transport

This is an integration skeleton, not a claimed production-ready firmware.
The signing callback and persistent device identity must be backed by secure
storage before deployment. Do not put a private key in `sdkconfig` for a real
product.

## Protocol boundary

The client uses the OpenClaw Gateway WebSocket protocol. It waits for
`connect.challenge`, signs the v3 device-auth payload, sends a `role: node`
connect request, and handles the Gateway's `node.invoke.request` event.
Responses use the Gateway's `node.invoke.result` request method.

The current component is intentionally independent of ESP-Claw's Agent loop.
It can later be embedded in the board-specific application after hardware
callbacks and secure identity storage are implemented.
