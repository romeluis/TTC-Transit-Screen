# Security policy

## Intended deployment

TTC Transit Screen is intended for a private, trusted local network. It does not implement user accounts, API authentication, transport encryption for its local web app, encrypted credential storage, or a signed OTA trust chain.

The setup access point is open. Wi-Fi credentials cross the local link over plaintext HTTP and are stored unencrypted in ESP32 NVS. The local status API exposes the SSID and IP address. Any client that can reach the device can read status, change stops and the sleep schedule, replace Wi-Fi settings, restart the device, or upload compatible firmware. The firmware also disables server-certificate validation when requesting public transit data.

Do not expose the device to the public internet, port-forward it, or attach it to a network with untrusted clients. Prefer a trusted IoT VLAN that has outbound internet access but no inbound access from guest networks.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting feature for this repository when it is available. Do not include real Wi-Fi credentials, IP addresses, device dumps, or other personal information in a public issue.

Security-hardening contributions are welcome, but changes to authentication, credential storage, certificate validation, or OTA trust should be treated as compatibility-affecting work and tested on both supported memory variants.
