#ifndef _WIFI_PORTAL_H_
#define _WIFI_PORTAL_H_

// [SANDBOX] Captive-portal handling for public / hotel / company / school WiFi that is
// "connected" at the radio level but blocks the internet until a web login is done.
//
//   1. Probe http://connectivitycheck.gstatic.com/generate_204 (expects an empty 204).
//   2. If the answer is a redirect or an HTML page, follow it to the login page.
//   3. Fill the page's <form> with the saved username / password (or just accept the terms
//      for click-through portals) and submit it, keeping cookies and following redirects.
//   4. Probe again to confirm that the internet is reachable.
//
// Limits: only plain HTML form portals work. Portals that need JavaScript, OTP / SMS codes,
// CAPTCHAs or a vendor app cannot be passed automatically.

#include <atomic>
#include <string>

enum class PortalResult {
    Online,       // internet reachable, no portal in the way
    LoggedIn,     // portal detected and passed
    Unreachable,  // probe could not complete (isolated LAN, no DNS, ...): treat as normal
    Failed,       // portal detected but login did not succeed
};

class WifiPortal {
public:
    // Blocking; run it from a task, never from the event loop. `cancel` may be null.
    static PortalResult EnsureOnline(const std::string& username, const std::string& password,
                                     const std::atomic<bool>* cancel = nullptr);
};

#endif // _WIFI_PORTAL_H_
