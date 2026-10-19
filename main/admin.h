#pragma once
// Joins the configured LAN; returns on connection failure, exit or timeout.
// Leaves Wi-Fi stopped and HTTP handlers finished. Credentials are set via BLE only.
void admin_run(void);
