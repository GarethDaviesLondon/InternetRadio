#include "provision.h"

static bool s_active = false;

void provisionStart()  { /* TODO: AP + captive portal -- see header */ }
void provisionPoll()   { /* not implemented */ }
void provisionStop()   { s_active = false; }
bool provisionActive() { return s_active; }
