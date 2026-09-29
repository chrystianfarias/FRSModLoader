#pragma once

#include <string>

// The game's own map data, served to the page at runtime as JSON.
//
// GameTextures hands a page the minimap's pixels; this hands it what is drawn
// on them, read out of the same folder the game reads:
//
//     http://nfsu2.data/L4RA/roads.json    every street, one polyline each,
//                                          in world units, with its id and class
//     http://nfsu2.data/L4RA/graph.json    the GPS's nodes: [street, point,
//                                          street, point], one street into the next
//     http://nfsu2.data/L4RA/events.json   every race start: where its trigger
//                                          sits and which races begin there
//     http://nfsu2.data/L4RA/shops.json    every shop with a zone: name, kind
//                                          (PAINT, BODY, PERFORMANCE, SPECIALTY,
//                                          CARLOT, GARAGE, OTHER) and position
//
// The streets are the GPS's road graph (TRACKS\ROUTES<region>\
// RoutesFreeRoam.bin), alleys included; being floats they stay sharp at any
// zoom, where the 512x512 TRACKMAP does not. The race starts come from the
// career in GLOBAL\GLOBALB.BUN and the trigger zones in PathsFreeRoam.bin.
// Formats: NOTES.md, "The minimap".
//
// Nothing is extracted ahead of time: a mod that edits the roads or moves a
// race shows up here too. Each answer is built on the first request and kept.
namespace GameData
{
    // `path` is the URL's path ("/L4RA/roads.json"). On success fills `body`
    // and `mime` and returns true; false means 404. Safe from any thread.
    bool Get(const std::string& path, std::string* body, std::string* mime);
}
