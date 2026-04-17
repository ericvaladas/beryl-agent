#pragma once

// Bind the per-DLL client WS listener on the first available port; returns the port or 0.
int StartClientServer();

// Bind the registry listener; returns true on success.
bool StartRegistryServer();

// Initiate a WS connection to the registry listener.
void ConnectToRegistry();
