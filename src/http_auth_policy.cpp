// The authentication managers are intentionally compiled separately from the
// HTTP server adapter.  This keeps their storage and token rules unit-testable
// without pulling in the application's network, logging, or server globals.
#define SUNSHINE_HTTP_AUTH_POLICY_ONLY
#include "http_auth.cpp"
