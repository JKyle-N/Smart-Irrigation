/* secrets.example.h -- placeholders for ESP1's commissioning secrets.
 *
 * Copy this file to secrets.h in this folder and put the real values there. secrets.h is ignored by git,
 * because this repository is public. Without a secrets.h, the firmware builds with these placeholders, so
 * CI and a fresh clone still compile.
 *
 * These are only first-boot defaults. A number or PIN changed on the setup portal is saved in NVS and wins.
 */
#ifndef SECRETS_H
#define SECRETS_H

// Owner phone: receives alerts and reports, and is the only sender allowed to give SMS commands. While it is
// empty no SMS command is accepted and no alert goes out. Set it here or on the portal's admin page.
#define SECRET_OWNER_NUMBER ""

// Password of the Irrig-Setup WiFi access point. WPA2 needs at least 8 characters.
#define SECRET_AP_PASS "change-this-password"

// First-boot PIN for the portal's admin page.
#define SECRET_ADMIN_PIN "0000"

#endif // SECRETS_H
