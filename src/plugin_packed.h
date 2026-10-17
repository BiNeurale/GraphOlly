////////////////////////////////////////////////////////////////////////////////
// Include di plugin.h con l'allineamento a byte SOLO per le strutture del PDK.
//
// Perche' esiste: plugin.h impone l'allineamento a byte, e compilando con
// -fpack-struct=1 si ottiene, ma si impacchettano ANCHE le strutture di Windows
// (WNDCLASS, SCROLLINFO, PAINTSTRUCT, BITMAPINFOHEADER...), che nel sistema non
// sono impacchettate: le chiamate API leggerebbero campi sbagliati, a partire
// dai cbSize. Qui il pack avvolge il solo plugin.h.
////////////////////////////////////////////////////////////////////////////////

#ifndef GRAPHOLLY_PLUGIN_PACKED_H
#define GRAPHOLLY_PLUGIN_PACKED_H

#include <windows.h>

#pragma pack(push,1)
#include "plugin.h"
#pragma pack(pop)

#endif
