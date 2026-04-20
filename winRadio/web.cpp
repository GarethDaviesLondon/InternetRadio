#include "web.h"

// TODO(web): see header for the planned ESPAsyncWebServer + WebSocket
// design. Until that lands these are no-ops so the orchestrator can
// already call webBegin() / webPoll().

static bool s_running = false;

void webBegin()    { /* not implemented */ }
void webPoll()     { /* not implemented */ }
void webStop()     { s_running = false; }
bool webRunning()  { return s_running; }
